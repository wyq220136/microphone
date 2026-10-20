/*
 * ICS-43434 I2S microphone -> BLE PCM streamer.
 *
 * Hardware defaults follow the local ICS-43434 breakout example:
 *   BCLK/SCK = GPIO5, WS/LRCLK = GPIO19, SD = GPIO18
 * Change the three constants below when using another board/wiring.
 *
 * The microphone produces a 24-bit signed sample in a 32-bit I2S slot.
 * AUDIO_RAW_SHIFT is deliberately a constant because different ESP32 core
 * versions and I2S alignment settings expose the left-justified word with
 * slightly different scaling.  The current value is intentionally reduced
 * from the original local example to leave headroom for louder speech.
 *
 * BLE packets use the same 14-byte little-endian header as the other local
 * microphone receiver:
 *   A5 5A | type | channels | sample_rate | frames | sequence | payload_bytes
 * The audio payload is signed little-endian 16-bit mono PCM.
 */

 #include <Arduino.h>
 #include <NimBLEDevice.h>
 #include <driver/i2s.h>
 
 // --------------------------- User configuration ---------------------------
 static constexpr int I2S_BCLK_PIN = 4;
 static constexpr int I2S_WS_PIN = 16;
 static constexpr int I2S_DATA_PIN = 17;
 static constexpr i2s_port_t I2S_PORT = I2S_NUM_1;
 
 static constexpr int USER_KEY_2 = 13;
 
 // Keep the microphone clock at 24 kHz and decimate the BLE stream to 12 kHz.
 // This keeps the I2S capture rate high while halving the BLE payload rate.
 static constexpr uint32_t I2S_CAPTURE_SAMPLE_RATE = 24000;
 static constexpr size_t BLE_DECIMATION_FACTOR = 2;
 static constexpr uint32_t AUDIO_SAMPLE_RATE =
     I2S_CAPTURE_SAMPLE_RATE / BLE_DECIMATION_FACTOR;
 static constexpr uint8_t AUDIO_CHANNELS = 1;
 static constexpr uint8_t AUDIO_BITS_PER_SAMPLE = 16;
 // Reduce the I2S-to-PCM gain before the 16-bit clamp.  The previous value
 // produced substantial clipping on louder speech.
 static constexpr int AUDIO_RAW_SHIFT = 13;
 // First-order DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1], R=255/256.
 static constexpr int AUDIO_DC_BLOCK_SHIFT = 8;
 // Five-tap binomial FIR low-pass before 2:1 decimation.  The coefficients
 // [1, 4, 6, 4, 1] / 16 give a zero at the 12-kHz input Nyquist frequency and
 // about 12 dB attenuation at the 6-kHz output Nyquist frequency.
 static constexpr int AUDIO_ANTI_ALIAS_TAPS = 5;
 
 static constexpr char BLE_DEVICE_NAME[] = "ESP32S3-ICS";
 static constexpr char BLE_SERVICE_UUID[] =
     "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
 static constexpr char BLE_AUDIO_UUID[] =
     "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
 
 // The packet buffer supports ATT MTU 247; each connection may use fewer
 // frames per notification when the central negotiates a smaller MTU.
 static constexpr size_t MAX_PACKET_FRAMES = 115;
 static constexpr size_t HEADER_BYTES = 14;
 static constexpr size_t MAX_PAYLOAD_BYTES =
     MAX_PACKET_FRAMES * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
 static constexpr size_t MAX_PACKET_BYTES = HEADER_BYTES + MAX_PAYLOAD_BYTES;
 static_assert(MAX_PACKET_BYTES <= 244, "BLE packet does not fit MTU 247");
 
 static constexpr uint8_t PACKET_DATA = 1;
 static constexpr uint8_t PACKET_START = 2;
 static constexpr uint8_t PACKET_END = 3;
 static constexpr uint8_t MAGIC0 = 0xA5;
 static constexpr uint8_t MAGIC1 = 0x5A;
 static constexpr uint32_t KEY_DEBOUNCE_MS = 35;
 // BLE connection parameters use 1.25 ms units. 7.5 ms gives the controller
 // enough events to carry the 12 kHz mono stream without adding a fixed delay
 // between notifications.
 static constexpr uint16_t BLE_CONNECTION_INTERVAL_MIN = 6;
 static constexpr uint16_t BLE_CONNECTION_INTERVAL_MAX = 6;
 static constexpr uint16_t BLE_CONNECTION_LATENCY = 0;
 static constexpr uint16_t BLE_CONNECTION_TIMEOUT = 200;
 static constexpr size_t BLE_PACKET_QUEUE_DEPTH = 64;
 static constexpr uint32_t BLE_NOTIFY_TIMEOUT_MS = 100;
 static constexpr uint32_t BLE_NOTIFY_RETRY_DELAY_MS = 10;
 static constexpr uint8_t BLE_NOTIFY_MAX_ATTEMPTS = 3;
 
 static NimBLECharacteristic *audioCharacteristic = nullptr;
 static NimBLEServer *bleServer = nullptr;
 static volatile bool bleConnected = false;
 static volatile bool lastNotifySucceeded = false;
 static volatile size_t packetFrames = 0;
 static constexpr uint16_t INVALID_CONN_HANDLE = 0xffff;
 static uint16_t bleConnHandle = INVALID_CONN_HANDLE;
 static volatile uint32_t notifyOk = 0;
 static volatile uint32_t notifyFailures = 0;
 static volatile uint32_t notifyTimeouts = 0;
 static volatile uint32_t notifyRetries = 0;
 static uint32_t sequenceNumber = 0;
 static volatile uint32_t packetsSent = 0;
 static volatile uint32_t packetsDropped = 0;
 static volatile uint32_t queueOverflows = 0;
 static volatile uint32_t queueHighWater = 0;
 static volatile uint16_t negotiatedMtu = 0;
 static volatile uint16_t negotiatedInterval = 0;
 static uint32_t lastStatsMs = 0;
 static QueueHandle_t blePacketQueue = nullptr;
 static TaskHandle_t bleSenderTaskHandle = nullptr;
 static bool recording = false;

 enum class BleSenderState : uint8_t {
   DISCONNECTED,
   WAITING_PACKET,
   WAITING_RESULT,
   RETRY_WAIT,
   DROPPED,
 };

 static volatile BleSenderState senderState = BleSenderState::DISCONNECTED;

 static const char *senderStateName(BleSenderState state) {
   switch (state) {
     case BleSenderState::DISCONNECTED: return "disconnected";
     case BleSenderState::WAITING_PACKET: return "waiting_packet";
     case BleSenderState::WAITING_RESULT: return "waiting_result";
     case BleSenderState::RETRY_WAIT: return "retry_wait";
     case BleSenderState::DROPPED: return "dropped";
   }
   return "unknown";
 }

 static void updateQueueHighWater() {
   if (blePacketQueue == nullptr) return;
   const UBaseType_t depth = uxQueueMessagesWaiting(blePacketQueue);
   if (depth > queueHighWater) {
     queueHighWater = depth;
   }
 }
 
 struct BlePacket {
   uint16_t length;
   uint8_t bytes[MAX_PACKET_BYTES];
 };
 
 struct KeyState {
   int pin;
   bool stableHigh;
   bool rawHigh;
   uint32_t changedAt;
 };
 
 static KeyState key2 = {USER_KEY_2, true, true, 0};
 
 static int32_t dcPreviousInput = 0;
 static int32_t dcPreviousOutput = 0;
 static int32_t antiAliasHistory[AUDIO_ANTI_ALIAS_TAPS] = {0, 0, 0, 0, 0};
 static size_t antiAliasHistoryPosition = 0;

 static size_t framesForMtu(uint16_t mtu);
 
 static void putLe16(uint8_t *dst, uint16_t value) {
   dst[0] = static_cast<uint8_t>(value & 0xffu);
   dst[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
 }
 
 static void putLe32(uint8_t *dst, uint32_t value) {
   dst[0] = static_cast<uint8_t>(value & 0xffu);
   dst[1] = static_cast<uint8_t>((value >> 8) & 0xffu);
   dst[2] = static_cast<uint8_t>((value >> 16) & 0xffu);
   dst[3] = static_cast<uint8_t>((value >> 24) & 0xffu);
 }
 
 static int16_t rawToPcm16(int32_t raw) {
   const int32_t scaled = raw >> AUDIO_RAW_SHIFT;
   const int32_t filtered =
       scaled - dcPreviousInput +
       (dcPreviousOutput - (dcPreviousOutput >> AUDIO_DC_BLOCK_SHIFT));
   dcPreviousInput = scaled;
   dcPreviousOutput = filtered;
 
   int32_t value = filtered;
   if (value > 32767) value = 32767;
   if (value < -32768) value = -32768;
   return static_cast<int16_t>(value);
 }
 
 static int16_t antiAliasFilter(int16_t input) {
   antiAliasHistory[antiAliasHistoryPosition] = input;
   antiAliasHistoryPosition =
       (antiAliasHistoryPosition + 1) % AUDIO_ANTI_ALIAS_TAPS;
 
   // antiAliasHistoryPosition now points to the oldest sample.  Sum oldest to
   // newest with the symmetric binomial coefficients.
   const size_t p = antiAliasHistoryPosition;
   const int32_t sum = antiAliasHistory[p] +
                       4 * antiAliasHistory[(p + 1) % AUDIO_ANTI_ALIAS_TAPS] +
                       6 * antiAliasHistory[(p + 2) % AUDIO_ANTI_ALIAS_TAPS] +
                       4 * antiAliasHistory[(p + 3) % AUDIO_ANTI_ALIAS_TAPS] +
                       antiAliasHistory[(p + 4) % AUDIO_ANTI_ALIAS_TAPS];
   int32_t value = sum / 16;
   if (value > 32767) value = 32767;
   if (value < -32768) value = -32768;
   return static_cast<int16_t>(value);
 }
 
 class ServerCallbacks final : public NimBLEServerCallbacks {
  public:
   void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
     bleConnected = true;
     bleConnHandle = info.getConnHandle();
     negotiatedMtu = info.getMTU();
     negotiatedInterval = info.getConnInterval();
     packetFrames = framesForMtu(negotiatedMtu);
     sequenceNumber = 0;
     dcPreviousInput = 0;
     dcPreviousOutput = 0;
     for (int32_t &sample : antiAliasHistory) sample = 0;
     antiAliasHistoryPosition = 0;
     if (blePacketQueue != nullptr) {
       xQueueReset(blePacketQueue);
     }
     queueHighWater = 0;
     server->updateConnParams(info.getConnHandle(),
                              BLE_CONNECTION_INTERVAL_MIN,
                              BLE_CONNECTION_INTERVAL_MAX,
                              BLE_CONNECTION_LATENCY,
                              BLE_CONNECTION_TIMEOUT);
     Serial.printf("BLE client connected: MTU=%u, frames/packet=%u, interval=%.2f ms; requested 7.50 ms\n",
                   negotiatedMtu, static_cast<unsigned>(packetFrames),
                   negotiatedInterval * 1.25f);
   }

   void onMTUChange(uint16_t mtu, NimBLEConnInfo &info) override {
     negotiatedMtu = mtu;
     negotiatedInterval = info.getConnInterval();
     packetFrames = framesForMtu(mtu);
     Serial.printf("BLE MTU changed: MTU=%u, frames/packet=%u, interval=%.2f ms\n",
                   mtu, static_cast<unsigned>(packetFrames),
                   negotiatedInterval * 1.25f);
     if (packetFrames == 0) {
       Serial.println("BLE MTU too small for the audio header and one sample");
     }
   }

   void onConnParamsUpdate(NimBLEConnInfo &info) override {
     negotiatedInterval = info.getConnInterval();
     Serial.printf("BLE params updated: interval=%.2f ms latency=%u timeout=%u ms\n",
                   info.getConnInterval() * 1.25f, info.getConnLatency(),
                   info.getConnTimeout() * 10);
   }
 
   void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
     bleConnected = false;
     bleConnHandle = INVALID_CONN_HANDLE;
     packetFrames = 0;
     recording = false;
     if (blePacketQueue != nullptr) {
       xQueueReset(blePacketQueue);
     }
     if (bleSenderTaskHandle != nullptr) {
       xTaskNotifyGive(bleSenderTaskHandle);
     }
     Serial.printf("BLE client disconnected, reason=%d\n", reason);
     NimBLEDevice::getAdvertising()->start();
   }
 };

 static size_t framesForMtu(uint16_t mtu) {
   const size_t mtuPayload = mtu > 3 ? mtu - 3 : 0;
   const size_t usablePayload = mtuPayload > HEADER_BYTES
                                    ? mtuPayload - HEADER_BYTES
                                    : 0;
   size_t frames = usablePayload /
                   (AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8));
   if (frames > MAX_PACKET_FRAMES) {
     frames = MAX_PACKET_FRAMES;
   }
   return frames;
 }

 static void refreshNegotiatedMtu() {
   if (!bleConnected || bleServer == nullptr ||
       bleConnHandle == INVALID_CONN_HANDLE) {
     return;
   }
   const uint16_t mtu = bleServer->getPeerMTU(bleConnHandle);
   const size_t frames = framesForMtu(mtu);
   if (mtu != negotiatedMtu || frames != packetFrames) {
     negotiatedMtu = mtu;
     packetFrames = frames;
     Serial.printf("BLE negotiated MTU=%u, frames/packet=%u, interval=%.2f ms\n",
                   mtu, static_cast<unsigned>(frames),
                   negotiatedInterval * 1.25f);
     if (frames == 0) {
       Serial.println("BLE MTU too small for the audio header and one sample");
     }
   }
 }
 
 class CharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
   // NimBLE-Arduino 2.x reports the asynchronous notification result here.
   void onStatus(NimBLECharacteristic *, int code) override {
     if (code == 0) {
       lastNotifySucceeded = true;
       ++notifyOk;
     } else {
       lastNotifySucceeded = false;
       ++notifyFailures;
     }
     if (bleSenderTaskHandle != nullptr) {
       xTaskNotifyGive(bleSenderTaskHandle);
     }
   }
 };
 
 static void bleSenderTask(void *) {
   BlePacket packet;
   while (true) {
     senderState = bleConnected ? BleSenderState::WAITING_PACKET
                                : BleSenderState::DISCONNECTED;
     if (xQueueReceive(blePacketQueue, &packet, pdMS_TO_TICKS(100)) != pdTRUE) {
       continue;
     }
     updateQueueHighWater();
     if (!bleConnected || audioCharacteristic == nullptr) {
       senderState = BleSenderState::DROPPED;
       ++packetsDropped;
       continue;
     }
 
     bool delivered = false;
     for (uint8_t attempt = 0; attempt < BLE_NOTIFY_MAX_ATTEMPTS; ++attempt) {
       if (!bleConnected || audioCharacteristic == nullptr) {
         break;
       }

       if (attempt != 0) {
         senderState = BleSenderState::RETRY_WAIT;
         vTaskDelay(pdMS_TO_TICKS(BLE_NOTIFY_RETRY_DELAY_MS));
         if (!bleConnected) break;
         ++notifyRetries;
       }

       // Clear a stale completion notification before submitting this packet.
       ulTaskNotifyTake(pdTRUE, 0);
       lastNotifySucceeded = false;
       senderState = BleSenderState::WAITING_RESULT;
       audioCharacteristic->setValue(packet.bytes, packet.length);
       if (!audioCharacteristic->notify()) {
         ++notifyFailures;
         continue;
       }

       // Keep one notification in flight so retries cannot reorder packets.
       if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(BLE_NOTIFY_TIMEOUT_MS)) == 0) {
         ++notifyTimeouts;
         continue;
       }
       if (lastNotifySucceeded) {
         delivered = true;
         break;
       }
     }

     if (delivered) {
       ++packetsSent;
     } else {
       senderState = BleSenderState::DROPPED;
       ++packetsDropped;
     }
   }
 }
 
 static bool setupBle() {
   NimBLEDevice::init(BLE_DEVICE_NAME);
   NimBLEDevice::setMTU(247);
 
   bleServer = NimBLEDevice::createServer();
   if (bleServer == nullptr) return false;
   bleServer->setCallbacks(new ServerCallbacks());
 
   NimBLEService *service = bleServer->createService(BLE_SERVICE_UUID);
   if (service == nullptr) return false;
   audioCharacteristic = service->createCharacteristic(
       BLE_AUDIO_UUID, NIMBLE_PROPERTY::NOTIFY);
   if (audioCharacteristic == nullptr) return false;
   audioCharacteristic->setCallbacks(new CharacteristicCallbacks());
   service->start();
 
   NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
   advertising->addServiceUUID(BLE_SERVICE_UUID);
   advertising->setName(BLE_DEVICE_NAME);
   advertising->start();
   blePacketQueue = xQueueCreate(BLE_PACKET_QUEUE_DEPTH, sizeof(BlePacket));
   if (blePacketQueue == nullptr) return false;
   const BaseType_t taskResult = xTaskCreatePinnedToCore(
       bleSenderTask, "BLE audio sender", 4096, nullptr, 2,
       &bleSenderTaskHandle, 0);
   if (taskResult != pdPASS) return false;
   Serial.printf("BLE advertising: %s\n", BLE_DEVICE_NAME);
   return true;
 }
 
 static bool enqueueControlPacket(uint8_t packetType) {
   if (!bleConnected || blePacketQueue == nullptr) {
     return false;
   }
 
   BlePacket packet = {};
   packet.length = HEADER_BYTES;
   packet.bytes[0] = MAGIC0;
   packet.bytes[1] = MAGIC1;
   packet.bytes[2] = packetType;
   packet.bytes[3] = AUDIO_CHANNELS;
   putLe16(packet.bytes + 4, static_cast<uint16_t>(AUDIO_SAMPLE_RATE));
   putLe16(packet.bytes + 6, 0);
   putLe32(packet.bytes + 8, sequenceNumber);
   putLe16(packet.bytes + 12, 0);
   // Give the sender task time to drain one in-flight audio packet. This is
   // especially important for END, which must reach the host to trigger WAV
   // saving even when the audio queue is briefly full.
   const bool queued =
       xQueueSend(blePacketQueue, &packet, pdMS_TO_TICKS(100)) == pdTRUE;
   if (queued) updateQueueHighWater();
   return queued;
 }
 
 static void resetAudioPipeline() {
   dcPreviousInput = 0;
   dcPreviousOutput = 0;
   for (int32_t &sample : antiAliasHistory) sample = 0;
   antiAliasHistoryPosition = 0;
   sequenceNumber = 0;
   if (blePacketQueue != nullptr) {
     xQueueReset(blePacketQueue);
   }
 }
 
 static void startRecording() {
   if (recording || !bleConnected) {
     return;
   }
   resetAudioPipeline();
   if (enqueueControlPacket(PACKET_START)) {
     recording = true;
     Serial.println("Recording started");
   } else {
     Serial.println("Recording start packet dropped");
   }
 }
 
 static void stopRecording() {
   if (!recording) {
     return;
   }
   // START, audio, and END share one FIFO, so END follows all audio already
   // queued when the key is pressed.
   if (enqueueControlPacket(PACKET_END)) {
     recording = false;
     Serial.println("Recording stopped");
   } else {
     Serial.println("Recording stop packet dropped");
   }
 }
 
 static bool pressed(KeyState &key, uint32_t now) {
   const bool high = digitalRead(key.pin) == HIGH;
   if (high != key.rawHigh) {
     key.rawHigh = high;
     key.changedAt = now;
   }
   if (now - key.changedAt < KEY_DEBOUNCE_MS || high == key.stableHigh) {
     return false;
   }
   const bool wasHigh = key.stableHigh;
   key.stableHigh = high;
   return wasHigh && !high;
 }
 
 static bool setupI2s() {
   i2s_config_t config = {};
   config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
   config.sample_rate = I2S_CAPTURE_SAMPLE_RATE;
   config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
   config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
   config.communication_format = I2S_COMM_FORMAT_I2S;
   config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
   config.dma_buf_count = 8;
   config.dma_buf_len = 256;
   config.use_apll = false;
   config.tx_desc_auto_clear = false;
   config.fixed_mclk = 0;
 
   esp_err_t err = i2s_driver_install(I2S_PORT, &config, 0, nullptr);
   if (err != ESP_OK) {
     Serial.printf("I2S driver install failed: %d\n", err);
     return false;
   }
 
   i2s_pin_config_t pins = {};
   pins.bck_io_num = I2S_BCLK_PIN;
   pins.ws_io_num = I2S_WS_PIN;
   pins.data_out_num = I2S_PIN_NO_CHANGE;
   pins.data_in_num = I2S_DATA_PIN;
   err = i2s_set_pin(I2S_PORT, &pins);
   if (err != ESP_OK) {
     Serial.printf("I2S pin setup failed: %d\n", err);
     i2s_driver_uninstall(I2S_PORT);
     return false;
   }
   i2s_zero_dma_buffer(I2S_PORT);
   Serial.printf("I2S ready: %lu Hz, BLE output: %lu Hz, BCLK=%d WS=%d SD=%d\n",
                 static_cast<unsigned long>(I2S_CAPTURE_SAMPLE_RATE),
                 static_cast<unsigned long>(AUDIO_SAMPLE_RATE), I2S_BCLK_PIN,
                 I2S_WS_PIN, I2S_DATA_PIN);
   return true;
 }
 
 static bool readFrames(int32_t *rawSamples) {
   size_t received = 0;
   const size_t wanted = packetFrames * BLE_DECIMATION_FACTOR * sizeof(int32_t);
   while (received < wanted) {
     size_t chunk = 0;
     const esp_err_t err = i2s_read(I2S_PORT,
                                    reinterpret_cast<uint8_t *>(rawSamples) + received,
                                    wanted - received, &chunk, pdMS_TO_TICKS(100));
     if (err != ESP_OK || chunk == 0) {
       Serial.printf("I2S read failed: err=%d bytes=%u\n", static_cast<int>(err),
                     static_cast<unsigned>(chunk));
       return false;
     }
     received += chunk;
   }
   return true;
 }
 
 static void buildAudioPacket(const int32_t *rawSamples, BlePacket &packet,
                              size_t frames) {
   const size_t payloadBytes =
       frames * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
   packet.length = HEADER_BYTES + payloadBytes;
   packet.bytes[0] = MAGIC0;
   packet.bytes[1] = MAGIC1;
   packet.bytes[2] = PACKET_DATA;
   packet.bytes[3] = AUDIO_CHANNELS;
   putLe16(packet.bytes + 4, static_cast<uint16_t>(AUDIO_SAMPLE_RATE));
   putLe16(packet.bytes + 6, static_cast<uint16_t>(frames));
   putLe32(packet.bytes + 8, sequenceNumber++);
   putLe16(packet.bytes + 12, static_cast<uint16_t>(payloadBytes));
 
   // Process every 24-kHz sample through the DC blocker and anti-alias filter,
   // then retain one filtered sample per two input samples for the 12-kHz BLE
   // stream.  The filter state is kept across packets.
   for (size_t i = 0; i < frames; ++i) {
     const size_t inputIndex = i * BLE_DECIMATION_FACTOR;
     (void)antiAliasFilter(rawToPcm16(rawSamples[inputIndex]));
     const int16_t sample =
         antiAliasFilter(rawToPcm16(rawSamples[inputIndex + 1]));
     const uint16_t wireSample = static_cast<uint16_t>(sample);
     packet.bytes[HEADER_BYTES + 2 * i] =
         static_cast<uint8_t>(wireSample & 0xffu);
     packet.bytes[HEADER_BYTES + 2 * i + 1] =
         static_cast<uint8_t>((wireSample >> 8) & 0xffu);
   }
 }
 
 void setup() {
   Serial.begin(115200);
   delay(300);
   Serial.println("ICS-43434 BLE microphone streamer");
   pinMode(USER_KEY_2, INPUT_PULLUP);
   const uint32_t now = millis();
   key2.stableHigh = key2.rawHigh = digitalRead(USER_KEY_2) == HIGH;
   key2.changedAt = now;
   Serial.printf("PCM: I2S %lu Hz -> BLE/WAV %lu Hz mono 16-bit, raw shift=%d\n",
                 static_cast<unsigned long>(I2S_CAPTURE_SAMPLE_RATE),
                 static_cast<unsigned long>(AUDIO_SAMPLE_RATE), AUDIO_RAW_SHIFT);
 
   if (!setupI2s() || !setupBle()) {
     Serial.println("Fatal initialization error; halting");
     while (true) delay(1000);
   }
 }
 
 void loop() {
   const uint32_t now = millis();
   refreshNegotiatedMtu();

   if (now - lastStatsMs >= 1000) {
     lastStatsMs = now;
     const UBaseType_t queueDepth = blePacketQueue == nullptr
                                        ? 0
                                        : uxQueueMessagesWaiting(blePacketQueue);
     if (queueDepth > queueHighWater) queueHighWater = queueDepth;
     Serial.printf(
         "BLE=%s recording=%s state=%s mtu=%u frames=%u interval=%.2fms "
         "queue=%u high=%u/%u sent=%lu dropped=%lu queue_full=%lu retries=%lu "
         "notify_ok=%lu notify_fail=%lu timeout=%lu\n",
         bleConnected ? "connected" : "waiting", recording ? "yes" : "no",
         senderStateName(senderState), negotiatedMtu,
         static_cast<unsigned>(packetFrames), negotiatedInterval * 1.25f,
         static_cast<unsigned>(queueDepth),
         static_cast<unsigned>(queueHighWater),
         static_cast<unsigned>(BLE_PACKET_QUEUE_DEPTH),
         static_cast<unsigned long>(packetsSent),
         static_cast<unsigned long>(packetsDropped),
         static_cast<unsigned long>(queueOverflows),
         static_cast<unsigned long>(notifyRetries),
         static_cast<unsigned long>(notifyOk),
         static_cast<unsigned long>(notifyFailures),
         static_cast<unsigned long>(notifyTimeouts));
   }

   if (packetFrames == 0) {
     delay(1);
     return;
   }
   if (pressed(key2, now)) {
     if (recording) {
       stopRecording();
     } else {
       startRecording();
     }
   }
 
   static int32_t rawSamples[MAX_PACKET_FRAMES * BLE_DECIMATION_FACTOR];
   if (!readFrames(rawSamples)) {
     ++packetsDropped;
     delay(5);
     return;
   }
 
   if (recording && bleConnected && blePacketQueue != nullptr) {
     BlePacket packet;
     buildAudioPacket(rawSamples, packet, packetFrames);
     if (xQueueSend(blePacketQueue, &packet, 0) != pdTRUE) {
       ++packetsDropped;
       ++queueOverflows;
     } else {
       updateQueueHighWater();
     }
   }
 }
 
