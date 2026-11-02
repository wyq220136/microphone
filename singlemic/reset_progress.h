#ifndef SINGLEMIC_RESET_PROGRESS_H_
#define SINGLEMIC_RESET_PROGRESS_H_

#include <stdint.h>

// Best-effort reset breadcrumbs, not a trace or a watchdog feeder. Each slot
// has one task writer. No locks, allocation, SDK calls or printing in mark().
namespace ResetProgress {
enum class Stage : uint8_t {
  BOOT, BUTTONS, SNAPSHOT, STATS, START_HEAP, START_QUEUE, I2S_READ, DSP,
  ENQUEUE, HUFFMAN_INIT, HUFFMAN_ENCODE, BLOCK_DELTA, CRC, MAIN_YIELD,
  SPP_EVENTS, SPP_STEP, SPP_WRITE, SPP_WRITE_RETURNED, SPP_PUBLISH,
  SPP_TRACE, SPP_YIELD, SPP_WAIT,
  FIXED_PCM, // Append only: retain previous stage numbers for RTC records.
};
void begin(); // Once in setup, before starting any application task.
void main(Stage stage); // Arduino loop task only.
void spp(Stage stage);  // SPP owner task only, never callbacks/ISRs.
}

#endif
