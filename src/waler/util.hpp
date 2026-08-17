#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace waler {
  void hexdump(const void *data, size_t size);

  // Reads the MAC address of a local network interface (e.g. "enp1s0f0np0")
  // via SIOCGIFHWADDR. Aborts if the interface doesn't exist.
  [[nodiscard]] std::array<std::uint8_t, 6> getInterfaceMac(const std::string &ifname);
}
