#pragma once

#include "../lighting_driver/lighting_driver.hpp"

namespace connected_lighting_drivers {

esp_err_t init();
esp_err_t status();

// Provisioned registry slots 1..6 are independent of hardware type.
constexpr uint32_t MAX_DRIVERS = 6;
rgb_lighting_driver *find(uint32_t id);
bool uses_gpio(uint32_t pin);

} // namespace connected_lighting_drivers
