#include "ble_transport.h"

#include <Arduino.h>

#include "cfg.h"

// 将 NimBLE 服务器回调转交给对应 BleTransport 实例，避免全局传输状态。
class BleServerCallbacks final : public NimBLEServerCallbacks {
 public:
  explicit BleServerCallbacks(BleTransport &transport) : transport_(transport) {}

  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    transport_.handleConnect(server, info);
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
    transport_.handleMtuChange(mtu, info);
  }
  void onConnParamsUpdate(NimBLEConnInfo &info) override {
    transport_.handleConnectionParams(info);
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &info, int reason) override {
    transport_.handleDisconnect(info, reason);
  }

 private:
  BleTransport &transport_;
};

// 通知发送完成/失败回调只更新结果并唤醒等待中的发送任务。
class BleCharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
 public:
  explicit BleCharacteristicCallbacks(BleTransport &transport)
      : transport_(transport) {}

  void onStatus(NimBLECharacteristic *, int code) override {
    transport_.handleNotifyStatus(code);
  }

 private:
  BleTransport &transport_;
};

// 启动 BLE 外设：建立服务与通知特征、开始广播、创建应用队列和发送任务。
bool BleTransport::begin() {
  NimBLEDevice::init(BLEConfig::DEVICE_NAME);
  NimBLEDevice::setMTU(BLEConfig::REQUESTED_MTU);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  server_ = NimBLEDevice::createServer();
  if (server_ == nullptr) return false;
  server_->setCallbacks(new BleServerCallbacks(*this));

  NimBLEService *service = server_->createService(BLEConfig::SERVICE_UUID);
  if (service == nullptr) return false;
  audioCharacteristic_ = service->createCharacteristic(
      BLEConfig::AUDIO_UUID, NIMBLE_PROPERTY::NOTIFY);
  if (audioCharacteristic_ == nullptr) return false;
  audioCharacteristic_->setCallbacks(new BleCharacteristicCallbacks(*this));
  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BLEConfig::SERVICE_UUID);
  advertising->setName(BLEConfig::DEVICE_NAME);
  advertising->setScanResponse(true);
  advertising->start();

  packetQueue_ = xQueueCreate(BLEConfig::PACKET_QUEUE_DEPTH,
                              sizeof(AudioProtocol::Packet));
  if (packetQueue_ == nullptr) return false;
  const BaseType_t result = xTaskCreatePinnedToCore(
      senderTask, "BLE audio sender", 4096, this, 2, &senderTaskHandle_, 0);
  if (result != pdPASS) return false;
  Serial.printf("BLE advertising: %s\n", BLEConfig::DEVICE_NAME);
  return true;
}

// 连接建立后读取初始 MTU/连接间隔，重置会话队列并请求期望参数。
void BleTransport::handleConnect(NimBLEServer *server, NimBLEConnInfo &info) {
  connected_ = true;
  connectionHandle_ = info.getConnHandle();
  negotiatedMtu_ = info.getMTU();
  negotiatedInterval_ = info.getConnInterval();
  packetFrames_ = AudioProtocol::framesForMtu(negotiatedMtu_);
  sequenceNumber_ = 0;
  if (packetQueue_ != nullptr) xQueueReset(packetQueue_);
  queueHighWater_ = 0;
  server->updateConnParams(info.getConnHandle(),
                           BLEConfig::CONNECTION_INTERVAL_MIN,
                           BLEConfig::CONNECTION_INTERVAL_MAX,
                           BLEConfig::CONNECTION_LATENCY,
                           BLEConfig::CONNECTION_TIMEOUT);
  Serial.printf("BLE client connected: MTU=%u, frames/packet=%u, interval=%.2f ms; requested %.2f-%.2f ms\n",
                negotiatedMtu_, static_cast<unsigned>(packetFrames_),
                negotiatedInterval_ * 1.25f,
                BLEConfig::CONNECTION_INTERVAL_MIN * 1.25f,
                BLEConfig::CONNECTION_INTERVAL_MAX * 1.25f);
}

