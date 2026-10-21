#include "I2SSampler.h"

#include <Arduino.h>
#include <driver/i2s.h>

#include "cfg.h"

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
  config.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
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
  Serial.printf("I2S ready: %lu Hz, BLE output: %lu Hz, BCLK=%d WS=%d SD=%d\n",
                static_cast<unsigned long>(I2SConfig::CAPTURE_SAMPLE_RATE),
                static_cast<unsigned long>(AudioConfig::SAMPLE_RATE),
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
bool I2SSampler::readSamples(int32_t *destination, size_t sampleCount) {
  if (!ready_ || destination == nullptr || sampleCount == 0) {
    return false;
  }

  size_t received = 0;
  const size_t requestedBytes = sampleCount * sizeof(int32_t);
  while (received < requestedBytes) {
    size_t chunk = 0;
    const esp_err_t error = i2s_read(
        I2SConfig::PORT,
        reinterpret_cast<uint8_t *>(destination) + received,
        requestedBytes - received, &chunk, pdMS_TO_TICKS(100));
    if (error != ESP_OK || chunk == 0) {
      Serial.printf("I2S read failed: err=%d bytes=%u\n",
                    static_cast<int>(error), static_cast<unsigned>(chunk));
      return false;
    }
    received += chunk;
  }
  return true;
}
