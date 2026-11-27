#include "audio_processor.h"

#include <math.h>

#include "cfg.h"

namespace {
struct PeqCoefficients {
  float b0, b1, b2, a1, a2;
};

// 与离线 peq_sos 相同的 RBJ peaking EQ，按 Hz 半增益带宽设计。
// 系数仅在启动时计算一次；逐采样路径不调用三角函数或分配内存。
const PeqCoefficients kCavityPeq = []() {
  constexpr double pi = 3.14159265358979323846;
  const double w = 2.0 * pi * AudioConfig::CAVITY_PEQ_CENTER_HZ /
      I2SConfig::CAPTURE_SAMPLE_RATE;
  const double alpha = tan(pi * AudioConfig::CAVITY_PEQ_BANDWIDTH_HZ /
                           I2SConfig::CAPTURE_SAMPLE_RATE);
  const double a = pow(10.0, AudioConfig::CAVITY_PEQ_GAIN_DB / 40.0);
  const double a0 = 1.0 + alpha / a;
  return PeqCoefficients{
      static_cast<float>((1.0 + alpha * a) / a0),
      static_cast<float>(-2.0 * cos(w) / a0),
      static_cast<float>((1.0 - alpha * a) / a0),
      static_cast<float>(-2.0 * cos(w) / a0),
      static_cast<float>((1.0 - alpha / a) / a0)};
}();
}  // namespace

// 新录音段开始时重置滤波器，避免上一段尾部状态污染新段开头。
void AudioProcessor::reset() {
  previousInput_ = 0;
  previousOutput_ = 0;
  for (auto &state : eqState_) {
    state[0] = 0.0f;
    state[1] = 0.0f;
  }
  peqState_[0] = 0.0f;
  peqState_[1] = 0.0f;
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

  if (AudioConfig::ENABLE_CAVITY_EQ) {
    // PCM24 输入的 DC 中间值可用 int32_t 容纳；避免在预衰减/EQ 前削波。
    previousOutput_ = static_cast<int32_t>(filtered);
    return applyCavityEq(previousOutput_);
  }

  if (filtered > 8388607) {
    previousOutput_ = 8388607;
  } else if (filtered < -8388608) {
    previousOutput_ = -8388608;
  } else {
    previousOutput_ = static_cast<int32_t>(filtered);
  }
  return previousOutput_;
}

int32_t AudioProcessor::applyCavityEq(int32_t input) {
  // 16 kHz，行格式为 b0, b1, b2, a1, a2（a0=1）。
  // 对应试听 strong_both：70 Hz HP2、1.5 kHz +3 dB 高架、
  // 50 Hz HP4（两个二阶节）、3 kHz +6 dB 高架；高架斜率 S=1。
  // 这五节之后再串联 cfg.h 中配置的 PEQ，最后统一转换为 PCM24。
  static constexpr float sos[EQ_SECTION_COUNT][5] = {
      {0.980750062245740f, -1.961500124491480f, 0.980750062245740f,
       -1.961129530052878f, 0.961870718930083f},
      {1.317489670586491f, -1.652670186376029f, 0.612571781023080f,
       -1.129494247229021f, 0.406885512462562f},
      {0.974671501147655f, -1.949343002295310f, 0.974671501147655f,
       -1.963989350903899f, 0.964368001532921f},
      {1.0f, -2.0f, 1.0f, -1.984702441358303f, 0.985085085402425f},
      {1.529673716654436f, -0.991109008783589f, 0.377499291051119f,
       -0.268258973596950f, 0.184322972518916f},
  };
  float sample = static_cast<float>(input) * AudioConfig::CAVITY_EQ_PRE_GAIN;
  for (size_t i = 0; i < EQ_SECTION_COUNT; ++i) {
    const float *c = sos[i];
    float *state = eqState_[i];
    const float value = c[0] * sample + state[0];
    state[0] = c[1] * sample - c[3] * value + state[1];
    state[1] = c[2] * sample - c[4] * value;
    sample = value;
  }
  if (AudioConfig::ENABLE_CAVITY_PEQ && AudioConfig::CAVITY_PEQ_GAIN_DB != 0.0f) {
    const float value = kCavityPeq.b0 * sample + peqState_[0];
    peqState_[0] = kCavityPeq.b1 * sample - kCavityPeq.a1 * value + peqState_[1];
    peqState_[1] = kCavityPeq.b2 * sample - kCavityPeq.a2 * value;
    sample = value;
  }
  if (sample > 8388607.0f) return 8388607;
  if (sample < -8388608.0f) return -8388608;
  return static_cast<int32_t>(sample);
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

// 倍率1：逐点转换、去直流和可选 EQ，不经过降采样 FIR。
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
