#include "connected_lighting_drivers/driver_provisioning.hpp"
#include DRIVER_CONFIG_HEADER
#include <array>
#include <fstream>
#include <iostream>

int main(int argc, char **argv) {
  if (argc != 2) { std::cerr << "Usage: create_driver_image OUTPUT.bin\n"; return 2; }
  std::array<uint8_t, driver_provisioning::MAX_IMAGE_SIZE> bytes{};
  const auto size = driver_provisioning::encode(DRIVER_CONFIGURATION, bytes.data(), bytes.size());
  if (!size) { std::cerr << "Invalid driver configuration; no image written\n"; return 1; }
  std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(bytes.data()), size);
  if (!output) { std::cerr << "Cannot write provisioning image\n"; return 1; }
  std::cout << "Wrote " << size << " bytes for " << DRIVER_CONFIGURATION.count << " drivers\n";
}
