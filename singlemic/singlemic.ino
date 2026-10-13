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
 
 // Keep the microphone clock at 8 kHz, but decimate the BLE stream to 4 kHz.
 // This halves the BLE payload rate without changing the I2S pin/clock setup.
 static constexpr uint32_t I2S_CAPTURE_SAMPLE_RATE = 8000;
 static constexpr uint32_t AUDIO_SAMPLE_RATE = 4000;
 static constexpr size_t BLE_DECIMATION_FACTOR = 2;
 static constexpr uint8_t AUDIO_CHANNELS = 1;
 static constexpr uint8_t AUDIO_BITS_PER_SAMPLE = 16;
 // Reduce the I2S-to-PCM gain before the 16-bit clamp.  The previous value
 // produced substantial clipping on louder speech.
 static constexpr int AUDIO_RAW_SHIFT = 13;
 // First-order DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1], R=255/256.
 static constexpr int AUDIO_DC_BLOCK_SHIFT = 8;
 // Five-tap binomial FIR low-pass before 2:1 decimation.  The coefficients
 // [1, 4, 6, 4, 1] / 16 give a zero at the 8-kHz input Nyquist frequency and
 // about 12 dB attenuation at the 4-kHz output Nyquist frequency.
 static constexpr int AUDIO_ANTI_ALIAS_TAPS = 5;
 
 static constexpr char BLE_DEVICE_NAME[] = "ESP32S3-ICS";
 static constexpr char BLE_SERVICE_UUID[] =
     "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
 static constexpr char BLE_AUDIO_UUID[] =
     "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
 
 // ATT MTU 247 leaves 244 bytes for a notification. 14 bytes are header,
 // leaving 230 bytes = 115 mono 16-bit samples.
 static constexpr size_t PACKET_FRAMES = 115;
 static constexpr size_t I2S_FRAMES_PER_PACKET =
     PACKET_FRAMES * BLE_DECIMATION_FACTOR;
 static constexpr size_t HEADER_BYTES = 14;
 static constexpr size_t PAYLOAD_BYTES =
     PACKET_FRAMES * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
 static constexpr size_t PACKET_BYTES = HEADER_BYTES + PAYLOAD_BYTES;
 static_assert(PACKET_BYTES <= 244, "BLE packet does not fit MTU 247");
 
 static constexpr uint8_t PACKET_DATA = 1;
 static constexpr uint8_t PACKET_START = 2;
 static constexpr uint8_t PACKET_END = 3;
 static constexpr uint8_t MAGIC0 = 0xA5;
 static constexpr uint8_t MAGIC1 = 0x5A;
 static constexpr uint32_t KEY_DEBOUNCE_MS = 35;
 
 static NimBLECharacteristic *audioCharacteristic = nullptr;
 static volatile bool bleConnected = false;
 static volatile uint32_t notifyOk = 0;
 static volatile uint32_t notifyFailures = 0;
 static volatile uint32_t notifyTimeouts = 0;
 static uint32_t sequenceNumber = 0;
 static uint32_t packetsSent = 0;
 static uint32_t packetsDropped = 0;
 static uint32_t lastStatsMs = 0;
 static QueueHandle_t blePacketQueue = nullptr;
 static TaskHandle_t bleSenderTaskHandle = nullptr;
 static bool recording = false;
 
 struct BlePacket {
   uint16_t length;
   uint8_t bytes[PACKET_BYTES];
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
     sequenceNumber = 0;
     dcPreviousInput = 0;
     dcPreviousOutput = 0;
     for (int32_t &sample : antiAliasHistory) sample = 0;
     antiAliasHistoryPosition = 0;
     if (blePacketQueue != nullptr) {
       xQueueReset(blePacketQueue);
     }
     // 12.5 ms interval is a reasonable starting point for 16 kB/s PCM.
     server->updateConnParams(info.getConnHandle(), 10, 10, 0, 200);
     Serial.println("BLE client connected");
   }
 
   void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int reason) override {
     bleConnected = false;
     recording = false;
     Serial.printf("BLE client disconnected, reason=%d\n", reason);
     NimBLEDevice::getAdvertising()->start();
   }
 };
 
 class CharacteristicCallbacks final : public NimBLECharacteristicCallbacks {
  public:
   // NimBLE-Arduino 2.x reports the asynchronous notification result here.
   void onStatus(NimBLECharacteristic *, int code) override {
     if (code == 0) {
       ++notifyOk;
     } else {
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
     if (xQueueReceive(blePacketQueue, &packet, portMAX_DELAY) != pdTRUE) {
       continue;
     }
     if (!bleConnected || audioCharacteristic == nullptr) {
       ++packetsDropped;
       continue;
     }
 
     // Clear a stale completion notification before submitting this packet.
     ulTaskNotifyTake(pdTRUE, 0);
     audioCharacteristic->setValue(packet.bytes, packet.length);
     audioCharacteristic->notify();
 
     // Serialize notifications. This prevents the NimBLE host/controller
     // queue from accumulating packets faster than the central can consume.
     if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100)) == 0) {
       ++notifyTimeouts;
       ++packetsDropped;
       continue;
     }
     ++packetsSent;
     vTaskDelay(pdMS_TO_TICKS(2));
   }
 }
 
 static bool setupBle() {
   NimBLEDevice::init(BLE_DEVICE_NAME);
   NimBLEDevice::setMTU(247);
 
   NimBLEServer *server = NimBLEDevice::createServer();
   if (server == nullptr) return false;
   server->setCallbacks(new ServerCallbacks());
 
   NimBLEService *service = server->createService(BLE_SERVICE_UUID);
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
   blePacketQueue = xQueueCreate(4, sizeof(BlePacket));
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
   return xQueueSend(blePacketQueue, &packet, pdMS_TO_TICKS(100)) == pdTRUE;
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
   const size_t wanted = I2S_FRAMES_PER_PACKET * sizeof(int32_t);
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
 
 static void buildAudioPacket(const int32_t *rawSamples, BlePacket &packet) {
   packet.length = PACKET_BYTES;
   packet.bytes[0] = MAGIC0;
   packet.bytes[1] = MAGIC1;
   packet.bytes[2] = PACKET_DATA;
   packet.bytes[3] = AUDIO_CHANNELS;
   putLe16(packet.bytes + 4, static_cast<uint16_t>(AUDIO_SAMPLE_RATE));
   putLe16(packet.bytes + 6, static_cast<uint16_t>(PACKET_FRAMES));
   putLe32(packet.bytes + 8, sequenceNumber++);
   putLe16(packet.bytes + 12, static_cast<uint16_t>(PAYLOAD_BYTES));
 
   // Process every 8-kHz sample through the DC blocker and anti-alias filter,
   // then retain one filtered sample per two input samples for the 4-kHz BLE
   // stream.  The filter state is kept across packets.
   for (size_t i = 0; i < PACKET_FRAMES; ++i) {
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
   if (pressed(key2, now)) {
     if (recording) {
       stopRecording();
     } else {
       startRecording();
     }
   }
 
   static int32_t rawSamples[I2S_FRAMES_PER_PACKET];
   if (!readFrames(rawSamples)) {
     ++packetsDropped;
     delay(5);
     return;
   }
 
   if (recording && bleConnected && blePacketQueue != nullptr) {
     BlePacket packet;
     buildAudioPacket(rawSamples, packet);
     if (xQueueSend(blePacketQueue, &packet, 0) != pdTRUE) {
       ++packetsDropped;
     }
   }
 
   if (now - lastStatsMs >= 1000) {
     lastStatsMs = now;
     Serial.printf("BLE=%s recording=%s sent=%lu dropped=%lu notify_ok=%lu notify_fail=%lu timeout=%lu\n",
                   bleConnected ? "connected" : "waiting",
                   recording ? "yes" : "no",
                   static_cast<unsigned long>(packetsSent),
                   static_cast<unsigned long>(packetsDropped),
                   static_cast<unsigned long>(notifyOk),
                   static_cast<unsigned long>(notifyFailures),
                   static_cast<unsigned long>(notifyTimeouts));
   }
 }
 