#include "button.h"

#include <Arduino.h>

#include "cfg.h"

DebouncedButton::DebouncedButton(int pin)
    : pin_(pin), stableHigh_(true), rawHigh_(true), changedAt_(0) {}

void DebouncedButton::begin(uint32_t now) {
  pinMode(pin_, INPUT_PULLUP);
  stableHigh_ = rawHigh_ = digitalRead(pin_) == HIGH;
  changedAt_ = now;
}

bool DebouncedButton::pressed(uint32_t now) {
  const bool high = digitalRead(pin_) == HIGH;
  if (high != rawHigh_) {
    rawHigh_ = high;
    changedAt_ = now;
  }
  if (now - changedAt_ < IcsTestConfig::KEY_DEBOUNCE_MS ||
      high == stableHigh_) {
    return false;
  }
  const bool wasHigh = stableHigh_;
  stableHigh_ = high;
  return wasHigh && !high;
}
