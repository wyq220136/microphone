#include "cfg.h"
#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_CLASSIC_SPP
#include "spp_transport.h"
#include "task_cooperation.h"
#include "reset_progress.h"
#include <esp32-hal-bt.h>
#if __has_include(<esp32-hal-alloc-bt-classic-mem.h>)
// Arduino 3.3.12 releases unused BT memory in initArduino(), before setup().
// Direct IDF SPP calls do not link BluetoothSerial's retention constructor.
// Use the same core-provided declaration, before either task can start.
#include <esp32-hal-alloc-bt-classic-mem.h>
#define SINGLEMIC_HAS_CLASSIC_MEM_RESERVATION 1
#endif
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_idf_version.h>
#include <cstring>
#include <esp_timer.h>

SppTransport *SppTransport::activeInstance_ = nullptr;

bool SppTransport::begin() {
  if (!transportAtomicStorageValid(eventOverflows_)) return false;
  // Keep the HAL linked for controller status. New cores require the explicit
  // memory reservation above; btStarted() alone does not retain Classic RAM.
  const bool controllerStarted = btStarted();
#if SINGLEMIC_HAS_CLASSIC_MEM_RESERVATION
  const bool memoryReleased = btMemReleased(BT_MODE_CLASSIC_BT);
  Serial.printf("SPP startup: classic_memory=%s controller=%d\n",
                memoryReleased ? "released" : "reserved",
                static_cast<int>(esp_bt_controller_get_status()));
  if (memoryReleased) {
    Serial.println("SPP initialization failed: Classic Bluetooth memory was released before setup");
    return false;
  }
#endif
  eventQueue_ = xQueueCreate(32, sizeof(Event));
  if (!eventQueue_ || !channel_.begin(SPPConfig::PACKET_QUEUE_DEPTH) ||
      !diagnostics_.begin()) return false;
  activeInstance_ = this;
  if (xTaskCreatePinnedToCore(senderTask, "SPP owner", SPPConfig::SENDER_TASK_STACK,
      this, SPPConfig::SENDER_TASK_PRIORITY, &senderTaskHandle_,
      SPPConfig::SENDER_TASK_CORE) != pdPASS) return false;
  channel_.setOwner(senderTaskHandle_);
  if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_config_t config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    config.mode = ESP_BT_MODE_CLASSIC_BT;
    if (esp_bt_controller_init(&config) != ESP_OK) return false;
  }
  if (!controllerStarted && esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT) != ESP_OK) return false;
  if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_UNINITIALIZED &&
      esp_bluedroid_init() != ESP_OK) return false;
  if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_ENABLED &&
      esp_bluedroid_enable() != ESP_OK) return false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
  const esp_err_t nameResult = esp_bt_gap_set_device_name(TransportConfig::DEVICE_NAME);
#else
  const esp_err_t nameResult = esp_bt_dev_set_device_name(TransportConfig::DEVICE_NAME);
#endif
  if (nameResult != ESP_OK ||
      esp_bt_gap_register_callback(gapCallback) != ESP_OK ||
      esp_spp_register_callback(sppCallback) != ESP_OK) return false;
  esp_bt_io_cap_t capability = ESP_BT_IO_CAP_NONE;
  if (esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &capability, sizeof(capability)) != ESP_OK)
    return false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // IDF 5's legacy esp_spp_init() enables ERTM implicitly. Select Basic
  // explicitly to preserve the IDF 4.4 RFCOMM negotiation used by this device.
  esp_spp_cfg_t sppConfig = {};
  sppConfig.mode = ESP_SPP_MODE_CB;
  sppConfig.enable_l2cap_ertm = false;
  sppConfig.tx_buffer_size = 0; // Only used in VFS mode, not callback mode.
  const esp_err_t sppInitResult = esp_spp_enhanced_init(&sppConfig);
#else
  const esp_err_t sppInitResult = esp_spp_init(ESP_SPP_MODE_CB);
#endif
  if (sppInitResult != ESP_OK) {
    Serial.printf("SPP initialization failed: %d\n", static_cast<int>(sppInitResult));
    return false;
  }
  Serial.println("SPP initialization requested: L2CAP Basic, ERTM disabled");
  Serial.printf("SPP TX: batch_max_packets=%u batch_max_bytes=%u retry_ms=%lu\n",
                static_cast<unsigned>(SPPConfig::WRITE_BATCH_PACKETS),
                static_cast<unsigned>(SPPConfig::WRITE_BATCH_BYTES),
                static_cast<unsigned long>(SPPConfig::WRITE_RETRY_DELAY_MS));
  const uint32_t started = millis();
  while (millis() - started < 10000) {
    const auto state = channel_.snapshot();
    if (state.failed) return false;
    if (state.ready) {
      Serial.printf("SPP server ready: %s; hold Key1 %lu seconds for pairing; "
                    "paired clients can reconnect without Key1; trace=static_spsc\n", TransportConfig::DEVICE_NAME,
                    static_cast<unsigned long>(IOConfig::PAIRING_HOLD_MS / 1000));
      return true;
    }
    delay(10); // Boot only; no locks or callback waits.
  }
  return false;
}

