#pragma once

#include "../trigger_source.hpp"
#include <driver/gpio.h>
#include <esp_attr.h>
#include <memory.h>

template <>
class trigger_source<trigger_source_type::DEFAULT_STATE>
    : public trigger_source_base {
public:
  trigger_source() {}
  esp_err_t init() override { return ESP_OK; };
  trigger_source_type type() const override {
    return trigger_source_type::DEFAULT_STATE;
  };
  bool poll(const trigger_context &context) override { return true; };
};