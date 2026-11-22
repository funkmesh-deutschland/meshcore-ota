#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "mcota_compact.h"

#include "mcota_sha256.h"

typedef struct {
  uint32_t at;
  bool wrapped;
} cursor_t;

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }
static uint32_t page_up(uint32_t v) { return (v + MCOTA_PAGE_SIZE - 1u) & ~(MCOTA_PAGE_SIZE - 1u); }
static uint32_t page_down(uint32_t v) { return v & ~(MCOTA_PAGE_SIZE - 1u); }

static uint32_t st_word(const mcota_compact_t *c, uint32_t index) {
  uint8_t w[4];
  c->flash->read(c->flash->ctx, c->status + 4u * index, w, 4u);
  return get32(w);
}

static void st_put(const mcota_compact_t *c, uint32_t index, uint32_t value) {
  uint8_t w[4];
  w[0] = (uint8_t)value; w[1] = (uint8_t)(value >> 8); w[2] = (uint8_t)(value >> 16); w[3] = (uint8_t)(value >> 24);
  c->flash->write(c->flash->ctx, c->status + 4u * index, w, 4u);
}

static uint32_t old_len(uint32_t old_size, uint32_t page) {
  uint32_t start = page * MCOTA_PAGE_SIZE;
  if (start >= old_size) return 0u;
  return old_size - start > MCOTA_PAGE_SIZE ? MCOTA_PAGE_SIZE : old_size - start;
}

static uint32_t block_offset(const mcota_manifest_t *m, uint32_t page) {
  uint32_t i, offset = 0;
  if (page > m->pages) page = m->pages;
  for (i = 0; i < page; i++) offset += align4(mcota_manifest_block_len(m, i));
  return offset;
}

static bool needs_save(const mcota_manifest_t *m, uint32_t page) {
  return page >= m->pages || !mcota_manifest_same(m, page);
}

