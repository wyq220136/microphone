#include "app_controller.h"

#include <Arduino.h>

#include "cfg.h"

namespace {
// 将内部状态枚举转成短字符串，便于串口统计日志阅读。
const char *senderStateName(BleSenderState state) {
  switch (state) {
    case BleSenderState::DISCONNECTED: return "disconnected";
    case BleSenderState::WAITING_PACKET: return "waiting_packet";
    case BleSenderState::WAITING_RESULT: return "waiting_result";
    case BleSenderState::RETRY_WAIT: return "retry_wait";
    case BleSenderState::DROPPED: return "dropped";
  }
  return "unknown";
}
}

MicrophoneApplication::MicrophoneApplication()
    : button_(IOConfig::USER_KEY_2) {}

// 初始化流程：串口信息 -> 按键 -> I2S -> BLE。失败时停在安全状态并报错。
void MicrophoneApplication::begin() {
  Serial.println("ICS-43434 BLE microphone streamer");
  button_.begin();
  Serial.printf("PCM: I2S %lu Hz -> BLE/WAV %lu Hz mono 16-bit, raw shift=%d pcm shift=%d\n",
                static_cast<unsigned long>(I2SConfig::CAPTURE_SAMPLE_RATE),
                static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
                AudioConfig::RAW_SHIFT,
                AudioConfig::PCM_SHIFT);

  if (!sampler_.begin() || !ble_.begin()) {
    Serial.println("Fatal initialization error; halting");
    while (true) delay(1000);
  }
}

// 只有连接并订阅通知后才开始录音，避免数据发出前接收端尚未就绪。
void MicrophoneApplication::startRecording() {
  if (recording_ || !ble_.connected()) return;
  if (!ble_.subscribed()) {
    Serial.println("BLE client connected but audio notifications are not subscribed");
    return;
  }
  processor_.reset();
  if (ble_.startStream()) {
    recording_ = true;
    Serial.println("Recording started");
  } else {
    Serial.println("Recording start packet dropped");
  }
}

// END 与此前已排队的数据使用同一 FIFO，保证接收端最后收到结束标记。
void MicrophoneApplication::stopRecording() {
  if (!recording_) return;
  if (ble_.stopStream()) {
    recording_ = false;
    Serial.println("Recording stopped");
  } else {
    Serial.println("Recording stop packet dropped");
  }
}

// 按键采用按下沿触发，短按一次切换录音状态。
void MicrophoneApplication::handleButton() {
  if (!button_.pressed()) return;
  if (recording_) {
    stopRecording();
  } else {
    startRecording();
  }
}

// 输出协商 MTU、实际连接间隔、发送状态和队列背压计数。
void MicrophoneApplication::printStats(uint32_t now) {
  if (now - lastStatsMs_ < 1000) return;
  lastStatsMs_ = now;
  const BleTransportStats stats = ble_.stats();
  Serial.printf(
      "BLE=%s recording=%s state=%s mtu=%u frames=%u interval=%.2fms "
      "queue=%u high=%u/%u sent=%lu dropped=%lu queue_full=%lu retries=%lu "
      "notify_ok=%lu notify_fail=%lu timeout=%lu\n",
      ble_.connected() ? "connected" : "waiting",
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
      static_cast<unsigned long>(stats.notifyTimeouts));
}

// 主处理流程按“状态/按键 -> I2S -> DSP -> BLE 队列”顺序执行。
void MicrophoneApplication::loop() {
  const uint32_t now = millis();
  button_.update(now);
  recording_ = ble_.streamActive();
  printStats(now);

  const size_t frames = ble_.packetFrames();
  if (frames == 0) {
    delay(1);
    return;
  }
  handleButton();

  // 每个输出帧消耗 DECIMATION_FACTOR 个原始输入采样。
  if (!sampler_.readSamples(rawSamples_, frames * AudioConfig::DECIMATION_FACTOR)) {
    ble_.recordCaptureFailure();
    delay(5);
    return;
  }

  if (recording_ && ble_.connected()) {
    // I2S 读取持续进行；只有录音且链路仍连接时才做转换和排队。
    processor_.convertToPcm16(rawSamples_,
                              frames * AudioConfig::DECIMATION_FACTOR,
                              pcmSamples_, frames);
    ble_.enqueueAudio(pcmSamples_, frames);
  }
}
