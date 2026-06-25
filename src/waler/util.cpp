#include <waler/util.hpp>
#include <stdio.h>
#include <ctype.h>

namespace waler {

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