#ifndef SINGLEMIC_TRANSPORT_ATOMIC_H_
#define SINGLEMIC_TRANSPORT_ATOMIC_H_

#include <atomic>
#include <stdint.h>
#include <esp_memory_utils.h>

// IDF 5.5 ESP32 builds disable inline hardware atomics for the PSRAM
// workaround. ATOMIC_INT_LOCK_FREE is then 1, not 2. The SDK dispatches
// aligned 32-bit atomics in internal RAM to its S32C1I implementation;
// external-RAM atomics can instead acquire an SDK lock. Keep std::atomic
// and its acquire/release ordering, and validate the actual storage before
// starting either task. Do not override the SDK's compiler flags.
static_assert(sizeof(uint32_t) == 4, "Transport atomics require 32-bit words");
static_assert(sizeof(std::atomic<uint32_t>) == 4,
              "Transport atomics require a 32-bit representation");
static_assert(alignof(std::atomic<uint32_t>) >= 4,
              "Transport atomics require word alignment");
static_assert(ATOMIC_INT_LOCK_FREE >= 1,
              "Transport requires supported 32-bit atomics");

inline bool transportAtomicStorageValid(const std::atomic<uint32_t> &value) {
  return (reinterpret_cast<uintptr_t>(&value) & 3U) == 0 &&
         esp_ptr_internal(&value);
}

#endif
