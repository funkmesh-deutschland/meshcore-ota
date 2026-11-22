#ifndef MCOTA_PATCH_H
#define MCOTA_PATCH_H

#include <stdint.h>

#include "mcota_boot.h"

#define MCOTA_BLOCK_MAX 4800u
#define MCOTA_BLOCK_OPS_MAX 80u

int32_t mcota_inflate(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t out_cap);
uint32_t mcota_deflate(const uint8_t *in, uint32_t len, uint8_t *out, uint32_t cap);
typedef void (*mcota_old_read_t)(void *ctx, uint32_t pos, uint8_t *out, uint32_t len);

bool mcota_block_apply(mcota_old_read_t old_read, void *ctx, uint32_t old_size,
                      const uint8_t *block, uint32_t block_len, uint8_t *work, uint32_t work_cap,
                      uint8_t *page, uint32_t page_len, uint32_t page_pos);
typedef struct {
  const uint8_t *shifts;
  uint32_t shift_count;
  uint32_t base;
  uint32_t old_code;
  uint32_t old_code_end;
  uint32_t new_code;
  uint32_t new_code_end;
  bool thumb;
  bool xtensa;
} mcota_view_t;

const uint8_t *mcota_view_parse(mcota_view_t *v, const uint8_t *p, const uint8_t *end, bool thumb, bool xtensa);
void mcota_view_predict(const mcota_view_t *v, uint8_t *data, uint32_t len, uint32_t pos);
void mcota_view_restore(const mcota_view_t *v, uint8_t *data, uint32_t len, uint32_t pos);

#endif
