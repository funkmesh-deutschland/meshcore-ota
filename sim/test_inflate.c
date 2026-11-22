#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_patch.h"

static int unhex(const char *text, uint8_t *out) {
  int n = 0;
  unsigned v;
  while (text[0] && text[1] && sscanf(text, "%2x", &v) == 1) { out[n++] = (uint8_t)v; text += 2; }
  return n;
}

int main(int argc, char **argv) {
  static char line[40000], packed_text[20000], plain_text[20000];
  static uint8_t packed[10000], plain[10000], out[4800];
  FILE *fh = fopen(argc > 1 ? argv[1] : "fixture/inflate.txt", "r");
  int cases = 0, failures = 0;
  if (!fh) return 2;
  while (fgets(line, sizeof(line), fh)) {
    int packed_len, plain_len, got;
    if (sscanf(line, "%19999s %19999s", packed_text, plain_text) != 2) continue;
    packed_len = unhex(packed_text, packed);
    plain_len = strcmp(plain_text, "-") == 0 ? 0 : unhex(plain_text, plain);
    got = mcota_inflate(packed, (uint32_t)packed_len, out, sizeof(out));
    cases++;
    if (got != plain_len || memcmp(out, plain, (size_t)plain_len) != 0) failures++;
    if (packed_len > 4) {
      packed[packed_len / 2] ^= 0x55u;
      mcota_inflate(packed, (uint32_t)packed_len, out, sizeof(out));
      mcota_inflate(packed, (uint32_t)packed_len / 2u, out, 64u);
    }
  }
  fclose(fh);
  {
    static uint8_t image[1 << 20], squeezed[5200], back[4096];
    FILE *img = argc > 2 ? fopen(argv[2], "rb") : NULL;
    size_t size = img ? fread(image, 1, sizeof(image), img) : 0, at;
    unsigned long total = 0;
    if (img) fclose(img);
    for (at = 0; at < size; at += 4096u) {
      uint32_t len = size - at < 4096u ? (uint32_t)(size - at) : 4096u;
      uint32_t n = mcota_deflate(image + at, len, squeezed, sizeof(squeezed));
      cases++;
      total += n;
      if (argc > 3) {
        FILE *dump = fopen(argv[3], at == 0 ? "wb" : "ab");
        uint8_t head[4] = { (uint8_t)n, (uint8_t)(n >> 8), (uint8_t)len, (uint8_t)(len >> 8) };
        if (dump) { fwrite(head, 1, 4, dump); fwrite(squeezed, 1, n, dump); fclose(dump); }
      }
      if (n == 0u || mcota_inflate(squeezed, n, back, sizeof(back)) != (int)len || memcmp(back, image + at, len) != 0) failures++;
    }
    if (size) printf("deflate of the old firmware: %lu of %lu bytes (%lu %%)\n", total, (unsigned long)size, total * 100u / (unsigned long)size);
  }
  printf("inflate: %d cases, %d errors\n", cases, failures);
  return failures || cases < 100 ? 1 : 0;
}
