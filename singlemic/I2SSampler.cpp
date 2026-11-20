#include "I2SSampler.h"
#include <Arduino.h>
#include "cfg.h"

I2SSampler::~I2SSampler() { end(); }

bool I2SSampler::begin() {
  if (ready_) return true;
  i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(I2SConfig::PORT, I2S_ROLE_MASTER);
  channel.dma_desc_num = I2SConfig::DMA_BUF_COUNT;
  channel.dma_frame_num = I2SConfig::DMA_BUF_LENGTH;
  if (i2s_new_channel(&channel, nullptr, &rx_) != ESP_OK) return false;
  i2s_std_config_t config = {};
  config.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2SConfig::CAPTURE_SAMPLE_RATE);
  config.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO);
  config.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
  config.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  config.gpio_cfg.mclk = I2S_GPIO_UNUSED;
  config.gpio_cfg.bclk = static_cast<gpio_num_t>(I2SConfig::BCLK_PIN);
  config.gpio_cfg.ws = static_cast<gpio_num_t>(I2SConfig::WS_PIN);
  config.gpio_cfg.dout = I2S_GPIO_UNUSED;
  config.gpio_cfg.din = static_cast<gpio_num_t>(I2SConfig::DATA_PIN);
  if (i2s_channel_init_std_mode(rx_, &config) != ESP_OK ||
      i2s_channel_enable(rx_) != ESP_OK) {
    i2s_del_channel(rx_);
    rx_ = nullptr;
    return false;
  }
  ready_ = true;
  Serial.printf("I2S ready: %lu Hz, INT32 slot, channel=LEFT, BCLK=%d WS=%d SD=%d MCLK=disabled\n",
      static_cast<unsigned long>(I2SConfig::CAPTURE_SAMPLE_RATE),
      I2SConfig::BCLK_PIN, I2SConfig::WS_PIN, I2SConfig::DATA_PIN);
  return true;
}
void I2SSampler::end() {
  if (rx_) {
    if (ready_) i2s_channel_disable(rx_);
    i2s_del_channel(rx_);
    rx_ = nullptr;
  }
  ready_ = false;
}

bool I2SSampler::readSamplesBounded(int32_t *dst, size_t count, size_t capacity) {
  if (!ready_ || !dst || !count || count > capacity || count > SIZE_MAX / sizeof(int32_t))
    return false;
  size_t received = 0;
  const size_t bytes = count * sizeof(int32_t);
  while (received < bytes) {
    size_t chunk = 0;
    const esp_err_t error = i2s_channel_read(rx_, reinterpret_cast<uint8_t *>(dst) + received,
        bytes - received, &chunk, I2SConfig::READ_TIMEOUT_MS);
    if (error != ESP_OK || !chunk || chunk > bytes - received || chunk % sizeof(int32_t))
      return false;
    received += chunk;
  }
  return true;
}
