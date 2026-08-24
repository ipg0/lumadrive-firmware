#pragma once

#include "../trigger_condition.hpp"

template <>
class trigger_condition<trigger_condition_type::ALWAYS>
    : public trigger_condition_base {
public:
  trigger_condition() {}
  trigger_condition_type type() const override {
    return trigger_condition_type::ALWAYS;
  };
  bool evaluate(const trigger_context &context) const override {
    return true;
  };
};