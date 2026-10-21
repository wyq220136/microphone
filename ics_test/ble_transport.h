#ifndef ICS_TEST_BLE_TRANSPORT_H_
#define ICS_TEST_BLE_TRANSPORT_H_

#include <stdint.h>

#include <NimBLEDevice.h>

#include "packet_protocol.h"

// BLE 服务与发送任务的所有者；连接态、队列、任务和发送统计不再散落为全局量。
class BleTransport {
 public:
  struct Stats {
    uint32_t sent = 0;
    uint32_t dropped = 0;
    uint32_t notifyOk = 0;
    uint32_t notifyFailures = 0;
    uint32_t notifyTimeouts = 0;
  };

  BleTransport();
  bool begin();
  bool connected() const { return connected_; }
  bool enqueue(const IcsTestProtocol::BlePacket &packet, uint32_t waitMs = 0);
  void recordDrop() { ++dropped_; }
  void resetQueue();
  Stats stats() const { return stats_; }

 private:
  friend class IcsBleServerCallbacks;
  friend class IcsBleCharacteristicCallbacks;
  static void senderTask(void *context);
  void handleConnect(NimBLEServer *server, NimBLEConnInfo &info);
  void handleDisconnect(int reason);
  void handleNotifyStatus(int code);

  NimBLECharacteristic *characteristic_;
  QueueHandle_t queue_;
  TaskHandle_t senderTask_;
  volatile bool connected_;
  volatile uint32_t notifyOk_;
  volatile uint32_t notifyFailures_;
  volatile uint32_t notifyTimeouts_;
  volatile uint32_t sent_;
  volatile uint32_t dropped_;
};

#endif
