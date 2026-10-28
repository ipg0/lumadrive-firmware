#pragma once

#include "../lighting_driver/lighting_driver.hpp"

namespace connected_lighting_drivers {

esp_err_t init();

// These drivers live for the lifetime of the firmware.
rgb_lighting_driver &ws2812();
rgb_lighting_driver &hc595();

} // namespace connected_lighting_drivers
