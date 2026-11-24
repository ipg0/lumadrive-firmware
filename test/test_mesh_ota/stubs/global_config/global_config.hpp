#pragma once
class global_config {
public:
  static global_config& instance() {
    static global_config value;
    return value;
  }
  bool is_master_node() const { return true; }
};
