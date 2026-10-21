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

// 两倍降采样：每个输入点都更新滤波器，输出每一对样本中的第二个滤波值。
void AudioProcessor::convertToPcm16(const int32_t *input, size_t inputSamples,
                                    int16_t *output, size_t outputCapacity) {
  if (input == nullptr || output == nullptr) return;

  const size_t frameCount = inputSamples / AudioConfig::DECIMATION_FACTOR;
  const size_t count = frameCount < outputCapacity ? frameCount : outputCapacity;
  for (size_t i = 0; i < count; ++i) {
    const size_t inputIndex = i * AudioConfig::DECIMATION_FACTOR;
    (void)antiAliasFilter(processRawSample(input[inputIndex]));
    const int32_t filtered =
        antiAliasFilter(processRawSample(input[inputIndex + 1]));
    const int32_t pcm = filtered >> AudioConfig::PCM_SHIFT;
    if (pcm > 32767) {
      output[i] = 32767;
    } else if (pcm < -32768) {
      output[i] = -32768;
    } else {
      output[i] = static_cast<int16_t>(pcm);
    }
  }
}
