#ifndef SINGLEMIC_TRANSPORT_DIAGNOSTICS_H_
#define SINGLEMIC_TRANSPORT_DIAGNOSTICS_H_

#include <Arduino.h>
#include <stdint.h>
#include "transport_atomic.h"

// One producer (Bluetooth owner task), one consumer (application task).
// Keep trace storage out of the heap and out of FreeRTOS queue critical sections.

class TransportDiagnostics {
 public:
  struct Entry {
    uint32_t time, epoch, handle;
    int event, status, length;
    uint32_t detail;
  };
  // Fault bits are latched until reboot. A trace fault never closes Bluetooth.
  enum Fault : uint32_t {
    MEMORY_GUARD = 1, HISTORY_STATE = 2, OUTPUT_STATE = 4, FORMAT = 8,
  };

  // Called once, before either task uses the buffer. No allocation or reset.
  bool begin() {
    return transportAtomicStorageValid(written_) &&
           transportAtomicStorageValid(read_) &&
           transportAtomicStorageValid(faults_) && healthy();
  }
  uint32_t faults() const { return faults_.load(std::memory_order_relaxed); }

  // Producer only. Freeze the last 32 events on a fault, as before.
  void record(uint32_t epoch, uint32_t handle, int event, int status,
              int length, uint32_t detail, bool fault = false, uint32_t time = 0) {
    if (!historyHealthy() || frozen_) return;
    history_[next_] = {time ? time : millis(), epoch, handle, event, status, length, detail};
    next_ = (next_ + 1) & kMask;
    if (count_ < kCapacity) ++count_;
    if (fault) frozen_ = true;
  }

  // Producer only. Transfer at most four entries, without waiting for the reader.
  void flush() {
    if (!historyHealthy() || !frozen_) return;
    for (unsigned i = 0; i < 4 && count_; ++i) {
      const uint32_t write = written_.load(std::memory_order_relaxed);
      const uint32_t read = read_.load(std::memory_order_acquire);
      const uint32_t used = write - read; // Unsigned subtraction handles wrap.
      if (used > kCapacity) { fail(OUTPUT_STATE); return; }
      if (used == kCapacity) return;
      output_[write & kMask] = history_[(next_ + kCapacity - count_) & kMask];
      // Publish only after all fields of the entry have been copied.
      written_.store(write + 1, std::memory_order_release);
      --count_;
    }
    if (!count_) frozen_ = false;
  }

  // Consumer only. Once formatted, retain any unsent UART suffix locally so a
  // short write cannot duplicate a prefix or hold a producer-owned entry.
  void print(const char *name) {
    if (!healthy()) return;
    uint32_t read = 0;
    if (pendingLength_ == 0) {
      read = read_.load(std::memory_order_relaxed);
      const uint32_t write = written_.load(std::memory_order_acquire);
      const uint32_t used = write - read;
      if (used > kCapacity) { fail(OUTPUT_STATE); return; }
      if (used == 0) return;
    }
    // An idle trace path must not acquire even the UART driver's internal lock.
    const int available = Serial.availableForWrite();
    if (available <= 0) return;
    if (pendingLength_ == 0) {
      const Entry entry = output_[read & kMask];
      const int length = snprintf(pendingLine_, sizeof(pendingLine_),
          "%s trace t=%lu epoch=%lu handle=%lu event=%d status=%d len=%d detail=%lu\n",
          name, static_cast<unsigned long>(entry.time), static_cast<unsigned long>(entry.epoch),
          static_cast<unsigned long>(entry.handle), entry.event, entry.status, entry.length,
          static_cast<unsigned long>(entry.detail));
      if (length <= 0 || length >= static_cast<int>(sizeof(pendingLine_))) {
        fail(FORMAT); return;
      }
      pendingLength_ = static_cast<size_t>(length);
      pendingOffset_ = 0;
      // The entry is now in consumer-owned storage; allow producer slot reuse.
      read_.store(read + 1, std::memory_order_release);
    }
    if (pendingLength_ > sizeof(pendingLine_) || pendingOffset_ > pendingLength_) {
      fail(OUTPUT_STATE); return;
    }
    const size_t remaining = pendingLength_ - pendingOffset_;
    if (static_cast<size_t>(available) < remaining) return;
    const size_t amount = remaining;
    const size_t sent = Serial.write(
        reinterpret_cast<const uint8_t *>(pendingLine_) + pendingOffset_, amount);
    if (sent > amount) { fail(OUTPUT_STATE); return; }
    pendingOffset_ += sent;
    if (pendingOffset_ == pendingLength_) pendingOffset_ = pendingLength_ = 0;
  }

 private:
  static constexpr uint32_t kCapacity = 32;
  static constexpr uint32_t kMask = kCapacity - 1;
  static constexpr uint32_t kGuard = 0x74524345;
  static_assert((kCapacity & kMask) == 0, "Trace capacity must be a power of two");

  void fail(Fault fault) { faults_.fetch_or(fault, std::memory_order_relaxed); }
  bool healthy() {
    if (faults()) return false;
    if (historyHead_ != kGuard || historyTail_ != kGuard ||
        outputHead_ != kGuard || outputTail_ != kGuard) {
      fail(MEMORY_GUARD); return false;
    }
    return true;
  }
  bool historyHealthy() {
    if (!healthy()) return false;
    if (next_ >= kCapacity || count_ > kCapacity) {
      fail(HISTORY_STATE); return false;
    }
    return true;
  }

  // Volatile guards make checks observe memory on each call. They diagnose
  // adjacent overwrites; they do not prove that unrelated memory is intact.
  volatile uint32_t historyHead_ = kGuard;
  Entry history_[kCapacity] = {};
  volatile uint32_t historyTail_ = kGuard;
  uint32_t next_ = 0, count_ = 0;
  bool frozen_ = false;
  volatile uint32_t outputHead_ = kGuard;
  Entry output_[kCapacity] = {};
  volatile uint32_t outputTail_ = kGuard;
  std::atomic<uint32_t> written_{0}, read_{0}, faults_{0};
  char pendingLine_[180] = {};
  size_t pendingLength_ = 0, pendingOffset_ = 0;
};

#endif
