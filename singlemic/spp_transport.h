#ifndef SINGLEMIC_SPP_TRANSPORT_H_
#define SINGLEMIC_SPP_TRANSPORT_H_
#include <esp_gap_bt_api.h>
#include <esp_spp_api.h>
#include "transport_channel.h"
#include "transport_diagnostics.h"

#if !defined(CONFIG_IDF_TARGET_ESP32) || !defined(CONFIG_BT_SPP_ENABLED)
#error "Classic SPP requires the classic ESP32 target with Bluedroid SPP"
#endif

enum class SppSenderState : uint8_t {
  DISCONNECTED, WAITING_PACKET, WAITING_CONGESTION, WRITING, CONTROL_RETRY,
  DROPPED, FAULT,
};
struct SppTransportStats {
  uint32_t packetsSent, packetsDropped, queueOverflows;
  uint32_t writeSubmissions, writeEvents, writeFailures, writeTimeouts;
  uint32_t congestionEvents, disconnects, epochDrops, bytesQueued;
  uint32_t eventOverflows, staleEvents, controlFailures, diagnosticFaults;
  uint32_t lastWriteEventMs, lastCongestionEventMs, waitMs;
  uint32_t lastCloseMs, lastClosePortStatus;
  int lastWriteStatus, lastCloseStatus;
  bool lastCloseAsync;
  size_t packetFrames, queueDepth, queueHighWater, senderStackMinFree;
  uint32_t txEpoch;
  bool congested, pairingMode;
  SppSenderState senderState;
};

class SppTransport {
 public:
  bool begin();
  bool setPairingMode(bool enabled) { return channel_.setPairingMode(enabled); }
  bool connected() const { return channel_.connected(); }
  bool subscribed() const { return channel_.subscribed(); }
  bool streamActive() const { return channel_.streamActive(); }
  size_t packetFrames() const { return channel_.packetFrames(); }
  bool startStream() { return channel_.startStream(); }
  bool stopStream() { return channel_.stopStream(); }
  bool enqueueAudio(const int32_t *samples, size_t frames) { return channel_.enqueueAudio(samples, frames); }
  void recordCaptureFailure() { channel_.recordCaptureFailure(); }
  SppTransportStats stats() const { return channel_.stats(); }
  void printDiagnostics() { diagnostics_.print("SPP"); }

 private:
  struct Event {
    int type = 0, status = 0, length = 0;
    uint32_t timeMs = 0, handle = 0, detail = 0;
    bool flag = false;
    esp_bd_addr_t address = {};
  };
  enum { TRACE_SUBMIT = 100, TRACE_TIMEOUT, TRACE_SCAN, TRACE_CONTROL,
         TRACE_EXTRA_CLIENT, TRACE_AUTH, TRACE_EVENT_OVERFLOW,
         GAP_CONFIRM = 200, GAP_AUTH };
  static void sppCallback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param);
  static void gapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param);
  static void senderTask(void *context);
  void postEvent(const Event &event);
  void processEvent(const Event &event);
  void step();
  void publish();
  void runSenderTask();
  void trace(int kind, uint32_t handle, int status, int length,
             uint32_t detail, bool fault = false, uint32_t timeMs = 0);
  void knownFailure();

  TransportChannel<SppTransportStats> channel_;
  TransportDiagnostics diagnostics_;
  QueueHandle_t eventQueue_ = nullptr;
  TaskHandle_t senderTaskHandle_ = nullptr;
  // Callback overflow signalling only; lifecycle state below has one writer.
  std::atomic<uint32_t> eventOverflows_{0};
  uint32_t seenOverflows_ = 0;
  bool connected_ = false, congested_ = false, pairingMode_ = false;
  bool scanDirty_ = false, serviceReady_ = false, initFailed_ = false;
  bool controlFault_ = false, eventFault_ = false, recoveryCloseSeen_ = false;
  bool hasPacket_ = false, writeInFlight_ = false, timeoutReported_ = false;
  bool retryPending_ = false, anonymousUncertain_ = false;
  uint32_t connectionHandle_ = 0, txEpoch_ = 0, activeStream_ = 0;
  uint32_t completedControl_ = 0, writeStartedMs_ = 0, retryStartedMs_ = 0;
  uint32_t lastScanAttemptMs_ = 0;
  TransportPacket currentPacket_;
  SppTransportStats counters_ = {};
  static SppTransport *activeInstance_;
};
#endif
