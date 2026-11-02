#include <Arduino.h>
#include "app_controller.h"
#include "task_cooperation.h"
#include "reset_progress.h"

// Arduino loopTask stack size in bytes (ESP32 FreeRTOS units).
SET_LOOP_TASK_STACK_SIZE(4096);

// Arduino 入口保持精简：应用对象管理完整生命周期。
namespace {
MicrophoneApplication application;
TaskCooperation::Progress loopProgress;
}

// Keep the SDK's default task/interrupt watchdog initialization and feeding.
// Do not override initVariant() to disable watchdog hardware.
// 上电后启动串口和所有子系统。
void setup() {
  // Buffer diagnostic bursts so UART output does not stall I2S capture.
  Serial.setTxBufferSize(2048);
  Serial.begin(115200);
  delay(300);
  // disableCore0WDT();
  // disableCore1WDT();
  ResetProgress::begin();
  application.begin();
  TaskCooperation::watchCurrentTask();
}

// Arduino 调度循环委托给应用协调器。
void loop() {
  application.loop();
  ResetProgress::main(ResetProgress::Stage::MAIN_YIELD);
  loopProgress.checkpoint();
}
