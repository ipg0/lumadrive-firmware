#pragma once
#include "../trigger_condition.hpp"

template <>
class trigger_condition<trigger_condition_type::GPIO_STATE> : public trigger_condition_base {
public:
  trigger_condition(const uint8_t* mac, uint32_t pin, bool high)
      : _mac(mac), _pin(pin), _high(high) {}
  trigger_condition_type type() const override { return trigger_condition_type::GPIO_STATE; }
  bool evaluate(const trigger_context& context) const override {
    bool level = false;
    return context.gpio_level && context.gpio_level(context.gpio_level_arg, _mac, _pin, level) &&
           level == _high;
  }

private:
  const uint8_t* _mac;
  uint32_t _pin;
  bool _high;
};