bool mcota_compact_open(mcota_compact_t *c, const mcota_flash_t *flash, const mcota_layout_t *layout, uint32_t store) {
  uint8_t head[8];
  uint32_t len, old_pages, i;
  c->flash = flash;
  c->slot_a = layout->slot_a;
  c->pool_end = layout->pool_end;
  c->status = layout->status;
  c->store = store;
  if (store < c->slot_a || store >= c->pool_end || (store & (MCOTA_PAGE_SIZE - 1u))) return false;
  flash->read(flash->ctx, store, head, 8u);
  len = get16(head + 4u);
  if (get32(head) != MCOTA_STORE_MAGIC || len > MCOTA_MANIFEST_MAX || store + 8u + len > c->pool_end) return false;
  flash->read(flash->ctx, store + 8u, c->raw, len);
  if (!mcota_manifest_parse(c->raw, len, &c->manifest)) return false;
  c->old_size = c->manifest.old_size;
  old_pages = (c->old_size + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE;
  c->pages = old_pages > c->manifest.pages ? old_pages : c->manifest.pages;
  if (store != c->slot_a + c->pages * MCOTA_PAGE_SIZE) return false;
  c->blocks = store + 8u + align4(len) + 4u * c->manifest.pages;
  c->area1 = page_up(c->blocks + block_offset(&c->manifest, c->manifest.pages));
  c->area2 = page_up(c->blocks);
  if (c->area1 > c->pool_end) return false;
  c->tick = 0;
  c->inplace = false;
  c->checking = false;
  c->scratch_valid = false;
  c->current = 0;
  for (i = 0; i < MCOTA_COMPACT_CACHE; i++) c->view[i].used = false;
  for (i = 0; i < MCOTA_MAX_PAGES; i++) c->saved[i] = false;
  return true;
}

static bool place_in(const mcota_manifest_t *m, uint32_t blocks, uint32_t pool_end, cursor_t *cur, uint32_t page, uint32_t len, uint32_t *at) {
  uint32_t need = align4(len), limit;
  if (!cur->wrapped) {
    if (cur->at + need <= pool_end) {
      *at = cur->at;
      cur->at += need;
      return true;
    }
    cur->wrapped = true;
    cur->at = page_up(blocks);
  }
  limit = page_down(blocks + block_offset(m, page));
  if (cur->at + need > limit) return false;
  *at = cur->at;
  cur->at += need;
  return true;
}

static bool place(const mcota_compact_t *c, cursor_t *cur, uint32_t page, uint32_t len, uint32_t *at) {
  return place_in(&c->manifest, c->blocks, c->pool_end, cur, page, len, at);
}

uint32_t mcota_compact_plan(const mcota_flash_t *flash, uint32_t slot_a, uint32_t pool_end, uint32_t store, const mcota_manifest_t *m, uint8_t *page, uint8_t *packed, uint32_t packed_cap) {
  uint32_t old_pages = (m->old_size + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE, pages = old_pages > m->pages ? old_pages : m->pages;
  uint32_t blocks = store + 8u + align4(m->len) + 4u * m->pages, end = blocks + block_offset(m, m->pages), k, at;
  cursor_t cur;
  bool fits = page_up(end) <= pool_end;
  cur.at = page_up(end);
  cur.wrapped = false;
  for (k = 0; k < pages && fits; k++) {
    uint32_t len = old_len(m->old_size, k), n = 0;
    if (!needs_save(m, k)) continue;
    if (len > 0u) {
      flash->read(flash->ctx, slot_a + k * MCOTA_PAGE_SIZE, page, len);
      n = mcota_deflate(page, len, packed, packed_cap);
      if (n == 0u) fits = false;
    }
    if (fits && !place_in(m, blocks, pool_end, &cur, k, n, &at)) fits = false;
  }
  if (fits) return cur.wrapped ? MCOTA_PLAN_REUSE : MCOTA_PLAN_KEEP;
  if ((m->flags & MCOTA_MANIFEST_INPLACE) && page_up(end) + MCOTA_PAGE_SIZE <= pool_end) return MCOTA_PLAN_INPLACE;
  return MCOTA_PLAN_NONE;
}

static bool old_raw(mcota_compact_t *c, uint32_t page, uint8_t *out) {
  uint32_t len = old_len(c->old_size, page);
  if (len == 0u) return true;
  if (c->inplace) {
    if (c->saved[page] && page != c->current) return false;
    if (page == c->current && c->scratch_valid) {
      c->flash->read(c->flash->ctx, c->pool_end - MCOTA_PAGE_SIZE, out, len);
      return true;
    }
    c->flash->read(c->flash->ctx, c->slot_a + page * MCOTA_PAGE_SIZE, out, len);
    return true;
  }
  if (c->saved[page]) {
    c->flash->read(c->flash->ctx, c->saved_at[page], c->packed, c->saved_len[page]);
    return mcota_inflate(c->packed, c->saved_len[page], out, MCOTA_PAGE_SIZE) == (int32_t)len;
  }
  c->flash->read(c->flash->ctx, c->slot_a + page * MCOTA_PAGE_SIZE, out, len);
  return true;
}

static const uint8_t *view_page(mcota_compact_t *c, uint32_t page) {
  mcota_compact_view_t *slot = &c->view[0];
  uint32_t i;
  for (i = 0; i < MCOTA_COMPACT_CACHE; i++) {
    if (c->view[i].used && c->view[i].page == page) {
      c->view[i].age = ++c->tick;
      return c->view[i].data;
    }
  }
  for (i = 0; i < MCOTA_COMPACT_CACHE; i++) {
    if (!c->view[i].used) { slot = &c->view[i]; break; }
    if (c->view[i].age < slot->age) slot = &c->view[i];
  }
  slot->used = false;
  if (!old_raw(c, page, slot->data)) return 0;
  mcota_view_predict(&c->manifest.view, slot->data, old_len(c->old_size, page), page * MCOTA_PAGE_SIZE);
  slot->used = true;
  slot->page = page;
  slot->age = ++c->tick;
  return slot->data;
}

static void view_read(void *ctx, uint32_t pos, uint8_t *out, uint32_t len) {
  mcota_compact_t *c = (mcota_compact_t *)ctx;
  while (len > 0u) {
    uint32_t page = pos / MCOTA_PAGE_SIZE, offset = pos % MCOTA_PAGE_SIZE, take = MCOTA_PAGE_SIZE - offset, i;
    const uint8_t *data = view_page(c, page);
    if (take > len) take = len;
    for (i = 0; i < take; i++) out[i] = data ? data[offset + i] : 0u;
    out += take;
    pos += take;
    len -= take;
  }
}

static bool build_page(mcota_compact_t *c, uint32_t page, uint8_t *out) {
  uint32_t len = mcota_manifest_page_len(&c->manifest, page), block_len;
  if (mcota_manifest_same(&c->manifest, page)) return old_raw(c, page, out);
  block_len = mcota_manifest_block_len(&c->manifest, page);
  c->flash->read(c->flash->ctx, c->blocks + block_offset(&c->manifest, page), c->block, block_len);
  if (!mcota_block_apply(view_read, c, c->old_size, c->block, block_len, c->work, sizeof(c->work), out, len, page * MCOTA_PAGE_SIZE)) return false;
  mcota_view_restore(&c->manifest.view, out, len, page * MCOTA_PAGE_SIZE);
  return true;
}

static uint32_t pack_old(mcota_compact_t *c, uint32_t page) {
  uint32_t len = old_len(c->old_size, page);
  if (len == 0u) return 0u;
  c->flash->read(c->flash->ctx, c->slot_a + page * MCOTA_PAGE_SIZE, c->page, len);
  return mcota_deflate(c->page, len, c->packed, sizeof(c->packed));
}

static void write_page(const mcota_compact_t *c, uint32_t page, const uint8_t *data, uint32_t len);

static void forget_view(mcota_compact_t *c, uint32_t page) {
  uint32_t i;
  for (i = 0; i < MCOTA_COMPACT_CACHE; i++) {
    if (c->view[i].used && c->view[i].page == page) c->view[i].used = false;
  }
}

static uint32_t inplace_page(const mcota_compact_t *c, uint32_t step) {
  return (c->manifest.flags & MCOTA_MANIFEST_DESCENDING) ? c->manifest.pages - 1u - step : step;
}

static bool check_inplace(mcota_compact_t *c) {
  uint32_t step, i;
  bool ok = true;
  c->inplace = true;
  c->scratch_valid = false;
  for (step = 0; step < c->manifest.pages && ok; step++) {
    uint32_t page = inplace_page(c, step), len = mcota_manifest_page_len(&c->manifest, page);
    mcota_sha256_t sha;
    uint8_t got[32];
    const uint8_t *want;
    if (mcota_manifest_same(&c->manifest, page)) continue;
    c->current = page;
    forget_view(c, page);
    if (!build_page(c, page, c->page)) { ok = false; break; }
    mcota_sha256_init(&sha);
    mcota_sha256_update(&sha, c->page, len);
    mcota_sha256_final(&sha, got);
    want = mcota_manifest_page_hash(&c->manifest, page);
    for (i = 0; i < MCOTA_PAGE_HASH; i++) {
      if (got[i] != want[i]) ok = false;
    }
    c->saved[page] = true;
    forget_view(c, page);
  }
  for (i = 0; i < MCOTA_MAX_PAGES; i++) c->saved[i] = false;
  for (i = 0; i < MCOTA_COMPACT_CACHE; i++) c->view[i].used = false;
  c->inplace = false;
  return ok;
}

bool mcota_compact_inplace(mcota_compact_t *c) {
  const mcota_flash_t *f = c->flash;
  uint32_t step, scratch = c->pool_end - MCOTA_PAGE_SIZE, i;
  c->inplace = true;
  for (step = 0; step < c->manifest.pages; step++) {
    uint32_t page = inplace_page(c, step), journal = MCOTA_W_JOURNAL_SWAP + 3u * page, len = old_len(c->old_size, page);
    if (mcota_manifest_same(&c->manifest, page) || st_word(c, journal + 1u) == 0u) continue;
    if (st_word(c, journal) != 0u) {
      uint8_t buf[64];
      uint32_t pos;
      f->erase(f->ctx, scratch);
      for (pos = 0; pos < len; pos += sizeof(buf)) {
        uint32_t take = len - pos > sizeof(buf) ? (uint32_t)sizeof(buf) : len - pos, total = align4(take);
        f->read(f->ctx, c->slot_a + page * MCOTA_PAGE_SIZE + pos, buf, take);
        for (i = take; i < total; i++) buf[i] = 0xFFu;
        f->write(f->ctx, scratch + pos, buf, total);
      }
      st_put(c, journal, 0u);
    }
    c->current = page;
    c->scratch_valid = true;
    forget_view(c, page);
    if (!build_page(c, page, c->page)) return false;
    write_page(c, page, c->page, mcota_manifest_page_len(&c->manifest, page));
    st_put(c, journal + 1u, 0u);
    c->scratch_valid = false;
    forget_view(c, page);
  }
  return true;
}

uint32_t mcota_compact_check(mcota_compact_t *c, const uint8_t *new_hash, uint32_t new_size, uint32_t old_size) {
  mcota_sha256_t sha;
  cursor_t cur;
  uint8_t got[32];
  uint32_t page, at, i, plan;
  if (new_size != c->manifest.new_size || old_size != c->manifest.old_size) return MCOTA_PLAN_NONE;
  for (i = 0; i < 32u; i++) {
    if (c->manifest.new_hash[i] != new_hash[i]) return MCOTA_PLAN_NONE;
  }
  plan = mcota_compact_plan(c->flash, c->slot_a, c->pool_end, c->store, &c->manifest, c->page, c->packed, sizeof(c->packed));
  if (plan == MCOTA_PLAN_NONE) return plan;
  if (plan == MCOTA_PLAN_INPLACE) return check_inplace(c) ? plan : MCOTA_PLAN_NONE;
  cur.at = c->area1;
  cur.wrapped = false;
  mcota_sha256_init(&sha);
  for (page = 0; page < c->pages; page++) {
    if (needs_save(&c->manifest, page)) {
      uint32_t n = pack_old(c, page);
      if ((n == 0u && old_len(c->old_size, page) > 0u) || !place(c, &cur, page, n, &at)) return MCOTA_PLAN_NONE;
    }
    if (page < c->manifest.pages) {
      if (!build_page(c, page, c->page)) return MCOTA_PLAN_NONE;
      mcota_sha256_update(&sha, c->page, mcota_manifest_page_len(&c->manifest, page));
    }
  }
  mcota_sha256_final(&sha, got);
  for (i = 0; i < 32u; i++) {
    if (got[i] != new_hash[i]) return MCOTA_PLAN_NONE;
  }
  return plan;
}

static void ring_write(const mcota_compact_t *c, uint32_t at, const uint8_t *data, uint32_t len) {
  const mcota_flash_t *f = c->flash;
  uint8_t buf[64];
  while (len > 0u) {
    uint32_t take = MCOTA_PAGE_SIZE - (at & (MCOTA_PAGE_SIZE - 1u)), total, i;
    if (take > sizeof(buf)) take = sizeof(buf);
    if (take > len) take = len;
    if ((at & (MCOTA_PAGE_SIZE - 1u)) == 0u) f->erase(f->ctx, at);
    total = align4(take);
    for (i = 0; i < total; i++) buf[i] = i < take ? data[i] : 0xFFu;
    f->write(f->ctx, at, buf, total);
    at += take;
    data += take;
    len -= take;
  }
}

static void write_page(const mcota_compact_t *c, uint32_t page, const uint8_t *data, uint32_t len) {
  const mcota_flash_t *f = c->flash;
  uint32_t addr = c->slot_a + page * MCOTA_PAGE_SIZE, pos;
  uint8_t buf[64];
  f->erase(f->ctx, addr);
  for (pos = 0; pos < len; pos += sizeof(buf)) {
    uint32_t take = len - pos > sizeof(buf) ? (uint32_t)sizeof(buf) : len - pos, total = align4(take), i;
    for (i = 0; i < total; i++) buf[i] = i < take ? data[pos + i] : 0xFFu;
    f->write(f->ctx, addr + pos, buf, total);
  }
}

static void mark_consumed(const mcota_compact_t *c) {
  uint8_t head[8];
  c->flash->read(c->flash->ctx, c->store, head, 8u);
  if (head[6] == 0u && head[7] == 0u) return;
  head[6] = 0u;
  head[7] = 0u;
  c->flash->write(c->flash->ctx, c->store + 4u, head + 4u, 4u);
}

bool mcota_compact_swap(mcota_compact_t *c) {
  cursor_t cur;
  uint32_t page;
  cur.at = c->area1;
  cur.wrapped = false;
  for (page = 0; page < c->pages; page++) {
    uint32_t journal = MCOTA_W_JOURNAL_SWAP + 3u * page, n, at;
    if (!needs_save(&c->manifest, page)) continue;
    if (st_word(c, journal + 2u) != 0u) {
      n = pack_old(c, page);
      if ((n == 0u && old_len(c->old_size, page) > 0u) || !place(c, &cur, page, n, &at)) return false;
      if (cur.wrapped) mark_consumed(c);
      ring_write(c, at, c->packed, n);
      st_put(c, journal, n);
      st_put(c, journal + 2u, 0u);
    } else {
      n = st_word(c, journal);
      if (n > MCOTA_COMPACT_PACKED || !place(c, &cur, page, n, &at)) return false;
    }
    c->saved[page] = true;
    c->saved_len[page] = (uint16_t)n;
    c->saved_at[page] = at;
    if (st_word(c, journal + 1u) != 0u) {
      uint32_t len = 0;
      if (page < c->manifest.pages) {
        if (!build_page(c, page, c->page)) return false;
        len = mcota_manifest_page_len(&c->manifest, page);
      }
      write_page(c, page, c->page, len);
      st_put(c, journal + 1u, 0u);
    }
  }
  return true;
}

void mcota_compact_revert(mcota_compact_t *c) {
  cursor_t cur;
  uint32_t page;
  cur.at = c->area1;
  cur.wrapped = false;
  for (page = 0; page < c->pages; page++) {
    uint32_t journal = MCOTA_W_JOURNAL_SWAP + 3u * page, n, at;
    if (!needs_save(&c->manifest, page) || st_word(c, journal + 2u) != 0u) continue;
    n = st_word(c, journal);
    if (n > MCOTA_COMPACT_PACKED || !place(c, &cur, page, n, &at)) return;
    c->saved[page] = true;
    c->saved_len[page] = (uint16_t)n;
    c->saved_at[page] = at;
    if (st_word(c, MCOTA_W_JOURNAL_REVERT + 3u * page) != 0u) {
      if (!old_raw(c, page, c->page)) return;
      write_page(c, page, c->page, old_len(c->old_size, page));
      st_put(c, MCOTA_W_JOURNAL_REVERT + 3u * page, 0u);
    }
  }
}
