#include "I2SSampler.h"

#include <Arduino.h>
#include <driver/i2s.h>

namespace {

size_t portIndex(i2s_port_t port) {
  return port == I2S_NUM_0 ? 0 : 1;
}

}  // namespace

I2SSampler::~I2SSampler() { end(); }

bool I2SSampler::begin() {
  if (ready_[0] && ready_[1]) return true;
  if (!setupPort(IcsTestConfig::LEFT_MIC)) return false;
  if (!setupPort(IcsTestConfig::RIGHT_MIC)) {
    end();
    return false;
  }
  return true;
}

void I2SSampler::end() {
  if (ready_[0]) i2s_driver_uninstall(I2S_NUM_0);
  if (ready_[1]) i2s_driver_uninstall(I2S_NUM_1);
  ready_[0] = ready_[1] = false;
}

bool I2SSampler::setupPort(const IcsTestConfig::MicrophoneConfig &mic) {
  i2s_config_t config = {};
  config.mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX);
  config.sample_rate = IcsTestConfig::CAPTURE_SAMPLE_RATE;
  config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
  config.channel_format = mic.channelFormat;
  config.communication_format = I2S_COMM_FORMAT_I2S;
  config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  config.dma_buf_count = 8;
  config.dma_buf_len = 256;
  config.use_apll = false;
  config.tx_desc_auto_clear = false;
  config.fixed_mclk = 0;

  esp_err_t err = i2s_driver_install(mic.port, &config, 0, nullptr);
  if (err != ESP_OK) {
    Serial.printf("I2S %s driver install failed: %d\n", mic.name,
                  static_cast<int>(err));
    return false;
  }

  i2s_pin_config_t pins = {};
  pins.bck_io_num = mic.bclkPin;
  pins.ws_io_num = mic.wsPin;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num = mic.dataPin;
  err = i2s_set_pin(mic.port, &pins);
  if (err != ESP_OK) {
    Serial.printf("I2S %s pin setup failed: %d\n", mic.name,
                  static_cast<int>(err));
    i2s_driver_uninstall(mic.port);
    return false;
  }
  i2s_zero_dma_buffer(mic.port);
  ready_[portIndex(mic.port)] = true;
  Serial.printf("I2S %s ready: %lu Hz, BCLK=%d WS=%d SD=%d format=%d\n",
                mic.name,
                static_cast<unsigned long>(IcsTestConfig::CAPTURE_SAMPLE_RATE),
                mic.bclkPin, mic.wsPin, mic.dataPin,
                static_cast<int>(mic.channelFormat));
  return true;
}

bool I2SSampler::readSamples(const IcsTestConfig::MicrophoneConfig &mic,
                             int32_t *samples, size_t sampleCount) {
  if (!ready_[portIndex(mic.port)] || samples == nullptr) return false;
  size_t received = 0;
  const size_t wanted = sampleCount * sizeof(int32_t);
  while (received < wanted) {
    size_t chunk = 0;
    const esp_err_t err = i2s_read(
        mic.port, reinterpret_cast<uint8_t *>(samples) + received,
        wanted - received, &chunk, pdMS_TO_TICKS(100));
    if (err != ESP_OK || chunk == 0) {
      Serial.printf("I2S %s read failed: err=%d bytes=%u\n", mic.name,
                    static_cast<int>(err), static_cast<unsigned>(chunk));
      return false;
    }
    received += chunk;
  }
  return true;
}
