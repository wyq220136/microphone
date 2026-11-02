#include "reset_progress.h"

#include <Arduino.h>
#include <esp_attr.h>
#include <esp_system.h>

namespace ResetProgress {
namespace {
constexpr uint32_t kMagic = 0x534D5201; // Change when stage numbering changes.
struct Record {
  uint32_t magic, inverseMagic;
  uint32_t mainWord, sppWord;
};
// RTC RAM is not initialized by the C runtime. A cold boot invalidates this
// record. Retention after a hard reset is best effort, never guaranteed.
RTC_NOINIT_ATTR volatile Record retained;

const char *name(uint32_t word) {
  switch (static_cast<Stage>(word & 0xff)) {
    case Stage::BOOT: return "boot";
    case Stage::BUTTONS: return "buttons";
    case Stage::SNAPSHOT: return "snapshot";
    case Stage::STATS: return "stats";
    case Stage::START_HEAP: return "start_heap_check";
    case Stage::START_QUEUE: return "start_queue";
    case Stage::I2S_READ: return "i2s_read";
    case Stage::FIXED_PCM: return "fixed_pcm";
    case Stage::DSP: return "dsp";
    case Stage::ENQUEUE: return "encode_or_enqueue";
    case Stage::HUFFMAN_INIT: return "huffman_init";
    case Stage::HUFFMAN_ENCODE: return "huffman_encode";
    case Stage::BLOCK_DELTA: return "block_delta";
    case Stage::CRC: return "crc";
    case Stage::MAIN_YIELD: return "main_yield";
    case Stage::SPP_EVENTS: return "spp_events";
    case Stage::SPP_STEP: return "spp_step";
    case Stage::SPP_WRITE: return "spp_write";
    case Stage::SPP_WRITE_RETURNED: return "spp_write_returned";
    case Stage::SPP_PUBLISH: return "spp_publish";
    case Stage::SPP_TRACE: return "spp_trace";
    case Stage::SPP_YIELD: return "spp_yield";
    case Stage::SPP_WAIT: return "spp_wait";
  }
  return "invalid";
}

void mark(volatile uint32_t &slot, Stage stage) {
  // Count and stage share one aligned 32-bit store, avoiding a torn pair.
  // Upper 24 bits count marks (wrap allowed), not packets or elapsed time.
  slot = ((slot + 0x100U) & 0xffffff00U) | static_cast<uint32_t>(stage);
  asm volatile("memw" ::: "memory");
}
}

void begin() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const bool abnormal = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT ||
                        reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
  if (abnormal) {
    if (retained.magic == kMagic && retained.inverseMagic == ~kMagic) {
      const uint32_t mainWord = retained.mainWord, sppWord = retained.sppWord;
      Serial.printf("Previous run: main=%s marks=%lu spp=%s marks=%lu "
                    "(last checkpoints, not proof of fault location)\n",
                    name(mainWord), static_cast<unsigned long>(mainWord >> 8),
                    name(sppWord), static_cast<unsigned long>(sppWord >> 8));
    } else {
      Serial.println("Previous run: no valid retained checkpoints");
    }
  }
  retained.magic = 0;
  retained.mainWord = retained.sppWord = static_cast<uint32_t>(Stage::BOOT);
  retained.inverseMagic = ~kMagic;
  asm volatile("memw" ::: "memory");
  retained.magic = kMagic;
  asm volatile("memw" ::: "memory");
}
void main(Stage stage) { mark(retained.mainWord, stage); }
void spp(Stage stage) { mark(retained.sppWord, stage); }
}
