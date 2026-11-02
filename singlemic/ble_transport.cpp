#include "cfg.h"
#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_BLE_NOTIFY
#include "ble_transport.h"
#include "task_cooperation.h"

class BleServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit BleServerCallbacks(BleTransport &transport) : transport_(transport) {}
#if SINGLEMIC_NIMBLE_API_V2
  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override { transport_.handleConnect(server, info); }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override { transport_.handleMtuChange(mtu, info); }
  void onConnParamsUpdate(NimBLEConnInfo &info) override { transport_.handleConnectionParams(info); }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override { transport_.handleDisconnect(info, reason); }
#else
  void onConnect(NimBLEServer *server, ble_gap_conn_desc *desc) override { transport_.handleConnect(server, desc); }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc *desc) override { transport_.handleMtuChange(mtu, desc); }
  void onDisconnect(NimBLEServer *, ble_gap_conn_desc *desc) override { transport_.handleDisconnect(desc); }
#endif
 private: BleTransport &transport_;
};
class BleCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit BleCharacteristicCallbacks(BleTransport &transport) : transport_(transport) {}
#if SINGLEMIC_NIMBLE_API_V2
  void onStatus(NimBLECharacteristic *, NimBLEConnInfo &info, int code) override {
    transport_.handleNotifyStatus(code, info.getConnHandle());
  }
  void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &info, uint16_t value) override {
    transport_.handleSubscribe(info.getConnHandle(), value);
  }
#else
  void onStatus(NimBLECharacteristic *, NimBLECharacteristicCallbacks::Status, int code) override {
    transport_.handleNotifyStatus(code, BleTransport::INVALID_CONN_HANDLE);
  }
  void onSubscribe(NimBLECharacteristic *, ble_gap_conn_desc *desc, uint16_t value) override {
    transport_.handleSubscribe(desc ? desc->conn_handle : BleTransport::INVALID_CONN_HANDLE, value);
  }
#endif
 private: BleTransport &transport_;
};

bool BleTransport::begin() {
  if (!transportAtomicStorageValid(eventOverflows_)) return false;
  eventQueue_ = xQueueCreate(32, sizeof(Event));
  if (!eventQueue_ || !channel_.begin(BLEConfig::PACKET_QUEUE_DEPTH)) return false;
  NimBLEDevice::init(TransportConfig::DEVICE_NAME);
  NimBLEDevice::setMTU(BLEConfig::REQUESTED_MTU);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  server_ = NimBLEDevice::createServer();
  if (!server_) return false;
  server_->setCallbacks(new BleServerCallbacks(*this));
#if SINGLEMIC_NIMBLE_API_V2
  server_->advertiseOnDisconnect(false);
#endif
  NimBLEService *service = server_->createService(BLEConfig::SERVICE_UUID);
  if (!service) return false;
  audioCharacteristic_ = service->createCharacteristic(BLEConfig::AUDIO_UUID, NIMBLE_PROPERTY::NOTIFY);
  if (!audioCharacteristic_) return false;
  audioCharacteristic_->setCallbacks(new BleCharacteristicCallbacks(*this));
  service->start();
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLEConfig::SERVICE_UUID);
  advertising->setName(TransportConfig::DEVICE_NAME);
#if SINGLEMIC_NIMBLE_API_V2
  advertising->enableScanResponse(true);
#else
  advertising->setScanResponse(true);
#endif
  if (xTaskCreatePinnedToCore(senderTask, "BLE owner", 4096, this, 2,
                             &senderTaskHandle_, 0) != pdPASS) return false;
  channel_.setOwner(senderTaskHandle_);
  Serial.printf("BLE ready: hold Key1 %lu seconds to advertise %s for %lu seconds\n",
                static_cast<unsigned long>(IOConfig::PAIRING_HOLD_MS / 1000),
                TransportConfig::DEVICE_NAME,
                static_cast<unsigned long>(IOConfig::PAIRING_WINDOW_MS / 1000));
  return true;
}

