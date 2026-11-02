#include "app_controller.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

#include "cfg.h"
#include "reset_progress.h"

namespace {
// Low-frequency task-context checks only. Never scan heaps in a Bluetooth
// callback/ISR or once per audio packet. Internal heaps cover the task stacks
// and internal Bluetooth allocations; this deliberately does not scan PSRAM.
bool checkInternalHeap(const char *stage) {
  Serial.printf("Memory check begin: stage=%s\n", stage);
  const bool ok = heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true);
  Serial.printf("Memory check end: stage=%s internal_heap=%s\n",
                stage, ok ? "OK" : "CORRUPT");
  return ok;
}

// 将内部状态枚举转成短字符串，便于串口统计日志阅读。
#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_CLASSIC_SPP
const char *senderStateName(SppSenderState state) {
  switch (state) {
    case SppSenderState::DISCONNECTED: return "disconnected";
    case SppSenderState::WAITING_PACKET: return "waiting_packet";
    case SppSenderState::WAITING_CONGESTION: return "waiting_congestion";
    case SppSenderState::CONTROL_RETRY: return "control_retry";
    case SppSenderState::FAULT: return "fault";
    case SppSenderState::WRITING: return "writing";
    case SppSenderState::DROPPED: return "dropped";
  }
  return "unknown";
}
#else
const char *senderStateName(BleSenderState state) {
  switch (state) {
    case BleSenderState::DISCONNECTED: return "disconnected";
    case BleSenderState::WAITING_PACKET: return "waiting_packet";
    case BleSenderState::WAITING_RESULT: return "waiting_result";
    case BleSenderState::RETRY_WAIT: return "retry_wait";
    case BleSenderState::DROPPED: return "dropped";
    case BleSenderState::FAULT: return "fault";
  }
  return "unknown";
}
#endif
}

MicrophoneApplication::MicrophoneApplication()
    : pairingButton_(IOConfig::USER_KEY_1),
      recordButton_(IOConfig::USER_KEY_2) {}

// 初始化流程：串口信息 -> 按键 -> I2S -> BLE。失败时停在安全状态并报错。
void MicrophoneApplication::begin() {
  Serial.printf("ESP32 reset reason=%d free_heap=%u min_heap=%u\n",
                static_cast<int>(esp_reset_reason()),
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMinFreeHeap()));
  char elfSha256[65] = {};
  esp_ota_get_app_elf_sha256(elfSha256, sizeof(elfSha256));
  Serial.printf("Firmware ELF SHA256=%s SDK=%s\n", elfSha256,
                esp_get_idf_version());
#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_CLASSIC_SPP
  Serial.println("ICS-43434 Classic Bluetooth SPP microphone streamer");
#else
  Serial.println("ICS-43434 BLE microphone streamer");
#endif
#if SINGLEMIC_ENABLE_HUFFMAN
  Serial.println("Audio encoding: Huffman with raw PCM24 fallback");
#else
  Serial.println("Audio encoding: RAW_PCM24 (Huffman disabled)");
#endif
  pairingButton_.begin();
  recordButton_.begin();
  Serial.printf("Key1 GPIO%d: hold %lu seconds for a %lu-second pairing window; "
                "Key2 GPIO%d: start/stop recording\n",
                IOConfig::USER_KEY_1,
                static_cast<unsigned long>(IOConfig::PAIRING_HOLD_MS / 1000),
                static_cast<unsigned long>(IOConfig::PAIRING_WINDOW_MS / 1000),
                IOConfig::USER_KEY_2);
#if SINGLEMIC_FIXED_PCM_TEST
  Serial.printf("Audio source=FIXED_PCM_TEST: I2S/DMA and microphone DSP disabled; "
                "%lu Hz mono PCM24, 32-sample repeating pattern; Key2 starts/stops\n",
                static_cast<unsigned long>(AudioConfig::SAMPLE_RATE));
  const bool inputReady = checkInternalHeap("fixed_input_no_i2s");
