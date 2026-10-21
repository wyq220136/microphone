#ifndef SINGLEMIC_BLE_TRANSPORT_H_
#define SINGLEMIC_BLE_TRANSPORT_H_

#include <stdint.h>

#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "packet_protocol.h"

// NimBLE-Arduino 2.x changed callback parameters from ble_gap_conn_desc to
// NimBLEConnInfo and made notify() return a bool.  The header guard is the
// only version marker exposed consistently by the library releases we use.
#if defined(NIMBLE_CPP_DEVICE_H_)
#define SINGLEMIC_NIMBLE_API_V2 1
#else
#define SINGLEMIC_NIMBLE_API_V2 0
#endif

// 发送任务状态，用于诊断当前队列消费/通知结果等待阶段。
enum class BleSenderState : uint8_t {
  DISCONNECTED,
  WAITING_PACKET,
  WAITING_RESULT,
  RETRY_WAIT,
  DROPPED,
};

// 每秒由应用读取的 BLE 传输快照；计数器为运行期累计值。
struct BleTransportStats {
  uint32_t packetsSent;
  uint32_t packetsDropped;
  uint32_t queueOverflows;
  uint32_t notifyRetries;
  uint32_t notifyOk;
  uint32_t notifyFailures;
  uint32_t notifyTimeouts;
  uint16_t mtu;
  uint16_t connectionInterval;
  size_t packetFrames;
  size_t queueDepth;
  size_t queueHighWater;
  BleSenderState senderState;
};

// BLE 外设端传输层：维护 GATT 服务、接收音频包队列并串行发送通知。
class BleTransport {
 public:
  // 初始化 NimBLE、GATT 服务、广告、FreeRTOS 队列和发送任务。
  bool begin();
  // 当前是否存在 BLE 中心连接。
  bool connected() const { return connected_; }
  // 中心端是否订阅了音频特征通知。
  bool subscribed() const;
  // 当前 START/END 控制状态。
  bool streamActive() const { return streamActive_; }
  // 根据协商 MTU 得到的每个音频包帧数。
  size_t packetFrames() const { return packetFrames_; }
  // 开始/结束音频段；控制包和音频数据共用 FIFO，保证顺序。
  bool startStream();
  bool stopStream();
  // 将 PCM 数据复制进应用层队列；队列满时立即失败并增加丢包计数。
  bool enqueueAudio(const int16_t *samples, size_t frames);
  // 记录采集侧失败（例如 I2S 读取失败）。
  void recordCaptureFailure() { ++packetsDropped_; }
  // 读取发送统计、当前队列水位和协商参数。
  BleTransportStats stats();

 private:
  // NimBLE 回调适配类需要调用下列私有事件处理函数。
  friend class BleServerCallbacks;
  friend class BleCharacteristicCallbacks;

  // 内部控制包入队，可指定等待队列空位的时长。
  bool enqueueControl(AudioProtocol::PacketType type, uint32_t timeoutMs);
  // 更新队列历史最高水位。
  void updateQueueHighWater();
  // NimBLE 事件入口；回调只更新状态/统计并唤醒发送任务。
#if SINGLEMIC_NIMBLE_API_V2
  void handleConnect(NimBLEServer *server, NimBLEConnInfo &info);
  void handleDisconnect(NimBLEConnInfo &info, int reason);
  void handleMtuChange(uint16_t mtu, NimBLEConnInfo &info);
  void handleConnectionParams(NimBLEConnInfo &info);
#else
  void handleConnect(NimBLEServer *server, ble_gap_conn_desc *desc);
  void handleDisconnect(ble_gap_conn_desc *desc);
  void handleMtuChange(uint16_t mtu, ble_gap_conn_desc *desc);
#endif
  void handleNotifyStatus(int code);
  void handleSubscribe(uint16_t subValue);
  void refreshNegotiatedMtu();
  static void senderTask(void *context);
  void runSenderTask();

  NimBLEServer *server_ = nullptr;
  NimBLECharacteristic *audioCharacteristic_ = nullptr;
  // 音频与控制包共用 FIFO；senderTask 是唯一通知发送者。
  QueueHandle_t packetQueue_ = nullptr;
  TaskHandle_t senderTaskHandle_ = nullptr;
  // 连接、录音、最后一次通知结果及动态包大小等跨任务状态。
  volatile bool connected_ = false;
  volatile bool subscribed_ = false;
  volatile bool streamActive_ = false;
  volatile bool lastNotifySucceeded_ = false;
  volatile size_t packetFrames_ = 0;
  static constexpr uint16_t INVALID_CONN_HANDLE = 0xffff;
  uint16_t connectionHandle_ = INVALID_CONN_HANDLE;
  volatile uint16_t negotiatedMtu_ = 0;
  volatile uint16_t negotiatedInterval_ = 0;
  volatile uint32_t notifyOk_ = 0;
  volatile uint32_t notifyFailures_ = 0;
  volatile uint32_t notifyTimeouts_ = 0;
  volatile uint32_t notifyRetries_ = 0;
  // 下一个 DATA 序号；同一包重试时复用包缓存中的原序号。
  uint32_t sequenceNumber_ = 0;
  // 通知与队列累计计数，供每秒串口诊断日志读取。
  volatile uint32_t packetsSent_ = 0;
  volatile uint32_t packetsDropped_ = 0;
  volatile uint32_t queueOverflows_ = 0;
  volatile size_t queueHighWater_ = 0;
  volatile BleSenderState senderState_ = BleSenderState::DISCONNECTED;
};

#endif
