#ifndef ICS_TEST_CFG_H_
#define ICS_TEST_CFG_H_

#include <driver/i2s.h>
#include <stddef.h>
#include <stdint.h>

// 双麦硬件描述。左右麦克风使用独立 I2S 外设和独立时钟/数据网络。
namespace IcsTestConfig {

struct MicrophoneConfig {
  i2s_port_t port;
  int bclkPin;
  int wsPin;
  int dataPin;
  i2s_channel_fmt_t channelFormat;
  const char *name;
};

constexpr MicrophoneConfig LEFT_MIC = {
    I2S_NUM_0, 5, 18, 19, I2S_CHANNEL_FMT_ONLY_LEFT, "left"};
constexpr MicrophoneConfig RIGHT_MIC = {
    I2S_NUM_1, 4, 16, 17, I2S_CHANNEL_FMT_ONLY_RIGHT, "right"};

constexpr int USER_KEY = 13;
constexpr uint32_t KEY_DEBOUNCE_MS = 35;

// I2S 以 8 kHz 采集，应用层每两点保留一点，得到 4 kHz PCM。
constexpr uint32_t CAPTURE_SAMPLE_RATE = 48000;
constexpr size_t DECIMATION_FACTOR = 2;
constexpr uint32_t AUDIO_SAMPLE_RATE = CAPTURE_SAMPLE_RATE / DECIMATION_FACTOR;
constexpr uint8_t AUDIO_CHANNELS = 1;
constexpr uint8_t AUDIO_BITS_PER_SAMPLE = 16;
constexpr int AUDIO_RAW_SHIFT = 13;
constexpr int AUDIO_DC_BLOCK_SHIFT = 8;
constexpr size_t AUDIO_ANTI_ALIAS_TAPS = 5;

constexpr char BLE_DEVICE_NAME[] = "ESP32S3-ICS";
constexpr char BLE_SERVICE_UUID[] =
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char BLE_AUDIO_UUID[] =
    "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

constexpr uint16_t REQUESTED_MTU = 247;
constexpr size_t MAX_PACKET_FRAMES = 115;
constexpr size_t HEADER_BYTES = 14;
constexpr size_t MAX_PAYLOAD_BYTES =
    MAX_PACKET_FRAMES * AUDIO_CHANNELS * (AUDIO_BITS_PER_SAMPLE / 8);
constexpr size_t MAX_PACKET_BYTES = HEADER_BYTES + MAX_PAYLOAD_BYTES;

// 保持原草图的 BLE 参数：连接间隔 12.5 ms，监督超时 2 秒。
constexpr uint16_t CONNECTION_INTERVAL_MIN = 10;
constexpr uint16_t CONNECTION_INTERVAL_MAX = 10;
constexpr uint16_t CONNECTION_LATENCY = 0;
constexpr uint16_t CONNECTION_TIMEOUT = 200;  // 10 ms 单位，即 2 秒。

constexpr size_t PACKET_QUEUE_DEPTH = 4;
constexpr uint32_t NOTIFY_TIMEOUT_MS = 100;
constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 10;
constexpr uint8_t NOTIFY_MAX_ATTEMPTS = 3;

}  // namespace IcsTestConfig

static_assert(IcsTestConfig::MAX_PACKET_BYTES <= 244,
              "BLE packet does not fit ATT MTU 247");

#endif
