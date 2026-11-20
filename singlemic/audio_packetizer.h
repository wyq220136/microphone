#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Only a partial packet needs a raw backup. Fully emitted packets are committed.
template <size_t Capacity> class AudioPacketizer {
 public:
  void reset() { fill_ = 0; }

  template <typename Emit>
  void append(const int32_t *samples, const int32_t *raw, size_t count,
              bool rawMode, int64_t captureUs, uint32_t rate, size_t frames, Emit emit) {
    if (rawMode && fill_) std::copy_n(raw_, fill_, pcm_);
    size_t offset = 0;
    while (offset < count) {
      if (!fill_) captureUs_ = captureUs + offset * 1000000LL / rate;
      const size_t take = std::min(frames - fill_, count - offset);
      std::copy_n(samples + offset, take, pcm_ + fill_);
      // A full packet is emitted now; avoid copying its unused raw backup.
      if (fill_ + take < frames) std::copy_n(raw + offset, take, raw_ + fill_);
      fill_ += take;
      offset += take;
      if (fill_ == frames) flush(emit);
    }
  }

  template <typename Emit> void flush(Emit emit) {
    if (fill_) emit(pcm_, fill_, captureUs_);
    fill_ = 0;
  }

 private:
  int32_t pcm_[Capacity] = {}, raw_[Capacity] = {};
  size_t fill_ = 0;
  int64_t captureUs_ = 0;
};
