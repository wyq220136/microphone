#ifndef ICS_TEST_APP_CONTROLLER_H_
#define ICS_TEST_APP_CONTROLLER_H_

#include "I2SSampler.h"
#include "audio_pipeline.h"
#include "ble_transport.h"
#include "button.h"

// 固件应用层只协调按键、采集、DSP 与 BLE，不直接实现各层内部细节。
class IcsTestApp {
 public:
  IcsTestApp();
  bool begin();
  void loop();

 private:
  void startRecording();
  void stopRecording();
  bool enqueueControl(IcsTestProtocol::PacketType type);
  void flushDspTail();
  void printStats(uint32_t now);

  I2SSampler sampler_;
  AudioPipeline pipeline_;
  BleTransport ble_;
  DebouncedButton button_;
  bool recording_;
  bool wasConnected_;
  uint32_t sequence_;
  uint32_t lastStatsMs_;
  uint32_t dspFailures_;
  uint32_t dspOverruns_;
  int32_t rawLeft_[IcsTestConfig::MAX_PACKET_FRAMES *
                   IcsTestConfig::DECIMATION_FACTOR];
  int32_t rawRight_[IcsTestConfig::MAX_PACKET_FRAMES *
                    IcsTestConfig::DECIMATION_FACTOR];
  float mono_[IcsTestConfig::MAX_PACKET_FRAMES];
};

#endif
