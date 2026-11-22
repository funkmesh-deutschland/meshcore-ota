#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "mcota_manifest.h"

#include "mcota_boot.h"
#include "mcota_patch.h"

static uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd16(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

bool mcota_manifest_same(const mcota_manifest_t *m, uint32_t page) {
  return (m->same[page >> 3] >> (page & 7u)) & 1u;
}

static const uint8_t *entry(const mcota_manifest_t *m, uint32_t page) {
  uint32_t i, rank = 0;
  for (i = 0; i < page; i++) {
    if (!mcota_manifest_same(m, i)) rank++;
  }
  return m->table + rank * MCOTA_MANIFEST_ENTRY;
}

bool mcota_manifest_parse(const uint8_t *raw, uint32_t len, mcota_manifest_t *m) {
  uint32_t i, bytes;
  if (len < MCOTA_MANIFEST_HEAD + 2u + MCOTA_MANIFEST_ENTRY + MCOTA_MANIFEST_SIG || len > MCOTA_MANIFEST_MAX) return false;
  if (rd32(raw) != MCOTA_MANIFEST_MAGIC || raw[4] != MCOTA_MANIFEST_FORMAT) return false;
  m->raw = raw;
  m->len = len;
  m->flags = raw[5];
  m->pages = rd16(raw + 6u);
  m->new_size = rd32(raw + 8u);
  m->old_size = rd32(raw + 12u);
  m->build = rd32(raw + 16u);
  m->board = (const char *)raw + 20u;
  m->version = (const char *)raw + 36u;
  m->new_hash = raw + 52u;
  m->old_hash = raw + 84u;
  m->key_id = raw + 116u;
  if (m->pages == 0u || m->pages > MCOTA_MAX_PAGES) return false;
  bytes = (m->pages + 7u) / 8u;
  if (len < MCOTA_MANIFEST_HEAD + bytes + 1u + MCOTA_MANIFEST_SIG) return false;
  m->same = raw + MCOTA_MANIFEST_HEAD;
  m->table = m->same + bytes;
  m->entries = 0;
  if (m->new_size == 0u || (m->new_size + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE != m->pages) return false;
  if (m->old_size == 0u || m->old_size > MCOTA_MAX_PAGES * MCOTA_PAGE_SIZE) return false;
  if (raw[35] != 0u || raw[51] != 0u) return false;
  for (i = 0; i < m->pages; i++) {
    if (!mcota_manifest_same(m, i)) m->entries++;
    else if (i * MCOTA_PAGE_SIZE + mcota_manifest_page_len(m, i) > m->old_size) return false;
  }
  for (i = m->pages; i < bytes * 8u; i++) {
    if (mcota_manifest_same(m, i)) return false;
  }
  if (m->entries == 0u || len < MCOTA_MANIFEST_HEAD + bytes + m->entries * MCOTA_MANIFEST_ENTRY + 2u + MCOTA_MANIFEST_SIG) return false;
  m->signature = raw + len - MCOTA_MANIFEST_SIG;
  if ((uint32_t)(m->signature - (m->table + m->entries * MCOTA_MANIFEST_ENTRY)) > MCOTA_SHIFT_BYTES) return false;
  if (mcota_view_parse(&m->view, m->table + m->entries * MCOTA_MANIFEST_ENTRY, m->signature,
                      (m->flags & MCOTA_MANIFEST_THUMB) != 0u, (m->flags & MCOTA_MANIFEST_XTENSA) != 0u) != m->signature) return false;
  for (i = 0; i < m->entries; i++) {
    uint32_t n = rd16(m->table + i * MCOTA_MANIFEST_ENTRY);
    if (n == 0u || n > MCOTA_BLOCK_MAX) return false;
  }
  return true;
}

uint32_t mcota_manifest_block_len(const mcota_manifest_t *m, uint32_t page) {
  if (mcota_manifest_same(m, page)) return 0u;
  return rd16(entry(m, page));
}

uint32_t mcota_manifest_block_offset(const mcota_manifest_t *m, uint32_t page) {
  uint32_t i, offset = 0;
  for (i = 0; i < page; i++) offset += mcota_manifest_block_len(m, i);
  return offset;
}

const uint8_t *mcota_manifest_page_hash(const mcota_manifest_t *m, uint32_t page) {
  return entry(m, page) + 2u;
}

uint32_t mcota_manifest_page_len(const mcota_manifest_t *m, uint32_t page) {
  if (page + 1u < m->pages) return MCOTA_PAGE_SIZE;
  return m->new_size - page * MCOTA_PAGE_SIZE;
}
