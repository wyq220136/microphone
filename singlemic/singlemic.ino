#include <Arduino.h>
#include "app_controller.h"

// Arduino 入口保持精简：应用对象管理完整生命周期。
namespace {
MicrophoneApplication application;
}

// 上电后启动串口和所有子系统。
void setup() {
  Serial.begin(115200);
  delay(300);
  application.begin();
}

// Arduino 调度循环委托给应用协调器。
void loop() {
  application.loop();
}
