#ifndef SINGLEMIC_CFG_H_
#define SINGLEMIC_CFG_H_

#include <driver/i2s.h>
#include <stddef.h>
#include <stdint.h>

// I2S 硬件参数：麦克风时钟、数据线、端口和原始采样速率。
namespace I2SConfig {
constexpr int BCLK_PIN = 4;
constexpr int WS_PIN = 16;
constexpr int DATA_PIN = 17;
constexpr i2s_port_t PORT = I2S_NUM_1;
constexpr uint32_t CAPTURE_SAMPLE_RATE = 24000;
constexpr size_t DMA_BUF_COUNT = 8;
constexpr size_t DMA_BUF_LENGTH = 256;
}

// 板级输入输出配置。USER_KEY_2 当前用于开始/停止录音。
namespace IOConfig {
constexpr int USER_KEY_1 = 12;
constexpr int USER_KEY_2 = 13;
constexpr uint32_t KEY_DEBOUNCE_MS = 35;
}

// 音频格式及 DSP 参数。BLE 输出采样率由 I2S 采样率和降采样倍率决定。
namespace AudioConfig {
constexpr size_t DECIMATION_FACTOR = 2;
constexpr uint32_t SAMPLE_RATE =
    I2SConfig::CAPTURE_SAMPLE_RATE / DECIMATION_FACTOR;
constexpr uint8_t CHANNELS = 1;
constexpr uint8_t BITS_PER_SAMPLE = 16;
constexpr int RAW_SHIFT = 13;
constexpr int DC_BLOCK_SHIFT = 8;
constexpr size_t ANTI_ALIAS_TAPS = 5;
}

// BLE 服务、线协议包尺寸、连接协商和应用层队列参数。
namespace BLEConfig {
constexpr char DEVICE_NAME[] = "ESP32S3-ICS";
constexpr char SERVICE_UUID[] =
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char AUDIO_UUID[] =
    "6e400003-b5a3-f393-e0a9-e50e24dcca9e";

// 请求的 ATT MTU；中心端实际协商值可能较小，包帧数会据此动态计算。
constexpr uint16_t REQUESTED_MTU = 247;
// MTU 247 下的最大单声道 16 位帧数：(247 - ATT 3 字节 - 头部 14 字节) / 2。
constexpr size_t MAX_PACKET_FRAMES = 115;
// 数据包固定头长度，布局定义见 README 和 packet_protocol.cpp。
constexpr size_t HEADER_BYTES = 14;
constexpr size_t MAX_PAYLOAD_BYTES =
    MAX_PACKET_FRAMES * AudioConfig::CHANNELS *
    (AudioConfig::BITS_PER_SAMPLE / 8);
constexpr size_t MAX_PACKET_BYTES = HEADER_BYTES + MAX_PAYLOAD_BYTES;

// Let the central choose a practical interval for sustained audio streaming.
// The wider 15-22.5 ms range gives Windows BLE adapters room to schedule
// multiple notifications per connection event instead of forcing 12.5 ms.
constexpr uint16_t CONNECTION_INTERVAL_MIN = 12;
constexpr uint16_t CONNECTION_INTERVAL_MAX = 18;
constexpr uint16_t CONNECTION_LATENCY = 0;
// BLE units are 10 ms; 400 = 4 seconds. This tolerates brief host scheduling
// pauses without treating them as a dead link.
constexpr uint16_t CONNECTION_TIMEOUT = 400;
// FreeRTOS 应用层包队列深度；只吸收短暂积压，不代表长期吞吐上限。
constexpr size_t PACKET_QUEUE_DEPTH = 64;
// 单次通知的异步状态等待上限，以及失败后的间隔和最多发送次数。
constexpr uint32_t NOTIFY_TIMEOUT_MS = 100;
constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 10;
constexpr uint8_t NOTIFY_MAX_ATTEMPTS = 3;
}

static_assert(BLEConfig::MAX_PACKET_BYTES <= 244,
              "BLE packet does not fit ATT MTU 247");

#endif
