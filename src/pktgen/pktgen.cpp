#include <stdio.h>

#include <waler/util.hpp>
#include <waler/waler.hpp>

int main(int argc, char **argv) {
  waler::PacketGenerator gen;

  uint8_t buffer[1024];

  for (int i = 0; i < 100; ++i) {

    size_t bytes = gen.nextPacket(buffer, 1024);
    printf("Generated packet of %zu bytes\n", bytes);
    waler::hexdump(buffer, bytes);
    printf("\n");
  }

  return 0;
}