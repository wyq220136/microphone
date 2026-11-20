#include "gtcrn_engine.h"
#include "cfg.h"
#include "model_contract.h"
#include <Arduino.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <esp_partition.h>
#include <esp_timer.h>
#include <psa/crypto.h>
#if SINGLEMIC_GTCRN_SELF_TEST
#include "golden_contract.h"
#endif

namespace {
bool matches(dl::TensorBase *t, const ModelContract::Port &p) {
  if (!t || !t->data || t->get_dtype() != p.dtype ||
      t->get_exponent() != p.exponent || t->shape.size() != static_cast<size_t>(p.rank)) return false;
  for (int i = 0; i < p.rank; ++i) if (t->shape[i] != p.dims[i]) return false;
  return (t->get_dtype() == dl::DATA_TYPE_INT16 || t->get_dtype() == dl::DATA_TYPE_FLOAT);
}

bool partitionMatches() {
  const esp_partition_t *partition = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
  if (!partition || partition->size < ModelContract::bytes) return false;
  const void *address = nullptr;
  esp_partition_mmap_handle_t handle;
  if (esp_partition_mmap(partition, 0, ModelContract::bytes, ESP_PARTITION_MMAP_DATA,
                         &address, &handle) != ESP_OK) return false;
  unsigned char hash[32];
  size_t hashLength = 0;
  psa_status_t status = psa_crypto_init();
  if (status == PSA_SUCCESS)
    status = psa_hash_compute(PSA_ALG_SHA_256, static_cast<const unsigned char *>(address),
                             ModelContract::bytes, hash, sizeof(hash), &hashLength);
  esp_partition_munmap(handle);
  if (status != PSA_SUCCESS || hashLength != sizeof(hash)) return false;
  constexpr char hex[] = "0123456789abcdef";
  char text[65] = {};
  for (size_t i = 0; i < 32; ++i) {
    text[2*i] = hex[hash[i] >> 4];
    text[2*i+1] = hex[hash[i] & 15];
  }
  return std::strcmp(text, ModelContract::sha256) == 0;
}
}

GtcrnEngine::~GtcrnEngine() {
  for (auto *view : stateViews_) delete view;
  delete model_;
  if (fft_) dl_fft_f32_deinit(fft_);
  heap_caps_free(stateSnapshot_);
}

