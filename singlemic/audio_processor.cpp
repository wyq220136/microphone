#include "audio_processor.h"

#include "cfg.h"

// 新录音段开始时重置滤波器，避免上一段尾部状态污染新段开头。
void AudioProcessor::reset() {
  previousInput_ = 0;
  previousOutput_ = 0;
  for (int32_t &sample : filterHistory_) {
    sample = 0;
  }
  filterHistoryPosition_ = 0;
}

// 将 I2S 左对齐原始值缩放为 24 位样本后进行一阶 DC 阻断。
int32_t AudioProcessor::processRawSample(int32_t raw) {
  const int32_t scaled = raw >> AudioConfig::RAW_SHIFT;
  const int64_t filtered = static_cast<int64_t>(scaled) - previousInput_ +
      (previousOutput_ - (previousOutput_ >> AudioConfig::DC_BLOCK_SHIFT));
  previousInput_ = scaled;

  if (filtered > 8388607) {
    previousOutput_ = 8388607;
  } else if (filtered < -8388608) {
    previousOutput_ = -8388608;
  } else {
    previousOutput_ = static_cast<int32_t>(filtered);
  }
  return previousOutput_;
}

// 对最近五个 24 位输入点做 [1, 4, 6, 4, 1] / 16 加权平均。
int32_t AudioProcessor::antiAliasFilter(int32_t input) {
  filterHistory_[filterHistoryPosition_] = input;
  filterHistoryPosition_ =
      (filterHistoryPosition_ + 1) % AudioConfig::ANTI_ALIAS_TAPS;

  const size_t oldest = filterHistoryPosition_;
  const int64_t sum = filterHistory_[oldest] +
      4LL * filterHistory_[(oldest + 1) % AudioConfig::ANTI_ALIAS_TAPS] +
      6LL * filterHistory_[(oldest + 2) % AudioConfig::ANTI_ALIAS_TAPS] +
      4LL * filterHistory_[(oldest + 3) % AudioConfig::ANTI_ALIAS_TAPS] +
      filterHistory_[(oldest + 4) % AudioConfig::ANTI_ALIAS_TAPS];
  const int64_t value = sum / 16;
  if (value > 8388607) return 8388607;
  if (value < -8388608) return -8388608;
  return static_cast<int32_t>(value);
}

// 倍率1：逐点转换和去直流，不经过降采样FIR。
// 倍率2：每个输入点都更新FIR，输出每组中最后一个滤波值，保持原算法。
void AudioProcessor::convertToPcm24(const int32_t *input, size_t inputSamples,
                                    int32_t *output, size_t outputCapacity) {
  if (input == nullptr || output == nullptr) return;

  const size_t frameCount = inputSamples / AudioConfig::DECIMATION_FACTOR;
  const size_t count = frameCount < outputCapacity ? frameCount : outputCapacity;
  for (size_t i = 0; i < count; ++i) {
    const size_t inputIndex = i * AudioConfig::DECIMATION_FACTOR;
    int32_t sample = 0;
    // 只访问当前完整输入组；倍率1的最后一帧不再多读一个样本。
    for (size_t j = 0; j < AudioConfig::DECIMATION_FACTOR; ++j) {
      sample = processRawSample(input[inputIndex + j]);
      if (AudioConfig::DECIMATION_FACTOR > 1) {
        sample = antiAliasFilter(sample);
      }
    }
    output[i] = sample;
  }
}
