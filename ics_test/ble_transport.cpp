#include "ble_transport.h"

#include <Arduino.h>

#include "cfg.h"

class IcsBleServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit IcsBleServerCallbacks(BleTransport *owner) : owner_(owner) {}
  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    owner_->handleConnect(server, info);
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
    owner_->handleDisconnect(reason);
  }

 private:
  BleTransport *owner_;
};

class IcsBleCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit IcsBleCharacteristicCallbacks(BleTransport *owner)
      : owner_(owner) {}
  void onStatus(NimBLECharacteristic *, int code) override {
    owner_->handleNotifyStatus(code);
  }

 private:
  BleTransport *owner_;
};

BleTransport::BleTransport()
    : characteristic_(nullptr), queue_(nullptr), senderTask_(nullptr),
      connected_(false), notifyOk_(0), notifyFailures_(0),
      notifyTimeouts_(0), sent_(0), dropped_(0) {}

bool BleTransport::begin() {
  NimBLEDevice::init(IcsTestConfig::BLE_DEVICE_NAME);
  NimBLEDevice::setMTU(IcsTestConfig::REQUESTED_MTU);
  NimBLEServer *server = NimBLEDevice::createServer();
  if (server == nullptr) return false;
  server->setCallbacks(new IcsBleServerCallbacks(this));
  NimBLEService *service = server->createService(IcsTestConfig::BLE_SERVICE_UUID);
  if (service == nullptr) return false;
  characteristic_ = service->createCharacteristic(
      IcsTestConfig::BLE_AUDIO_UUID, NIMBLE_PROPERTY::NOTIFY);
  if (characteristic_ == nullptr) return false;
  characteristic_->setCallbacks(new IcsBleCharacteristicCallbacks(this));
  service->start();

  queue_ = xQueueCreate(IcsTestConfig::PACKET_QUEUE_DEPTH,
                        sizeof(IcsTestProtocol::BlePacket));
  if (queue_ == nullptr) return false;
  const BaseType_t result = xTaskCreatePinnedToCore(
      senderTask, "BLE audio sender", 4096, this, 2, &senderTask_, 0);
  if (result != pdPASS) return false;

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(IcsTestConfig::BLE_SERVICE_UUID);
  advertising->setName(IcsTestConfig::BLE_DEVICE_NAME);
  advertising->start();
  Serial.printf("BLE advertising: %s\n", IcsTestConfig::BLE_DEVICE_NAME);
  return true;
}

bool BleTransport::enqueue(const IcsTestProtocol::BlePacket &packet,
                           uint32_t waitMs) {
  if (queue_ == nullptr) return false;
  return xQueueSend(queue_, &packet, pdMS_TO_TICKS(waitMs)) == pdTRUE;
}

void BleTransport::resetQueue() {
  if (queue_ != nullptr) xQueueReset(queue_);
}

BleTransport::Stats BleTransport::stats() const {
  Stats result;
  result.sent = sent_;
  result.dropped = dropped_;
  result.notifyOk = notifyOk_;
  result.notifyFailures = notifyFailures_;
  result.notifyTimeouts = notifyTimeouts_;
  return result;
}

void BleTransport::handleConnect(NimBLEServer *server, NimBLEConnInfo &info) {
  connected_ = true;
  resetQueue();
  server->updateConnParams(info.getConnHandle(),
      IcsTestConfig::CONNECTION_INTERVAL_MIN,
      IcsTestConfig::CONNECTION_INTERVAL_MAX,
      IcsTestConfig::CONNECTION_LATENCY, IcsTestConfig::CONNECTION_TIMEOUT);
  Serial.println("BLE client connected");
}

void BleTransport::handleDisconnect(int reason) {
  connected_ = false;
  resetQueue();
  Serial.printf("BLE client disconnected, reason=%d\n", reason);
  NimBLEDevice::getAdvertising()->start();
}

void BleTransport::handleNotifyStatus(int code) {
  if (code == 0) ++notifyOk_;
  else ++notifyFailures_;
  if (senderTask_ != nullptr) xTaskNotifyGive(senderTask_);
}

void BleTransport::senderTask(void *context) {
  BleTransport *self = static_cast<BleTransport *>(context);
  IcsTestProtocol::BlePacket packet;
  while (true) {
    if (xQueueReceive(self->queue_, &packet, portMAX_DELAY) != pdTRUE) continue;
    if (!self->connected_ || self->characteristic_ == nullptr) {
      ++self->dropped_;
      continue;
    }
    ulTaskNotifyTake(pdTRUE, 0);
    self->characteristic_->setValue(packet.bytes, packet.length);
    self->characteristic_->notify();
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(
            IcsTestConfig::NOTIFY_TIMEOUT_MS)) == 0) {
      ++self->notifyTimeouts_;
      ++self->dropped_;
      continue;
    }
    ++self->sent_;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}