bool GtcrnEngine::begin() {
  static_assert(ModelContract::stateCount <= kMaxStates);
  if (!partitionMatches()) {
    Serial.println("GTCRN unavailable: model partition missing or SHA256 mismatch");
    return false;
  }
  model_ = new dl::Model("model", fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
                         0, dl::MEMORY_MANAGER_GREEDY, nullptr, false);
  if (!model_ || !model_->is_built()) return false;
  spec_ = model_->get_input("spec");
  enhanced_ = model_->get_output("enh");
  if (!matches(spec_, ModelContract::spec) || !matches(enhanced_, ModelContract::enhanced)) return false;
  for (size_t i = 0; i < ModelContract::stateCount; ++i) {
    stateIn_[i] = model_->get_input(ModelContract::inputs[i].name);
    stateOut_[i] = model_->get_output(ModelContract::outputs[i].name);
    if (!matches(stateIn_[i], ModelContract::inputs[i]) || !matches(stateOut_[i], ModelContract::outputs[i]))
      return false;
    snapshotBytes_ += stateOut_[i]->get_bytes();
  }
  stateSnapshot_ = static_cast<uint8_t *>(heap_caps_malloc(snapshotBytes_, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  fft_ = dl_fft_f32_init(512, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!fft_ || !stateSnapshot_) return false;
  size_t offset = 0;
  for (size_t i = 0; i < ModelContract::stateCount; ++i) {
    stateViews_[i] = new dl::TensorBase(stateOut_[i]->shape, stateSnapshot_ + offset,
        stateOut_[i]->get_exponent(), stateOut_[i]->get_dtype(), false);
    if (!stateViews_[i]) return false;
    offset += stateOut_[i]->get_bytes();
  }
  LevelSettings levels;
  levels.normalize = ModelContract::normalize;
  levels.match = ModelContract::match;
  levels.targetRms = ModelContract::targetRms;
  levels.minInputGain = ModelContract::minInputGain;
  levels.maxInputGain = ModelContract::maxInputGain;
  levels.minOutputGain = ModelContract::minOutputGain;
  levels.maxOutputGain = ModelContract::maxOutputGain;
  dsp_.begin(*this, levels);
  if (!verifyGolden()) return false;
  Serial.printf("GTCRN streaming loaded: bytes=%u sha256=%s states=%u\n",
      static_cast<unsigned>(ModelContract::bytes), ModelContract::sha256,
      static_cast<unsigned>(ModelContract::stateCount));
  return true;
}

void GtcrnEngine::resetModel() {
  if (!model_) return;
  model_->reset();
  for (auto *state : stateIn_) if (state) state->memset(0);
  saturations_ = 0;
}

void GtcrnEngine::reset() { dsp_.reset(); }

bool GtcrnEngine::transform(float *data, bool inverse) {
  return (inverse ? dl_ifft_f32_run(fft_, data) : dl_fft_f32_run(fft_, data)) == ESP_OK;
}

bool GtcrnEngine::infer(const float *input, float *output) {
  const float scale = std::ldexp(1.0f, -spec_->get_exponent());
  for (size_t i = 0; i < 514; ++i) {
    if (!std::isfinite(input[i])) return false;
    if (spec_->get_dtype() == dl::DATA_TYPE_FLOAT) {
      spec_->get_element_ptr<float>()[i] = input[i];
    } else {
      const float value = std::floor(input[i] * scale + 0.5f); // ESP32 ROUND_HALF_UP
      if (value > 32767 || value < -32768) ++saturations_;
      spec_->get_element_ptr<int16_t>()[i] = static_cast<int16_t>(std::clamp(value, -32768.0f, 32767.0f));
    }
  }
  model_->run(dl::RUNTIME_MODE_SINGLE_CORE);
  const float outScale = std::ldexp(1.0f, enhanced_->get_exponent());
  for (size_t i = 0; i < 514; ++i) {
    output[i] = enhanced_->get_dtype() == dl::DATA_TYPE_FLOAT ?
        enhanced_->get_element_ptr<float>()[i] : enhanced_->get_element_ptr<int16_t>()[i] * outScale;
    if (!std::isfinite(output[i])) return false;
  }
  // Snapshot every output before modifying any input: model memory can be reused.
  size_t offset = 0;
  for (size_t i = 0; i < ModelContract::stateCount; ++i) {
    std::memcpy(stateSnapshot_ + offset, stateOut_[i]->data, stateOut_[i]->get_bytes());
    offset += stateOut_[i]->get_bytes();
  }
  for (size_t i = 0; i < ModelContract::stateCount; ++i) {
    if (!stateIn_[i]->assign(stateViews_[i])) return false;
  }
  return true;
}

bool GtcrnEngine::process(const int32_t *input, size_t count, int32_t *output, size_t &produced) {
  return dsp_.process(input, count, output, produced);
}

bool GtcrnEngine::benchmark() {
  int32_t input[256], output[256];
  uint32_t durations[DenoiseConfig::BENCHMARK_FRAMES];
  uint64_t total = 0;
  for (size_t i = 0; i < 256; ++i)
    input[i] = static_cast<int32_t>(700000.0f * std::sin(0.1727876f * i));
  reset();
  bool valid = true;
  for (size_t i = 0; i < DenoiseConfig::WARMUP_FRAMES + DenoiseConfig::BENCHMARK_FRAMES; ++i) {
    size_t produced;
    const int64_t before = esp_timer_get_time();
    if (!process(input, 256, output, produced)) { valid = false; break; }
    const uint32_t us = static_cast<uint32_t>(esp_timer_get_time() - before);
    if (i >= DenoiseConfig::WARMUP_FRAMES) {
      durations[i-DenoiseConfig::WARMUP_FRAMES] = us;
      total += us;
    }
    vTaskDelay(1); // Allows both IDLE tasks to feed the watchdog during startup.
  }
  if (!valid) { reset(); return false; }
  std::sort(std::begin(durations), std::end(durations));
  const uint32_t average = total / DenoiseConfig::BENCHMARK_FRAMES;
  const uint32_t p95 = durations[(DenoiseConfig::BENCHMARK_FRAMES * 95 + 99)/100 - 1];
  const bool passed = average <= 12800 && p95 <= 16000 &&
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= DenoiseConfig::INTERNAL_RESERVE +
          DenoiseConfig::STARTUP_INTERNAL_BYTES;
  Serial.printf("GTCRN preflight: mean_us=%lu p95_us=%lu realtime=%s internal_free=%u\n",
      static_cast<unsigned long>(average), static_cast<unsigned long>(p95), passed ? "PASS" : "FAIL_RAW",
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  reset();
  return passed;
}

bool GtcrnEngine::verifyGolden() {
#if SINGLEMIC_GTCRN_SELF_TEST
  float input[514], output[514];
  int maximum = 0;
  reset();
  for (int frame = 0; frame < GoldenStream::count; ++frame) {
    for (size_t i = 0; i < 514; ++i)
      input[i] = std::ldexp(static_cast<float>(GoldenStream::spec[frame][i]), GoldenStream::inputExponent);
    if (!infer(input, output)) return false;
    for (size_t i = 0; i < 514; ++i) {
      const int actual = static_cast<int>(std::floor(std::ldexp(output[i], -GoldenStream::outputExponent) + 0.5f));
      maximum = std::max(maximum, std::abs(actual - GoldenStream::enh[frame][i]));
    }
    vTaskDelay(1);
  }
  Serial.printf("GTCRN golden: frames=%d max_lsb=%d result=%s\n",
      GoldenStream::count, maximum, maximum <= 2 ? "PASS" : "FAIL");
  reset();
  return maximum <= 2;
#else
  return true;
#endif
}