void SppTransport::postEvent(const Event &event) {
  // SDK task callback, not a hardware ISR: copy state without waiting.
  // Keep ordered events (a single flag would lose consecutive completions).
  // Never encode audio, read I2S, log, delay or call Bluetooth APIs here.
  if (xQueueSend(eventQueue_, &event, 0) != pdTRUE)
    eventOverflows_.fetch_add(1, std::memory_order_relaxed);
  channel_.wake();
}
void SppTransport::gapCallback(esp_bt_gap_cb_event_t type, esp_bt_gap_cb_param_t *param) {
  auto *self = activeInstance_;
  if (!self || !param) return;
  Event event;
  event.timeMs = millis();
  if (type == ESP_BT_GAP_CFM_REQ_EVT) {
    event.type = GAP_CONFIRM;
    std::memcpy(event.address, param->cfm_req.bda, sizeof(event.address));
  } else if (type == ESP_BT_GAP_AUTH_CMPL_EVT) {
    event.type = GAP_AUTH;
    event.status = param->auth_cmpl.stat;
  } else return;
  self->postEvent(event);
}
void SppTransport::sppCallback(esp_spp_cb_event_t type, esp_spp_cb_param_t *param) {
  auto *self = activeInstance_;
  if (!self || !param) return;
  Event event;
  event.type = type;
  event.timeMs = millis();
  switch (type) {
    case ESP_SPP_INIT_EVT: event.status = param->init.status; break;
    case ESP_SPP_START_EVT: event.status = param->start.status; break;
    case ESP_SPP_SRV_OPEN_EVT:
      event.status = param->srv_open.status; event.handle = param->srv_open.handle; break;
    case ESP_SPP_CLOSE_EVT:
      event.status = param->close.status; event.handle = param->close.handle;
      event.detail = param->close.port_status; event.flag = param->close.async; break;
    case ESP_SPP_WRITE_EVT:
      event.status = param->write.status; event.handle = param->write.handle;
      event.length = param->write.len; event.flag = param->write.cong; break;
    case ESP_SPP_CONG_EVT:
      event.status = param->cong.status; event.handle = param->cong.handle;
      event.flag = param->cong.cong; break;
    default: return;
  }
  self->postEvent(event);
}
void SppTransport::trace(int kind, uint32_t handle, int status, int length,
                         uint32_t detail, bool fault, uint32_t timeMs) {
  diagnostics_.record(txEpoch_, handle, kind, status, length, detail, fault, timeMs);
}
void SppTransport::knownFailure() {
  ++counters_.writeFailures;
  writeInFlight_ = false;
  retryStartedMs_ = millis();
  retryPending_ = true;
  if (currentPacket_.isControl()) ++counters_.controlFailures;
  // Only called for an immediate rejection or an explicitly zero-byte result.
  // Keep DATA too: dropping it here turns transient stack pressure into gaps.
}

void SppTransport::clearWriteBatch() {
  hasPacket_ = false;
  writeBatchLength_ = writeBatchPackets_ = writeBatchOffset_ = 0;
  writeBatchLatencyRecorded_ = false;
}

void SppTransport::prepareWriteBatch() {
  // Called after validating the first packet, never while a write is pending.
  // START/END stay separate so control completion refers to that exact packet.
  writeBatchLength_ = currentPacket_.packet.length;
  writeBatchPackets_ = 1;
  writeBatchCaptureUs_[0] = currentPacket_.captureUs;
  std::memcpy(writeBatch_, currentPacket_.packet.bytes, writeBatchLength_);
  if (!currentPacket_.isControl()) {
    TransportPacket next;
    while (writeBatchPackets_ < SPPConfig::WRITE_BATCH_PACKETS &&
           channel_.peek(next)) {
      if (next.isControl() || next.epoch != currentPacket_.epoch ||
          next.stream != currentPacket_.stream ||
          next.packet.length > sizeof(writeBatch_) - writeBatchLength_) break;
      if (!channel_.take(next)) break;  // This task is the sole consumer.
      std::memcpy(writeBatch_ + writeBatchLength_, next.packet.bytes,
                  next.packet.length);
      writeBatchLength_ += next.packet.length;
      writeBatchCaptureUs_[writeBatchPackets_] = next.captureUs;
      ++writeBatchPackets_;
    }
  }
  if (writeBatchPackets_ > counters_.writeBatchHighWater)
    counters_.writeBatchHighWater = writeBatchPackets_;
}

