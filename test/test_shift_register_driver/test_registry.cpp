#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <array>
#include "../../src/connected_lighting_drivers/connected_lighting_drivers.cpp"

namespace {
struct strip { size_t symbols; };
size_t rmt_used = 0;
std::vector<size_t> allocations;
std::vector<uint8_t> last_pixels;
std::array<uint8_t, driver_provisioning::MAX_IMAGE_SIZE> partition_bytes{};
esp_partition_t partition{partition_bytes.size()};
bool partition_present = true;
}
const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char *) {
  return partition_present ? &partition : nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t *, size_t offset, void *data, size_t size) {
  if (offset + size > partition_bytes.size()) return ESP_ERR_INVALID_SIZE;
  memcpy(data, partition_bytes.data() + offset, size); return ESP_OK;
}
esp_err_t rmt_new_tx_channel(const rmt_tx_channel_config_t *cfg, rmt_channel_handle_t *handle) {
  assert(cfg->mem_block_symbols && cfg->mem_block_symbols % 64 == 0);
  if (rmt_used + cfg->mem_block_symbols > 512) return ESP_ERR_NO_MEM;
  rmt_used += cfg->mem_block_symbols; allocations.push_back(cfg->mem_block_symbols);
  *handle = new strip{cfg->mem_block_symbols}; return ESP_OK;
}
esp_err_t rmt_del_channel(rmt_channel_handle_t handle) { auto *s = static_cast<strip *>(handle); rmt_used -= s->symbols; delete s; return ESP_OK; }
esp_err_t rmt_enable(rmt_channel_handle_t) { return ESP_OK; }
esp_err_t rmt_disable(rmt_channel_handle_t) { return ESP_OK; }
esp_err_t rmt_tx_wait_all_done(rmt_channel_handle_t, int) { return ESP_OK; }
esp_err_t rmt_new_simple_encoder(const rmt_simple_encoder_config_t *config, rmt_encoder_handle_t *handle) {
  *handle = new rmt_simple_encoder_config_t(*config); return ESP_OK;
}
esp_err_t rmt_del_encoder(rmt_encoder_handle_t handle) { delete static_cast<rmt_simple_encoder_config_t *>(handle); return ESP_OK; }
esp_err_t rmt_transmit(rmt_channel_handle_t, rmt_encoder_handle_t handle, const void *data, size_t size, const rmt_transmit_config_t *) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  last_pixels.assign(bytes, bytes + size);
  const auto &encoder = *static_cast<rmt_simple_encoder_config_t *>(handle);
  // Exercise byte boundaries and a reset symbol deferred to the next chunk.
  for (size_t chunk : {1, 7, 64, 512}) {
    std::vector<rmt_symbol_word_t> symbols(chunk);
    size_t written = 0; bool done = false;
    while (!done) {
      const auto count = encoder.callback(data, size, written, chunk, symbols.data(), &done, encoder.arg);
      assert(count && count <= chunk);
      for (size_t i = 0; i < count; ++i) {
        const auto bit = written + i;
        if (bit < size * 8) {
          const bool one = bytes[bit / 8] & (0x80u >> (bit % 8));
          assert(symbols[i].level0 == 1 && symbols[i].level1 == 0);
          assert(symbols[i].duration0 == (one ? 9 : 3) && symbols[i].duration1 == (one ? 3 : 9));
        } else {
          assert(bit == size * 8 && !symbols[i].level0 && !symbols[i].level1);
          assert(symbols[i].duration0 + symbols[i].duration1 == 2800 && done);
        }
      }
      written += count;
    }
    assert(written == size * 8 + 1);
  }
  return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *) { return ESP_OK; }
