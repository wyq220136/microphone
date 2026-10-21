#ifndef SINGLEMIC_I2S_SAMPLER_H_
#define SINGLEMIC_I2S_SAMPLER_H_

#include <stddef.h>
#include <stdint.h>

// I2S 采集适配层：负责驱动初始化、释放以及读取麦克风的 32 位原始采样。
// 音频转换和 BLE 发送由其他模块负责，避免采集驱动依赖应用协议。
class I2SSampler {
 public:
  // 默认构造不启动硬件；begin() 由应用初始化流程显式调用。
  I2SSampler() = default;
  // 对象销毁时释放已安装的 I2S 驱动。
  ~I2SSampler();

  // 安装 I2S 驱动并配置 cfg.h 中的端口和引脚；重复调用是安全的。
  bool begin();
  // 卸载驱动；尚未启动时不执行操作。
  void end();
  // 查询 I2S 驱动是否完成初始化。
  bool isReady() const { return ready_; }
  // 阻塞读取指定数量的 32 位输入采样，成功返回 true。
  bool readSamples(int32_t *destination, size_t sampleCount);

 private:
  // 防止重复安装/卸载驱动。
  bool ready_ = false;
};

#endif
