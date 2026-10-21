#ifndef ICS_TEST_AUDIO_PIPELINE_H_
#define ICS_TEST_AUDIO_PIPELINE_H_

#include <stdint.h>

#include "algorithm.h"
#include "cfg.h"

// 双麦前端：各通道独立滤波，再以等权平均生成单声道输出。
class AudioPipeline {
 public:
  AudioPipeline();
  void reset();
  bool process(const int32_t *left, const int32_t *right, float *mono);

 private:
  IcsDSP::AudioFilter leftFilter_;
  IcsDSP::AudioFilter rightFilter_;
};

#endif
