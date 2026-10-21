#ifndef ICS_TEST_I2S_SAMPLER_H_
#define ICS_TEST_I2S_SAMPLER_H_

#include <stddef.h>
#include <stdint.h>

#include "cfg.h"

// 双麦 I2S 采集适配层。每个 I2S 端口由应用按包读取，避免旧版全局 reader
// 任务、双缓冲和 writer 通知状态与 BLE 发送逻辑互相耦合。
class I2SSampler {
 public:
  I2SSampler() = default;
  ~I2SSampler();

  // 安装两个 I2S RX 驱动并配置左右麦克风的引脚。
  bool begin();
  // 卸载已经成功安装的端口。
  void end();
  // 读取一个麦克风的指定数量 32 位原始采样。
  bool readSamples(const IcsTestConfig::MicrophoneConfig &microphone,
                  int32_t *destination, size_t sampleCount);

 private:
  bool setupPort(const IcsTestConfig::MicrophoneConfig &microphone);
  bool ready_[2] = {false, false};
};

#endif
