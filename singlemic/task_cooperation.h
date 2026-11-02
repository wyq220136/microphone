#ifndef SINGLEMIC_TASK_COOPERATION_H_
#define SINGLEMIC_TASK_COOPERATION_H_

#include <esp_err.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace TaskCooperation {

// Call once from the task itself, after SDK watchdog initialization.
// Do not change the SDK timeout or unsubscribe either core's IDLE task.
inline void watchCurrentTask() {
  const esp_err_t status = esp_task_wdt_status(nullptr);
  if (status == ESP_OK) return;
  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
}

// Task context only, outside critical sections and with no application lock held.
// Keep one instance across iterations of each loop. Never use this in an ISR
// or Bluetooth callback. Feeding the current task does not feed the IDLE task.
class Progress {
 public:
  void checkpoint() {
    constexpr TickType_t interval =
        pdMS_TO_TICKS(2) > 0 ? pdMS_TO_TICKS(2) : 1;
    const TickType_t now = xTaskGetTickCount();
    if (static_cast<TickType_t>(now - lastYield_) < interval) return;

    // A real blocked tick allows lower-priority tasks to run; taskYIELD alone
    // would leave this task runnable and need not let IDLE run.
    vTaskDelay(1);
    lastYield_ = xTaskGetTickCount();
    // Nested bounded computations may also run before a caller subscribes.
    // Only reset a task that is actually registered with the task watchdog.
    if (esp_task_wdt_status(nullptr) == ESP_OK) {
      ESP_ERROR_CHECK(esp_task_wdt_reset());
    }
  }

 private:
  TickType_t lastYield_ = 0;
};

}  // namespace TaskCooperation

#endif
