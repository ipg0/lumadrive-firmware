#pragma once

#include "animation_effect/animation_effect.hpp"
#include "trigger_condition/trigger_condition.hpp"
#include "trigger_source/trigger_source.hpp"

class animation_trigger_entry {
public:
  animation_trigger_entry(animation_effect_descriptor_base *effect,
                          trigger_source_base *source,
                          trigger_condition_base *condition,
                          rgb_lighting_driver *driver, uint8_t priority = 0)
      : _priority(priority), _effect(effect), _source(source),
        _condition(condition), _driver(driver) {}

  esp_err_t init() {
    if (_effect == nullptr || _source == nullptr || _condition == nullptr ||
        _driver == nullptr) {
      return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = _source->init();
    if (err != ESP_OK) {
      return err;
    }
    return ESP_OK;
  }

  uint8_t priority() const { return _priority; }
  const animation_effect_descriptor_base &effect() const { return *_effect; }
  trigger_source_base &source() const { return *_source; }
  const trigger_condition_base &condition() const { return *_condition; }
  rgb_lighting_driver &driver() const { return *_driver; }

private:
  uint8_t _priority;
  animation_effect_descriptor_base *_effect;
  trigger_source_base *_source;
  trigger_condition_base *_condition;
  rgb_lighting_driver *_driver;
};