void BleTransport::postEvent(const Event &event) {
  // NimBLE task callback, not a hardware ISR. Only record ordered state and
  // wake the owner; no encoding, I2S reads, logging or waiting in callbacks.
  if (xQueueSend(eventQueue_, &event, 0) != pdTRUE)
    eventOverflows_.fetch_add(1, std::memory_order_relaxed);
  channel_.wake();
}
#if SINGLEMIC_NIMBLE_API_V2
void BleTransport::handleConnect(NimBLEServer *, NimBLEConnInfo &info) {
  Event e; e.type = CONNECT; e.handle = info.getConnHandle(); e.mtu = info.getMTU(); e.interval = info.getConnInterval(); postEvent(e);
}
void BleTransport::handleDisconnect(NimBLEConnInfo &info, int reason) {
  Event e; e.type = DISCONNECT; e.handle = info.getConnHandle(); e.status = reason; postEvent(e);
}
void BleTransport::handleMtuChange(uint16_t mtu, NimBLEConnInfo &info) {
  Event e; e.type = MTU; e.handle = info.getConnHandle(); e.mtu = mtu; e.interval = info.getConnInterval(); postEvent(e);
}
void BleTransport::handleConnectionParams(NimBLEConnInfo &info) {
  Event e; e.type = PARAMS; e.handle = info.getConnHandle(); e.interval = info.getConnInterval(); postEvent(e);
}
#else
void BleTransport::handleConnect(NimBLEServer *server, ble_gap_conn_desc *desc) {
  if (!desc) return; Event e; e.type = CONNECT; e.handle = desc->conn_handle;
  e.mtu = server->getPeerMTU(e.handle); e.interval = desc->conn_itvl; postEvent(e);
}
void BleTransport::handleDisconnect(ble_gap_conn_desc *desc) {
  if (!desc) return; Event e; e.type = DISCONNECT; e.handle = desc->conn_handle; postEvent(e);
}
void BleTransport::handleMtuChange(uint16_t mtu, ble_gap_conn_desc *desc) {
  Event e; e.type = MTU; e.handle = desc ? desc->conn_handle : INVALID_CONN_HANDLE;
  e.mtu = mtu; e.interval = desc ? desc->conn_itvl : 0; postEvent(e);
}
#endif
void BleTransport::handleNotifyStatus(int code, uint16_t handle) {
  Event e; e.type = NOTIFY_STATUS; e.handle = handle; e.status = code; postEvent(e);
}
void BleTransport::handleSubscribe(uint16_t handle, uint16_t value) {
  Event e; e.type = SUBSCRIBE; e.handle = handle; e.value = value; postEvent(e);
}

void BleTransport::processEvent(const Event &e) {
  const bool handleMatches = e.handle == connectionHandle_ || e.handle == INVALID_CONN_HANDLE;
  switch (e.type) {
    case CONNECT:
      if (connected_ && e.handle != connectionHandle_) {
        ++counters_.staleNotifyIgnored;
        break;
      }
      connected_ = true; subscribed_ = sessionFault_ = notifyInFlight_ = hasPacket_ = retryPending_ = false;
      connectionHandle_ = e.handle; negotiatedMtu_ = e.mtu; negotiatedInterval_ = e.interval;
      completedControl_ = activeStream_ = notifyAttempts_ = 0; ++txEpoch_; advertiseDirty_ = true;
#if SINGLEMIC_NIMBLE_API_V2
      senderReadyMs_ = 0;
#else
      senderReadyMs_ = millis() + BLEConfig::NOTIFY_TIMEOUT_MS;
#endif
      server_->updateConnParams(e.handle, BLEConfig::CONNECTION_INTERVAL_MIN,
          BLEConfig::CONNECTION_INTERVAL_MAX, BLEConfig::CONNECTION_LATENCY,
          BLEConfig::CONNECTION_TIMEOUT);
      break;
    case DISCONNECT:
      if (!connected_ || !handleMatches) { ++counters_.staleNotifyIgnored; break; }
      if (notifyInFlight_) ++counters_.inFlightAborts;
      connected_ = subscribed_ = notifyInFlight_ = hasPacket_ = retryPending_ = false;
      connectionHandle_ = INVALID_CONN_HANDLE; completedControl_ = activeStream_ = 0;
      ++txEpoch_; advertiseDirty_ = true; sessionFault_ = false;
      break;
    case MTU:
      if (!connected_ || !handleMatches) { ++counters_.staleNotifyIgnored; break; }
      negotiatedMtu_ = e.mtu; if (e.interval) negotiatedInterval_ = e.interval; break;
    case PARAMS:
      if (!connected_ || !handleMatches) { ++counters_.staleNotifyIgnored; break; }
      negotiatedInterval_ = e.interval; break;
    case SUBSCRIBE:
      if (!connected_ || !handleMatches) { ++counters_.staleNotifyIgnored; break; }
      if (e.value) subscribed_ = true;
      else {
        if (notifyInFlight_) ++counters_.inFlightAborts;
        if (subscribed_) ++counters_.subscriptionAborts;
        subscribed_ = notifyInFlight_ = hasPacket_ = retryPending_ = false;
        completedControl_ = activeStream_ = 0; ++txEpoch_;
      }
      break;
    case NOTIFY_STATUS:
      if (!connected_ || !subscribed_ || !notifyInFlight_ || !handleMatches ||
          currentPacket_.epoch != txEpoch_) { ++counters_.staleNotifyIgnored; break; }
      notifyInFlight_ = false;
      if (e.status == 0) {
        ++counters_.notifyOk; ++counters_.packetsSent;
        if (currentPacket_.isControl()) completedControl_ = currentPacket_.control;
        hasPacket_ = retryPending_ = false; notifyAttempts_ = 0;
      } else {
        ++counters_.notifyFailures;
        if (notifyAttempts_ >= BLEConfig::NOTIFY_MAX_ATTEMPTS) {
          if (currentPacket_.isControl()) sessionFault_ = true;
          else { hasPacket_ = false; ++counters_.packetsDropped; }
          retryPending_ = false;
        } else {
          retryStartedMs_ = millis(); retryPending_ = true;
        }
      }
      break;
  }
}

