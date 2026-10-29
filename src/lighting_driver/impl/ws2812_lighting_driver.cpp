#include "ws2812_lighting_driver.hpp"
#include <cstdlib>
#include <cstring>
#include <new>

ws2812_lighting_driver::~ws2812_lighting_driver() {
  deinit(); std::free(_pixels); std::free(_wire_pixels);
}

esp_err_t ws2812_lighting_driver::reserve_layout_pixels(uint32_t source, size_t count) {
  if (source || !count || count > MAX_PIXELS) return ESP_ERR_INVALID_ARG;
  if (count <= _capacity) return ESP_OK;
  auto *pixels = static_cast<color *>(std::calloc(count, sizeof(color)));
  auto *wire = static_cast<uint8_t *>(std::calloc(count, 3));
  if (!pixels || !wire) { std::free(pixels); std::free(wire); return ESP_ERR_NO_MEM; }
  for (size_t i = 0; i < count; ++i) new (pixels + i) color();
  if (_pixels) std::memcpy(pixels, _pixels, _capacity * sizeof(color));
  // show() waits for transmission completion; resizing runs on the same worker.
  std::free(_pixels); std::free(_wire_pixels);
  _pixels = pixels; _wire_pixels = wire; _capacity = count;
  return ESP_OK;
}

esp_err_t ws2812_lighting_driver::init() {
  if (_ready || _channel) return ESP_ERR_INVALID_STATE;
  auto error = reserve_layout_pixels(0, _cfg.led_count);
  if (error != ESP_OK) return error;
  rmt_tx_channel_config_t config = {};
  config.clk_src = RMT_CLK_SRC_DEFAULT; config.gpio_num = _cfg.data_pin;
  config.resolution_hz = 10000000; config.mem_block_symbols = _cfg.rmt_mem_block_symbols;
  config.trans_queue_depth = 1;
  error = rmt_new_tx_channel(&config, &_channel);
  if (error != ESP_OK) return error;
  rmt_simple_encoder_config_t encoder = {};
  encoder.callback = encode_pixels; encoder.min_chunk_size = 1;
  error = rmt_new_simple_encoder(&encoder, &_encoder);
  if (error == ESP_OK) error = rmt_enable(_channel);
  if (error != ESP_OK) { deinit(); return error; }
  _ready = true;
  return ESP_OK;
}

esp_err_t ws2812_lighting_driver::deinit() {
  if (_channel) {
    if (_ready) rmt_disable(_channel);
    rmt_del_channel(_channel); _channel = nullptr;
  }
  if (_encoder) { rmt_del_encoder(_encoder); _encoder = nullptr; }
  _ready = false;
  return ESP_OK;
}

esp_err_t ws2812_lighting_driver::set_pixel(uint16_t index, alpha_color value) {
  if (index >= _capacity) return ESP_ERR_INVALID_ARG;
  _pixels[index] = value.alpha == 255 ? value.base_color : value.blend_with(_pixels[index]);
  return ESP_OK;
}
esp_err_t ws2812_lighting_driver::fill(alpha_color value) {
  for (size_t i = 0; i < _capacity; ++i) set_pixel(i, value);
  return ESP_OK;
}
esp_err_t ws2812_lighting_driver::show() {
  if (!_ready) return ESP_ERR_INVALID_STATE;
  for (size_t i = 0; i < _capacity; ++i) {
    _wire_pixels[i * 3] = uint16_t(_pixels[i].g) * _brightness / 255;
    _wire_pixels[i * 3 + 1] = uint16_t(_pixels[i].r) * _brightness / 255;
    _wire_pixels[i * 3 + 2] = uint16_t(_pixels[i].b) * _brightness / 255;
  }
  rmt_transmit_config_t config = {};
  auto error = rmt_transmit(_channel, _encoder, _wire_pixels, _capacity * 3, &config);
  if (error == ESP_OK) error = rmt_tx_wait_all_done(_channel, -1);
  return error;
}

size_t ws2812_lighting_driver::encode_pixels(const void *data, size_t size, size_t written,
    size_t available, rmt_symbol_word_t *symbols, bool *done, void *) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t count = 0;
  while (count < available && written < size * 8) {
    const bool bit = bytes[written / 8] & (0x80u >> (written % 8));
    auto &symbol = symbols[count++];
    symbol.level0 = 1; symbol.duration0 = bit ? 9 : 3;
    symbol.level1 = 0; symbol.duration1 = bit ? 3 : 9;
    ++written;
  }
  if (count < available && written == size * 8) {
    auto &reset = symbols[count++];
    reset.level0 = reset.level1 = 0;
    reset.duration0 = reset.duration1 = 1400; // 280 us latch/reset, including WS2812B-V5.
    *done = true;
  }
  return count;
}
