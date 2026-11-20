#include "audio_packetizer.h"
#include <cassert>
#include <vector>

int main() {
  AudioPacketizer<76> p;
  std::vector<int32_t> actual;
  std::vector<int64_t> times;
  auto emit = [&](const int32_t *x, size_t n, int64_t time) {
    assert(n && n <= 76);
    actual.insert(actual.end(), x, x+n);
    times.push_back(time);
  };
  int32_t raw[256], enhanced[256];
  for (int i = 0; i < 256; ++i) {raw[i] = i; enhanced[i] = i+10000;}
  p.append(enhanced, raw, 256, false, 1000000, 16000, 76, emit);
  assert(actual.size() == 228);
  for (int i = 0; i < 256; ++i) raw[i] = i+256;
  p.append(raw, raw, 256, true, 1016000, 16000, 76, emit);
  p.flush(emit);
  assert(actual.size() == 512);
  for (int i = 0; i < 512; ++i) assert(actual[i] == i+(i<228 ? 10000 : 0));
  assert(times[0] == 1000000 && times[1] == 1004750 && times[3] == 1014250);
  p.flush(emit);
  assert(actual.size() == 512); // END cannot emit the tail twice.
  p.append(raw, raw, 17, true, 0, 16000, 76, emit);
  p.reset(); // Disconnect discards an unsubmitted tail.
  p.append(raw, raw, 1, true, 2000000, 16000, 76, emit);
  p.flush(emit);
  assert(actual.size() == 513 && times.back() == 2000000);
}
