#ifndef SINGLEMIC_I2S_SAMPLER_H_
#define SINGLEMIC_I2S_SAMPLER_H_

#include <stddef.h>
#include <stdint.h>
#include <driver/i2s_std.h>

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
  // 保留数组容量，在调用驱动前拒绝越界请求；不接受丢失容量的裸指针。
  template <size_t Capacity>
  bool readSamples(int32_t (&destination)[Capacity], size_t sampleCount) {
    return readSamplesBounded(destination, sampleCount, Capacity);
  }

 private:
  bool readSamplesBounded(int32_t *destination, size_t sampleCount,
                          size_t capacity);
  // 防止重复安装/卸载驱动。
  bool ready_ = false;
  i2s_chan_handle_t rx_ = nullptr;
};

#endif
