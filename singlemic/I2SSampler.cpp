#include "I2SSampler.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "cfg.h"
#include "task_cooperation.h"

I2SSampler::~I2SSampler() {
  end();
}

// 配置 ESP32 旧版 I2S 驱动，并将麦克风数据脚接入 RX 通道。
bool I2SSampler::begin() {
  if (ready_) {
    return true;
  }

  i2s_config_t config = {};
  // 麦克风由 ESP32 提供时钟，DMA 负责连续搬运 32 位采样槽数据。
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = I2SConfig::CAPTURE_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  // 接收新麦克风的右声道时隙，32位槽中保留PCM24有效数据。
  config.channel_format = I2S_CHANNEL_FMT_ONLY_RIGHT;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = I2SConfig::DMA_BUF_COUNT;
  config.dma_buf_len = I2SConfig::DMA_BUF_LENGTH;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  esp_err_t error = i2s_driver_install(I2SConfig::PORT, &config, 0, nullptr);
  if (error != ESP_OK) {
    Serial.printf("I2S driver install failed: %d\n", error);
    return false;
  }

  i2s_pin_config_t pins = {};
  // 麦克风只需BCLK/WS；避免零初始化将未使用的MCLK输出到GPIO0。
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = I2SConfig::BCLK_PIN;
  pins.ws_io_num = I2SConfig::WS_PIN;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = I2SConfig::DATA_PIN;
  error = i2s_set_pin(I2SConfig::PORT, &pins);
  if (error != ESP_OK) {
    Serial.printf("I2S pin setup failed: %d\n", error);
    i2s_driver_uninstall(I2SConfig::PORT);
    return false;
  }

  i2s_zero_dma_buffer(I2SConfig::PORT);
  ready_ = true;
  Serial.printf("I2S ready: %lu Hz, transport output: %lu Hz, read_timeout=%lums, BCLK=%d WS=%d SD=%d, channel=RIGHT MCLK=disabled\n",
                static_cast<unsigned long>(I2SConfig::CAPTURE_SAMPLE_RATE),
                static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
                static_cast<unsigned long>(I2SConfig::READ_TIMEOUT_MS),
                I2SConfig::BCLK_PIN, I2SConfig::WS_PIN, I2SConfig::DATA_PIN);
  return true;
}

void I2SSampler::end() {
  if (!ready_) {
    return;
  }
  i2s_driver_uninstall(I2SConfig::PORT);
  ready_ = false;
}

// i2s_read 可能分多次返回，因此循环累积字节，直到拿满请求的样本数。
bool I2SSampler::readSamplesBounded(int32_t *destination, size_t sampleCount,
                                   size_t capacity) {
  if (!ready_ || destination == nullptr || sampleCount == 0 ||
      sampleCount > capacity || sampleCount > SIZE_MAX / sizeof(int32_t)) {
    return false;
  }

  size_t received = 0;
  const size_t requestedBytes = sampleCount * sizeof(int32_t);
  TaskCooperation::Progress progress;
  while (received < requestedBytes) {
    size_t chunk = 0;
    const esp_err_t error = i2s_read(
        I2SConfig::PORT,
        reinterpret_cast<uint8_t *>(destination) + received,
        requestedBytes - received, &chunk,
        pdMS_TO_TICKS(I2SConfig::READ_TIMEOUT_MS));
    if (error != ESP_OK || chunk == 0 ||
        chunk > requestedBytes - received) {
      Serial.printf("I2S read failed: err=%d bytes=%u\n",
                    static_cast<int>(error), static_cast<unsigned>(chunk));
      return false;
    }
    received += chunk;
    if (received < requestedBytes) progress.checkpoint();
  }
  return true;
}
