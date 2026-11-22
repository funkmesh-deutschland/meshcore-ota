#ifndef MCOTA_MANIFEST_H
#define MCOTA_MANIFEST_H

#include <stdbool.h>
#include <stdint.h>

#include "mcota_boot.h"
#include "mcota_patch.h"

#define MCOTA_MANIFEST_MAGIC 0x41544F4Cu
#define MCOTA_MANIFEST_FORMAT 5u
#define MCOTA_SHIFT_BYTES 1200u
#define MCOTA_MANIFEST_HEAD 124u
#define MCOTA_MANIFEST_ENTRY 10u
#define MCOTA_MANIFEST_SIG 64u
#define MCOTA_ITEM_MAX 4864u
#if MCOTA_MAX_PAGES > 256u
#define MCOTA_MANIFEST_MAX MCOTA_ITEM_MAX
#else
#define MCOTA_MANIFEST_MAX (MCOTA_MANIFEST_HEAD + MCOTA_MAX_PAGES / 8u + MCOTA_MAX_PAGES * MCOTA_MANIFEST_ENTRY + MCOTA_SHIFT_BYTES + MCOTA_MANIFEST_SIG)
#endif
#define MCOTA_PAGE_HASH 8u
#define MCOTA_STORE_MAGIC 0x32544F4Cu
#define MCOTA_MANIFEST_AUTO_APPLY 1u
#define MCOTA_MANIFEST_INPLACE 2u
#define MCOTA_MANIFEST_DESCENDING 4u
#define MCOTA_MANIFEST_NO_ROLLBACK 8u
#define MCOTA_MANIFEST_THUMB 16u
#define MCOTA_MANIFEST_XTENSA 32u

typedef struct {
  const uint8_t *raw;
  uint32_t len;
  uint32_t pages;
  uint32_t new_size;
  uint32_t old_size;
  uint32_t build;
  uint8_t flags;
  const char *board;
  const char *version;
  const uint8_t *new_hash;
  const uint8_t *old_hash;
  const uint8_t *key_id;
  const uint8_t *signature;
  const uint8_t *same;
  const uint8_t *table;
  uint32_t entries;
  mcota_view_t view;
} mcota_manifest_t;

bool mcota_manifest_parse(const uint8_t *raw, uint32_t len, mcota_manifest_t *out);
bool mcota_manifest_same(const mcota_manifest_t *m, uint32_t page);
uint32_t mcota_manifest_block_len(const mcota_manifest_t *m, uint32_t page);
uint32_t mcota_manifest_block_offset(const mcota_manifest_t *m, uint32_t page);
const uint8_t *mcota_manifest_page_hash(const mcota_manifest_t *m, uint32_t page);
uint32_t mcota_manifest_page_len(const mcota_manifest_t *m, uint32_t page);

#endif
