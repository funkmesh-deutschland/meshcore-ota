#include <stdio.h>
#include <string.h>

#include "../core/mcota_sha256.h"

static int check(const char *text, size_t repeat, const char *expected) {
  mcota_sha256_t s;
  uint8_t out[32];
  char hex[65];
  size_t i;
  mcota_sha256_init(&s);
  for (i = 0; i < repeat; i++) mcota_sha256_update(&s, (const uint8_t *)text, strlen(text));
  mcota_sha256_final(&s, out);
  for (i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", out[i]);
  if (strcmp(hex, expected) != 0) {
    printf("ERROR: SHA-256 wrong: %s\n", hex);
    return 1;
  }
  return 0;
}

int main(void) {
  int failures = 0;
  failures += check("", 1, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  failures += check("abc", 1, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  failures += check("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 1, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  failures += check("a", 1000000, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
  printf("SHA-256: %d errors\n", failures);
  return failures;
}
