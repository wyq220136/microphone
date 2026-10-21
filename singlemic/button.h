#ifndef SINGLEMIC_BUTTON_H_
#define SINGLEMIC_BUTTON_H_

#include <stdint.h>

// 基于 INPUT_PULLUP 的低电平按键，并通过稳定时间过滤机械抖动。
class DebouncedButton {
 public:
  // pin 为按键 GPIO；按键另一端应接地。
  explicit DebouncedButton(int pin) : pin_(pin) {}

  // 配置 GPIO 并读取初始电平，避免上电时误判按下。
  void begin();
  // 在调用者提供的 millis() 时间点更新状态；仅在确认按下沿时返回 true。
  bool pressed(uint32_t now);

 private:
  // GPIO 编号、原始/稳定电平及最近一次原始变化时间。
  int pin_;
  bool stableHigh_ = true;
  bool rawHigh_ = true;
  uint32_t changedAt_ = 0;
};

#endif