void BleTransport::step() {
  channel_.discardOld(txEpoch_, counters_);
  const bool requested = channel_.pairingRequested();
  if (requested != pairingMode_) { pairingMode_ = requested; advertiseDirty_ = true; }
  if (advertiseDirty_) {
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    if (connected_ || !pairingMode_) advertising->stop();
    else advertising->start();
    advertiseDirty_ = false;
  }
  if (eventOverflows_.load(std::memory_order_relaxed) != seenOverflows_) {
    seenOverflows_ = eventOverflows_.load(std::memory_order_relaxed);
    sessionFault_ = true;
  }
  if (sessionFault_) { counters_.senderState = BleSenderState::FAULT; return; }
  if (!connected_ || !subscribed_) { counters_.senderState = BleSenderState::DISCONNECTED; return; }
  if (senderReadyMs_ && static_cast<int32_t>(millis() - senderReadyMs_) < 0) {
    counters_.senderState = BleSenderState::WAITING_PACKET; return;
  }
  if (notifyInFlight_) {
    counters_.senderState = BleSenderState::WAITING_RESULT;
    if (millis() - notifyStartedMs_ >= BLEConfig::NOTIFY_TIMEOUT_MS) {
      ++counters_.notifyTimeouts;
      // Delivery is ambiguous. Keep the link up and stop this session rather
      // than submitting a second notification and mis-associating late status.
      sessionFault_ = true;
    }
    return;
  }
  if (retryPending_ && millis() - retryStartedMs_ < BLEConfig::NOTIFY_RETRY_DELAY_MS) {
    counters_.senderState = BleSenderState::RETRY_WAIT; return;
  }
  if (retryPending_) { retryPending_ = false; ++counters_.notifyRetries; }
  if (!hasPacket_) {
    hasPacket_ = channel_.take(currentPacket_);
    if (hasPacket_) notifyAttempts_ = 0;
  }
  counters_.senderState = BleSenderState::WAITING_PACKET;
  if (!hasPacket_) return;
  if (currentPacket_.epoch != txEpoch_) {
    hasPacket_ = false; ++counters_.packetsDropped; ++counters_.epochDrops; return;
  }
  if (currentPacket_.packet.bytes[2] == 2) activeStream_ = currentPacket_.stream;
  else if (currentPacket_.stream != activeStream_) {
    hasPacket_ = false; ++counters_.packetsDropped; ++counters_.epochDrops; return;
  }
  notifyInFlight_ = true; notifyStartedMs_ = millis(); ++notifyAttempts_;
  counters_.senderState = BleSenderState::WAITING_RESULT;
#if SINGLEMIC_NIMBLE_API_V2
  if (!audioCharacteristic_->notify(currentPacket_.packet.bytes,
                                    currentPacket_.packet.length, connectionHandle_)) {
    notifyInFlight_ = false; ++counters_.notifyFailures;
    if (notifyAttempts_ >= BLEConfig::NOTIFY_MAX_ATTEMPTS) {
      if (currentPacket_.isControl()) sessionFault_ = true;
      else { hasPacket_ = false; ++counters_.packetsDropped; }
      retryPending_ = false;
    } else {
      retryStartedMs_ = millis(); retryPending_ = true;
    }
  }
#else
  audioCharacteristic_->setValue(currentPacket_.packet.bytes, currentPacket_.packet.length);
  audioCharacteristic_->notify();
#endif
}

void BleTransport::publish() {
  counters_.txEpoch = txEpoch_;
  counters_.mtu = negotiatedMtu_;
  counters_.connectionInterval = negotiatedInterval_;
  counters_.packetFrames = connected_ ? AudioProtocol::framesForMtu(negotiatedMtu_) : 0;
  counters_.queueDepth = channel_.depth();
  counters_.eventOverflows = eventOverflows_.load(std::memory_order_relaxed);
  if (counters_.queueDepth > counters_.queueHighWater) counters_.queueHighWater = counters_.queueDepth;
  TransportChannel<BleTransportStats>::Snapshot snapshot;
  snapshot.stats = counters_;
  snapshot.epoch = txEpoch_;
  snapshot.completedControl = completedControl_;
  snapshot.frames = counters_.packetFrames;
  snapshot.ready = true;
  snapshot.connected = connected_;
  snapshot.subscribed = subscribed_;
  snapshot.fault = sessionFault_;
  channel_.publish(snapshot);
}
void BleTransport::senderTask(void *context) { static_cast<BleTransport *>(context)->runSenderTask(); }
void BleTransport::runSenderTask() {
  TaskCooperation::watchCurrentTask();
  TaskCooperation::Progress progress;
  while (true) {
    Event event;
    for (unsigned i = 0; i < 32 && xQueueReceive(eventQueue_, &event, 0) == pdTRUE; ++i) {
      processEvent(event);
      progress.checkpoint();
    }
    step(); publish();
    progress.checkpoint();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));
  }
}
#endif
