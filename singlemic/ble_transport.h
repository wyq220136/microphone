#ifndef SINGLEMIC_BLE_TRANSPORT_H_
#define SINGLEMIC_BLE_TRANSPORT_H_
#include <NimBLEDevice.h>
#include "transport_channel.h"

#if defined(NIMBLE_CPP_DEVICE_H_)
#define SINGLEMIC_NIMBLE_API_V2 1
#else
#define SINGLEMIC_NIMBLE_API_V2 0
#endif

enum class BleSenderState : uint8_t {
  DISCONNECTED, WAITING_PACKET, WAITING_RESULT, RETRY_WAIT, DROPPED, FAULT,
};
struct BleTransportStats {
  uint32_t packetsSent, packetsDropped, queueOverflows;
  uint32_t notifyRetries, notifyOk, notifyFailures, notifyTimeouts;
  uint16_t mtu, connectionInterval;
  size_t packetFrames, queueDepth, queueHighWater;
  uint32_t txEpoch, staleNotifyIgnored, epochDrops;
  uint32_t subscriptionAborts, inFlightAborts, eventOverflows;
  BleSenderState senderState;
};

class BleTransport {
 public:
  bool begin();
  bool setPairingMode(bool enabled) { return channel_.setPairingMode(enabled); }
  bool connected() const { return channel_.connected(); }
  bool subscribed() const { return channel_.subscribed(); }
  bool streamActive() const { return channel_.streamActive(); }
  size_t packetFrames() const { return channel_.packetFrames(); }
  bool startStream() { return channel_.startStream(); }
  bool stopStream() { return channel_.stopStream(); }
  bool enqueueAudio(const int32_t *samples, size_t frames, int64_t captureUs = 0) { return channel_.enqueueAudio(samples, frames, captureUs); }
  void recordCaptureFailure() { channel_.recordCaptureFailure(); }
  BleTransportStats stats() const { return channel_.stats(); }

 private:
  friend class BleServerCallbacks;
  friend class BleCharacteristicCallbacks;
  enum EventType { CONNECT, DISCONNECT, MTU, PARAMS, SUBSCRIBE, NOTIFY_STATUS };
  struct Event {
    EventType type = CONNECT;
    uint16_t handle = 0xffff, mtu = 0, interval = 0, value = 0;
    int status = 0;
  };
#if SINGLEMIC_NIMBLE_API_V2
  void handleConnect(NimBLEServer *, NimBLEConnInfo &);
  void handleDisconnect(NimBLEConnInfo &, int);
  void handleMtuChange(uint16_t, NimBLEConnInfo &);
  void handleConnectionParams(NimBLEConnInfo &);
#else
  void handleConnect(NimBLEServer *, ble_gap_conn_desc *);
  void handleDisconnect(ble_gap_conn_desc *);
  void handleMtuChange(uint16_t, ble_gap_conn_desc *);
#endif
  void handleNotifyStatus(int code, uint16_t handle);
  void handleSubscribe(uint16_t handle, uint16_t value);
  void postEvent(const Event &event);
  void processEvent(const Event &event);
  void step();
  void publish();
  static void senderTask(void *context);
  void runSenderTask();
  static constexpr uint16_t INVALID_CONN_HANDLE = 0xffff;

  NimBLEServer *server_ = nullptr;
  NimBLECharacteristic *audioCharacteristic_ = nullptr;
  TransportChannel<BleTransportStats> channel_;
  QueueHandle_t eventQueue_ = nullptr;
  TaskHandle_t senderTaskHandle_ = nullptr;
  std::atomic<uint32_t> eventOverflows_{0};
  uint32_t seenOverflows_ = 0;
  bool connected_ = false, subscribed_ = false, pairingMode_ = false;
  bool advertiseDirty_ = false, notifyInFlight_ = false, sessionFault_ = false;
  bool hasPacket_ = false, retryPending_ = false;
  uint16_t connectionHandle_ = INVALID_CONN_HANDLE;
  uint16_t negotiatedMtu_ = 0, negotiatedInterval_ = 0;
  uint32_t txEpoch_ = 0, activeStream_ = 0, completedControl_ = 0;
  uint32_t notifyStartedMs_ = 0, retryStartedMs_ = 0;
  uint32_t senderReadyMs_ = 0;
  uint8_t notifyAttempts_ = 0;
  TransportPacket currentPacket_;
  BleTransportStats counters_ = {};
};
#endif
