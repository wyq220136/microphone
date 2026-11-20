#pragma once
#include "cfg.h"
#include "I2SSampler.h"
#include "audio_processor.h"
#include "transport_atomic.h"
#include <freertos/queue.h>

class GtcrnEngine;

class AudioPipeline {
 public:
  enum class Mode : uint8_t { ENHANCED, RAW };
  enum class Reason : uint32_t { NONE, MODEL_UNAVAILABLE, PREFLIGHT, BACKLOG, AGE, INFERENCE, POOL, CAPTURE };
  struct Stats {
    Mode mode;
    Reason reason;
    size_t pending, freeBlocks;
    uint32_t frames, meanUs, maxUs, inputSaturations, peakProtections, captureFailures;
    size_t internalFree, psramFree, captureStackFree, inferenceStackFree;
  };
  struct Output {
    const int32_t *samples = nullptr;
    size_t count = 0;
    uint32_t session = 0, firstSample = 0;
    int64_t captureUs = 0;
    Mode mode = Mode::RAW;
    uint8_t token = 0;
    const int32_t *rawSamples = nullptr;
  };
  bool begin();
  bool start(uint32_t session);
  void requestStop();
  void abort();
  bool poll(Output &output);
  void release(const Output &output);
  bool drained();
  Stats stats() const;
  void printStats();
 private:
  static constexpr size_t kBlocks = DenoiseConfig::POOL_BLOCKS;
  struct Samples { int32_t raw[256], enhanced[256]; };
  // Metadata and atomics always live in internal RAM. PCM payloads are PSRAM.
  struct Block {
    std::atomic<uint32_t> refs{0}, ready{0};
    uint32_t session = 0, firstSample = 0;
    int64_t captureUs = 0;
    size_t count = 0;
  } blocks_[kBlocks];
  enum class JobType : uint8_t { START, AUDIO, FLUSH };
  struct Job { JobType type; uint8_t index; uint32_t session; };
  struct CaptureCommand { uint32_t session; bool start; };
  static void captureEntry(void *arg);
  static void inferenceEntry(void *arg);
  void captureLoop();
  void inferenceLoop();
  void collect();
  void fallback(Reason reason);
  void unref(uint8_t index);
  bool job(JobType type, uint8_t index = 0);
  I2SSampler sampler_;
  AudioProcessor processor_;
  Samples *samples_ = nullptr;
  GtcrnEngine *engine_ = nullptr;
  QueueHandle_t free_ = nullptr, captured_ = nullptr, jobs_ = nullptr, captureCommands_ = nullptr;
  TaskHandle_t captureTask_ = nullptr, inferenceTask_ = nullptr;
  std::atomic<uint32_t> requestedSession_{0}, stoppedSession_{0}, bypass_{1};
  std::atomic<uint32_t> failures_{0}, fault_{0}, frames_{0}, totalUs_{0}, maxUs_{0};
  std::atomic<uint32_t> saturations_{0}, peaks_{0};
  uint8_t pending_[kBlocks] = {};
  size_t pendingCount_ = 0;
  uint32_t session_ = 0;
  bool stopping_ = false, flushQueued_ = false, realtime_ = false;
  Reason reason_ = Reason::MODEL_UNAVAILABLE;
};
