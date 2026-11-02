#ifndef SINGLEMIC_FIXED_PCM_SOURCE_H_
#define SINGLEMIC_FIXED_PCM_SOURCE_H_

#include <stddef.h>
#include <stdint.h>

// Deterministic PCM24, application-task only. No hardware, allocation or waits.
// Pacing replaces the I2S clock; a delayed task never drains a catch-up backlog.
class FixedPcmSource {
 public:
  void reset(uint32_t nowUs) {
    position_ = 0;
    nextPacketUs_ = nowUs;
    remainder_ = 0;
  }

  template <size_t Capacity>
  bool read(int32_t (&output)[Capacity], size_t frames,
            uint32_t nowUs, uint32_t sampleRate) {
    if (!frames || frames > Capacity || frames > 76 ||
        !sampleRate || sampleRate > 1000000) return false;
    if (static_cast<int32_t>(nowUs - nextPacketUs_) < 0) return false;
    // Mix full-range and small signed samples. This is test data, not silence.
    static constexpr int32_t pattern[] = {
        0, 1, -1, 8388607, -8388608, 0x123456, -0x123456, 0x654321,
        -0x654321, 0x234567, -0x234567, 0x345678, -0x345678, 255, -256, 65536,
        -65536, 0x456789, -0x456789, 0x56789a, -0x56789a, 0x6789ab, -0x6789ab, 127,
        -128, 32767, -32768, 0x2468ac, -0x2468ac, 0x13579b, -0x13579b, 0,
    };
    constexpr size_t count = sizeof(pattern) / sizeof(pattern[0]);
    for (size_t i = 0; i < frames; ++i) {
      output[i] = pattern[position_];
      if (++position_ == count) position_ = 0;
    }
    // Carry fractional microseconds: 76/12000 s -> 6333,6333,6334 us.
    const uint64_t scaled = static_cast<uint64_t>(frames) * 1000000 + remainder_;
    const uint32_t intervalUs = static_cast<uint32_t>(scaled / sampleRate);
    remainder_ = static_cast<uint32_t>(scaled % sampleRate);
    nextPacketUs_ += intervalUs;
    if (static_cast<int32_t>(nowUs - nextPacketUs_) >= 0)
      nextPacketUs_ = nowUs + intervalUs;
    return true;
  }

 private:
  size_t position_ = 0;
  uint32_t nextPacketUs_ = 0, remainder_ = 0;
};

#endif
