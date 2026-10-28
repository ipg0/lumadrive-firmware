#pragma once

#include "../trigger_context.hpp"
#include <esp_err.h>

enum class trigger_source_type {
  DEFAULT_STATE = 0,
  GPIO_EDGE = 1,
};

class trigger_source_base {
public:
  virtual ~trigger_source_base() = default;
  virtual trigger_source_type type() const = 0;
  virtual esp_err_t init() = 0;
  virtual bool poll(const trigger_context &context) = 0;
};

template <trigger_source_type source_type> class trigger_source;
