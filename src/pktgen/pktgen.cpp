#include <stdio.h>

#include <waler/util.hpp>
#include <waler/waler.hpp>

#include <unistd.h>

int main(int argc, char **argv) {
  waler::PacketGenerator gen;

  std::byte buffer[1024];

  waler::SimpleOrderBook ob;

  for (int i = 0; 1; ++i) {

    size_t bytes = gen.nextPacket((unsigned char *)buffer, 1024);
    bool result = ob.applyPacket({buffer, bytes});
    // printf("Generated packet of %zu bytes\n", bytes);
    // waler::hexdump(buffer, bytes);
    // printf("\n");

    // clear the console.
    printf("\033[2J\033[H");
    // ob.printAnsi(stdout, 25);
    ob.printDepthChart(stdout, 36, 36);
    usleep(10000);
  }

  return 0;
}