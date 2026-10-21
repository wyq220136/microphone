#include "app_controller.h"

#include <Arduino.h>

#include "packet_protocol.h"

IcsTestApp::IcsTestApp()
    : button_(IcsTestConfig::USER_KEY), recording_(false), wasConnected_(false), sequence_(0),
      lastStatsMs_(0), dspFailures_(0), dspOverruns_(0), rawLeft_{},
      rawRight_{}, mono_{} {}

bool IcsTestApp::begin() {
  Serial.begin(115200);
  delay(300);
  Serial.println("ICS-43434 BLE microphone streamer");
  button_.begin(millis());
  Serial.printf("PCM: I2S %lu Hz -> BLE/WAV %lu Hz mono 16-bit, raw shift=%d\n",
      static_cast<unsigned long>(IcsTestConfig::CAPTURE_SAMPLE_RATE),
      static_cast<unsigned long>(IcsTestConfig::AUDIO_SAMPLE_RATE),
      IcsTestConfig::AUDIO_RAW_SHIFT);
  if (!sampler_.begin() || !ble_.begin()) {
    Serial.println("Fatal initialization error; halting");
    return false;
  }
  Serial.println("DSP ready: lightweight dual-microphone averaging");
  Serial.println("Warning: left/right I2S use independent clocks; verify sync on hardware");
  return true;
}

bool IcsTestApp::enqueueControl(IcsTestProtocol::PacketType type) {
  if (!ble_.connected()) return false;
  IcsTestProtocol::BlePacket packet = {};
  IcsTestProtocol::buildControlPacket(type, sequence_, packet);
  return ble_.enqueue(packet, 100);
}

void IcsTestApp::startRecording() {
  if (recording_ || !ble_.connected()) return;
  pipeline_.reset();
  sequence_ = 0;
  ble_.resetQueue();
  if (enqueueControl(IcsTestProtocol::START)) {
    recording_ = true;
    Serial.println("Recording started");
  } else {
    Serial.println("Recording start packet dropped");
  }
}

void IcsTestApp::flushDspTail() {
  int32_t zeros[IcsTestConfig::MAX_PACKET_FRAMES *
                IcsTestConfig::DECIMATION_FACTOR] = {};
  for (int block = 0; block < 2; ++block) {
    if (!pipeline_.process(zeros, zeros, mono_)) ++dspFailures_;
    IcsTestProtocol::BlePacket packet = {};
    IcsTestProtocol::buildAudioPacket(mono_, sequence_, packet);
    if (!ble_.enqueue(packet, 100)) ble_.recordDrop();
  }
}

void IcsTestApp::stopRecording() {
  if (!recording_) return;
  if (ble_.connected()) flushDspTail();
  if (enqueueControl(IcsTestProtocol::END)) {
    recording_ = false;
    Serial.println("Recording stopped");
  } else {
    Serial.println("Recording stop packet dropped");
  }
}

void IcsTestApp::printStats(uint32_t now) {
  if (now - lastStatsMs_ < 1000) return;
  lastStatsMs_ = now;
  const BleTransport::Stats stats = ble_.stats();
  Serial.printf("BLE=%s recording=%s sent=%lu dropped=%lu dsp_fail=%lu "
                "dsp_overrun=%lu notify_ok=%lu notify_fail=%lu timeout=%lu\n",
      ble_.connected() ? "connected" : "waiting",
      recording_ ? "yes" : "no",
      static_cast<unsigned long>(stats.sent),
      static_cast<unsigned long>(stats.dropped),
      static_cast<unsigned long>(dspFailures_),
      static_cast<unsigned long>(dspOverruns_),
      static_cast<unsigned long>(stats.notifyOk),
      static_cast<unsigned long>(stats.notifyFailures),
      static_cast<unsigned long>(stats.notifyTimeouts));
}

void IcsTestApp::loop() {
  const uint32_t now = millis();
  const bool connected = ble_.connected();
  if (connected != wasConnected_) {
    wasConnected_ = connected;
    pipeline_.reset();
    sequence_ = 0;
    recording_ = false;
  }
  if (button_.pressed(now)) {
    if (recording_) stopRecording();
    else startRecording();
  }

  const size_t inputFrames = IcsTestConfig::MAX_PACKET_FRAMES *
                             IcsTestConfig::DECIMATION_FACTOR;
  if (!sampler_.readSamples(IcsTestConfig::LEFT_MIC, rawLeft_, inputFrames) ||
      !sampler_.readSamples(IcsTestConfig::RIGHT_MIC, rawRight_, inputFrames)) {
    delay(5);
    printStats(now);
    return;
  }

  const uint32_t dspStartUs = micros();
  if (!pipeline_.process(rawLeft_, rawRight_, mono_)) ++dspFailures_;
  const uint32_t budgetUs = 1000000UL * IcsTestConfig::MAX_PACKET_FRAMES /
                            IcsTestConfig::AUDIO_SAMPLE_RATE;
  if (micros() - dspStartUs > budgetUs) ++dspOverruns_;

  if (recording_ && ble_.connected()) {
    IcsTestProtocol::BlePacket packet = {};
    IcsTestProtocol::buildAudioPacket(mono_, sequence_, packet);
    if (!ble_.enqueue(packet)) {
      ble_.recordDrop();
    }
  }
  printStats(now);
}
