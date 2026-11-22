#ifndef MCOTA_SHA256_H
#define MCOTA_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t state[8];
  uint64_t length;
  uint8_t block[64];
  uint32_t fill;
} mcota_sha256_t;

void mcota_sha256_init(mcota_sha256_t *s);
void mcota_sha256_update(mcota_sha256_t *s, const uint8_t *data, size_t len);
void mcota_sha256_final(mcota_sha256_t *s, uint8_t out[32]);

#endif
