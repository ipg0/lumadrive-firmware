#pragma once

#include "../trigger_context.hpp"

enum class trigger_condition_type {
  ALWAYS = 0,
  MODE = 1,
  AND = 2,
  OR = 3,
  NEVER = 4,
  GPIO_STATE = 5,
};

class trigger_condition_base {
public:
  virtual ~trigger_condition_base() = default;
  virtual trigger_condition_type type() const = 0;
  virtual bool evaluate(const trigger_context& context) const = 0;
};

template <trigger_condition_type condition_type> class trigger_condition;
