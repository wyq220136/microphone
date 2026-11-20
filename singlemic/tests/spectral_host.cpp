#include "spectral_dsp.h"
#include <algorithm>
#include <complex>
#include <cmath>
#include <cstring>

class ReferenceBackend : public SpectralBackend {
 public:
  float gain = 1;
  float bias = 0;
  bool transform(float *data, bool inverse) override {
    std::complex<float> x[512];
    for (int i = 0; i < 512; ++i) x[i] = {data[2*i], data[2*i+1]};
    for (unsigned i = 1, j = 0; i < 512; ++i) {
      unsigned bit = 256;
      for (; j & bit; bit >>= 1) j ^= bit;
      j ^= bit;
      if (i < j) std::swap(x[i], x[j]);
    }
    for (unsigned len = 2; len <= 512; len <<= 1) {
      const std::complex<float> step = std::polar(1.0f, (inverse ? 1 : -1) * 6.283185307179586f / len);
      for (unsigned i = 0; i < 512; i += len) {
        std::complex<float> w(1, 0);
        for (unsigned j = 0; j < len/2; ++j) {
          const auto a = x[i+j], b = x[i+j+len/2] * w;
          x[i+j] = a+b;
          x[i+j+len/2] = a-b;
          w *= step;
        }
      }
    }
    for (unsigned i = 0; i < 512; ++i) {
      data[2*i] = x[i].real() / (inverse ? 512 : 1);
      data[2*i+1] = x[i].imag() / (inverse ? 512 : 1);
    }
    return true;
  }
  bool infer(const float *spectrum, float *output) override {
    for (unsigned i = 0; i < 514; ++i) output[i] = spectrum[i] * gain;
    output[0] += bias; // Simulate a model's low-level residual on silent input.
    return true;
  }
  void resetModel() override {}
};

extern "C" __declspec(dllexport) int run_dsp(const int32_t *input, size_t length,
    int32_t *output, float gain, int normalize, int match, int resetTwice, float bias) {
  ReferenceBackend backend;
  backend.gain = gain;
  backend.bias = bias;
  LevelSettings settings;
  settings.normalize = normalize;
  settings.match = match;
  SpectralDsp dsp;
  dsp.begin(backend, settings);
  if (resetTwice) dsp.reset();
  size_t written = 0;
  int32_t block[256];
  for (size_t position = 0; position < length; position += 256) {
    size_t n = 0;
    if (!dsp.process(input+position, std::min<size_t>(256, length-position), block, n)) return -1;
    std::copy_n(block, n, output+written);
    written += n;
  }
  if (length) {
    size_t n = 0;
    if (!dsp.process(nullptr, 0, block, n)) return -1;
    std::copy_n(block, n, output+written);
    written += n;
  }
  return static_cast<int>(written);
}
