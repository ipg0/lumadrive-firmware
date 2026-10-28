#include "animation_trigger_table.hpp"
#include <algorithm>
#include <utility>

esp_err_t animation_trigger_table::add_entry(
    const animation_trigger_entry &entry) {
  if (_entry_count == _entries.size()) {
    return ESP_ERR_NO_MEM;
  }

  auto registered_entry = entry;
  esp_err_t err = registered_entry.init();
  if (err != ESP_OK) {
    return err;
  }

  std::unique_ptr<animation_effect_instance_base> instance(
      registered_entry.effect().create_instance());
  if (!instance) {
    return ESP_ERR_NO_MEM;
  }

  auto &state = _entries[_entry_count++];
  state.entry = registered_entry;
  state.instance = std::move(instance);
  state.active = false;
  return ESP_OK;
}

esp_err_t animation_trigger_table::remove_entry(size_t index) {
  if (index >= _entry_count) {
    return ESP_ERR_INVALID_ARG;
  }

  auto *driver = &_entries[index].entry->driver();
  auto removed = std::move(_entries[index]);
  for (size_t i = index; i + 1 < _entry_count; ++i) {
    _entries[i] = std::move(_entries[i + 1]);
  }
  _entries[--_entry_count] = entry_state{};
  // Recompose surviving layers and clear a driver even when its last row left.
  esp_err_t err = render_frame(driver);
  if (err != ESP_OK) {
    // Keep the registration available for retry if the output could not clear.
    for (size_t i = _entry_count; i > index; --i) {
      _entries[i] = std::move(_entries[i - 1]);
    }
    _entries[index] = std::move(removed);
    ++_entry_count;
  }
  return err;
}

esp_err_t animation_trigger_table::clear() {
  for (size_t i = 0; i < _entry_count; ++i) {
    _entries[i].active = false;
  }
  esp_err_t err = render_frame();
  if (err != ESP_OK) {
    return err;
  }
  for (size_t i = 0; i < _entry_count; ++i) {
    _entries[i] = entry_state{};
  }
  _entry_count = 0;
  return err;
}

esp_err_t animation_trigger_table::tick(const trigger_context &context) {
  if (context.gpio_edges_count != 0 && context.gpio_edges == nullptr) {
    return ESP_ERR_INVALID_ARG;
  }

  for (size_t i = 0; i < _entry_count; ++i) {
    auto &state = _entries[i];
    const auto &entry = *state.entry;
    const bool fired = entry.source().poll(context);
    const bool allowed = entry.condition().evaluate(context);
    const bool is_default =
        entry.source().type() == trigger_source_type::DEFAULT_STATE;

    if (is_default && (!fired || !allowed)) {
      state.active = false;
    } else if (fired && allowed && (!is_default || !state.active)) {
      state.instance->start(context.now_ms);
      state.active = true;
    }

    if (state.active) {
      state.instance->update(context.now_ms);
      if (state.instance->is_finished()) {
        state.active = false;
      }
    }
  }

  return render_frame();
}

esp_err_t animation_trigger_table::render_frame(
    rgb_lighting_driver *removed_driver) {
  std::array<rgb_lighting_driver *, ANIMATION_TRIGGER_TABLE_MAX_ENTRIES + 1>
      drivers{};
  std::array<size_t, ANIMATION_TRIGGER_TABLE_MAX_ENTRIES> order{};
  size_t driver_count = 0;
  if (removed_driver != nullptr) {
    drivers[driver_count++] = removed_driver;
  }

  for (size_t i = 0; i < _entry_count; ++i) {
    order[i] = i;
    auto *driver = &_entries[i].entry->driver();
    if (std::find(drivers.begin(), drivers.begin() + driver_count, driver) ==
        drivers.begin() + driver_count) {
      drivers[driver_count++] = driver;
    }
  }

  // Registration order breaks priority ties, without allocating during a tick.
  std::sort(order.begin(), order.begin() + _entry_count,
            [this](size_t a, size_t b) {
              const auto priority_a = _entries[a].entry->priority();
              const auto priority_b = _entries[b].entry->priority();
              return priority_a == priority_b ? a < b : priority_a < priority_b;
            });

  for (size_t i = 0; i < driver_count; ++i) {
    esp_err_t err = drivers[i]->clear();
    if (err != ESP_OK) {
      return err;
    }
  }

  for (size_t i = 0; i < _entry_count; ++i) {
    const auto &state = _entries[order[i]];
    if (state.active) {
      esp_err_t err = state.instance->render(state.entry->driver());
      if (err != ESP_OK) {
        return err;
      }
    }
  }

  // Flush every independent output even if another driver's show fails.
  esp_err_t result = ESP_OK;
  for (size_t i = 0; i < driver_count; ++i) {
    esp_err_t err = drivers[i]->show();
    if (result == ESP_OK) {
      result = err;
    }
  }
  return result;
}
