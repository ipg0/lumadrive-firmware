#pragma once

#include "../lighting_driver/lighting_driver.hpp"

namespace connected_lighting_drivers {

esp_err_t init();

// These drivers live for the lifetime of the firmware.
rgb_lighting_driver &ws2812();
rgb_lighting_driver &hc595();
// Stable protobuf MatrixDriver IDs: 1 = WS2812, 2 = HC595.
rgb_lighting_driver *find(uint32_t id);
bool uses_gpio(uint32_t pin);

} // namespace connected_lighting_drivers
