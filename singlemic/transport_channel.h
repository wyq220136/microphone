#ifndef SINGLEMIC_TRANSPORT_CHANNEL_H_
#define SINGLEMIC_TRANSPORT_CHANNEL_H_

#include <Arduino.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include "packet_protocol.h"
#include "transport_atomic.h"

// Public methods are called only by the application task. The Bluetooth owner
// is the sole consumer/publisher. No application critical section is needed.

struct TransportPacket {
  AudioProtocol::Packet packet;
  uint32_t epoch = 0, stream = 0, control = 0;
  int64_t captureUs = 0; // Local telemetry only; never serialized on the wire.
  bool isControl() const { return packet.bytes[2] == 2 || packet.bytes[2] == 3; }
};

template <typename Stats>
class TransportChannel {
 public:
  struct Snapshot {
    Stats stats = {};
    uint32_t epoch = 0, completedControl = 0;
    size_t frames = 0;
    bool ready = false, failed = false, connected = false;
    bool subscribed = false, fault = false;
  };

  bool begin(size_t depth) {
    if (!transportAtomicStorageValid(pairingRequest_)) return false;
    packets_ = xQueueCreate(depth, sizeof(TransportPacket));
    snapshots_ = xQueueCreate(1, sizeof(Snapshot));
    return packets_ && snapshots_;
  }
  void setOwner(TaskHandle_t task) { owner_ = task; }
  void wake() const { if (owner_) xTaskNotifyGive(owner_); }
  bool setPairingMode(bool enabled) {
    pairingRequest_.store(enabled ? 1 : 0, std::memory_order_release);
    wake();
    return true;
  }
  bool pairingRequested() const {
    return pairingRequest_.load(std::memory_order_acquire) != 0;
  }

  // Application-only cache: peek copies the complete snapshot, never a
  // concurrently modified C++ struct. Empty queue means the initial state.
  Snapshot snapshot() const {
    if (snapshots_) xQueuePeek(snapshots_, &cached_, 0);
    return cached_;
  }
  bool connected() const { return snapshot().connected; }
  bool subscribed() const {
    const Snapshot s = snapshot();
    return s.connected && s.subscribed;
  }
  size_t packetFrames() const {
    const Snapshot s = snapshot();
    return s.connected ? s.frames : 0;
  }
  bool streamActive() const {
    const Snapshot s = snapshot();
    return intent_ && producerEpoch_ == s.epoch && usable(s);
  }
  bool startStream() { return enqueueControl(true); }
  bool stopStream() { return enqueueControl(false); }

  bool enqueueAudio(const int32_t *samples, size_t frames, int64_t captureUs = 0) {
    // Called synchronously from MicrophoneApplication::loop() on loopTask.
    // Full-byte Huffman and CRC run here before the completed packet enters
    // the FIFO. Never call from a Bluetooth callback, ISR or sender task.
    const Snapshot s = snapshot();
    if (!intent_ || producerEpoch_ != s.epoch || !usable(s) ||
        !samples || !frames || frames > s.frames) return false;
    TransportPacket packet;
    packet.epoch = s.epoch;
    packet.stream = stream_;
    packet.captureUs = captureUs;
    AudioProtocol::buildAudioPacket(packet.packet, samples, frames, sequence_++);
    const Snapshot latest = snapshot();
    if (latest.epoch != s.epoch || !usable(latest)) {
      ++producerDrops_; ++producerEpochDrops_; return false;
    }
    // Sole producer; the consumer can only make more room. Reserve two slots
    // for START/END and never reset a queue concurrently with an enqueue.
    if (uxQueueSpacesAvailable(packets_) <= 2 ||
        xQueueSend(packets_, &packet, 0) != pdTRUE) {
      ++producerDrops_; ++producerOverflows_; return false;
    }
    noteDepth();
    wake();
    return true;
  }
  void recordCaptureFailure() { ++producerDrops_; }
  Stats stats() const {
    Stats result = snapshot().stats;
    result.packetsDropped += producerDrops_;
    result.queueOverflows += producerOverflows_;
    result.epochDrops += producerEpochDrops_;
    if (producerHighWater_ > result.queueHighWater)
      result.queueHighWater = producerHighWater_;
    return result;
  }

  // Owner-task operations only.
  bool peek(TransportPacket &packet) const { return xQueuePeek(packets_, &packet, 0) == pdTRUE; }
  bool take(TransportPacket &packet) { return xQueueReceive(packets_, &packet, 0) == pdTRUE; }
  size_t depth() const { return uxQueueMessagesWaiting(packets_); }
  void publish(const Snapshot &value) { xQueueOverwrite(snapshots_, &value); }
  void discardOld(uint32_t epoch, Stats &stats) {
    TransportPacket packet;
    for (unsigned i = 0; i < 16; ++i) {
      if (xQueuePeek(packets_, &packet, 0) != pdTRUE || packet.epoch == epoch) break;
      if (!take(packet)) break;
      ++stats.packetsDropped; ++stats.epochDrops;
    }
  }

 private:
  static bool usable(const Snapshot &s) {
    return s.ready && s.connected && s.subscribed && !s.fault && s.frames;
  }
  bool enqueueControl(bool starting) {
    const Snapshot s = snapshot();
    if (!usable(s)) return false;
    if (producerEpoch_ != s.epoch) {
      producerEpoch_ = s.epoch;
      intent_ = pendingControl_ = false;
      sequence_ = 0;
    }
    if (pendingControl_ && s.completedControl == lastControl_) pendingControl_ = false;
    if (starting ? (intent_ || pendingControl_) : !intent_) return false;
    TransportPacket packet;
    packet.epoch = s.epoch;
    packet.stream = starting ? stream_ + 1 : stream_;
    packet.control = nextControl_ + 1;
    AudioProtocol::buildControlPacket(packet.packet,
        starting ? AudioProtocol::PacketType::START : AudioProtocol::PacketType::END,
        starting ? 0 : sequence_);
    if (xQueueSend(packets_, &packet, 0) != pdTRUE) {
      ++producerOverflows_; return false;
    }
    intent_ = starting;
    stream_ = packet.stream;
    nextControl_ = lastControl_ = packet.control;
    pendingControl_ = true;
    if (starting) sequence_ = 0;
    noteDepth();
    wake();
    return true;
  }
  void noteDepth() {
    const size_t value = depth();
    if (value > producerHighWater_) producerHighWater_ = value;
  }
  QueueHandle_t packets_ = nullptr, snapshots_ = nullptr;
  TaskHandle_t owner_ = nullptr;
  std::atomic<uint32_t> pairingRequest_{0};
  mutable Snapshot cached_ = {};
  // Only the application task reads/writes these fields, including sequence
  // numbers and recording intent. A true result means accepted into the FIFO.
  bool intent_ = false, pendingControl_ = false;
  uint32_t producerEpoch_ = UINT32_MAX, sequence_ = 0, stream_ = 0;
  uint32_t nextControl_ = 0, lastControl_ = 0;
  uint32_t producerDrops_ = 0, producerOverflows_ = 0, producerEpochDrops_ = 0;
  size_t producerHighWater_ = 0;
};

#endif
