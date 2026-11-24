#pragma once
#include <cstdint>
class mesh_interop {
public:
  static mesh_interop& instance() {
    static mesh_interop value;
    return value;
  }
  bool master_node_matches(const uint8_t*) { return true; }
};
