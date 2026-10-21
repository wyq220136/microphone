#ifndef ICS_TEST_BUTTON_H_
#define ICS_TEST_BUTTON_H_

#include <stdint.h>

// 带消抖的按键：只在稳定的高电平到低电平转换时报告一次按下。
class DebouncedButton {
 public:
  explicit DebouncedButton(int pin);
  void begin(uint32_t now);
  bool pressed(uint32_t now);

 private:
  int pin_;
  bool stableHigh_;
  bool rawHigh_;
  uint32_t changedAt_;
};

#endif