// 断开时停止流状态、丢弃旧连接缓存并恢复广播等待重连。
void BleTransport::handleDisconnect(NimBLEConnInfo &, int reason) {
  connected_ = false;
  streamActive_ = false;
  connectionHandle_ = INVALID_CONN_HANDLE;
  packetFrames_ = 0;
  if (packetQueue_ != nullptr) xQueueReset(packetQueue_);
  if (senderTaskHandle_ != nullptr) xTaskNotifyGive(senderTaskHandle_);
  Serial.printf("BLE client disconnected, reason=%d\n", reason);
  NimBLEDevice::getAdvertising()->start();
}

// MTU 变化会改变单包帧数；包生成端据此适配中心端能力。
void BleTransport::handleMtuChange(uint16_t mtu, NimBLEConnInfo &info) {
  negotiatedMtu_ = mtu;
  negotiatedInterval_ = info.getConnInterval();
  packetFrames_ = AudioProtocol::framesForMtu(mtu);
  Serial.printf("BLE MTU changed: MTU=%u, frames/packet=%u, interval=%.2f ms\n",
                mtu, static_cast<unsigned>(packetFrames_),
                negotiatedInterval_ * 1.25f);
  if (packetFrames_ == 0) {
    Serial.println("BLE MTU too small for the audio header and one sample");
  }
}

// 连接参数采用 BLE 规范单位：间隔 1.25 ms，监督超时 10 ms。
void BleTransport::handleConnectionParams(NimBLEConnInfo &info) {
  negotiatedInterval_ = info.getConnInterval();
  Serial.printf("BLE params updated: interval=%.2f ms latency=%u timeout=%u ms\n",
                info.getConnInterval() * 1.25f, info.getConnLatency(),
                info.getConnTimeout() * 10);
}

// NimBLE 异步报告本次通知状态；任务通知用于解除 senderTask 的等待。
void BleTransport::handleNotifyStatus(int code) {
  if (code == 0) {
    lastNotifySucceeded_ = true;
    ++notifyOk_;
  } else {
    lastNotifySucceeded_ = false;
    ++notifyFailures_;
  }
  if (senderTaskHandle_ != nullptr) xTaskNotifyGive(senderTaskHandle_);
}

// 主循环统计时同步检查 peer MTU，作为 MTU 回调的兜底更新路径。
void BleTransport::refreshNegotiatedMtu() {
  if (!connected_ || server_ == nullptr ||
      connectionHandle_ == INVALID_CONN_HANDLE) return;

  const uint16_t mtu = server_->getPeerMTU(connectionHandle_);
  const size_t frames = AudioProtocol::framesForMtu(mtu);
  if (mtu != negotiatedMtu_ || frames != packetFrames_) {
    negotiatedMtu_ = mtu;
    packetFrames_ = frames;
    Serial.printf("BLE negotiated MTU=%u, frames/packet=%u, interval=%.2f ms\n",
                  mtu, static_cast<unsigned>(frames),
                  negotiatedInterval_ * 1.25f);
    if (frames == 0) {
      Serial.println("BLE MTU too small for the audio header and one sample");
    }
  }
}

// 仅在连接且中心端开启 CCCD 通知订阅时允许启动录音。
bool BleTransport::subscribed() const {
  return connected_ && audioCharacteristic_ != nullptr &&
         audioCharacteristic_->getSubscribedCount() > 0;
}

