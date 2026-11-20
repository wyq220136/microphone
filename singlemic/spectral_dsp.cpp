#include "spectral_dsp.h"
#include <algorithm>
#include <cmath>
#include <cstring>

void SpectralDsp::begin(SpectralBackend &backend, LevelSettings settings) {
  backend_ = &backend;
  settings_ = settings;
  for (size_t i = 0; i < kFft; ++i)
    window_[i] = std::sqrt(0.5f - 0.5f * std::cos(6.283185307179586f * i / kFft));
  reset();
}

void SpectralDsp::reset() {
  std::memset(time_, 0, sizeof(time_));
  std::memset(overlap_, 0, sizeof(overlap_));
  std::memset(overlapWeight_, 0, sizeof(overlapWeight_));
  inputPower_ = spectralIn_ = spectralOut_ = 0;
  limiter_ = 1;
  previousCount_ = 0;
  peakProtections_ = 0;
  primed_ = false;
  if (backend_) backend_->resetModel();
}

bool SpectralDsp::process(const int32_t *input, size_t count, int32_t *output, size_t &produced) {
  produced = 0;
  if (!backend_ || count > kHop || (count && !input) || !output) return false;
  std::memmove(time_, time_ + kHop, kHop * sizeof(float));
  float power = 0;
  for (size_t i = 0; i < kHop; ++i) {
    float x = i < count ? static_cast<float>(input[i]) / 8388608.0f : 0;
    time_[kHop + i] = x;
    power += x * x / kHop;
  }
  constexpr float alpha = 0.938005f; // exp(-256/(16000*0.250))
  inputPower_ = primed_ ? alpha * inputPower_ + (1-alpha) * power : power;
  const float inputGain = settings_.normalize && inputPower_ > 1e-14f ?
      std::clamp(settings_.targetRms / std::sqrt(inputPower_),
                 settings_.minInputGain, settings_.maxInputGain) : 1.0f;
  for (size_t i = 0; i < kFft; ++i) {
    fft_[2*i] = time_[i] * window_[i] * inputGain;
    fft_[2*i+1] = 0;
  }
  if (!backend_->transform(fft_, false) || !backend_->infer(fft_, enhanced_)) return false;
  float inPower = 0, outPower = 0;
  for (size_t i = 0; i < kSpectrum; ++i) {
    if (!std::isfinite(enhanced_[i])) return false;
    inPower += fft_[i] * fft_[i];
    outPower += enhanced_[i] * enhanced_[i];
  }
  spectralIn_ = primed_ ? alpha * spectralIn_ + (1-alpha) * inPower : inPower;
  spectralOut_ = primed_ ? alpha * spectralOut_ + (1-alpha) * outPower : outPower;
  float gain = 1.0f;
  // Stale EMA energy must not amplify model residual noise after speech stops.
  if (settings_.match && inPower > 1e-10f && outPower > 1e-10f &&
      spectralIn_ > 1e-10f && spectralOut_ > 1e-10f)
    gain = std::clamp(std::sqrt(spectralIn_ / spectralOut_),
                      settings_.minOutputGain, settings_.maxOutputGain);
  gain /= inputGain;
  for (size_t i = 0; i < kSpectrum; ++i) fft_[i] = enhanced_[i] * gain;
  fft_[1] = fft_[kFft+1] = 0;
  for (size_t bin = 1; bin < kFft/2; ++bin) {
    fft_[2*(kFft-bin)] = fft_[2*bin];
    fft_[2*(kFft-bin)+1] = -fft_[2*bin+1];
  }
  if (!backend_->transform(fft_, true)) return false;
  float peak = 0;
  for (size_t i = 0; i < kHop; ++i) {
    const float w = window_[i];
    const float denominator = overlapWeight_[i] + w*w;
    const float sample = denominator > 1e-12f ?
        (overlap_[i] + fft_[2*i] * w) / denominator : 0;
    enhanced_[i] = sample;
    if (!std::isfinite(sample)) return false;
    if (i < previousCount_) peak = std::max(peak, std::fabs(sample));
    const float tailW = window_[kHop+i];
    overlap_[i] = fft_[2*(kHop+i)] * tailW;
    overlapWeight_[i] = tailW * tailW;
  }
  const float required = peak > 0 ? std::min(1.0f, (8388607.0f/8388608.0f) / peak) : 1;
  limiter_ = std::min(required, 1.0f - (1.0f-limiter_) * 0.852144f);
  if (required < 1) ++peakProtections_;
  produced = primed_ ? previousCount_ : 0;
  for (size_t i = 0; i < produced; ++i) {
    const float scaled = enhanced_[i] * limiter_ * 8388608.0f;
    output[i] = static_cast<int32_t>(std::clamp(std::round(scaled), -8388608.0f, 8388607.0f));
  }
  previousCount_ = count;
  primed_ = true;
  return true;
}
