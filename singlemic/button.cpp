#include "button.h"

#include <Arduino.h>

#include "cfg.h"

// 使用上拉输入；记录初始电平和时间，启动时不产生虚假按键事件。
void DebouncedButton::begin() {
  pinMode(pin_, INPUT_PULLUP);
  stableHigh_ = rawHigh_ = digitalRead(pin_) == HIGH;
  changedAt_ = millis();
  changed_ = false;
  interruptPending_ = 0;
  // Keep the ISR minimal; debounce timing and state changes stay in loop().
  attachInterruptArg(pin_, &DebouncedButton::onInterrupt, this, CHANGE);
}

// 原始电平改变后等待配置的去抖时间，再更新稳定状态。
// 本项目按键为低电平有效，只在稳定的 HIGH -> LOW 转换时返回 true。
#if defined(IRAM_ATTR)
void IRAM_ATTR DebouncedButton::onInterrupt(void *arg) {
#else
void DebouncedButton::onInterrupt(void *arg) {
#endif
  auto *button = static_cast<DebouncedButton *>(arg);
  button->interruptPending_ = 1;
}

bool DebouncedButton::takeInterruptPending() {
  noInterrupts();
  const bool pending = interruptPending_ != 0;
  interruptPending_ = 0;
  interrupts();
  return pending;
}

bool DebouncedButton::update(uint32_t now) {
  changed_ = false;
  // Continue checking a candidate state after the last interrupt until it is
  // stable for the configured interval.
  if (!takeInterruptPending() && rawHigh_ == stableHigh_) {
    return false;
  }

  const bool high = digitalRead(pin_) == HIGH;
  if (high != rawHigh_) {
    rawHigh_ = high;
    changedAt_ = now;
    return false;
  }
  if (now - changedAt_ < IOConfig::KEY_DEBOUNCE_MS ||
      high == stableHigh_) {
    return false;
  }
  stableHigh_ = high;
  changed_ = true;
  return true;
}
