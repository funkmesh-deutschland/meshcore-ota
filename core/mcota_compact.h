#ifndef MCOTA_COMPACT_H
#define MCOTA_COMPACT_H

#include <stdbool.h>
#include <stdint.h>

#include "mcota_boot.h"
#include "mcota_manifest.h"
#include "mcota_patch.h"

#define MCOTA_COMPACT_CACHE 8u
#define MCOTA_COMPACT_PACKED (MCOTA_PAGE_SIZE + 64u)
#define MCOTA_PLAN_NONE 0u
#define MCOTA_PLAN_KEEP 1u
#define MCOTA_PLAN_REUSE 2u
#define MCOTA_PLAN_INPLACE 3u

typedef struct {
  bool used;
  uint32_t page;
  uint32_t age;
  uint8_t data[MCOTA_PAGE_SIZE];
} mcota_compact_view_t;

typedef struct {
  const mcota_flash_t *flash;
  uint32_t slot_a;
  uint32_t pool_end;
  uint32_t status;
  uint32_t store;
  uint32_t blocks;
  uint32_t area1;
  uint32_t area2;
  uint32_t pages;
  uint32_t old_size;
  uint32_t tick;
  bool inplace;
  bool checking;
  bool scratch_valid;
  uint32_t current;
  mcota_manifest_t manifest;
  bool saved[MCOTA_MAX_PAGES];
  uint16_t saved_len[MCOTA_MAX_PAGES];
  uint32_t saved_at[MCOTA_MAX_PAGES];
  uint8_t raw[MCOTA_MANIFEST_MAX];
  uint8_t block[MCOTA_BLOCK_MAX];
  uint8_t work[MCOTA_BLOCK_MAX];
  uint8_t page[MCOTA_PAGE_SIZE];
  uint8_t packed[MCOTA_COMPACT_PACKED];
  mcota_compact_view_t view[MCOTA_COMPACT_CACHE];
} mcota_compact_t;

bool mcota_compact_open(mcota_compact_t *c, const mcota_flash_t *flash, const mcota_layout_t *layout, uint32_t store);
uint32_t mcota_compact_check(mcota_compact_t *c, const uint8_t *new_hash, uint32_t new_size, uint32_t old_size);
bool mcota_compact_inplace(mcota_compact_t *c);
uint32_t mcota_compact_plan(const mcota_flash_t *flash, uint32_t slot_a, uint32_t pool_end, uint32_t store, const mcota_manifest_t *m, uint8_t *page, uint8_t *packed, uint32_t packed_cap);
bool mcota_compact_swap(mcota_compact_t *c);
void mcota_compact_revert(mcota_compact_t *c);

#endif
