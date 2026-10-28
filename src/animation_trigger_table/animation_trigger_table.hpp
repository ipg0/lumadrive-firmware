#pragma once

#include "animation_trigger_entry.hpp"
#include "trigger_context.hpp"
#include <array>
#include <memory>
#include <optional>

class animation_trigger_table {
public:
  static constexpr size_t ANIMATION_TRIGGER_TABLE_MAX_ENTRIES = 32;

  animation_trigger_table() = default;
  animation_trigger_table(const animation_trigger_table &) = delete;
  animation_trigger_table &operator=(const animation_trigger_table &) = delete;

  // Descriptors, sources, conditions, and drivers are borrowed and must outlive
  // their registrations. Effect instances are owned by the table.
  esp_err_t add_entry(const animation_trigger_entry &entry);
  // Indices follow registration order; removing an entry shifts later indices.
  esp_err_t remove_entry(size_t index);
  esp_err_t clear();
  size_t size() const { return _entry_count; }

  // Call from one task, once per frame, with a fresh event batch and timestamp.
  esp_err_t tick(const trigger_context &context);

private:
  struct entry_state {
    std::optional<animation_trigger_entry> entry;
    std::unique_ptr<animation_effect_instance_base> instance;
    bool active = false;
  };

  esp_err_t render_frame(rgb_lighting_driver *removed_driver = nullptr);

  size_t _entry_count = 0;
  std::array<entry_state, ANIMATION_TRIGGER_TABLE_MAX_ENTRIES> _entries;
};
