#ifndef SINGLEMIC_APP_CONTROLLER_H_
#define SINGLEMIC_APP_CONTROLLER_H_

#include <stdint.h>

#include "I2SSampler.h"
#include "active_transport.h"
#include "audio_processor.h"
#include "button.h"
#if SINGLEMIC_FIXED_PCM_TEST
#include "fixed_pcm_source.h"
#endif

// 应用协调器：将按键、I2S 采集、音频处理和 BLE 传输串成完整工作流。
// Arduino 的 setup()/loop() 只调用本类，不直接操作底层模块。
class MicrophoneApplication {
 public:
  // 按配置的用户按键引脚构造去抖按键对象。
  MicrophoneApplication();

  // 初始化按键、I2S 和 BLE；关键硬件初始化失败时停止运行。
  void begin();
  // 执行一次主循环：状态同步、按键处理、采样、转换和入队。
  void loop();

 private:
  // 每秒输出一次 BLE 队列和发送状态，便于定位背压或链路问题。
  void printStats(uint32_t now);
  // 处理按键按下事件，并在录音开始/停止间切换。
  void handleRecordButton();
  // Key1 连续长按 3 秒开启 2 分钟配对窗口；松开不提前关闭。
  void handlePairingButton(uint32_t now);
  // 清理处理器状态并发送 START 控制包。
  void startRecording();
  // 排入 END 控制包，通知接收端结束当前录音段。
  void stopRecording();

  // 各子系统实例由应用统一拥有，按职责彼此隔离。
  I2SSampler sampler_;
  AudioProcessor processor_;
#if SINGLEMIC_FIXED_PCM_TEST
  FixedPcmSource fixedSource_;
#endif
  ActiveTransport transport_;
  DebouncedButton pairingButton_;
  DebouncedButton recordButton_;
  // 当前录音状态和统计日志节拍。
  bool recording_ = false;
  bool pairingButtonHeld_ = false;
  bool pairingHoldTriggered_ = false;
  bool pairingWindowActive_ = false;
  uint32_t pairingPressedAt_ = 0;
  uint32_t pairingWindowStartedAt_ = 0;
  uint32_t lastStatsMs_ = 0;
  // 按最大 BLE 包尺寸预留输入/输出缓存，避免循环中动态分配内存。
  int32_t rawSamples_[PacketConfig::MAX_PACKET_FRAMES *
                     AudioConfig::DECIMATION_FACTOR] = {};
  int32_t pcmSamples_[PacketConfig::MAX_PACKET_FRAMES] = {};
};

#endif
