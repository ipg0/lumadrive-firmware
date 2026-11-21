#include "hardware_config/hardware_config.hpp"
#include HARDWARE_CONFIG_HEADER
#include <array>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2) { std::cerr << "Usage: create_hardware_image OUTPUT.bin\n"; return 2; }
  std::array<uint8_t, hardware_config::MAX_IMAGE_SIZE> bytes{};
  const auto size = hardware_config::encode(HARDWARE_CONFIGURATION, bytes.data(), bytes.size());
  if (!size) { std::cerr << "Invalid hardware configuration or missing role; no image written\n"; return 1; }
  std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()), size);
  if (!output) { std::cerr << "Cannot write provisioning image\n"; return 1; }
  std::cout << "Wrote " << size << " bytes for " << HARDWARE_CONFIGURATION.count << " drivers\n";
}
