#include "audio_pipeline.h"
#include "gtcrn_engine.h"
#include "task_cooperation.h"
#include <Arduino.h>
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <esp_timer.h>
#include <new>

namespace {
// These queues have capacity reserved by the single producer/ownership rules.
// Execute the send even in builds that compile configASSERT out.
void publishOwned(QueueHandle_t queue, const void *item) {
  if (xQueueSend(queue, item, 0) != pdTRUE) std::abort();
}
}

bool AudioPipeline::begin() {
  for (const auto &b : blocks_)
    if (!transportAtomicStorageValid(b.refs) || !transportAtomicStorageValid(b.ready)) return false;
  if (!transportAtomicStorageValid(requestedSession_)) return false;
  samples_ = static_cast<Samples *>(heap_caps_calloc(kBlocks, sizeof(Samples), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  free_ = xQueueCreate(kBlocks, sizeof(uint8_t));
  captured_ = xQueueCreate(kBlocks, sizeof(uint8_t));
  jobs_ = xQueueCreate(kBlocks + 4, sizeof(Job));
  captureCommands_ = xQueueCreate(8, sizeof(CaptureCommand));
  if (!samples_ || !free_ || !captured_ || !jobs_ || !captureCommands_) return false;
  for (uint8_t i = 0; i < kBlocks; ++i) xQueueSend(free_, &i, 0);
#if SINGLEMIC_ENABLE_GTCRN
  // FFT/window arrays must not be placed in PSRAM by the default allocator.
  void *storage = heap_caps_aligned_alloc(16, sizeof(GtcrnEngine), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (storage) {
    engine_ = new (storage) GtcrnEngine;
    if (engine_->begin()) {
      realtime_ = engine_->benchmark();
      reason_ = realtime_ ? Reason::NONE : Reason::PREFLIGHT;
      if (!realtime_) {
        engine_->~GtcrnEngine();
        heap_caps_free(engine_);
        engine_ = nullptr;
      }
    } else {
      engine_->~GtcrnEngine();
      heap_caps_free(engine_);
      engine_ = nullptr;
    }
  }
#endif
  if (engine_ && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < DenoiseConfig::INTERNAL_RESERVE +
      DenoiseConfig::STARTUP_INTERNAL_BYTES) {
    engine_->~GtcrnEngine();
    heap_caps_free(engine_);
    engine_ = nullptr;
    realtime_ = false;
    reason_ = Reason::PREFLIGHT;
    Serial.println("GTCRN released to preserve internal RAM for capture/Bluetooth");
  }
  if (!sampler_.begin()) return false;
  if (xTaskCreatePinnedToCore(inferenceEntry, "GTCRN", DenoiseConfig::INFERENCE_STACK,
      this, 1, &inferenceTask_, 1) != pdPASS) return false;
  if (xTaskCreatePinnedToCore(captureEntry, "I2S capture", DenoiseConfig::CAPTURE_STACK,
      this, 3, &captureTask_, 1) != pdPASS) return false;
  return heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >= DenoiseConfig::INTERNAL_RESERVE;
}

bool AudioPipeline::job(JobType type, uint8_t index) {
  const Job item{type, index, session_};
  return xQueueSend(jobs_, &item, 0) == pdTRUE;
}

bool AudioPipeline::start(uint32_t session) {
  collect();
  // The sole command producer reserves the matching STOP before accepting START.
  if (!session || session_ || pendingCount_ || uxQueueSpacesAvailable(captureCommands_) < 2) return false;
  session_ = session;
  stopping_ = flushQueued_ = false;
  fault_.store(0);
  if (engine_) reason_ = realtime_ ? Reason::NONE : Reason::PREFLIGHT;
  else if (reason_ != Reason::PREFLIGHT) reason_ = Reason::MODEL_UNAVAILABLE;
  bypass_.store(realtime_ ? 0 : 1, std::memory_order_release);
  if (!job(JobType::START)) { session_ = 0; return false; }
  requestedSession_.store(session, std::memory_order_release);
  const CaptureCommand command{session, true};
  publishOwned(captureCommands_, &command);
  Serial.printf("Audio session=%lu mode=%s reason=%u\n", static_cast<unsigned long>(session),
      realtime_ ? "GTCRN" : "RAW", static_cast<unsigned>(reason_));
  return true;
}

void AudioPipeline::requestStop() {
  if (!session_ || stopping_) return;
  stopping_ = true;
  requestedSession_.store(0, std::memory_order_release);
  const CaptureCommand command{session_, false};
  publishOwned(captureCommands_, &command);
}

void AudioPipeline::fallback(Reason reason) {
  if (!bypass_.exchange(1, std::memory_order_acq_rel)) {
    reason_ = reason;
    Serial.printf("GTCRN -> RAW: session=%lu reason=%u; retry on next recording\n",
        static_cast<unsigned long>(session_), static_cast<unsigned>(reason));
  }
}

void AudioPipeline::unref(uint8_t index) {
  const auto before = blocks_[index].refs.fetch_sub(1, std::memory_order_acq_rel);
  configASSERT(before > 0);
  if (before == 1) publishOwned(free_, &index);
}

void AudioPipeline::collect() {
  uint8_t index;
  while (xQueueReceive(captured_, &index, 0) == pdTRUE) {
    Block &block = blocks_[index];
    if (block.session != session_ || !session_) { unref(index); continue; }
    configASSERT(pendingCount_ < kBlocks);
    pending_[pendingCount_++] = index;
    if (!bypass_.load(std::memory_order_acquire)) {
      block.refs.fetch_add(1, std::memory_order_relaxed);
      if (!job(JobType::AUDIO, index)) {
        unref(index);
        fallback(Reason::BACKLOG);
      }
    }
  }
  // Publish reason + session tag in one internal-RAM 32-bit atomic. A late
  // failure from an aborted inference must not stop a newly started recording.
  const uint32_t word = fault_.load(std::memory_order_acquire);
  const uint32_t failure = session_ && (word >> 3) == (session_ & 0x1fffffffu) ? word & 7u : 0;
  if (failure) fallback(static_cast<Reason>(failure));
  if (failure == static_cast<uint32_t>(Reason::POOL) ||
      failure == static_cast<uint32_t>(Reason::CAPTURE)) requestStop();
  if (session_ && !stopping_ && requestedSession_.load(std::memory_order_acquire) == 0 &&
      stoppedSession_.load(std::memory_order_acquire) == session_) requestStop();
  if (!bypass_.load(std::memory_order_acquire) && pendingCount_) {
    if (pendingCount_ >= DenoiseConfig::BACKLOG_LIMIT) fallback(Reason::BACKLOG);
    else if (esp_timer_get_time() - blocks_[pending_[0]].captureUs > DenoiseConfig::MAX_BLOCK_AGE_US)
      fallback(Reason::AGE);
  }
  // Capture publishes every block before acknowledging the stop. collect() is
  // called again after that acquire so FLUSH can never overtake its final job.
  if (stopping_ && !flushQueued_ && stoppedSession_.load(std::memory_order_acquire) == session_) {
    if (uxQueueMessagesWaiting(captured_)) { collect(); return; }
    flushQueued_ = job(JobType::FLUSH);
  }
}

bool AudioPipeline::poll(Output &output) {
  collect();
  if (!pendingCount_) return false;
  const uint8_t index = pending_[0];
  Block &block = blocks_[index];
  const bool raw = bypass_.load(std::memory_order_acquire) != 0;
  if (!raw && !block.ready.load(std::memory_order_acquire)) return false;
  output = {raw ? samples_[index].raw : samples_[index].enhanced, block.count,
            block.session, block.firstSample, block.captureUs,
            raw ? Mode::RAW : Mode::ENHANCED, index, samples_[index].raw};
  std::memmove(pending_, pending_+1, --pendingCount_ * sizeof(uint8_t));
  return true;
}

void AudioPipeline::release(const Output &output) { unref(output.token); }

bool AudioPipeline::drained() {
  collect();
  if (!stopping_ || !flushQueued_ || pendingCount_) return false;
  session_ = 0;
  return true;
}

void AudioPipeline::abort() {
  if (!session_) return;
  requestedSession_.store(0, std::memory_order_release);
  if (!stopping_) {
    const CaptureCommand command{session_, false};
    publishOwned(captureCommands_, &command);
  }
  bypass_.store(1, std::memory_order_release);
  job(JobType::FLUSH);
  while (pendingCount_) unref(pending_[--pendingCount_]);
  session_ = 0;
  stopping_ = flushQueued_ = false;
  collect();
}

void AudioPipeline::captureEntry(void *arg) { static_cast<AudioPipeline *>(arg)->captureLoop(); }
void AudioPipeline::inferenceEntry(void *arg) { static_cast<AudioPipeline *>(arg)->inferenceLoop(); }

void AudioPipeline::captureLoop() {
  TaskCooperation::watchCurrentTask();
  int32_t raw[256];
  uint32_t session = 0, position = 0;
  TaskCooperation::Progress progress;
  for (;;) {
    CaptureCommand command;
    while (xQueueReceive(captureCommands_, &command, 0) == pdTRUE) {
      if (command.start) {
        session = command.session;
        position = 0;
        processor_.reset();
      } else {
        if (session == command.session) session = 0;
        stoppedSession_.store(command.session, std::memory_order_release);
      }
    }
    const int64_t captureUs = esp_timer_get_time();
    if (!sampler_.readSamples(raw, 256)) {
      ++failures_;
      if (session) {
        fault_.store(((session & 0x1fffffffu) << 3) | static_cast<uint32_t>(Reason::CAPTURE),
                     std::memory_order_release);
        uint32_t expected = session;
        requestedSession_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        stoppedSession_.store(session, std::memory_order_release);
        session = 0;
      }
      progress.checkpoint();
      vTaskDelay(1);
      continue;
    }
    if (session) {
      uint8_t index;
      if (xQueueReceive(free_, &index, 0) != pdTRUE) {
        ++failures_;
        fault_.store(((session & 0x1fffffffu) << 3) | static_cast<uint32_t>(Reason::POOL),
                     std::memory_order_release);
        // Do not silently splice audio after a capture loss: finish this session.
        uint32_t expected = session;
        requestedSession_.compare_exchange_strong(expected, 0, std::memory_order_acq_rel);
        stoppedSession_.store(session, std::memory_order_release);
        session = 0;
      } else {
        Block &block = blocks_[index];
        block.refs.store(1, std::memory_order_relaxed);
        block.ready.store(0, std::memory_order_relaxed);
        block.session = session;
        block.firstSample = position;
        block.captureUs = captureUs;
        block.count = 256;
        processor_.convertToPcm24(raw, 256, samples_[index].raw, 256);
        publishOwned(captured_, &index);
        position += 256;
      }
    }
    progress.checkpoint();
  }
}

void AudioPipeline::inferenceLoop() {
  TaskCooperation::watchCurrentTask();
  Job item;
  int previous = -1;
  uint32_t session = 0;
  int32_t output[256];
  TaskCooperation::Progress progress;
  for (;;) {
    if (xQueueReceive(jobs_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
      progress.checkpoint();
      continue;
    }
    if (item.type == JobType::START) {
      if (previous >= 0) unref(static_cast<uint8_t>(previous));
      previous = -1;
      session = item.session;
      if (engine_) engine_->reset();
    } else {
      // A requested stop still needs the queued final frames and FLUSH.
      const uint32_t wanted = requestedSession_.load(std::memory_order_acquire);
      const bool normalStop = engine_ && item.session == session && (!wanted || wanted == item.session) &&
          !bypass_.load(std::memory_order_acquire);
      if (normalStop) {
        size_t produced = 0;
        const int64_t before = esp_timer_get_time();
        const bool ok = item.type == JobType::FLUSH ?
            engine_->process(nullptr, 0, output, produced) :
            engine_->process(samples_[item.index].raw, blocks_[item.index].count, output, produced);
        const uint32_t elapsed = static_cast<uint32_t>(esp_timer_get_time() - before);
        ++frames_; totalUs_.fetch_add(elapsed);
        if (elapsed > maxUs_.load()) maxUs_.store(elapsed);
        saturations_.store(engine_->inputSaturations());
        peaks_.store(engine_->peakProtections());
        if (!ok) fault_.store(((item.session & 0x1fffffffu) << 3) | static_cast<uint32_t>(Reason::INFERENCE),
                              std::memory_order_release);
        if (ok && previous >= 0 && produced == blocks_[previous].count) {
          std::memcpy(samples_[previous].enhanced, output, produced * sizeof(int32_t));
          blocks_[previous].ready.store(1, std::memory_order_release);
        }
      }
      if (previous >= 0) unref(static_cast<uint8_t>(previous));
      previous = -1;
      if (item.type == JobType::AUDIO) previous = item.index;
      if (item.type == JobType::FLUSH) session = 0;
    }
    progress.checkpoint();
  }
}

AudioPipeline::Stats AudioPipeline::stats() const {
  const uint32_t count = frames_.load();
  return {bypass_.load() ? Mode::RAW : Mode::ENHANCED, reason_, pendingCount_,
      uxQueueMessagesWaiting(free_), count, count ? totalUs_.load()/count : 0,
      maxUs_.load(), saturations_.load(), peaks_.load(), failures_.load(),
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
      uxTaskGetStackHighWaterMark(captureTask_), uxTaskGetStackHighWaterMark(inferenceTask_)};
}

void AudioPipeline::printStats() {
  const Stats s = stats();
  Serial.printf("GTCRN mode=%s reason=%u pending=%u free_blocks=%u frames=%lu mean_us=%lu max_us=%lu "
      "input_saturation=%lu peak_protection=%lu capture_fail=%lu internal_free=%u psram_free=%u "
      "stack_capture=%u stack_infer=%u\n",
      s.mode == Mode::RAW ? "RAW" : "ENHANCED", static_cast<unsigned>(s.reason),
      static_cast<unsigned>(s.pending), static_cast<unsigned>(s.freeBlocks),
      static_cast<unsigned long>(s.frames), static_cast<unsigned long>(s.meanUs),
      static_cast<unsigned long>(s.maxUs), static_cast<unsigned long>(s.inputSaturations),
      static_cast<unsigned long>(s.peakProtections), static_cast<unsigned long>(s.captureFailures),
      static_cast<unsigned>(s.internalFree), static_cast<unsigned>(s.psramFree),
      static_cast<unsigned>(s.captureStackFree), static_cast<unsigned>(s.inferenceStackFree));
}
