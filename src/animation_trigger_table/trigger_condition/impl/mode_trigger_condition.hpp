#pragma once

#include "../trigger_condition.hpp"

template <>
class trigger_condition<trigger_condition_type::MODE> : public trigger_condition_base {
public:
  explicit trigger_condition(uint32_t mode_id, bool inverted = false) : _mode_id(mode_id), _inverted(inverted) {}
  trigger_condition_type type() const override { return trigger_condition_type::MODE; }
  bool evaluate(const trigger_context &context) const override {
    return (context.active_mode_id == _mode_id) != _inverted;
  }
private:
  uint32_t _mode_id;
  bool _inverted;
};
