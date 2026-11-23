#pragma once
#include "../trigger_condition.hpp"

template <> class trigger_condition<trigger_condition_type::NEVER> : public trigger_condition_base {
public:
  trigger_condition_type type() const override { return trigger_condition_type::NEVER; }
  bool evaluate(const trigger_context&) const override { return false; }
};