#else
  Serial.printf("PCM: I2S %lu Hz -> transport/WAV %lu Hz mono 24-bit, raw shift=%d\n",
                static_cast<unsigned long>(I2SConfig::CAPTURE_SAMPLE_RATE),
                static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
                AudioConfig::RAW_SHIFT);

  const bool inputReady = checkInternalHeap("before_i2s") && sampler_.begin() &&
                          checkInternalHeap("after_i2s");
#endif
  if (!inputReady || !transport_.begin() ||
      !checkInternalHeap("after_transport_begin")) {
    Serial.println("Fatal initialization error; halting");
    while (true) delay(1000);
  }
}

// 只有连接并订阅通知后才开始录音，避免数据发出前接收端尚未就绪。
void MicrophoneApplication::startRecording() {
  if (recording_) return;
  if (!transport_.connected()) {
    Serial.println("Recording start refused: transport is not connected");
    return;
  }
  if (!transport_.subscribed()) {
    Serial.println("Transport client connected but audio stream is not ready");
    return;
  }
  ResetProgress::main(ResetProgress::Stage::START_HEAP);
  if (!checkInternalHeap("before_recording")) {
    Serial.println("Recording start refused: internal heap corruption detected");
    return;
  }
#if !SINGLEMIC_FIXED_PCM_TEST
  processor_.reset();
#endif
  ResetProgress::main(ResetProgress::Stage::START_QUEUE);
  if (transport_.startStream()) {
    recording_ = true;
#if SINGLEMIC_FIXED_PCM_TEST
    fixedSource_.reset(micros());
    Serial.println("Recording started: FIXED_PCM_TEST (no microphone input)");
#else
    Serial.println("Recording started");
#endif
  } else {
    Serial.println("Recording start packet dropped");
  }
}

// END 与此前已排队的数据使用同一 FIFO，保证接收端最后收到结束标记。
void MicrophoneApplication::stopRecording() {
  if (!recording_) return;
  if (transport_.stopStream()) {
    recording_ = false;
    Serial.println("Recording stopped");
  } else {
    Serial.println("Recording stop packet dropped");
  }
}

// 按键采用按下沿触发，短按一次切换录音状态。
void MicrophoneApplication::handleRecordButton() {
  if (!recordButton_.pressed()) return;
  if (recording_) {
    stopRecording();
  } else {
    startRecording();
  }
}

void MicrophoneApplication::handlePairingButton(uint32_t now) {
  const bool down = pairingButton_.down();
  if (down && !pairingButtonHeld_) pairingPressedAt_ = now;
  if (!down) pairingHoldTriggered_ = false;
  pairingButtonHeld_ = down;

  bool active = pairingWindowActive_;
  if (down && !pairingHoldTriggered_ &&
      now - pairingPressedAt_ >= IOConfig::PAIRING_HOLD_MS) {
    // One trigger per hold: release is required before another 3-second hold.
    pairingHoldTriggered_ = true;
    pairingWindowStartedAt_ = now;
    active = true;
  }
  if (active && now - pairingWindowStartedAt_ >= IOConfig::PAIRING_WINDOW_MS) {
    active = false;
  }
  if (active == pairingWindowActive_) return;

  if (!transport_.setPairingMode(active)) {
    Serial.printf("Bluetooth pairing visibility could not be %s\n",
                  active ? "enabled" : "disabled");
    return;
  }
  pairingWindowActive_ = active;
  if (active) {
    Serial.printf("Bluetooth pairing window opened for %lu seconds; Key1 may be released\n",
                  static_cast<unsigned long>(IOConfig::PAIRING_WINDOW_MS / 1000));
  } else {
    Serial.println("Bluetooth pairing window expired; existing connections remain open");
  }
}

