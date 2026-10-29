#pragma once

#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

namespace matrix {
template <typename object_type> struct heap_object_deleter {
  void operator()(object_type *object) const {
    if (!object) return;
    object->~object_type();
    std::free(object);
  }
};

template <typename object_type>
using heap_object = std::unique_ptr<object_type, heap_object_deleter<object_type>>;

// ESP-IDF's exception-disabled libstdc++ can abort inside nothrow new before
// returning nullptr. Check malloc directly for our large staging objects.
// This checks object storage; allocations inside constructors remain separate.
template <typename object_type, typename... arguments>
heap_object<object_type> make_heap_object(arguments &&...args) {
  void *storage = std::malloc(sizeof(object_type));
  if (!storage) return {};
  return heap_object<object_type>(new (storage) object_type(std::forward<arguments>(args)...));
}
} // namespace matrix
