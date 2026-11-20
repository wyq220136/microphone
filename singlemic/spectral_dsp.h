#pragma once
#include <cstddef>
#include <cstdint>

struct SpectralBackend {
  virtual ~SpectralBackend() = default;
  virtual bool transform(float *complex, bool inverse) = 0;
  virtual bool infer(const float *spectrum, float *enhanced) = 0;
  virtual void resetModel() = 0;
};

struct LevelSettings {
  bool normalize = true, match = true;
  float targetRms = 0.1f, minInputGain = 0.1f, maxInputGain = 10.0f;
  float minOutputGain = 1.0f, maxOutputGain = 15.848932f;
};

// No allocations, platform APIs, PCM16 conversions, or ownership of the backend.
class SpectralDsp {
 public:
  static constexpr size_t kHop = 256, kFft = 512, kSpectrum = 514;
  void begin(SpectralBackend &backend, LevelSettings settings);
  void reset();
  // A zero-count call flushes the previous partial/full hop. First call emits 0.
  bool process(const int32_t *input, size_t count, int32_t *output, size_t &produced);
  uint32_t peakProtections() const { return peakProtections_; }
 private:
  SpectralBackend *backend_ = nullptr;
  LevelSettings settings_;
  float window_[kFft] = {}, time_[kFft] = {};
  alignas(16) float fft_[kFft * 2] = {};
  float enhanced_[kSpectrum] = {}, overlap_[kHop] = {}, overlapWeight_[kHop] = {};
  float inputPower_ = 0, spectralIn_ = 0, spectralOut_ = 0, limiter_ = 1;
  size_t previousCount_ = 0;
  uint32_t peakProtections_ = 0;
  bool primed_ = false;
};