// 所有控制包走同一 FIFO，保证 START、音频数据和 END 的发送顺序。
bool BleTransport::enqueueControl(AudioProtocol::PacketType type,
                                  uint32_t timeoutMs) {
  if (!connected_ || packetQueue_ == nullptr) return false;
  AudioProtocol::Packet packet;
  AudioProtocol::buildControlPacket(packet, type, sequenceNumber_);
  const bool queued = xQueueSend(packetQueue_, &packet,
                                 pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
  if (queued) updateQueueHighWater();
  return queued;
}

// 开始一个新会话：重置序号和旧队列，然后排入 START 控制包。
bool BleTransport::startStream() {
  if (!subscribed()) return false;
  sequenceNumber_ = 0;
  if (packetQueue_ != nullptr) xQueueReset(packetQueue_);
  streamActive_ = enqueueControl(AudioProtocol::PacketType::START, 100);
  return streamActive_;
}

// 排入 END 标记；只在成功入队后关闭本地流状态。
bool BleTransport::stopStream() {
  if (!enqueueControl(AudioProtocol::PacketType::END, 100)) return false;
  streamActive_ = false;
  return true;
}

// 将 PCM 编码并立即排队；队列满时不阻塞采集主循环。
bool BleTransport::enqueueAudio(const int16_t *samples, size_t frames) {
  if (!connected_ || packetQueue_ == nullptr) return false;
  AudioProtocol::Packet packet;
  AudioProtocol::buildAudioPacket(packet, samples, frames, sequenceNumber_++);
  if (xQueueSend(packetQueue_, &packet, 0) != pdTRUE) {
    ++packetsDropped_;
    ++queueOverflows_;
    return false;
  }
  updateQueueHighWater();
  return true;
}

// 记录 FreeRTOS 队列当前长度的历史最大值。
void BleTransport::updateQueueHighWater() {
  if (packetQueue_ == nullptr) return;
  const UBaseType_t depth = uxQueueMessagesWaiting(packetQueue_);
  if (depth > queueHighWater_) queueHighWater_ = depth;
}

// 生成给应用层的统计快照；不会清零累计计数器。
BleTransportStats BleTransport::stats() {
  refreshNegotiatedMtu();
  updateQueueHighWater();
  BleTransportStats value = {};
  value.packetsSent = packetsSent_;
  value.packetsDropped = packetsDropped_;
  value.queueOverflows = queueOverflows_;
  value.notifyRetries = notifyRetries_;
  value.notifyOk = notifyOk_;
  value.notifyFailures = notifyFailures_;
  value.notifyTimeouts = notifyTimeouts_;
  value.mtu = negotiatedMtu_;
  value.connectionInterval = negotiatedInterval_;
  value.packetFrames = packetFrames_;
  value.queueDepth = packetQueue_ == nullptr ? 0 : uxQueueMessagesWaiting(packetQueue_);
  value.queueHighWater = queueHighWater_;
  value.senderState = senderState_;
  return value;
}

// FreeRTOS C 风格入口，将任务上下文还原为 BleTransport 对象。
void BleTransport::senderTask(void *context) {
  static_cast<BleTransport *>(context)->runSenderTask();
}

// 单一发送任务串行消费 FIFO，避免多个通知同时在控制器中排队。
// 每个包最多尝试 NOTIFY_MAX_ATTEMPTS 次；失败/超时后延迟再用原包重发。
void BleTransport::runSenderTask() {
  AudioProtocol::Packet packet;
  while (true) {
    senderState_ = connected_ ? BleSenderState::WAITING_PACKET
                              : BleSenderState::DISCONNECTED;
    if (xQueueReceive(packetQueue_, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }
    updateQueueHighWater();
    if (!connected_ || audioCharacteristic_ == nullptr) {
      senderState_ = BleSenderState::DROPPED;
      ++packetsDropped_;
      continue;
    }

    bool delivered = false;
    for (uint8_t attempt = 0; attempt < BLEConfig::NOTIFY_MAX_ATTEMPTS; ++attempt) {
      if (!connected_ || audioCharacteristic_ == nullptr) break;
      if (attempt != 0) {
        senderState_ = BleSenderState::RETRY_WAIT;
        vTaskDelay(pdMS_TO_TICKS(BLEConfig::NOTIFY_RETRY_DELAY_MS));
        if (!connected_) break;
        ++notifyRetries_;
      }

      // 清除上一轮残留唤醒，避免误把旧结果当作本包的完成状态。
      ulTaskNotifyTake(pdTRUE, 0);
      lastNotifySucceeded_ = false;
      senderState_ = BleSenderState::WAITING_RESULT;
      audioCharacteristic_->setValue(packet.bytes, packet.length);
      if (!audioCharacteristic_->notify()) {
        ++notifyFailures_;
        continue;
      }
      // 等待 NimBLE 的异步发送结果；超时计数后进入有限次数重试。
      if (ulTaskNotifyTake(pdTRUE,
                           pdMS_TO_TICKS(BLEConfig::NOTIFY_TIMEOUT_MS)) == 0) {
        ++notifyTimeouts_;
        continue;
      }
      if (lastNotifySucceeded_) {
        delivered = true;
        break;
      }
    }

    if (delivered) {
      ++packetsSent_;
    } else {
      senderState_ = BleSenderState::DROPPED;
      ++packetsDropped_;
    }
  }
}