esp_err_t gpio_set_level(gpio_num_t, uint32_t) { return ESP_OK; }
esp_err_t gptimer_new_timer(const gptimer_config_t *, gptimer_handle_t *handle) { *handle = reinterpret_cast<void *>(1); return ESP_OK; }
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t, const gptimer_event_callbacks_t *, void *) { return ESP_OK; }
esp_err_t gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t *) { return ESP_OK; }
esp_err_t gptimer_get_raw_count(gptimer_handle_t, uint64_t *count) { *count = 0; return ESP_OK; }
esp_err_t gptimer_enable(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_start(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_disable(gptimer_handle_t) { return ESP_OK; }
esp_err_t gptimer_del_timer(gptimer_handle_t) { return ESP_OK; }

int main() {
  using namespace driver_provisioning;
  configuration config{}; config.count = 2;
  config.drivers[0] = {.id = 1, .type = WS2812, .data_pin = 19, .default_pixels = 60};
  config.drivers[1] = {.id = 2, .type = HC595, .data_pin = 23, .clock_pin = 18,
      .latch_pin = 5, .led_count = 1, .bcm_base_period_us = 10, .leds = {{{0, 1, 2, false}, {}}}};
  assert(encode(config, partition_bytes.data(), partition_bytes.size()));
  assert(init_registered_drivers() == ESP_OK);
  assert(allocations.back() == 512);
  auto *led_strip_driver = connected_lighting_drivers::find(1);
  assert(led_strip_driver && connected_lighting_drivers::find(2));
  assert(connected_lighting_drivers::uses_gpio(19) && connected_lighting_drivers::uses_gpio(23));
  assert(!connected_lighting_drivers::uses_gpio(12));
  led_strip_driver->set_pixel(0, alpha_color(color(10, 20, 30)));
  const auto initial_allocations = allocations.size();
  assert(led_strip_driver->reserve_layout_pixels(0, 120) == ESP_OK);
  assert(led_strip_driver->get_pixel_count() == 120 && allocations.size() == initial_allocations);
  led_strip_driver->show(); assert(last_pixels.size() == 360);
  assert(last_pixels[0] == 20 && last_pixels[1] == 10 && last_pixels[2] == 30 && last_pixels.back() == 0);
  assert(led_strip_driver->reserve_layout_pixels(0, ws2812_lighting_driver::MAX_PIXELS + 1) == ESP_ERR_INVALID_ARG);
  assert(led_strip_driver->reserve_layout_pixels(1, 10) == ESP_ERR_INVALID_ARG);
  assert(led_strip_driver->get_pixel_count() == 120);
  for (auto &driver : drivers) if (driver) { driver->deinit(); driver.reset(); }
  const int pins[] = {19, 21, 22, 25, 26, 27};
  config = {}; config.count = 6;
  for (size_t i = 0; i < config.count; ++i)
    config.drivers[i] = {.id = uint8_t(i + 1), .type = WS2812, .data_pin = uint8_t(pins[i]), .default_pixels = 60};
  assert(encode(config, partition_bytes.data(), partition_bytes.size()));
  const auto start = allocations.size();
  assert(init_registered_drivers() == ESP_OK);
  assert(rmt_used == 384 && allocations.size() == start + 6);
  for (size_t i = start; i < allocations.size(); ++i) assert(allocations[i] == 64);
  for (auto &driver : drivers) if (driver) { driver->deinit(); driver.reset(); }
  config.drivers[1] = config.drivers[5]; config.count = 2;
  assert(encode(config, partition_bytes.data(), partition_bytes.size()));
  assert(init_registered_drivers() == ESP_OK);
  assert(connected_lighting_drivers::find(1) && connected_lighting_drivers::find(6));
  assert(!connected_lighting_drivers::find(2) && rmt_used == 512);
  for (auto &driver : drivers) if (driver) { driver->deinit(); driver.reset(); }
  assert(rmt_used == 0);
  partition_bytes[16] ^= 1;
  assert(init_registered_drivers() == ESP_ERR_INVALID_ARG && !connected_lighting_drivers::find(1));
  partition_present = false;
  assert(init_registered_drivers() == ESP_ERR_NOT_FOUND && !connected_lighting_drivers::find(1));
  std::puts("Provisioned registry: image validation, six strips, RMT budget and sparse slots passed.");
}
