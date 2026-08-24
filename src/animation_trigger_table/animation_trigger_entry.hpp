#include "animation_effect/animation_effect.hpp"
#include "trigger_condition/trigger_condition.hpp"
#include "trigger_source/trigger_source.hpp"

class animation_trigger_entry {
public:
  animation_trigger_entry(animation_effect_descriptor_base *effect,
                          trigger_source_base *source,
                          trigger_condition_base *condition)
      : _effect(effect), _source(source), _condition(condition) {}

  esp_err_t init() {
    if (_effect == nullptr || _source == nullptr || _condition == nullptr) {
      return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = _source->init();
    if (err != ESP_OK) {
      return err;
    }
    return ESP_OK;
  }

private:
  uint8_t priority;
  animation_effect_descriptor_base *_effect;
  trigger_source_base *_source;
  trigger_condition_base *_condition;
};