// 输出协商 MTU、实际连接间隔、发送状态和队列背压计数。
void MicrophoneApplication::printStats(uint32_t now) {
  if (now - lastStatsMs_ < 1000) return;
  lastStatsMs_ = now;
#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_CLASSIC_SPP
  const SppTransportStats stats = transport_.stats();
  Serial.printf(
      "SPP=%s pairing=%s recording=%s state=%s frames=%u queue=%u high=%u/%u "
      "sent=%lu bytes=%lu dropped=%lu queue_full=%lu "
      "write_submit=%lu write_evt=%lu write_fail=%lu "
      "write_timeout=%lu wait_ms=%lu last_write_ms=%lu last_cong_ms=%lu "
      "write_status=%d event_full=%lu stale=%lu control_fail=%lu "
      "congestion=%lu congested=%s disconnects=%lu epoch=%lu epoch_drop=%lu "
      "close_ms=%lu close_status=%d port_status=%lu close_async=%u "
      "heap=%u min_heap=%u stack_loop=%u stack_spp=%u diag_fault=%lu\n",
      transport_.connected() ? "connected" : "waiting",
      stats.pairingMode ? "yes" : "no",
      recording_ ? "yes" : "no",
      senderStateName(stats.senderState),
      static_cast<unsigned>(stats.packetFrames),
      static_cast<unsigned>(stats.queueDepth),
      static_cast<unsigned>(stats.queueHighWater),
      static_cast<unsigned>(SPPConfig::PACKET_QUEUE_DEPTH),
      static_cast<unsigned long>(stats.packetsSent),
      static_cast<unsigned long>(stats.bytesQueued),
      static_cast<unsigned long>(stats.packetsDropped),
      static_cast<unsigned long>(stats.queueOverflows),
      static_cast<unsigned long>(stats.writeSubmissions),
      static_cast<unsigned long>(stats.writeEvents),
      static_cast<unsigned long>(stats.writeFailures),
      static_cast<unsigned long>(stats.writeTimeouts),
      static_cast<unsigned long>(stats.waitMs),
      static_cast<unsigned long>(stats.lastWriteEventMs),
      static_cast<unsigned long>(stats.lastCongestionEventMs),
      stats.lastWriteStatus,
      static_cast<unsigned long>(stats.eventOverflows),
      static_cast<unsigned long>(stats.staleEvents),
      static_cast<unsigned long>(stats.controlFailures),
      static_cast<unsigned long>(stats.congestionEvents),
      stats.congested ? "yes" : "no",
      static_cast<unsigned long>(stats.disconnects),
      static_cast<unsigned long>(stats.txEpoch),
      static_cast<unsigned long>(stats.epochDrops),
      static_cast<unsigned long>(stats.lastCloseMs),
      stats.lastCloseStatus,
      static_cast<unsigned long>(stats.lastClosePortStatus),
      static_cast<unsigned>(stats.lastCloseAsync),
      static_cast<unsigned>(ESP.getFreeHeap()),
      static_cast<unsigned>(ESP.getMinFreeHeap()),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
      static_cast<unsigned>(stats.senderStackMinFree),
      static_cast<unsigned long>(stats.diagnosticFaults));
#else
  const BleTransportStats stats = transport_.stats();
  Serial.printf(
      "BLE=%s recording=%s state=%s mtu=%u frames=%u interval=%.2fms "
      "queue=%u high=%u/%u sent=%lu dropped=%lu queue_full=%lu retries=%lu "
      "notify_ok=%lu notify_fail=%lu timeout=%lu epoch=%lu stale=%lu "
      "epoch_drop=%lu sub_abort=%lu inflight_abort=%lu event_full=%lu\n",
      transport_.connected() ? "connected" : "waiting",
      recording_ ? "yes" : "no",
      senderStateName(stats.senderState),
      stats.mtu,
      static_cast<unsigned>(stats.packetFrames),
      stats.connectionInterval * 1.25f,
      static_cast<unsigned>(stats.queueDepth),
      static_cast<unsigned>(stats.queueHighWater),
      static_cast<unsigned>(BLEConfig::PACKET_QUEUE_DEPTH),
      static_cast<unsigned long>(stats.packetsSent),
      static_cast<unsigned long>(stats.packetsDropped),
      static_cast<unsigned long>(stats.queueOverflows),
      static_cast<unsigned long>(stats.notifyRetries),
      static_cast<unsigned long>(stats.notifyOk),
      static_cast<unsigned long>(stats.notifyFailures),
      static_cast<unsigned long>(stats.notifyTimeouts),
      static_cast<unsigned long>(stats.txEpoch),
      static_cast<unsigned long>(stats.staleNotifyIgnored),
      static_cast<unsigned long>(stats.epochDrops),
      static_cast<unsigned long>(stats.subscriptionAborts),
      static_cast<unsigned long>(stats.inFlightAborts),
      static_cast<unsigned long>(stats.eventOverflows));
#endif
}

