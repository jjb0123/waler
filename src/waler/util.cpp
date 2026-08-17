#include <waler/util.hpp>
#include <stdio.h>
#include <ctype.h>
#include <cstdlib>
#include <cstring>

#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace waler {

  std::array<std::uint8_t, 6> getInterfaceMac(const std::string &ifname) {
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
      perror("getInterfaceMac: socket");
      std::abort();
    }

    ifreq ifr{};
    std::snprintf(ifr.ifr_name, IFNAMSIZ, "%s", ifname.c_str());
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
      perror("getInterfaceMac: ioctl(SIOCGIFHWADDR)");
      close(s);
      std::abort();
    }
    close(s);

    std::array<std::uint8_t, 6> mac{};
    std::memcpy(mac.data(), ifr.ifr_hwaddr.sa_data, mac.size());
    return mac;
  }
  void hexdump(const void *data, size_t size) {
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < size; ++i) {
      if (i % 16 == 0) {
        printf("%08zx  ", i);
      }
      printf("%02x ", p[i]);
      if (i % 16 == 15 || i == size - 1) {
        for (size_t j = i % 16 + 1; j < 16; ++j) {
          printf("   ");
        }
        printf(" |");
        for (size_t j = i - i % 16; j <= i; ++j) {
          if (j < size) {
            printf("%c", isprint(p[j]) ? p[j] : '.');
          } else {
            printf(" ");
          }
        }
        printf("|\n");
      }
    }
  }

}