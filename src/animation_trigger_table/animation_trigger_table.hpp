#pragma once

#include "animation_trigger_entry.hpp"
#include "trigger_context.hpp"

class animation_trigger_table {
  static constexpr size_t ANIMATION_TRIGGER_TABLE_MAX_ENTRIES = 32;
public:
  void tick(const trigger_context &context);

private:
  size_t _entry_count;
  animation_trigger_entry _entries[ANIMATION_TRIGGER_TABLE_MAX_ENTRIES];
};