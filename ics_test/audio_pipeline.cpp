#include "audio_pipeline.h"

namespace {

IcsDSP::AudioFilterConfig makeFilterConfig() {
  IcsDSP::AudioFilterConfig config;
  config.raw_shift = IcsTestConfig::AUDIO_RAW_SHIFT;
  config.dc_block_shift = IcsTestConfig::AUDIO_DC_BLOCK_SHIFT;
  return config;
}

}  // namespace

AudioPipeline::AudioPipeline()
    : leftFilter_(makeFilterConfig()), rightFilter_(makeFilterConfig()) {}

void AudioPipeline::reset() {
  leftFilter_.reset();
  rightFilter_.reset();
}

bool AudioPipeline::process(const int32_t *left, const int32_t *right,
                            float *mono) {
  if (left == nullptr || right == nullptr || mono == nullptr) return false;
  for (size_t i = 0; i < IcsTestConfig::MAX_PACKET_FRAMES; ++i) {
    const size_t inputIndex = i * IcsTestConfig::DECIMATION_FACTOR;
    (void)leftFilter_.process(left[inputIndex]);
    (void)rightFilter_.process(right[inputIndex]);
    const float filteredLeft = leftFilter_.process(left[inputIndex + 1]);
    const float filteredRight = rightFilter_.process(right[inputIndex + 1]);
    mono[i] = 0.5f * (filteredLeft + filteredRight);
  }
  return true;
}