void SppTransport::processEvent(const Event &event) {
  // Owner-task context only; callbacks merely post the event above.
  if (event.type == GAP_CONFIRM) {
    // This API may wait for SDK queue space: call it only from the owner task.
    esp_bd_addr_t address;
    std::memcpy(address, event.address, sizeof(address));
    const esp_err_t result = esp_bt_gap_ssp_confirm_reply(address, true);
    trace(GAP_CONFIRM, 0, result, 0, 0, result != ESP_OK, event.timeMs);
    return;
  }
  if (event.type == GAP_AUTH) {
    trace(TRACE_AUTH, 0, event.status, 0, 0, event.status != ESP_BT_STATUS_SUCCESS, event.timeMs);
    return;
  }
  trace(event.type, event.handle, event.status, event.length,
        event.type == ESP_SPP_CLOSE_EVT ? (event.detail << 1) | event.flag : event.flag,
        event.type == ESP_SPP_CLOSE_EVT || event.status != ESP_SPP_SUCCESS, event.timeMs);
  switch (event.type) {
    case ESP_SPP_INIT_EVT:
      initFailed_ = event.status != ESP_SPP_SUCCESS ||
          esp_spp_start_srv(ESP_SPP_SEC_AUTHENTICATE, ESP_SPP_ROLE_SLAVE, 0, "ESP32SPP") != ESP_OK;
      break;
    case ESP_SPP_START_EVT:
      serviceReady_ = event.status == ESP_SPP_SUCCESS;
      initFailed_ = !serviceReady_;
      scanDirty_ = serviceReady_;
      break;
    case ESP_SPP_SRV_OPEN_EVT:
      if (event.status != ESP_SPP_SUCCESS) break;
      if (connected_) { ++counters_.staleEvents; trace(TRACE_EXTRA_CLIENT, event.handle, 0, 0, 0, true); break; }
      connectionHandle_ = event.handle;
      ++txEpoch_;
      connected_ = true;
      congested_ = controlFault_ = hasPacket_ = writeInFlight_ = retryPending_ = false;
      clearWriteBatch();
      completedControl_ = activeStream_ = 0;
      if (recoveryCloseSeen_) eventFault_ = recoveryCloseSeen_ = false;
      scanDirty_ = true;
      break;
    case ESP_SPP_CLOSE_EVT:
      if (!connected_ || event.handle != connectionHandle_) { ++counters_.staleEvents; break; }
      counters_.lastCloseMs = event.timeMs;
      counters_.lastCloseStatus = event.status;
      counters_.lastClosePortStatus = event.detail;
      counters_.lastCloseAsync = event.flag;
      if (hasPacket_) {
        counters_.packetsDropped += writeBatchPackets_;
        counters_.epochDrops += writeBatchPackets_;
      }
      // Anonymous SDK rejection cannot be assigned safely across an abandoned
      // write/session. Ignore such rejections thereafter rather than mis-ACK.
      if (writeInFlight_) anonymousUncertain_ = true;
      connected_ = congested_ = hasPacket_ = writeInFlight_ = retryPending_ = false;
      clearWriteBatch();
      connectionHandle_ = activeStream_ = completedControl_ = 0;
      ++txEpoch_; ++counters_.disconnects;
      if (eventFault_) recoveryCloseSeen_ = true;
      scanDirty_ = true;
      break;
    case ESP_SPP_CONG_EVT:
      if (connected_ && event.handle == connectionHandle_ && event.status == ESP_SPP_SUCCESS) {
        congested_ = event.flag;
        ++counters_.congestionEvents;
        counters_.lastCongestionEventMs = event.timeMs;
      } else ++counters_.staleEvents;
      break;
    case ESP_SPP_WRITE_EVT: {
      const bool rejected = event.status != ESP_SPP_SUCCESS && event.handle == 0 && event.length == -1;
      if (!connected_ || !writeInFlight_ || currentPacket_.epoch != txEpoch_ ||
          (rejected ? anonymousUncertain_ : event.handle != connectionHandle_)) {
        ++counters_.staleEvents; break;
      }
      ++counters_.writeEvents;
      counters_.lastWriteEventMs = event.timeMs;
      counters_.lastWriteStatus = event.status;
      if (!rejected) congested_ = event.flag;
      if (event.status == ESP_SPP_SUCCESS && event.length > 0 &&
          static_cast<size_t>(event.length) <= writeBatchLength_ - writeBatchOffset_) {
        writeInFlight_ = retryPending_ = false;
        counters_.bytesQueued += event.length;
        writeBatchOffset_ += event.length;
        if (writeBatchOffset_ == writeBatchLength_) {
          counters_.packetsSent += writeBatchPackets_;
          if (currentPacket_.isControl()) completedControl_ = currentPacket_.control;
          clearWriteBatch();
        } else {
          // IDF's PORT_WriteDataCO reports the exact accepted prefix on success.
          // Resume only the suffix, respecting the callback's congestion flag.
          ++counters_.partialWrites;
        }
      } else if (rejected || event.length == 0 ||
                 (event.status != ESP_SPP_SUCCESS && event.length == -1 && event.flag)) {
        // IDF 4.4 can report congestion before PORT_WriteDataCO (len=-1),
        // or PORT_SUCCESS with zero accepted bytes. Both are known-zero cases.
        knownFailure();
      } else {
        writeInFlight_ = false;
        ++counters_.writeFailures;
        counters_.packetsDropped += writeBatchPackets_;
        if (currentPacket_.isControl()) ++counters_.controlFailures;
        controlFault_ = true;
        trace(TRACE_CONTROL, event.handle, event.status, event.length, writeBatchLength_, true);
        clearWriteBatch();
      }
      break;
    }
    default: break;
  }
}

