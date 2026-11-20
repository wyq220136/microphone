#ifndef SINGLEMIC_CFG_H_
#define SINGLEMIC_CFG_H_

#include <driver/i2s_std.h>
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

#ifndef SINGLEMIC_ENABLE_GTCRN
#define SINGLEMIC_ENABLE_GTCRN 1
#endif

namespace DenoiseConfig {
constexpr size_t FFT_SIZE = 512;
constexpr size_t HOP_SIZE = 256;
constexpr size_t POOL_BLOCKS = 8;
constexpr size_t BACKLOG_LIMIT = 4;
constexpr uint32_t MAX_BLOCK_AGE_US = 128000;
constexpr uint32_t INTERNAL_RESERVE = 48 * 1024;
constexpr uint32_t CAPTURE_STACK = 4096;
constexpr uint32_t INFERENCE_STACK = 8192;
constexpr uint32_t WARMUP_FRAMES = 32;
constexpr uint32_t BENCHMARK_FRAMES = 128;
}

// 音频输入：0=真实I2S麦克风采样（默认）；1=固定PCM24循环隔离测试。
// 测试模式不初始化/读取I2S、不执行麦克风DSP；按键、压缩、CRC和蓝牙保持原流程。
#ifndef SINGLEMIC_FIXED_PCM_TEST
#define SINGLEMIC_FIXED_PCM_TEST 0
#endif
#if SINGLEMIC_FIXED_PCM_TEST != 0 && SINGLEMIC_FIXED_PCM_TEST != 1
#error "SINGLEMIC_FIXED_PCM_TEST must be 0 or 1"
#endif

// 编码隔离测试：0=直接发送原始PCM24；1=Huffman压缩，变大时回退PCM24。
// 当前关闭Huffman；仅改变编码，不改变采样、DSP、包头或CRC。
#ifndef SINGLEMIC_ENABLE_HUFFMAN
#define SINGLEMIC_ENABLE_HUFFMAN 0
#endif
#if SINGLEMIC_ENABLE_HUFFMAN != 0 && SINGLEMIC_ENABLE_HUFFMAN != 1
#error "SINGLEMIC_ENABLE_HUFFMAN must be 0 or 1"
#endif

namespace I2SConfig {
// 板载另一组麦克风：SCK/BCLK=GPIO5，WS=GPIO18，DATA=GPIO19。
constexpr int BCLK_PIN = 5;
constexpr int WS_PIN = 18;
constexpr int DATA_PIN = 19;
constexpr int PORT = I2S_NUM_1;
constexpr uint32_t CAPTURE_SAMPLE_RATE = 16000;
constexpr size_t DMA_BUF_COUNT = 8;
constexpr size_t DMA_BUF_LENGTH = 256;
// Bound a stalled DMA read without being shorter than one normal DMA period.
constexpr uint32_t READ_TIMEOUT_MS = 25;
}

namespace DenoiseConfig {
// Space still needed after model preflight: stacks, RX DMA and driver metadata.
constexpr size_t STARTUP_INTERNAL_BYTES = CAPTURE_STACK + INFERENCE_STACK +
    I2SConfig::DMA_BUF_COUNT * I2SConfig::DMA_BUF_LENGTH * sizeof(int32_t) + 4096;
}

namespace IOConfig {
constexpr int USER_KEY_1 = 12;
constexpr int USER_KEY_2 = 13;
constexpr uint32_t KEY_DEBOUNCE_MS = 35;
// Key1: hold once for 3 seconds to open a 2-minute discovery window.
constexpr uint32_t PAIRING_HOLD_MS = 3000;
constexpr uint32_t PAIRING_WINDOW_MS = 120000;
}

namespace AudioConfig {
// 1=逐点输出，不降采样、不经过降采样FIR；2=保留原两倍降采样路径。
constexpr size_t DECIMATION_FACTOR = 1;
#if SINGLEMIC_ENABLE_GTCRN
static_assert(DECIMATION_FACTOR == 1 && I2SConfig::CAPTURE_SAMPLE_RATE == 16000,
              "GTCRN requires 16 kHz without decimation");
#endif
static_assert(DECIMATION_FACTOR == 1 || DECIMATION_FACTOR == 2,
              "Audio DSP supports decimation factors 1 and 2 only");
constexpr uint32_t SAMPLE_RATE =
    I2SConfig::CAPTURE_SAMPLE_RATE / DECIMATION_FACTOR;
constexpr uint8_t CHANNELS = 1;
constexpr uint8_t BITS_PER_SAMPLE = 24;
constexpr uint8_t BYTES_PER_SAMPLE = 3;
constexpr int RAW_SHIFT = 8;
constexpr int DC_BLOCK_SHIFT = 5;
constexpr size_t ANTI_ALIAS_TAPS = 5;
static_assert(ANTI_ALIAS_TAPS == 5,
              "Decimation FIR requires exactly five taps");
}

