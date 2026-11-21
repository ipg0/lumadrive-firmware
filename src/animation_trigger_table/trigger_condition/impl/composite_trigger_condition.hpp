#pragma once
#include "../trigger_condition.hpp"

template <trigger_condition_type operation>
class composite_trigger_condition : public trigger_condition_base {
public:
  composite_trigger_condition(const trigger_condition_base &left, const trigger_condition_base &right)
      : _left(left), _right(right) {}
  trigger_condition_type type() const override { return operation; }
  bool evaluate(const trigger_context &context) const override {
    if constexpr (operation == trigger_condition_type::AND) return _left.evaluate(context) && _right.evaluate(context);
    return _left.evaluate(context) || _right.evaluate(context);
  }
private:
  const trigger_condition_base &_left, &_right;
};
template <> class trigger_condition<trigger_condition_type::AND> : public composite_trigger_condition<trigger_condition_type::AND> {
public: using composite_trigger_condition::composite_trigger_condition;
};
template <> class trigger_condition<trigger_condition_type::OR> : public composite_trigger_condition<trigger_condition_type::OR> {
public: using composite_trigger_condition::composite_trigger_condition;
};
