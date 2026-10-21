// ICS-43434 双麦 BLE 录音固件入口。具体功能分别由采集、DSP、协议、
// 按键和 BLE 模块实现，此文件只负责 Arduino 生命周期入口。
#include "app_controller.h"

namespace {
IcsTestApp app;
}

void setup() {
  if (!app.begin()) {
    while (true) delay(1000);
  }
}

void loop() { app.loop(); }