#define SINGLEMIC_TRANSPORT_BLE_NOTIFY 1
#define SINGLEMIC_TRANSPORT_CLASSIC_SPP 2

/* ==================== 蓝牙传输模式选择 ====================
下面两行只能启用一行：

1. 经典蓝牙 SPP：
   保留 CLASSIC_SPP 这行，注释 BLE_NOTIFY 那行。

2. BLE Notify：
   注释 CLASSIC_SPP 这行，取消 BLE_NOTIFY 那行的注释。

当前默认：经典蓝牙 SPP。*/

#ifndef SINGLEMIC_TRANSPORT_MODE
#define SINGLEMIC_TRANSPORT_MODE SINGLEMIC_TRANSPORT_CLASSIC_SPP
// #define SINGLEMIC_TRANSPORT_MODE SINGLEMIC_TRANSPORT_BLE_NOTIFY
#endif

#if SINGLEMIC_TRANSPORT_MODE != SINGLEMIC_TRANSPORT_BLE_NOTIFY && \
    SINGLEMIC_TRANSPORT_MODE != SINGLEMIC_TRANSPORT_CLASSIC_SPP
#error "Unsupported SINGLEMIC_TRANSPORT_MODE"
#endif

namespace TransportConfig {
enum class Mode : uint8_t {
  BLE_NOTIFY = SINGLEMIC_TRANSPORT_BLE_NOTIFY,
  CLASSIC_SPP = SINGLEMIC_TRANSPORT_CLASSIC_SPP,
};

constexpr Mode MODE = static_cast<Mode>(SINGLEMIC_TRANSPORT_MODE);
constexpr char DEVICE_NAME[] = "ESP32S3-ICS";
}

// Shared wire format. Keep this BLE-MTU-sized so both transports emit the
// same packets and the receiver can use one decoder.
namespace PacketConfig {
constexpr size_t MAX_PACKET_FRAMES = 76;
constexpr size_t HEADER_BYTES = 14;
constexpr size_t CRC_BYTES = 2;
constexpr size_t MAX_PAYLOAD_BYTES =
    MAX_PACKET_FRAMES * AudioConfig::CHANNELS *
    AudioConfig::BYTES_PER_SAMPLE;
constexpr size_t MAX_PACKET_BYTES =
    HEADER_BYTES + MAX_PAYLOAD_BYTES + CRC_BYTES;
}

namespace BLEConfig {
constexpr char SERVICE_UUID[] =
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e";
constexpr char AUDIO_UUID[] =
    "6e400003-b5a3-f393-e0a9-e50e24dcca9e";
constexpr uint16_t REQUESTED_MTU = 247;
constexpr uint16_t CONNECTION_INTERVAL_MIN = 12;
constexpr uint16_t CONNECTION_INTERVAL_MAX = 18;
constexpr uint16_t CONNECTION_LATENCY = 0;
constexpr uint16_t CONNECTION_TIMEOUT = 400;
constexpr size_t PACKET_QUEUE_DEPTH = 96;
constexpr uint32_t NOTIFY_TIMEOUT_MS = 100;
constexpr uint32_t NOTIFY_RETRY_DELAY_MS = 10;
constexpr uint8_t NOTIFY_MAX_ATTEMPTS = 3;
}

namespace SPPConfig {
constexpr size_t PACKET_QUEUE_DEPTH = 64;
constexpr uint32_t CONTROL_QUEUE_TIMEOUT_MS = 100;
constexpr uint32_t WRITE_TIMEOUT_MS = 2000;
constexpr uint32_t SENDER_TASK_STACK = 4096;  // Bytes, not StackType_t words.
constexpr UBaseType_t SENDER_TASK_PRIORITY = 2;
constexpr BaseType_t SENDER_TASK_CORE = 0;
}

static_assert(PacketConfig::MAX_PACKET_BYTES <= 244,
              "Shared audio packet must remain BLE MTU 247 compatible");

#endif