void SppTransport::step() {
  channel_.discardOld(txEpoch_, counters_);
  const bool requested = channel_.pairingRequested();
  if (requested != pairingMode_) { pairingMode_ = requested; scanDirty_ = true; }
  const uint32_t now = millis();
  if (serviceReady_ && scanDirty_ && now - lastScanAttemptMs_ >= 20) {
    lastScanAttemptMs_ = now;
    const bool connectable = !connected_, discoverable = connectable && pairingMode_;
    const esp_err_t result = esp_bt_gap_set_scan_mode(
        connectable ? ESP_BT_CONNECTABLE : ESP_BT_NON_CONNECTABLE,
        discoverable ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    scanDirty_ = result != ESP_OK;
    trace(TRACE_SCAN, 0, result, 0, (connectable << 1) | discoverable, result != ESP_OK);
  }
  if (eventFault_ || controlFault_) { counters_.senderState = SppSenderState::FAULT; return; }
  if (!connected_) { counters_.senderState = SppSenderState::DISCONNECTED; return; }
  if (writeInFlight_) {
    counters_.senderState = SppSenderState::WRITING;
    if (!timeoutReported_ && now - writeStartedMs_ >= SPPConfig::WRITE_TIMEOUT_MS) {
      timeoutReported_ = true; ++counters_.writeTimeouts;
      trace(TRACE_TIMEOUT, connectionHandle_, 0, writeBatchLength_ - writeBatchOffset_, 0, true);
    }
    return; // Wait for the real completion; timeout never starts a second write.
  }
  if (congested_) { counters_.senderState = SppSenderState::WAITING_CONGESTION; return; }
  if (retryPending_ && now - retryStartedMs_ < SPPConfig::WRITE_RETRY_DELAY_MS) {
    counters_.senderState = SppSenderState::CONTROL_RETRY; return;
  }
  retryPending_ = false;
  if (!hasPacket_) hasPacket_ = channel_.take(currentPacket_);
  counters_.senderState = SppSenderState::WAITING_PACKET;
  if (!hasPacket_) return;
  if (currentPacket_.epoch != txEpoch_) {
    const size_t dropped = writeBatchPackets_ ? writeBatchPackets_ : 1;
    counters_.packetsDropped += dropped;
    counters_.epochDrops += dropped;
    clearWriteBatch();
    return;
  }
  if (currentPacket_.packet.bytes[2] == 2) activeStream_ = currentPacket_.stream;
  else if (currentPacket_.stream != activeStream_) {
    const size_t dropped = writeBatchPackets_ ? writeBatchPackets_ : 1;
    counters_.packetsDropped += dropped;
    counters_.epochDrops += dropped;
    clearWriteBatch();
    return;
  }
  if (writeBatchLength_ == 0) prepareWriteBatch();
  writeInFlight_ = true;
  writeStartedMs_ = now;
  timeoutReported_ = false;
  counters_.senderState = SppSenderState::WRITING;
  ResetProgress::spp(ResetProgress::Stage::SPP_WRITE);
  const size_t remaining = writeBatchLength_ - writeBatchOffset_;
  const esp_err_t result = esp_spp_write(connectionHandle_, remaining,
                                        writeBatch_ + writeBatchOffset_);
  ResetProgress::spp(ResetProgress::Stage::SPP_WRITE_RETURNED);
  trace(TRACE_SUBMIT, connectionHandle_, result, remaining, 0, result != ESP_OK);
  if (result == ESP_OK) {
    ++counters_.writeSubmissions;
    // Record each DATA packet once, even if a batch is retried or its suffix
    // is submitted again after a successful partial write.
    if (!currentPacket_.isControl() && !writeBatchLatencyRecorded_) {
      writeBatchLatencyRecorded_ = true;
      const int64_t submittedUs = esp_timer_get_time();
      for (size_t packet = 0; packet < writeBatchPackets_; ++packet) {
        if (writeBatchCaptureUs_[packet] <= 0) continue;
        const uint32_t age = static_cast<uint32_t>(submittedUs - writeBatchCaptureUs_[packet]);
        const size_t bucket = age / 4000 < 63 ? age / 4000 : 63;
        ++latencyHistogram_[bucket];
        ++counters_.latencySamples;
        if (age > counters_.latencyMaxUs) counters_.latencyMaxUs = age;
        uint32_t accumulated = 0;
        const uint32_t threshold = (counters_.latencySamples * 95ULL + 99) / 100;
        for (size_t i = 0; i < 64; ++i) {
          accumulated += latencyHistogram_[i];
          if (accumulated >= threshold) {
            counters_.latencyP95Us = i == 63 ? counters_.latencyMaxUs : (i+1) * 4000;
            break;
          }
        }
      }
    }
  }
  else knownFailure();
}

void SppTransport::publish() {
  counters_.eventOverflows = eventOverflows_.load(std::memory_order_relaxed);
  counters_.txEpoch = txEpoch_;
  counters_.pairingMode = pairingMode_;
  counters_.congested = congested_;
  counters_.packetFrames = connected_ ? PacketConfig::MAX_PACKET_FRAMES : 0;
  counters_.queueDepth = channel_.depth();
  if (counters_.queueDepth > counters_.queueHighWater) counters_.queueHighWater = counters_.queueDepth;
  counters_.waitMs = writeInFlight_ ? millis() - writeStartedMs_ : 0;
  counters_.senderStackMinFree = uxTaskGetStackHighWaterMark(nullptr);
  counters_.diagnosticFaults = diagnostics_.faults();
  TransportChannel<SppTransportStats>::Snapshot snapshot;
  snapshot.stats = counters_;
  snapshot.epoch = txEpoch_;
  snapshot.completedControl = completedControl_;
  snapshot.frames = counters_.packetFrames;
  snapshot.ready = serviceReady_;
  snapshot.failed = initFailed_;
  snapshot.connected = snapshot.subscribed = connected_;
  snapshot.fault = eventFault_ || controlFault_;
  channel_.publish(snapshot);
}
void SppTransport::senderTask(void *context) { static_cast<SppTransport *>(context)->runSenderTask(); }
void SppTransport::runSenderTask() {
  TaskCooperation::watchCurrentTask();
  TaskCooperation::Progress progress;
  while (true) {
    ResetProgress::spp(ResetProgress::Stage::SPP_EVENTS);
    const uint32_t overflow = eventOverflows_.load(std::memory_order_relaxed);
    if (overflow != seenOverflows_) {
      seenOverflows_ = overflow; eventFault_ = true; recoveryCloseSeen_ = !connected_;
      trace(TRACE_EVENT_OVERFLOW, connectionHandle_, 0, 0, overflow, true);
    }
    Event event;
    for (unsigned i = 0; i < 32 && xQueueReceive(eventQueue_, &event, 0) == pdTRUE; ++i) {
      processEvent(event);
      progress.checkpoint();
    }
    if (eventOverflows_.load(std::memory_order_relaxed) != seenOverflows_) eventFault_ = true;
    ResetProgress::spp(ResetProgress::Stage::SPP_STEP);
    step();
    ResetProgress::spp(ResetProgress::Stage::SPP_PUBLISH);
    publish();
    ResetProgress::spp(ResetProgress::Stage::SPP_TRACE);
    diagnostics_.flush();
    ResetProgress::spp(ResetProgress::Stage::SPP_YIELD);
    progress.checkpoint();
    ResetProgress::spp(ResetProgress::Stage::SPP_WAIT);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
}
#endif
