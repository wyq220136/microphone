#ifndef SINGLEMIC_AUDIO_PROCESSOR_H_
#define SINGLEMIC_AUDIO_PROCESSOR_H_

#include <stddef.h>
#include <stdint.h>

#include "cfg.h"

// 音频 DSP 层：把 I2S 的 32 位原始采样转换为 BLE 协议使用的 16 位 PCM。
class AudioProcessor {
 public:
  // 清除 DC 阻断器和抗混叠滤波器的历史值，通常在新录音段开始时调用。
  void reset();
  // 对 inputSamples 个输入点进行处理和降采样，最多写 outputCapacity 个输出点。
  // 当前配置按 2:1 降采样；调用者应保证输入输出缓冲区有效。
  void convertToPcm16(const int32_t *input, size_t inputSamples,
                      int16_t *output, size_t outputCapacity);

 private:
  // 单点处理：缩放、去直流并饱和裁剪到有符号 16 位范围。
  int16_t processRawSample(int32_t raw);
  // 五抽头低通 FIR，用于降采样前削弱奈奎斯特频率以上的分量。
  int16_t antiAliasFilter(int16_t input);

  // 一阶 DC 阻断器的上一输入/输出状态，需跨数据包保留。
  int32_t previousInput_ = 0;
  int32_t previousOutput_ = 0;
  // FIR 环形历史缓存及下一写入位置。
  int32_t filterHistory_[AudioConfig::ANTI_ALIAS_TAPS] = {};
  size_t filterHistoryPosition_ = 0;
};

#endif