// 主处理流程按“状态/按键 -> I2S -> DSP -> BLE 队列”顺序执行。
void MicrophoneApplication::loop() {
  const uint32_t now = millis();
  ResetProgress::main(ResetProgress::Stage::BUTTONS);
  pairingButton_.update(now);
  recordButton_.update(now);
  handlePairingButton(now);
  ResetProgress::main(ResetProgress::Stage::SNAPSHOT);
  recording_ = transport_.streamActive();
  ResetProgress::main(ResetProgress::Stage::STATS);
  printStats(now);
#if SINGLEMIC_TRANSPORT_MODE == SINGLEMIC_TRANSPORT_CLASSIC_SPP
  transport_.printDiagnostics();
#endif

  // Report rejected presses even when disconnected; startRecording checks the link.
  handleRecordButton();

  ResetProgress::main(ResetProgress::Stage::SNAPSHOT);
  const size_t frames = transport_.packetFrames();
  if (frames == 0) {
    delay(1);
    return;
  }
  // Validate before multiplying the input count and before passing the output
  // buffer to DSP/transport. A corrupt transport snapshot must not set capacity.
  if (frames > PacketConfig::MAX_PACKET_FRAMES) {
    transport_.recordCaptureFailure();
    Serial.printf("Invalid packet frame count: %u\n", static_cast<unsigned>(frames));
    delay(5);
    return;
  }
#if SINGLEMIC_FIXED_PCM_TEST
  if (!recording_) {
    delay(1);
    return;
  }
  ResetProgress::main(ResetProgress::Stage::FIXED_PCM);
  if (!fixedSource_.read(pcmSamples_, frames, micros(), AudioConfig::SAMPLE_RATE)) {
    delay(1);
    return;
  }
  // Same producer, Huffman/raw selection, CRC, FIFO and Bluetooth flow control.
  ResetProgress::main(ResetProgress::Stage::ENQUEUE);
  transport_.enqueueAudio(pcmSamples_, frames);
#else
  // 每个输出帧消耗 DECIMATION_FACTOR 个原始输入采样。
  ResetProgress::main(ResetProgress::Stage::I2S_READ);
  if (!sampler_.readSamples(rawSamples_, frames * AudioConfig::DECIMATION_FACTOR)) {
    transport_.recordCaptureFailure();
    delay(5);
    return;
  }

  ResetProgress::main(ResetProgress::Stage::SNAPSHOT);
  if (recording_ && transport_.connected()) {
    // I2S 读取持续进行；只有录音且链路仍连接时才做转换和排队。
    ResetProgress::main(ResetProgress::Stage::DSP);
    processor_.convertToPcm24(rawSamples_,
                              frames * AudioConfig::DECIMATION_FACTOR,
                              pcmSamples_, frames);
    ResetProgress::main(ResetProgress::Stage::ENQUEUE);
    transport_.enqueueAudio(pcmSamples_, frames);
  }
#endif
}
