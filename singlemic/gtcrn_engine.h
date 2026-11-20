#pragma once
#include "spectral_dsp.h"
#include "dl_model_base.hpp"
#include "dl_fft.h"

// Owned by the inference task after startup; never called from BT callbacks.
class GtcrnEngine final : public SpectralBackend {
 public:
  ~GtcrnEngine() override;
  bool begin();
  bool benchmark();
  bool verifyGolden();
  bool process(const int32_t *input, size_t count, int32_t *output, size_t &produced);
  void reset();
  bool transform(float *complex, bool inverse) override;
  bool infer(const float *spectrum, float *enhanced) override;
  void resetModel() override;
  uint32_t inputSaturations() const { return saturations_; }
  uint32_t peakProtections() const { return dsp_.peakProtections(); }
 private:
  static constexpr size_t kMaxStates = 16;
  SpectralDsp dsp_;
  dl::Model *model_ = nullptr;
  dl_fft_f32_t *fft_ = nullptr;
  dl::TensorBase *spec_ = nullptr, *enhanced_ = nullptr;
  dl::TensorBase *stateIn_[kMaxStates] = {}, *stateOut_[kMaxStates] = {};
  dl::TensorBase *stateViews_[kMaxStates] = {};
  uint8_t *stateSnapshot_ = nullptr;
  size_t snapshotBytes_ = 0;
  uint32_t saturations_ = 0;
};
