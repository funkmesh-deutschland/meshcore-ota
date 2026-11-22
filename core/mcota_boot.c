#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "mcota_boot.h"
#include "mcota_sha256.h"
#ifdef MCOTA_BOOT_COMPACT
#include "mcota_compact.h"
#endif

static uint32_t word(const mcota_flash_t *f, const mcota_layout_t *l, uint32_t index) {
  uint32_t v;
  f->read(f->ctx, l->status + 4u * index, &v, 4u);
  return v;
}

static bool marked(const mcota_flash_t *f, const mcota_layout_t *l, uint32_t index) {
  return word(f, l, index) == 0u;
}

static void mark(const mcota_flash_t *f, const mcota_layout_t *l, uint32_t index) {
  uint32_t zero = 0u;
  f->write(f->ctx, l->status + 4u * index, &zero, 4u);
}

static uint32_t checksum(const uint32_t *words, uint32_t count) {
  uint32_t crc = 0xFFFFFFFFu, i, bit;
  for (i = 0; i < count * 4u; i++) {
    crc ^= ((const uint8_t *)words)[i];
    for (bit = 0; bit < 8u; bit++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

static bool header(const mcota_flash_t *f, const mcota_layout_t *l, uint32_t out[MCOTA_W_CHECK]) {
  uint32_t i, pages;
  for (i = 0; i < MCOTA_W_CHECK; i++) out[i] = word(f, l, i);
  if (out[MCOTA_W_MAGIC] != MCOTA_MAGIC || out[MCOTA_W_FORMAT] != MCOTA_FORMAT) return false;
  if (checksum(out, MCOTA_W_CHECK) != word(f, l, MCOTA_W_CHECK)) return false;
  pages = out[MCOTA_W_PAGES];
  if (pages == 0u || pages > l->slot_pages || pages > MCOTA_MAX_PAGES) return false;
  if (out[MCOTA_W_NEW_SIZE] == 0u || out[MCOTA_W_NEW_SIZE] > pages * MCOTA_PAGE_SIZE) return false;
  if (out[MCOTA_W_OLD_SIZE] == 0u || out[MCOTA_W_OLD_SIZE] > pages * MCOTA_PAGE_SIZE) return false;
  return marked(f, l, MCOTA_W_ARMED);
}

void mcota_hash_region(const mcota_flash_t *f, uint32_t addr, uint32_t len, uint8_t out[32]) {
  mcota_sha256_t s;
  uint8_t buf[256];
  mcota_sha256_init(&s);
  while (len > 0u) {
    uint32_t n = len > sizeof(buf) ? (uint32_t)sizeof(buf) : len;
    f->read(f->ctx, addr, buf, n);
    mcota_sha256_update(&s, buf, n);
    addr += n;
    len -= n;
  }
  mcota_sha256_final(&s, out);
}

static bool matches(const mcota_flash_t *f, uint32_t addr, uint32_t len, const uint32_t *expected) {
  uint8_t got[32];
  uint32_t i;
  mcota_hash_region(f, addr, len, got);
  for (i = 0; i < 32u; i++) {
    if (got[i] != ((const uint8_t *)expected)[i]) return false;
  }
  return true;
}

static void copy_page(const mcota_flash_t *f, uint32_t from, uint32_t to) {
  uint8_t buf[256];
  uint32_t offset;
  f->erase(f->ctx, to);
  for (offset = 0; offset < MCOTA_PAGE_SIZE; offset += sizeof(buf)) {
    f->read(f->ctx, from + offset, buf, sizeof(buf));
    f->write(f->ctx, to + offset, buf, sizeof(buf));
  }
}

static void exchange(const mcota_flash_t *f, const mcota_layout_t *l, uint32_t pages, uint32_t journal) {
  uint32_t i;
  for (i = 0; i < pages; i++) {
    uint32_t a = l->slot_a + i * MCOTA_PAGE_SIZE;
    uint32_t b = l->slot_b + i * MCOTA_PAGE_SIZE;
    uint32_t j = journal + 3u * i;
    if (!marked(f, l, j)) {
      copy_page(f, a, l->scratch);
      mark(f, l, j);
    }
    if (!marked(f, l, j + 1u)) {
      copy_page(f, b, a);
      mark(f, l, j + 1u);
    }
    if (!marked(f, l, j + 2u)) {
      copy_page(f, l->scratch, b);
      mark(f, l, j + 2u);
    }
  }
}

#ifdef MCOTA_BOOT_COMPACT
static bool compact_open(const mcota_flash_t *f, const mcota_layout_t *l) {
  return l->compact && mcota_compact_open((mcota_compact_t *)l->compact, f, l, word(f, l, MCOTA_W_STORE));
}

static uint32_t compact_check(const mcota_layout_t *l, const uint32_t *h) {
  return mcota_compact_check((mcota_compact_t *)l->compact, (const uint8_t *)&h[MCOTA_W_NEW_HASH], h[MCOTA_W_NEW_SIZE], h[MCOTA_W_OLD_SIZE]);
}

static bool compact_swap(const mcota_layout_t *l, bool inplace) {
  return inplace ? mcota_compact_inplace((mcota_compact_t *)l->compact) : mcota_compact_swap((mcota_compact_t *)l->compact);
}

static void compact_revert(const mcota_layout_t *l) {
  mcota_compact_revert((mcota_compact_t *)l->compact);
}
#else
static bool compact_open(const mcota_flash_t *f, const mcota_layout_t *l) { (void)f; (void)l; return false; }
static uint32_t compact_check(const mcota_layout_t *l, const uint32_t *h) { (void)l; (void)h; return 0u; }
static bool compact_swap(const mcota_layout_t *l, bool inplace) { (void)l; (void)inplace; return false; }
static void compact_revert(const mcota_layout_t *l) { (void)l; }
#endif

static void revert(const mcota_flash_t *f, const mcota_layout_t *l, uint32_t pages) {
  if (!marked(f, l, MCOTA_W_REVERT_STARTED)) mark(f, l, MCOTA_W_REVERT_STARTED);
  if (word(f, l, MCOTA_W_COMPACT) == MCOTA_MODE_COMPACT) {
    if (!compact_open(f, l)) return;
    compact_revert(l);
  } else {
    exchange(f, l, pages, MCOTA_W_JOURNAL_REVERT);
  }
  mark(f, l, MCOTA_W_REVERT_DONE);
}

uint32_t mcota_trials(const mcota_flash_t *f, const mcota_layout_t *l) {
  uint32_t n = 0;
  while (n < MCOTA_MAX_TRIALS && marked(f, l, MCOTA_W_TRIAL + n)) n++;
  return n;
}

static void boot_direct(const mcota_flash_t *f, const mcota_layout_t *l, const uint32_t *h) {
  bool runs_new;
  uint32_t trials;
  if (marked(f, l, MCOTA_W_CONFIRMED) && !marked(f, l, MCOTA_W_REVERT_STARTED)) return;
  runs_new = matches(f, l->slot_a, h[MCOTA_W_NEW_SIZE], &h[MCOTA_W_NEW_HASH]);
  if (marked(f, l, MCOTA_W_REVERT_STARTED)) {
    if (runs_new) { if (l->activate) l->activate(l->activate_ctx, false); }
    else mark(f, l, MCOTA_W_REVERT_DONE);
    return;
  }
  if (!marked(f, l, MCOTA_W_SWAP_DONE)) {
    if (!runs_new) {
      if (marked(f, l, MCOTA_W_STARTED)) {
        mark(f, l, MCOTA_W_REVERT_STARTED);
        mark(f, l, MCOTA_W_REVERT_DONE);
        return;
      }
      if (!matches(f, l->slot_a, h[MCOTA_W_OLD_SIZE], &h[MCOTA_W_OLD_HASH]) || !matches(f, l->slot_b, h[MCOTA_W_NEW_SIZE], &h[MCOTA_W_NEW_HASH])) {
        mark(f, l, MCOTA_W_REJECTED);
        return;
      }
      mark(f, l, MCOTA_W_STARTED);
      if (l->activate) l->activate(l->activate_ctx, true);
      return;
    }
    mark(f, l, MCOTA_W_SWAP_DONE);
  }
  if (!runs_new) {
    mark(f, l, MCOTA_W_REVERT_STARTED);
    mark(f, l, MCOTA_W_REVERT_DONE);
    return;
  }
  trials = mcota_trials(f, l);
  if (trials >= MCOTA_MAX_TRIALS) {
    mark(f, l, MCOTA_W_REVERT_STARTED);
    if (l->activate) l->activate(l->activate_ctx, false);
    return;
  }
  mark(f, l, MCOTA_W_TRIAL + trials);
}

void mcota_boot(const mcota_flash_t *f, const mcota_layout_t *l) {
  uint32_t h[MCOTA_W_CHECK];
  uint32_t pages, trials;
  if (!header(f, l, h)) return;
  if (marked(f, l, MCOTA_W_REJECTED) || marked(f, l, MCOTA_W_REVERT_DONE)) return;
  if (h[MCOTA_W_COMPACT] == MCOTA_MODE_DIRECT) {
    boot_direct(f, l, h);
    return;
  }
  pages = h[MCOTA_W_PAGES];

  if (marked(f, l, MCOTA_W_REVERT_STARTED)) {
    revert(f, l, pages);
    return;
  }

  if (!marked(f, l, MCOTA_W_SWAP_DONE)) {
    bool compact = h[MCOTA_W_COMPACT] == MCOTA_MODE_COMPACT;
    if (compact && !compact_open(f, l)) {
      if (!marked(f, l, MCOTA_W_STARTED)) mark(f, l, MCOTA_W_REJECTED);
      return;
    }
    if (!marked(f, l, MCOTA_W_STARTED)) {
      bool ok = matches(f, l->slot_a, h[MCOTA_W_OLD_SIZE], &h[MCOTA_W_OLD_HASH]);
      if (ok && compact) {
        uint32_t plan = compact_check(l, h);
        ok = plan != 0u;
        if (plan == 3u) mark(f, l, MCOTA_W_INPLACE);
      } else if (ok) {
        ok = matches(f, l->slot_b, h[MCOTA_W_NEW_SIZE], &h[MCOTA_W_NEW_HASH]);
      }
      if (!ok) {
        mark(f, l, MCOTA_W_REJECTED);
        return;
      }
      mark(f, l, MCOTA_W_STARTED);
    }
    if (compact && marked(f, l, MCOTA_W_INPLACE)) {
      if (!compact_swap(l, true)) return;
    } else if (compact) {
      if (!compact_swap(l, false)) {
        revert(f, l, pages);
        return;
      }
    } else {
      exchange(f, l, pages, MCOTA_W_JOURNAL_SWAP);
    }
    if (!matches(f, l->slot_a, h[MCOTA_W_NEW_SIZE], &h[MCOTA_W_NEW_HASH])) {
      revert(f, l, pages);
      return;
    }
    mark(f, l, MCOTA_W_SWAP_DONE);
  }

  if (marked(f, l, MCOTA_W_INPLACE) && !marked(f, l, MCOTA_W_CONFIRMED)) mark(f, l, MCOTA_W_CONFIRMED);
  if (marked(f, l, MCOTA_W_CONFIRMED)) return;
  trials = mcota_trials(f, l);
  if (trials >= MCOTA_MAX_TRIALS) {
    revert(f, l, pages);
    return;
  }
  mark(f, l, MCOTA_W_TRIAL + trials);
}

mcota_state_t mcota_state(const mcota_flash_t *f, const mcota_layout_t *l) {
  uint32_t h[MCOTA_W_CHECK];
  if (!header(f, l, h)) return MCOTA_STATE_NONE;
  if (marked(f, l, MCOTA_W_REJECTED)) return MCOTA_STATE_REJECTED;
  if (marked(f, l, MCOTA_W_REVERT_DONE)) return MCOTA_STATE_REVERTED;
  if (marked(f, l, MCOTA_W_REVERT_STARTED)) return MCOTA_STATE_ARMED;
  if (!marked(f, l, MCOTA_W_SWAP_DONE)) return MCOTA_STATE_ARMED;
  if (marked(f, l, MCOTA_W_CONFIRMED)) return MCOTA_STATE_CONFIRMED;
  return MCOTA_STATE_TRIAL;
}

bool mcota_request(const mcota_flash_t *f, const mcota_layout_t *l, const mcota_request_t *r) {
  uint32_t h[MCOTA_W_CHECK + 1u];
  uint32_t i;
  mcota_state_t state = mcota_state(f, l);
  if (state == MCOTA_STATE_ARMED || state == MCOTA_STATE_TRIAL) return false;
  if (r->pages == 0u || r->pages > l->slot_pages || r->pages > MCOTA_MAX_PAGES) return false;
  if (r->new_size == 0u || r->new_size > r->pages * MCOTA_PAGE_SIZE) return false;
  if (r->old_size == 0u || r->old_size > r->pages * MCOTA_PAGE_SIZE) return false;
  h[MCOTA_W_MAGIC] = MCOTA_MAGIC;
  h[MCOTA_W_FORMAT] = MCOTA_FORMAT;
  h[MCOTA_W_PAGES] = r->pages;
  h[MCOTA_W_NEW_SIZE] = r->new_size;
  h[MCOTA_W_OLD_SIZE] = r->old_size;
  h[MCOTA_W_COMPACT] = r->direct ? MCOTA_MODE_DIRECT : r->compact ? MCOTA_MODE_COMPACT : MCOTA_MODE_SWAP;
  h[MCOTA_W_STORE] = r->compact ? r->store : 0u;
  for (i = 0; i < 32u; i++) {
    ((uint8_t *)&h[MCOTA_W_NEW_HASH])[i] = r->new_hash[i];
    ((uint8_t *)&h[MCOTA_W_OLD_HASH])[i] = r->old_hash[i];
  }
  h[MCOTA_W_CHECK] = checksum(h, MCOTA_W_CHECK);
  for (i = 0; i < MCOTA_STATUS_PAGES; i++) f->erase(f->ctx, l->status + i * MCOTA_PAGE_SIZE);
  f->write(f->ctx, l->status, h, sizeof(h));
  mark(f, l, MCOTA_W_ARMED);
  return mcota_state(f, l) == MCOTA_STATE_ARMED;
}

bool mcota_request_read(const mcota_flash_t *f, const mcota_layout_t *l, mcota_request_t *out) {
  uint32_t h[MCOTA_W_CHECK];
  uint32_t i;
  if (!header(f, l, h)) return false;
  out->pages = h[MCOTA_W_PAGES];
  out->new_size = h[MCOTA_W_NEW_SIZE];
  out->old_size = h[MCOTA_W_OLD_SIZE];
  out->compact = h[MCOTA_W_COMPACT] == MCOTA_MODE_COMPACT;
  out->direct = h[MCOTA_W_COMPACT] == MCOTA_MODE_DIRECT;
  out->store = h[MCOTA_W_STORE];
  for (i = 0; i < 32u; i++) {
    out->new_hash[i] = ((const uint8_t *)&h[MCOTA_W_NEW_HASH])[i];
    out->old_hash[i] = ((const uint8_t *)&h[MCOTA_W_OLD_HASH])[i];
  }
  return true;
}

void mcota_confirm(const mcota_flash_t *f, const mcota_layout_t *l) {
  if (mcota_state(f, l) == MCOTA_STATE_TRIAL) mark(f, l, MCOTA_W_CONFIRMED);
}
