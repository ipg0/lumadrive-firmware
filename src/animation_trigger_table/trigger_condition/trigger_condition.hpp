#pragma once

#include "../trigger_context.hpp"

enum class trigger_condition_type {
  ALWAYS = 0,
};

class trigger_condition_base {
public:
  virtual ~trigger_condition_base() = default;
  virtual trigger_condition_type type() const = 0;
  virtual bool evaluate(const trigger_context &context) const = 0;
};

template <trigger_condition_type T> class trigger_condition;