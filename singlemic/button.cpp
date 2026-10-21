#include "button.h"

#include <Arduino.h>

#include "cfg.h"

// 使用上拉输入；记录初始电平和时间，启动时不产生虚假按键事件。
void DebouncedButton::begin() {
  pinMode(pin_, INPUT_PULLUP);
  stableHigh_ = rawHigh_ = digitalRead(pin_) == HIGH;
  changedAt_ = millis();
}

// 原始电平改变后等待配置的去抖时间，再更新稳定状态。
// 本项目按键为低电平有效，只在稳定的 HIGH -> LOW 转换时返回 true。
bool DebouncedButton::pressed(uint32_t now) {
  const bool high = digitalRead(pin_) == HIGH;
  if (high != rawHigh_) {
    rawHigh_ = high;
    changedAt_ = now;
  }
  if (now - changedAt_ < IOConfig::KEY_DEBOUNCE_MS ||
      high == stableHigh_) {
    return false;
  }
  const bool wasHigh = stableHigh_;
  stableHigh_ = high;
  return wasHigh && !high;
}
