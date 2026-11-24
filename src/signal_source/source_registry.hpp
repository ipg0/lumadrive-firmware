#pragma once

#include "signal_source.hpp"
#include <array>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

namespace signal_source {
struct source_deleter {
  void operator()(source* value) const {
    if (!value)
      return;
    value->~source();
    std::free(value);
  }
};
using source_ptr = std::unique_ptr<source, source_deleter>;

template <typename source_type, typename... arguments> source_ptr make_source(arguments&&... args) {
  void* storage = std::malloc(sizeof(source_type));
  if (!storage)
    return {};
  return source_ptr(new (storage) source_type(std::forward<arguments>(args)...));
}

// A revision owns its source instances until it is replaced. Sampling never
// allocates once a source is installed. Two revisions may overlap at activation.
class source_registry {
public:
  ~source_registry() { retain(0); }
  source* find(uint64_t revision, uint32_t id) const {
    for (const auto& entry : _entries)
      if (entry.revision == revision && entry.id == id)
        return entry.value.get();
    return nullptr;
  }
  bool install(uint64_t revision, uint32_t id, source_ptr value) {
    if (!revision || !id || !value || find(revision, id))
      return false;
    for (auto& entry : _entries) {
      if (!entry.value) {
        if (!value->init()) {
          value->deinit();
          return false;
        }
        entry.revision = revision;
        entry.id = id;
        entry.value = std::move(value);
        return true;
      }
    }
    return false;
  }
  void retain(uint64_t current, uint64_t retiring = 0) {
    for (auto& entry : _entries)
      if (entry.value && entry.revision != current && entry.revision != retiring) {
        entry.value->deinit();
        entry.value.reset();
        entry.revision = 0;
        entry.id = 0;
      }
  }

private:
  struct entry {
    uint64_t revision = 0;
    uint32_t id = 0;
    source_ptr value;
  };
  std::array<entry, 64> _entries{};
};
} // namespace signal_source
