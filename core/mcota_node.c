#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "mcota_node.h"

#include "mcota_sha256.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }

static bool same(const uint8_t *a, const uint8_t *b, uint32_t n) {
  uint32_t i;
  uint8_t d = 0;
  for (i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

static void copy(uint8_t *to, const uint8_t *from, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; i++) to[i] = from[i];
}

static void sha(const uint8_t *data, uint32_t len, uint8_t out[32]) {
  mcota_sha256_t s;
  mcota_sha256_init(&s);
  mcota_sha256_update(&s, data, len);
  mcota_sha256_final(&s, out);
}

static uint32_t crc16(const uint8_t *data, uint32_t len) {
  uint32_t crc = 0xFFFFu, i, bit;
  for (i = 0; i < len; i++) {
    crc ^= (uint32_t)data[i] << 8;
    for (bit = 0; bit < 8u; bit++) crc = (crc & 0x8000u) ? ((crc << 1) ^ 0x1021u) & 0xFFFFu : (crc << 1) & 0xFFFFu;
  }
  return crc;
}

void mcota_key_id(const uint8_t pub_key[32], uint8_t out[8]) {
  uint8_t h[32];
  sha(pub_key, 32u, h);
  copy(out, h, 8u);
}

static void board_hash(const char *board, uint8_t out[4]) {
  uint8_t h[32];
  uint32_t len = 0;
  while (len < 15u && board[len]) len++;
  sha((const uint8_t *)board, len, h);
  copy(out, h, 4u);
}

static bool board_matches(const mcota_node_t *n, const char *board) {
  uint32_t i;
  for (i = 0; i < 16u; i++) {
    if (n->cfg.board[i] != board[i]) return false;
    if (!board[i]) break;
  }
  return true;
}

static int key_index(const mcota_node_t *n, const uint8_t *key_id) {
  uint32_t i;
  for (i = 0; i < MCOTA_KEYS; i++) {
    if (n->cfg.key_set[i] && same(n->key_ids[i], key_id, 8u)) return (int)i;
  }
  return -1;
}

static const uint8_t *running_hash(mcota_node_t *n, uint32_t size) {
  if (!n->hash_cached || n->hash_size != size) {
    mcota_hash_region(n->cfg.flash, n->cfg.layout.slot_a, size, n->hash);
    n->hash_size = size;
    n->hash_cached = true;
  }
  return n->hash;
}

static bool map_get(const uint8_t *map, uint32_t i) { return (map[i >> 3] >> (i & 7u)) & 1u; }
static void map_set(uint8_t *map, uint32_t i) { map[i >> 3] |= (uint8_t)(1u << (i & 7u)); }
static void map_clear(uint8_t *map, uint32_t i) { map[i >> 3] &= (uint8_t)~(1u << (i & 7u)); }
static void map_zero(uint8_t *map) { uint32_t i; for (i = 0; i < MCOTA_MAP_BYTES; i++) map[i] = 0; }
static uint32_t map_bytes(uint32_t pages) { return (pages + 7u) / 8u; }

static uint32_t now_ms(const mcota_node_t *n) { return n->cfg.now_ms(n->cfg.ctx); }
static uint32_t rnd(const mcota_node_t *n, uint32_t range) { return range ? n->cfg.random(n->cfg.ctx) % range : 0u; }
static uint32_t align4(uint32_t v) { return (v + 3u) & ~3u; }

static const mcota_flash_t *store_flash(const mcota_node_t *n) { return n->cfg.store ? n->cfg.store : n->cfg.flash; }

static void flash_write(const mcota_node_t *n, uint32_t addr, const uint8_t *data, uint32_t len) {
  const mcota_flash_t *f = store_flash(n);
  uint8_t buf[64];
  while (len > 0u) {
    uint32_t take = len < sizeof(buf) ? len : (uint32_t)sizeof(buf), total = align4(take), i;
    for (i = 0; i < total; i++) buf[i] = 0xFFu;
    copy(buf, data, take);
    f->write(f->ctx, addr, buf, total);
    addr += take;
    data += take;
    len -= take;
  }
}

static bool flash_equals(const mcota_node_t *n, uint32_t addr, const uint8_t *data, uint32_t len) {
  const mcota_flash_t *f = store_flash(n);
  uint8_t buf[64];
  while (len > 0u) {
    uint32_t take = len < sizeof(buf) ? len : (uint32_t)sizeof(buf);
    f->read(f->ctx, addr, buf, take);
    if (!same(buf, data, take)) return false;
    addr += take;
    data += take;
    len -= take;
  }
  return true;
}

static uint32_t swap_pages(const mcota_manifest_t *m) {
  uint32_t old_pages = (m->old_size + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE;
  return old_pages > m->pages ? old_pages : m->pages;
}

static uint32_t store_offset(const mcota_manifest_t *m, uint32_t page) {
  uint32_t i, offset = 0;
  for (i = 0; i < page; i++) offset += align4(mcota_manifest_block_len(m, i));
  return offset;
}

static bool block_stored(const mcota_node_t *n, uint32_t index);

static bool page_valid(mcota_node_t *n, uint32_t index) {
  uint8_t h[32];
  if (n->cfg.compact) return mcota_manifest_same(&n->manifest, index) || block_stored(n, index);
  if (mcota_manifest_same(&n->manifest, index)) {
    uint8_t a[64], b[64];
    uint32_t len = mcota_manifest_page_len(&n->manifest, index), pos;
    for (pos = 0; pos < len; pos += sizeof(a)) {
      uint32_t take = len - pos < sizeof(a) ? len - pos : (uint32_t)sizeof(a);
      n->cfg.flash->read(n->cfg.flash->ctx, n->cfg.layout.slot_a + index * MCOTA_PAGE_SIZE + pos, a, take);
      n->cfg.flash->read(n->cfg.flash->ctx, n->cfg.layout.slot_b + index * MCOTA_PAGE_SIZE + pos, b, take);
      if (!same(a, b, take)) return false;
    }
    return true;
  }
  mcota_hash_region(n->cfg.flash, n->cfg.layout.slot_b + index * MCOTA_PAGE_SIZE, mcota_manifest_page_len(&n->manifest, index), h);
  return same(h, mcota_manifest_page_hash(&n->manifest, index), MCOTA_PAGE_HASH);
}

static bool block_stored(const mcota_node_t *n, uint32_t index) {
  uint8_t w[4];
  store_flash(n)->read(store_flash(n)->ctx, n->marks_base + 4u * index, w, 4u);
  return get32(w) == 0u;
}

static bool failed_before(const mcota_node_t *n, const mcota_manifest_t *m) {
  mcota_request_t r;
  mcota_state_t s = mcota_state(n->cfg.flash, &n->cfg.layout);
  if (s != MCOTA_STATE_REVERTED && s != MCOTA_STATE_REJECTED) return false;
  return mcota_request_read(n->cfg.flash, &n->cfg.layout, &r) && same(r.new_hash, m->new_hash, 32u);
}

static bool busy_swapping(const mcota_node_t *n) {
  mcota_state_t s = mcota_state(n->cfg.flash, &n->cfg.layout);
  return s == MCOTA_STATE_ARMED || s == MCOTA_STATE_TRIAL;
}

static uint32_t interval_ms(mcota_node_t *n) {
  uint64_t t = (uint64_t)n->cfg.airtime_ms(n->cfg.ctx, MCOTA_PACKET_MAX) * 1000000u / n->cfg.budget_ppm;
  if (n->provider_set && n->provider_routed) t *= (uint32_t)n->provider_route.hops + 1u;
  return t > 1800000u ? 1800000u : (uint32_t)t;
}

static uint32_t quiet_ms(mcota_node_t *n) { return 2u * interval_ms(n) + 5000u; }

static void arm_quiet(mcota_node_t *n) {
  n->quiet_at = now_ms(n);
  n->wait_ms = quiet_ms(n) + rnd(n, interval_ms(n) + 1u);
}

static void ask_soon(mcota_node_t *n, uint32_t within) {
  n->quiet_at = now_ms(n);
  n->wait_ms = rnd(n, within + 1u);
}

static void useful(mcota_node_t *n) {
  n->useful_at = now_ms(n);
  n->retries = 0;
  arm_quiet(n);
}

static void old_read(void *ctx, uint32_t pos, uint8_t *out, uint32_t len);

static bool verify_image(mcota_node_t *n) {
  mcota_sha256_t s;
  uint8_t h[32];
  uint32_t i;
  mcota_sha256_init(&s);
  for (i = 0; i < n->manifest.pages; i++) {
    uint32_t len = mcota_manifest_page_len(&n->manifest, i);
    if (mcota_manifest_same(&n->manifest, i)) {
      n->cfg.flash->read(n->cfg.flash->ctx, n->cfg.layout.slot_a + i * MCOTA_PAGE_SIZE, n->page, len);
    } else {
      uint32_t block_len = mcota_manifest_block_len(&n->manifest, i);
      uint8_t *block = n->partial[0].data;
      store_flash(n)->read(store_flash(n)->ctx, n->blocks_base + store_offset(&n->manifest, i), block, block_len);
      if (!mcota_block_apply(old_read, n, n->manifest.old_size, block, block_len, n->work, sizeof(n->work), n->page, len, i * MCOTA_PAGE_SIZE)) return false;
      mcota_view_restore(&n->manifest.view, n->page, len, i * MCOTA_PAGE_SIZE);
    }
    mcota_sha256_update(&s, n->page, len);
  }
  mcota_sha256_final(&s, h);
  return same(h, n->manifest.new_hash, 32u);
}

static void set_complete(mcota_node_t *n) {
  n->survives = true;
  if (n->cfg.compact) {
    uint32_t end = n->blocks_base + store_offset(&n->manifest, n->manifest.pages);
    uint32_t room = n->cfg.layout.pool_end - ((end + MCOTA_PAGE_SIZE - 1u) & ~(MCOTA_PAGE_SIZE - 1u));
    (void)room;
    n->plan = mcota_compact_plan(n->cfg.flash, n->cfg.layout.slot_a, n->cfg.layout.pool_end, n->store_base, &n->manifest, n->page, n->partial[0].data, sizeof(n->partial[0].data));
    n->survives = n->plan == MCOTA_PLAN_KEEP || n->plan == MCOTA_PLAN_INPLACE;
  }
  n->state = MCOTA_NODE_COMPLETE;
  n->busy_at = now_ms(n);
  n->apply_jitter = rnd(n, n->cfg.apply_idle_ms + 1u);
}

static void scan_pages(mcota_node_t *n) {
  uint32_t i;
  map_zero(n->have_map);
  n->have = 0;
  for (i = 0; i < n->manifest.pages; i++) {
    if (page_valid(n, i)) { map_set(n->have_map, i); n->have++; }
  }
}

static void finish_if_complete(mcota_node_t *n) {
  uint8_t h[32];
  if (n->have < n->manifest.pages) return;
  if (n->cfg.compact) {
    if (verify_image(n)) {
      set_complete(n);
    } else {
      store_flash(n)->erase(store_flash(n)->ctx, n->store_base);
      n->has_manifest = false;
      n->state = MCOTA_NODE_IDLE;
    }
    return;
  }
  mcota_hash_region(n->cfg.flash, n->cfg.layout.slot_b, n->manifest.new_size, h);
  if (same(h, n->manifest.new_hash, 32u)) {
    set_complete(n);
  } else {
    scan_pages(n);
    if (n->have >= n->manifest.pages) { map_zero(n->have_map); n->have = 0; }
  }
}

static void reset_transfer(mcota_node_t *n) {
  uint32_t i;
  for (i = 0; i < MCOTA_PARTIALS; i++) n->partial[i].used = false;
  for (i = 0; i < MCOTA_MAX_PAGES; i++) n->chunk_mask[i] = 0;
  map_zero(n->bad_map);
  n->open_page = 0;
  for (i = 0; i < MCOTA_PEERS; i++) n->peers[i].used = false;
  n->repair_count = 0;
  n->retries = 0;
  n->asked = false;
}

static uint32_t slot_count(const mcota_node_t *n) {
  if (!n->cfg.store) return 1u;
  return n->cfg.store_size >= MCOTA_SLOTS * MCOTA_SLOT_MIN ? MCOTA_SLOTS : 1u;
}

static uint32_t slot_size(const mcota_node_t *n) {
  return (n->cfg.store_size / slot_count(n)) & ~(MCOTA_PAGE_SIZE - 1u);
}

static uint32_t slot_base(const mcota_node_t *n, uint32_t k) {
  return n->cfg.store_addr + k * slot_size(n);
}

static bool slot_head(const mcota_node_t *n, uint32_t base, uint32_t *len, uint32_t *seq) {
  uint8_t head[8];
  store_flash(n)->read(store_flash(n)->ctx, base, head, 8u);
  *len = get16(head + 4u);
  *seq = get16(head + 6u);
  return get32(head) == MCOTA_STORE_MAGIC && *len >= MCOTA_MANIFEST_HEAD && *len <= MCOTA_MANIFEST_MAX && 8u + *len <= slot_size(n);
}

static void scan_archives(mcota_node_t *n) {
  const mcota_flash_t *f = store_flash(n);
  uint32_t k, used = 0, i;
  for (k = 0; k < MCOTA_SLOTS - 1u; k++) n->archives[k].used = false;
  if (!n->cfg.store) return;
  for (k = 0; k < slot_count(n) && used < MCOTA_SLOTS - 1u; k++) {
    uint32_t base = slot_base(n, k), len, seq, marks, stored = 0;
    mcota_manifest_t m;
    mcota_archive_t *a = &n->archives[used];
    uint8_t id[32];
    int key;
    if (n->has_manifest && n->can_store && base == n->store_base) continue;
    if (!slot_head(n, base, &len, &seq)) continue;
    f->read(f->ctx, base + 8u, n->work, len);
    if (!mcota_manifest_parse(n->work, len, &m)) continue;
    key = key_index(n, m.key_id);
    if (key < 0 || !board_matches(n, m.board)) continue;
    if (!n->cfg.verify(n->cfg.ctx, n->cfg.keys[key], m.signature, n->work, len - MCOTA_MANIFEST_SIG)) continue;
    marks = base + 8u + align4(len);
    for (i = 0; i < m.pages; i++) {
      uint8_t w[4];
      f->read(f->ctx, marks + 4u * i, w, 4u);
      if (mcota_manifest_same(&m, i) || get32(w) == 0u) stored++;
    }
    if (stored != m.pages) continue;
    sha(n->work, len, id);
    a->used = true;
    a->base = base;
    a->marks_base = marks;
    a->blocks_base = marks + 4u * m.pages;
    a->old_size = m.old_size;
    a->pages = (uint16_t)m.pages;
    a->manifest_len = (uint16_t)len;
    copy(a->campaign, id, 8u);
    copy(a->key_id, m.key_id, 8u);
    copy(a->old_hash, m.old_hash, 8u);
    copy((uint8_t *)a->version, (const uint8_t *)m.version, 16u);
    used++;
  }
}

static bool adopt(mcota_node_t *n, const uint8_t *raw, uint32_t len, bool from_store) {
  const mcota_flash_t *f = store_flash(n);
  mcota_manifest_t m;
  uint8_t id[32], head[8];
  uint32_t first, avail, need_head, need_all, i, seq = 0;
  bool keep = false, consumed = false;
  int key;
  if (!mcota_manifest_parse(raw, len, &m)) return false;
  key = key_index(n, m.key_id);
  if (key < 0 || !board_matches(n, m.board)) return false;
  if (m.pages > n->cfg.layout.slot_pages || swap_pages(&m) > n->cfg.layout.slot_pages) return false;
  if (!n->cfg.verify(n->cfg.ctx, n->cfg.keys[key], m.signature, raw, len - MCOTA_MANIFEST_SIG)) return false;

  if (raw != n->manifest_raw) copy(n->manifest_raw, raw, len);
  mcota_manifest_parse(n->manifest_raw, len, &n->manifest);

  if (same(running_hash(n, n->manifest.new_size), n->manifest.new_hash, 32u)) {
    n->state = MCOTA_NODE_APPLIED;
  } else if (!same(running_hash(n, n->manifest.old_size), n->manifest.old_hash, 32u)) {
    n->has_manifest = false;
    n->state = MCOTA_NODE_IDLE;
    return false;
  } else if (failed_before(n, &n->manifest)) {
    n->state = MCOTA_NODE_FAILED;
  } else {
    n->state = MCOTA_NODE_FETCHING;
  }

  sha(n->manifest_raw, len, id);
  copy(n->campaign, id, 8u);
  n->has_manifest = true;
  n->old_valid = false;
  n->cand_active = false;
  n->provider_set = n->provider_set && !from_store;
  reset_transfer(n);

  if (n->cfg.store) {
    avail = slot_size(n);
    if (from_store) {
      n->store_base = n->adopt_base;
    } else {
      uint32_t k, other, other_seq, lowest = 0xFFFFFFFFu, highest = 0;
      n->store_base = slot_base(n, 0);
      for (k = 0; k < slot_count(n); k++) {
        uint32_t base = slot_base(n, k);
        if (!slot_head(n, base, &other, &other_seq)) {
          if (lowest != 0u) { lowest = 0u; n->store_base = base; }
          continue;
        }
        if (other == len && flash_equals(n, base + 8u, n->manifest_raw, len)) {
          n->store_base = base;
          keep = true;
          break;
        }
        if (other_seq + 1u > highest) highest = other_seq + 1u;
        if (other_seq + 1u < lowest) { lowest = other_seq + 1u; n->store_base = base; }
      }
      seq = highest & 0xFFFFu;
    }
  } else {
    first = swap_pages(&n->manifest);
    seq = 0xFFFFu;
    if (n->cfg.compact) {
      n->store_base = n->cfg.layout.slot_a + first * MCOTA_PAGE_SIZE;
      avail = n->cfg.layout.pool_end > n->store_base ? n->cfg.layout.pool_end - n->store_base : 0u;
    } else {
      avail = (n->cfg.layout.slot_pages - first) * MCOTA_PAGE_SIZE;
      n->store_base = n->cfg.layout.slot_b + first * MCOTA_PAGE_SIZE;
    }
    if (avail >= 8u + len) {
      f->read(f->ctx, n->store_base, head, 8u);
      if (get32(head) == MCOTA_STORE_MAGIC && get16(head + 4u) == len && flash_equals(n, n->store_base + 8u, n->manifest_raw, len)) {
        keep = true;
        consumed = get16(head + 6u) != 0xFFFFu;
      }
    }
  }
  need_head = 8u + align4(len);
  n->marks_base = n->store_base + need_head;
  n->blocks_base = n->marks_base + 4u * n->manifest.pages;
  need_all = (n->blocks_base - n->store_base) + store_offset(&n->manifest, n->manifest.pages);
  n->can_store = need_head <= avail;
  n->can_serve = need_all <= avail && !consumed;
  if (n->cfg.store && from_store) keep = true;
  if (n->cfg.compact && !n->can_serve && n->state != MCOTA_NODE_APPLIED) {
    n->has_manifest = false;
    n->state = MCOTA_NODE_IDLE;
    return false;
  }

  if (n->can_store && !keep) {
    uint32_t pages = ((n->can_serve ? need_all : need_head) + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE;
    for (i = 0; i < pages; i++) f->erase(f->ctx, n->store_base + i * MCOTA_PAGE_SIZE);
    put16(head + 4u, len);
    put16(head + 6u, seq);
    flash_write(n, n->store_base + 4u, head + 4u, 4u);
    flash_write(n, n->store_base + 8u, n->manifest_raw, len);
    put32(head, MCOTA_STORE_MAGIC);
    flash_write(n, n->store_base, head, 4u);
  }

  map_zero(n->serve_map);
  n->serve_count = 0;
  n->stored_blocks = 0;
  if (n->can_serve) {
    for (i = 0; i < n->manifest.pages; i++) {
      if (mcota_manifest_same(&n->manifest, i)) { map_set(n->serve_map, i); n->serve_count++; }
      else if (block_stored(n, i)) { map_set(n->serve_map, i); n->serve_count++; n->stored_blocks++; }
    }
  }

  map_zero(n->have_map);
  n->have = 0;
  if (n->state == MCOTA_NODE_APPLIED) {
    for (i = 0; i < n->manifest.pages; i++) map_set(n->have_map, i);
    n->have = n->manifest.pages;
  } else if (n->state == MCOTA_NODE_FETCHING) {
    scan_pages(n);
    finish_if_complete(n);
  }
  scan_archives(n);
  n->offer_interval = n->cfg.offer_min_ms;
  n->offer_at = now_ms(n) + n->cfg.offer_min_ms / 2u + rnd(n, n->cfg.offer_min_ms / 2u + 1u);
  n->seek_interval = n->cfg.offer_min_ms;
  n->seek_at = now_ms(n) + rnd(n, n->cfg.offer_min_ms / 2u + 1u);
  ask_soon(n, quiet_ms(n) / 4u);
  n->wait_ms += quiet_ms(n) / 4u;
  return true;
}

static void load_store(mcota_node_t *n) {
  const mcota_flash_t *f = store_flash(n);
  uint32_t p, len, seq;
  if (n->cfg.store) {
    uint32_t tried = 0;
    for (;;) {
      uint32_t best = 0xFFFFFFFFu, best_seq = 0, k;
      for (k = 0; k < slot_count(n); k++) {
        if ((tried >> k) & 1u) continue;
        if (!slot_head(n, slot_base(n, k), &len, &seq)) { tried |= 1u << k; continue; }
        if (best == 0xFFFFFFFFu || seq >= best_seq) { best = k; best_seq = seq; }
      }
      if (best == 0xFFFFFFFFu) break;
      tried |= 1u << best;
      n->adopt_base = slot_base(n, best);
      slot_head(n, n->adopt_base, &len, &seq);
      f->read(f->ctx, n->adopt_base + 8u, n->manifest_raw, len);
      if (adopt(n, n->manifest_raw, len, true)) return;
      n->has_manifest = false;
      n->state = MCOTA_NODE_IDLE;
    }
    scan_archives(n);
    n->offer_at = now_ms(n) + n->cfg.offer_min_ms / 2u + rnd(n, n->cfg.offer_min_ms / 2u + 1u);
    return;
  }
  for (p = 1u; p < n->cfg.layout.slot_pages; p++) {
    uint8_t head[8];
    uint32_t base = (n->cfg.compact ? n->cfg.layout.slot_a : n->cfg.layout.slot_b) + p * MCOTA_PAGE_SIZE;
    if (n->cfg.compact && base + 8u > n->cfg.layout.pool_end) break;
    f->read(f->ctx, base, head, 8u);
    if (get32(head) != MCOTA_STORE_MAGIC) continue;
    len = get16(head + 4u);
    if (len > MCOTA_MANIFEST_MAX || 8u + len > (n->cfg.layout.slot_pages - p) * MCOTA_PAGE_SIZE) continue;
    f->read(f->ctx, base + 8u, n->manifest_raw, len);
    if (adopt(n, n->manifest_raw, len, true) && n->store_base == base) return;
    n->has_manifest = false;
    n->state = MCOTA_NODE_IDLE;
  }
}

void mcota_node_init(mcota_node_t *n, const mcota_config_t *cfg) {
  uint32_t i;
  uint8_t *raw = (uint8_t *)n;
  for (i = 0; i < sizeof(*n); i++) raw[i] = 0;
  n->cfg = *cfg;
  for (i = 0; i < MCOTA_KEYS; i++) {
    if (cfg->key_set[i]) mcota_key_id(cfg->keys[i], n->key_ids[i]);
  }
  n->sequence = (uint8_t)cfg->random(cfg->ctx);
  n->budget_us = 2000000u;
  n->budget_at = cfg->now_ms(cfg->ctx);
  n->offer_interval = cfg->offer_min_ms;
  n->seek_interval = cfg->offer_min_ms;
  if (cfg->key_set[0] || cfg->key_set[1]) load_store(n);
}

static void refill(mcota_node_t *n) {
  uint32_t now = now_ms(n), elapsed = now - n->budget_at;
  uint64_t add = (uint64_t)elapsed * n->cfg.budget_ppm / 1000u + n->budget_us;
  n->budget_at = now;
  n->budget_us = add > 10000000u ? 10000000u : (uint32_t)add;
}

static bool transmit(mcota_node_t *n, const uint8_t *data, uint32_t len, const mcota_route_t *route) {
  uint32_t cost = n->cfg.airtime_ms(n->cfg.ctx, len + (route ? (uint32_t)route->hops * route->hop_size : 0u));
  if (route) cost *= (uint32_t)route->hops + 1u;
  refill(n);
  if ((uint64_t)cost * 1000u > n->budget_us) return false;
  if (!n->cfg.send(n->cfg.ctx, data, len, route)) return false;
  n->budget_us -= cost * 1000u;
  n->sent_ms += cost;
  n->tx_packets++;
  return true;
}

static uint32_t header(mcota_node_t *n, uint8_t *p, uint8_t type) {
  p[0] = 'L';
  p[1] = '2';
  p[2] = type;
  p[3] = n->sequence++;
  return MCOTA_HEADER;
}

static bool serves(const mcota_node_t *n);

static bool src_ok(const mcota_node_t *n, uint32_t src) {
  return src == 0u ? serves(n) : n->archives[src - 1u].used;
}

static const uint8_t *src_campaign(const mcota_node_t *n, uint32_t src) {
  return src == 0u ? n->campaign : n->archives[src - 1u].campaign;
}

static uint32_t src_pages(const mcota_node_t *n, uint32_t src) {
  return src == 0u ? n->manifest.pages : n->archives[src - 1u].pages;
}

static uint32_t src_manifest_len(const mcota_node_t *n, uint32_t src) {
  return src == 0u ? n->manifest.len : n->archives[src - 1u].manifest_len;
}

static bool src_has(const mcota_node_t *n, uint32_t src, uint32_t page) {
  return src == 0u ? map_get(n->serve_map, page) : true;
}

static uint32_t src_block_len(const mcota_node_t *n, uint32_t src, uint32_t page) {
  uint8_t w[4], bits[MCOTA_MAP_BYTES];
  uint32_t i, rank = 0, bytes, base;
  if (src == 0u) return mcota_manifest_block_len(&n->manifest, page);
  bytes = map_bytes(n->archives[src - 1u].pages);
  base = n->archives[src - 1u].base + 8u + MCOTA_MANIFEST_HEAD;
  store_flash(n)->read(store_flash(n)->ctx, base, bits, bytes);
  if (map_get(bits, page)) return 0u;
  for (i = 0; i < page; i++) {
    if (!map_get(bits, i)) rank++;
  }
  store_flash(n)->read(store_flash(n)->ctx, base + bytes + rank * MCOTA_MANIFEST_ENTRY, w, 2u);
  return get16(w);
}

static uint32_t src_block_addr(const mcota_node_t *n, uint32_t src, uint32_t page) {
  uint32_t i, offset = 0;
  if (src == 0u) return n->blocks_base + store_offset(&n->manifest, page);
  for (i = 0; i < page; i++) offset += align4(src_block_len(n, src, i));
  return n->archives[src - 1u].blocks_base + offset;
}

static int src_find(const mcota_node_t *n, const uint8_t *campaign) {
  uint32_t src;
  for (src = 0; src < MCOTA_SLOTS; src++) {
    if (src == 0u && !n->has_manifest) continue;
    if (src_ok(n, src) && same(campaign, src_campaign(n, src), 8u)) return (int)src;
  }
  return -1;
}

static bool send_offer(mcota_node_t *n, uint32_t src) {
  uint8_t p[128];
  uint32_t i = header(n, p, MCOTA_MSG_OFFER), pages = src_pages(n, src), mb = map_bytes(pages), k;
  copy(p + i, n->cfg.self, 4u); i += 4u;
  copy(p + i, src_campaign(n, src), 8u); i += 8u;
  copy(p + i, src == 0u ? n->manifest.key_id : n->archives[src - 1u].key_id, 8u); i += 8u;
  board_hash(n->cfg.board, p + i); i += 4u;
  put32(p + i, src == 0u ? n->manifest.old_size : n->archives[src - 1u].old_size); i += 4u;
  copy(p + i, src == 0u ? n->manifest.old_hash : n->archives[src - 1u].old_hash, 8u); i += 8u;
  put16(p + i, pages); i += 2u;
  put16(p + i, src_manifest_len(n, src)); i += 2u;
  p[i++] = 0;
  for (k = 0; k < mb; k++) p[i + k] = 0;
  for (k = 0; k < pages; k++) {
    if (src_has(n, src, k)) map_set(p + i, k);
  }
  i += mb;
  return transmit(n, p, i, 0);
}

static uint32_t chunks_of(uint32_t len) { return (len + MCOTA_CHUNK - 1u) / MCOTA_CHUNK; }

static uint32_t full_mask(uint32_t len) {
  uint32_t c = chunks_of(len);
  return c >= 32u ? 0xFFFFFFFFu : ((1u << c) - 1u);
}

static bool fetching(const mcota_node_t *n) {
  return n->has_manifest && n->state == MCOTA_NODE_FETCHING && n->have < n->manifest.pages;
}

static bool filling(const mcota_node_t *n) {
  return n->has_manifest && n->state == MCOTA_NODE_COMPLETE && n->can_serve && n->serve_count < n->manifest.pages && n->provider_set;
}

static bool takes_blocks(const mcota_node_t *n) {
  return n->has_manifest && (n->state == MCOTA_NODE_FETCHING || n->state == MCOTA_NODE_COMPLETE);
}

static bool needs(const mcota_node_t *n, uint32_t index) {
  if (mcota_manifest_same(&n->manifest, index)) return false;
  if (!map_get(n->have_map, index)) return true;
  return n->can_serve && !map_get(n->serve_map, index);
}

static mcota_partial_t *partial_find(mcota_node_t *n, uint8_t kind, uint32_t index) {
  uint32_t i;
  for (i = 0; i < MCOTA_PARTIALS; i++) {
    if (n->partial[i].used && n->partial[i].kind == kind && n->partial[i].index == index) return &n->partial[i];
  }
  return 0;
}

static bool send_request(mcota_node_t *n) {
  uint8_t p[160];
  uint32_t i = header(n, p, MCOTA_MSG_REQUEST), k, count = 0, at;
  copy(p + i, n->provider, 4u); i += 4u;
  copy(p + i, n->cfg.self, 4u); i += 4u;
  if (n->cand_active) {
    mcota_partial_t *part = partial_find(n, 0u, 0u);
    copy(p + i, n->cand_campaign, 8u); i += 8u;
    p[i++] = 0u;
    put32(p + i, full_mask(n->cand_manifest_len) & ~(part ? part->mask : 0u)); i += 4u;
  } else {
    uint32_t mb = map_bytes(n->manifest.pages);
    copy(p + i, n->campaign, 8u); i += 8u;
    p[i++] = 1u;
    for (k = 0; k < mb; k++) p[i + k] = 0;
    for (k = 0; k < n->manifest.pages; k++) {
      if (n->have < n->manifest.pages ? !map_get(n->have_map, k) && !mcota_manifest_same(&n->manifest, k) : needs(n, k)) map_set(p + i, k);
    }
    i += mb;
    at = i++;
    for (k = 0; k < MCOTA_PARTIALS; k++) {
      if (!n->partial[k].used || n->partial[k].kind != 1u) continue;
      put16(p + i, n->partial[k].index); i += 2u;
      put32(p + i, full_mask(n->partial[k].len) & ~n->partial[k].mask); i += 4u;
      count++;
    }
    for (k = 0; k < n->manifest.pages && count < MCOTA_ASK_PARTS; k++) {
      uint32_t full;
      if (n->chunk_mask[k] == 0u || map_get(n->serve_map, k)) continue;
      full = full_mask(mcota_manifest_block_len(&n->manifest, k));
      if (n->chunk_mask[k] == full) continue;
      put16(p + i, k); i += 2u;
      put32(p + i, full & ~n->chunk_mask[k]); i += 4u;
      count++;
    }
    p[at] = (uint8_t)count;
  }
  if (!transmit(n, p, i, n->provider_routed ? &n->provider_route : 0)) return false;
  n->asked = true;
  n->asked_at = now_ms(n);
  n->requests_sent++;
  return true;
}

static bool send_seek(mcota_node_t *n) {
  uint8_t p[16];
  uint32_t i = header(n, p, MCOTA_MSG_SEEK);
  copy(p + i, n->cfg.self, 4u); i += 4u;
  copy(p + i, n->campaign, 8u); i += 8u;
  if (!transmit(n, p, i, 0)) return false;
  n->seeks_sent++;
  return true;
}

static void old_read(void *ctx, uint32_t pos, uint8_t *out, uint32_t len) {
  mcota_node_t *n = (mcota_node_t *)ctx;
  while (len > 0u) {
    uint32_t page = pos / MCOTA_PAGE_SIZE, offset = pos % MCOTA_PAGE_SIZE, take = MCOTA_PAGE_SIZE - offset;
    if (take > len) take = len;
    if (!n->old_valid || n->old_cached != page) {
      uint32_t start = page * MCOTA_PAGE_SIZE, size = n->manifest.old_size - start;
      if (size > MCOTA_PAGE_SIZE) size = MCOTA_PAGE_SIZE;
      n->cfg.flash->read(n->cfg.flash->ctx, n->cfg.layout.slot_a + start, n->old_page, size);
      mcota_view_predict(&n->manifest.view, n->old_page, size, start);
      n->old_cached = page;
      n->old_valid = true;
    }
    copy(out, n->old_page + offset, take);
    out += take;
    pos += take;
    len -= take;
  }
}

static bool block_done(mcota_node_t *n, uint32_t index, const uint8_t *data, uint32_t len) {
  const mcota_flash_t *f = n->cfg.flash;
  uint32_t page_len = mcota_manifest_page_len(&n->manifest, index), i;
  uint8_t h[32];
  bool gained = false;
  if (!mcota_block_apply(old_read, n, n->manifest.old_size, data, len, n->work, sizeof(n->work), n->page, page_len, index * MCOTA_PAGE_SIZE)) return false;
  mcota_view_restore(&n->manifest.view, n->page, page_len, index * MCOTA_PAGE_SIZE);
  sha(n->page, page_len, h);
  if (!same(h, mcota_manifest_page_hash(&n->manifest, index), MCOTA_PAGE_HASH)) return false;
  if (n->can_serve && !map_get(n->serve_map, index)) {
    uint32_t addr = n->blocks_base + store_offset(&n->manifest, index);
    uint8_t zero[4] = { 0, 0, 0, 0 };
    if (!flash_equals(n, addr, data, len)) flash_write(n, addr, data, len);
    if (flash_equals(n, addr, data, len)) {
      store_flash(n)->write(store_flash(n)->ctx, n->marks_base + 4u * index, zero, 4u);
      map_set(n->serve_map, index);
      n->serve_count++;
      n->stored_blocks++;
      gained = true;
    }
  }
  if (n->cfg.compact) {
    if (map_get(n->serve_map, index) && !map_get(n->have_map, index)) {
      map_set(n->have_map, index);
      n->have++;
      gained = true;
      finish_if_complete(n);
    }
  } else if (!map_get(n->have_map, index)) {
    for (i = page_len; i < MCOTA_PAGE_SIZE; i++) n->page[i] = 0xFFu;
    f->erase(f->ctx, n->cfg.layout.slot_b + index * MCOTA_PAGE_SIZE);
    f->write(f->ctx, n->cfg.layout.slot_b + index * MCOTA_PAGE_SIZE, n->page, MCOTA_PAGE_SIZE);
    if (!page_valid(n, index)) return gained;
    map_set(n->have_map, index);
    n->have++;
    gained = true;
    finish_if_complete(n);
  }
  if (gained) {
    useful(n);
    if (n->state == MCOTA_NODE_COMPLETE) n->busy_at = now_ms(n);
  }
  return gained;
}

static void manifest_done(mcota_node_t *n, const uint8_t *data, uint32_t len) {
  uint8_t id[32];
  sha(data, len, id);
  if (!n->cand_active || !same(id, n->cand_campaign, 8u)) return;
  if (!adopt(n, data, len, false)) {
    copy(n->ignored, n->cand_campaign, 8u);
    n->ignored_set = true;
    n->cand_active = false;
  }
}

static void partial_chunk(mcota_node_t *n, uint8_t kind, uint32_t index, uint32_t len, uint32_t chunk, const uint8_t *data, uint32_t body) {
  mcota_partial_t *part = partial_find(n, kind, index);
  uint32_t expect, i, now = now_ms(n);
  if (chunk >= chunks_of(len)) return;
  expect = chunk + 1u == chunks_of(len) ? len - chunk * MCOTA_CHUNK : MCOTA_CHUNK;
  if (body != expect) return;
  if (!part) {
    bool stalled = false;
    for (i = 0; i < MCOTA_PARTIALS; i++) {
      if (n->partial[i].used && n->partial[i].kind == 1u) stalled = true;
      else if (!part) part = &n->partial[i];
    }
    if (!part) {
      part = &n->partial[0];
      for (i = 1; i < MCOTA_PARTIALS; i++) {
        if ((int32_t)(n->partial[i].at - part->at) < 0) part = &n->partial[i];
      }
    }
    if (stalled && kind == 1u && n->provider_set && (!n->asked || now - n->asked_at > interval_ms(n))) ask_soon(n, 2000u);
    part->used = true;
    part->kind = kind;
    part->index = (uint16_t)index;
    part->len = len;
    part->mask = 0;
  }
  if (part->mask & (1u << chunk)) return;
  copy(part->data + chunk * MCOTA_CHUNK, data, body);
  part->mask |= 1u << chunk;
  part->at = now;
  useful(n);
  if (part->mask != full_mask(len)) return;
  part->used = false;
  if (kind == 0u) manifest_done(n, part->data, len);
  else block_done(n, index, part->data, len);
}

static void direct_chunk(mcota_node_t *n, uint32_t index, uint32_t len, uint32_t chunk, const uint8_t *data, uint32_t body) {
  uint32_t expect, addr, full = full_mask(len), now = now_ms(n);
  if (chunk >= chunks_of(len)) return;
  expect = chunk + 1u == chunks_of(len) ? len - chunk * MCOTA_CHUNK : MCOTA_CHUNK;
  if (body != expect || (n->chunk_mask[index] & (1u << chunk))) return;
  if (n->open_page != index) {
    uint32_t open = n->chunk_mask[n->open_page];
    if (open != 0u && open != full_mask(mcota_manifest_block_len(&n->manifest, n->open_page)) && !map_get(n->serve_map, n->open_page)
        && n->provider_set && (!n->asked || now - n->asked_at > interval_ms(n))) ask_soon(n, 2000u);
    n->open_page = index;
  }
  addr = n->blocks_base + store_offset(&n->manifest, index) + chunk * MCOTA_CHUNK;
  if (!flash_equals(n, addr, data, body)) {
    flash_write(n, addr, data, body);
    if (!flash_equals(n, addr, data, body)) {
      map_set(n->bad_map, index);
      n->chunk_mask[index] = 0;
      return;
    }
  }
  n->chunk_mask[index] |= 1u << chunk;
  n->direct_chunks++;
  useful(n);
  if (n->chunk_mask[index] != full) return;
  n->partial[1].used = false;
  store_flash(n)->read(store_flash(n)->ctx, n->blocks_base + store_offset(&n->manifest, index), n->partial[1].data, len);
  if (!block_done(n, index, n->partial[1].data, len) && takes_blocks(n) && !map_get(n->serve_map, index)) {
    n->chunk_mask[index] = 0;
    if (n->cfg.compact) mcota_node_drop(n);
    else map_set(n->bad_map, index);
  }
}

static bool accepts_new_campaign(const mcota_node_t *n) {
  if (busy_swapping(n)) return false;
  return !n->has_manifest || n->state == MCOTA_NODE_APPLIED || n->state == MCOTA_NODE_FAILED || n->state == MCOTA_NODE_IDLE;
}

static uint32_t offer_score(const mcota_node_t *n, const uint8_t *map) {
  uint32_t i, score = 0;
  for (i = 0; i < n->manifest.pages; i++) {
    if (map_get(map, i) && needs(n, i)) score++;
  }
  return score;
}

static void set_provider(mcota_node_t *n, const uint8_t *sender, const uint8_t *map, uint32_t mb, const mcota_route_t *route) {
  bool changed = !n->provider_set || !same(sender, n->provider, 4u);
  copy(n->provider, sender, 4u);
  map_zero(n->provider_map);
  copy(n->provider_map, map, mb);
  n->provider_routed = route != 0;
  if (route) n->provider_route = *route;
  n->provider_set = true;
  if (changed) {
    n->retries = 0;
    ask_soon(n, quiet_ms(n) / 4u);
  }
}

static bool read_route(const uint8_t *q, uint32_t len, mcota_route_t *r) {
  uint32_t size;
  if (len < 6u) return false;
  r->hop_size = q[4];
  r->hops = q[5];
  size = (uint32_t)r->hop_size * r->hops;
  if (r->hop_size < 1u || r->hop_size > 3u || r->hops == 0u || size > MCOTA_ROUTE_MAX || len < 6u + size) return false;
  copy(r->bytes, q + 6u, size);
  return true;
}

static void on_offer(mcota_node_t *n, const uint8_t *p, uint32_t len) {
  const uint8_t *sender = p, *campaign = p + 4u, *key_id = p + 12u, *board = p + 20u, *old_prefix = p + 28u, *map = p + 41u;
  uint32_t old_size, pages, manifest_len, mb;
  uint8_t mine[4];
  mcota_route_t route, *via = 0;
  bool for_me = true;
  if (len < 41u) return;
  old_size = get32(p + 24u);
  pages = get16(p + 36u);
  manifest_len = get16(p + 38u);
  mb = map_bytes(pages);
  if (pages == 0u || pages > MCOTA_MAX_PAGES || len < 41u + mb) return;
  if (p[40] & MCOTA_OFFER_ROUTED) {
    if (!read_route(map + mb, len - 41u - mb, &route)) return;
    for_me = same(map + mb, n->cfg.self, 4u);
    via = &route;
  }
  for (old_size = 0; old_size < MCOTA_SLOTS - 1u; old_size++) {
    if (n->archives[old_size].used && same(campaign, n->archives[old_size].campaign, 8u)) return;
  }
  old_size = get32(p + 24u);
  if (n->has_manifest && same(campaign, n->campaign, 8u)) {
    if ((n->state != MCOTA_NODE_FETCHING && n->state != MCOTA_NODE_COMPLETE) || !for_me || pages != n->manifest.pages) return;
    if (offer_score(n, map) == 0u && n->state == MCOTA_NODE_COMPLETE) return;
    if (!n->provider_set || same(sender, n->provider, 4u) || offer_score(n, map) > offer_score(n, n->provider_map))
      set_provider(n, sender, map, mb, via);
    return;
  }
  if (n->cand_active && same(campaign, n->cand_campaign, 8u)) {
    if (for_me && !n->provider_set) { set_provider(n, sender, map, mb, via); ask_soon(n, 2000u); }
    return;
  }
  if (n->cand_active || !accepts_new_campaign(n)) return;
  if (n->ignored_set && same(campaign, n->ignored, 8u)) return;
  if (key_index(n, key_id) < 0) return;
  board_hash(n->cfg.board, mine);
  if (!same(board, mine, 4u)) return;
  if (old_size == 0u || old_size > n->cfg.layout.slot_pages * MCOTA_PAGE_SIZE) return;
  if (manifest_len < MCOTA_MANIFEST_HEAD || manifest_len > MCOTA_MANIFEST_MAX) return;
  if (!same(running_hash(n, old_size), old_prefix, 8u)) return;
  n->cand_active = true;
  n->cand_at = now_ms(n);
  copy(n->cand_campaign, campaign, 8u);
  n->cand_manifest_len = manifest_len;
  n->provider_set = false;
  reset_transfer(n);
  if (for_me) { set_provider(n, sender, map, mb, via); ask_soon(n, 2000u); }
}

static void add_repair(mcota_node_t *n, uint32_t src, uint8_t kind, uint32_t index, uint32_t mask) {
  uint32_t i;
  if (mask == 0u) return;
  for (i = 0; i < n->repair_count; i++) {
    if (n->repairs[i].src == src && n->repairs[i].kind == kind && n->repairs[i].index == index) { n->repairs[i].mask |= mask; return; }
  }
  if (n->repair_count >= MCOTA_REPAIRS) return;
  n->repairs[n->repair_count].src = (uint8_t)src;
  n->repairs[n->repair_count].kind = kind;
  n->repairs[n->repair_count].index = (uint16_t)index;
  n->repairs[n->repair_count].mask = mask;
  n->repair_count++;
}

static bool serves(const mcota_node_t *n) {
  if (!n->has_manifest || !n->can_serve || n->stored_blocks == 0u) return false;
  return n->state == MCOTA_NODE_FETCHING || n->state == MCOTA_NODE_COMPLETE || n->state == MCOTA_NODE_APPLIED;
}

static void on_request(mcota_node_t *n, const uint8_t *p, uint32_t len) {
  uint32_t mb, count, i, pos, now = now_ms(n), src, pages;
  mcota_peer_t *peer = 0;
  int found;
  if (len < 17u || !same(p, n->cfg.self, 4u)) return;
  found = src_find(n, p + 8u);
  if (found < 0) return;
  src = (uint32_t)found;
  pages = src_pages(n, src);
  if (src == 0u) n->busy_at = now;
  if (p[16] == 0u) {
    if (len < 21u) return;
    add_repair(n, src, 0u, 0u, get32(p + 17u) & full_mask(src_manifest_len(n, src)));
    return;
  }
  mb = map_bytes(pages);
  if (p[16] != 1u || len < 17u + mb + 1u) return;
  for (i = 0; i < MCOTA_PEERS && !peer; i++) {
    if (n->peers[i].used && same(n->peers[i].id, p + 4u, 4u)) peer = &n->peers[i];
  }
  if (!peer) n->offer_interval = n->cfg.offer_min_ms;
  for (i = 0; i < MCOTA_PEERS && !peer; i++) {
    if (!n->peers[i].used) peer = &n->peers[i];
  }
  if (!peer) {
    peer = &n->peers[0];
    for (i = 1; i < MCOTA_PEERS; i++) {
      if ((int32_t)(n->peers[i].at - peer->at) < 0) peer = &n->peers[i];
    }
  }
  peer->used = true;
  peer->src = (uint8_t)src;
  peer->at = now;
  copy(peer->id, p + 4u, 4u);
  map_zero(peer->want);
  for (i = 0; i < pages; i++) {
    if (map_get(p + 17u, i)) map_set(peer->want, i);
  }
  for (i = 0; i < n->repair_count; i++) {
    if (n->repairs[i].src == src && n->repairs[i].kind == 1u) map_clear(peer->want, n->repairs[i].index);
  }
  count = p[17u + mb];
  pos = 18u + mb;
  for (i = 0; i < count && pos + 6u <= len; i++, pos += 6u) {
    uint32_t index = get16(p + pos), mask = get32(p + pos + 2u);
    if (index >= pages || !src_has(n, src, index)) continue;
    add_repair(n, src, 1u, index, mask & full_mask(src_block_len(n, src, index)));
    map_clear(peer->want, index);
  }
}

static void on_data(mcota_node_t *n, const uint8_t *p, uint32_t len) {
  uint8_t kind;
  uint32_t index, chunk, body;
  if (len < 11u) return;
  len -= 2u;
  if (crc16(p, len) != get16(p + len)) return;
  kind = p[4];
  index = get16(p + 5u);
  chunk = p[7];
  body = len - 8u;
  if (kind == 0u) {
    if (n->cand_active && same(p, n->cand_campaign, 4u)) partial_chunk(n, 0u, 0u, n->cand_manifest_len, chunk, p + 8u, body);
    return;
  }
  if (!takes_blocks(n) || !same(p, n->campaign, 4u)) return;
  if (kind == 2u) {
    uint32_t count = chunk, offset = 8u, i;
    bool gained = false;
    if (count == 0u || index + count > n->manifest.pages) return;
    for (i = 0; i < count; i++) offset += mcota_manifest_block_len(&n->manifest, index + i);
    if (offset != len) return;
    offset = 8u;
    for (i = 0; i < count; i++) {
      uint32_t block_len = mcota_manifest_block_len(&n->manifest, index + i);
      if (takes_blocks(n) && needs(n, index + i) && block_done(n, index + i, p + offset, block_len)) gained = true;
      offset += block_len;
    }
    if (gained) {
      n->bundles++;
      if (!n->asked) n->overheard++;
    }
    return;
  }
  if (kind != 1u || index >= n->manifest.pages || !needs(n, index)) return;
  if (n->can_serve && !map_get(n->serve_map, index) && !map_get(n->bad_map, index)) direct_chunk(n, index, mcota_manifest_block_len(&n->manifest, index), chunk, p + 8u, body);
  else partial_chunk(n, 1u, index, mcota_manifest_block_len(&n->manifest, index), chunk, p + 8u, body);
}

static void on_seek(mcota_node_t *n, const uint8_t *p, uint32_t len) {
  uint32_t t;
  int src;
  if (len < 12u) return;
  src = src_find(n, p + 4u);
  if (src < 0) return;
  t = now_ms(n) + rnd(n, n->cfg.offer_min_ms / 2u + 1u);
  if ((int32_t)(n->offer_at - t) > 0) n->offer_at = t;
  n->offer_turn = (uint32_t)src;
  n->offer_interval = n->cfg.offer_min_ms;
}

void mcota_node_receive(mcota_node_t *n, const uint8_t *data, uint32_t len) {
  uint8_t out[16];
  const uint8_t *body = data + MCOTA_HEADER;
  uint32_t i, body_len;
  if (len < MCOTA_HEADER || data[0] != 'L' || data[1] != '2') return;
  body_len = len - MCOTA_HEADER;
  n->rx_packets++;
  switch (data[2]) {
    case MCOTA_MSG_OFFER: on_offer(n, body, body_len); break;
    case MCOTA_MSG_REQUEST: on_request(n, body, body_len); break;
    case MCOTA_MSG_DATA: on_data(n, body, body_len); break;
    case MCOTA_MSG_SEEK: on_seek(n, body, body_len); break;
    case MCOTA_MSG_QUERY:
      if (body_len < 12u || n->cfg.image_size == 0u || key_index(n, body + 4u) < 0) break;
      if (body_len > 12u) {
        mcota_route_t back;
        if (!read_route(body + 12u, body_len - 12u, &back) || !same(body + 12u, n->cfg.self, 4u)) break;
        n->hello_route = back;
        n->hello_routed = true;
        n->hello_key = (uint8_t)key_index(n, body + 4u);
        n->hello_due = true;
        n->hello_at = now_ms(n) + 500u + rnd(n, 2000u);
      } else if (!n->hello_due) {
        n->hello_routed = false;
        n->hello_key = (uint8_t)key_index(n, body + 4u);
        n->hello_due = true;
        n->hello_at = now_ms(n) + rnd(n, n->cfg.offer_min_ms / 2u + 1u);
      }
      break;
    case MCOTA_MSG_PING:
      if (body_len < 8u) break;
      i = header(n, out, MCOTA_MSG_PONG);
      copy(out + i, body, 4u); i += 4u;
      copy(out + i, n->cfg.self, 4u); i += 4u;
      copy(out + i, body + 4u, 4u); i += 4u;
      transmit(n, out, i, 0);
      break;
    case MCOTA_MSG_PONG:
      if (body_len >= 12u && same(body, n->cfg.self, 4u) && n->ping_nonce != 0u && get32(body + 8u) == n->ping_nonce) n->pong_seen = true;
      break;
    default: break;
  }
}

bool mcota_node_ping(mcota_node_t *n) {
  uint8_t p[16];
  uint32_t i = header(n, p, MCOTA_MSG_PING);
  if (n->ping_nonce == 0u) n->ping_nonce = n->cfg.random(n->cfg.ctx) | 1u;
  copy(p + i, n->cfg.self, 4u); i += 4u;
  put32(p + i, n->ping_nonce); i += 4u;
  return transmit(n, p, i, 0);
}

static uint32_t demand(const mcota_node_t *n, uint32_t src, uint32_t page) {
  uint32_t i, count = 0;
  for (i = 0; i < MCOTA_PEERS; i++) {
    if (n->peers[i].used && n->peers[i].src == src && map_get(n->peers[i].want, page)) count++;
  }
  return count;
}

static void served(mcota_node_t *n, uint32_t src, uint32_t page) {
  uint32_t i, now = now_ms(n);
  for (i = 0; i < MCOTA_PEERS; i++) {
    if (!n->peers[i].used || n->peers[i].src != src) continue;
    if (map_get(n->peers[i].want, page)) n->peers[i].at = now;
    map_clear(n->peers[i].want, page);
  }
}

static bool next_page(const mcota_node_t *n, uint32_t *from, uint32_t *page) {
  uint32_t src, i, best = 0;
  for (src = 0; src < MCOTA_SLOTS; src++) {
    bool wanted = false;
    if (!src_ok(n, src)) continue;
    for (i = 0; i < MCOTA_PEERS; i++) {
      if (n->peers[i].used && n->peers[i].src == src) wanted = true;
    }
    if (!wanted) continue;
    for (i = 0; i < src_pages(n, src); i++) {
      uint32_t d;
      if (!src_has(n, src, i)) continue;
      d = demand(n, src, i);
      if (d > best) { best = d; *from = src; *page = i; }
    }
  }
  return best > 0u;
}

static uint32_t data_head(mcota_node_t *n, uint8_t *p, uint32_t src, uint8_t kind, uint32_t index, uint32_t chunk) {
  uint32_t i = header(n, p, MCOTA_MSG_DATA);
  copy(p + i, src_campaign(n, src), 4u); i += 4u;
  p[i++] = kind;
  put16(p + i, index); i += 2u;
  p[i++] = (uint8_t)chunk;
  return i;
}

static bool send_data(mcota_node_t *n, uint8_t *p, uint32_t len) {
  put16(p + len, crc16(p + MCOTA_HEADER, len - MCOTA_HEADER));
  return transmit(n, p, len + 2u, 0);
}

static bool serve_step(mcota_node_t *n) {
  const mcota_flash_t *f = store_flash(n);
  uint8_t p[MCOTA_PACKET_MAX];
  uint32_t page = 0, src = 0, i, len;
  while (n->repair_count > 0u && !src_ok(n, n->repairs[0].src)) {
    for (i = 1; i < n->repair_count; i++) n->repairs[i - 1u] = n->repairs[i];
    n->repair_count--;
  }
  if (n->repair_count > 0u) {
    mcota_repair_t *r = &n->repairs[0];
    uint32_t chunk = 0, take;
    src = r->src;
    while (!(r->mask & (1u << chunk))) chunk++;
    len = r->kind == 0u ? src_manifest_len(n, src) : src_block_len(n, src, r->index);
    take = chunk + 1u == chunks_of(len) ? len - chunk * MCOTA_CHUNK : MCOTA_CHUNK;
    i = data_head(n, p, src, r->kind, r->index, chunk);
    if (r->kind == 0u && src == 0u) copy(p + i, n->manifest_raw + chunk * MCOTA_CHUNK, take);
    else if (r->kind == 0u) f->read(f->ctx, n->archives[src - 1u].base + 8u + chunk * MCOTA_CHUNK, p + i, take);
    else f->read(f->ctx, src_block_addr(n, src, r->index) + chunk * MCOTA_CHUNK, p + i, take);
    if (send_data(n, p, i + take)) {
      r->mask &= ~(1u << chunk);
      if (src == 0u) n->busy_at = now_ms(n);
      if (r->mask == 0u) {
        for (i = 1; i < n->repair_count; i++) n->repairs[i - 1u] = n->repairs[i];
        n->repair_count--;
      }
    }
    return true;
  }
  if (!next_page(n, &src, &page)) return false;
  len = src_block_len(n, src, page);
  if (len > MCOTA_CHUNK) {
    add_repair(n, src, 1u, page, full_mask(len));
    served(n, src, page);
    return true;
  } else {
    uint32_t count = 0, total = 0, head = MCOTA_HEADER + 8u, addr = src_block_addr(n, src, page);
    while (page + count < src_pages(n, src) && count < 255u) {
      uint32_t next = src_block_len(n, src, page + count);
      if (total + next > MCOTA_CHUNK || !src_has(n, src, page + count)) break;
      if (count > 0u && demand(n, src, page + count) == 0u) break;
      f->read(f->ctx, addr, p + head + total, next);
      addr += align4(next);
      total += next;
      count++;
    }
    i = data_head(n, p, src, 2u, page, count);
    if (send_data(n, p, i + total)) {
      for (i = 0; i < count; i++) served(n, src, page + i);
      if (src == 0u) n->busy_at = now_ms(n);
    }
    return true;
  }
}

static bool copy_same(mcota_node_t *n) {
  const mcota_flash_t *f = n->cfg.flash;
  uint32_t i, k, len;
  if (!n->has_manifest || n->state != MCOTA_NODE_FETCHING || n->cfg.compact) return false;
  for (i = 0; i < n->manifest.pages; i++) {
    if (mcota_manifest_same(&n->manifest, i) && !map_get(n->have_map, i)) break;
  }
  if (i >= n->manifest.pages) return false;
  len = mcota_manifest_page_len(&n->manifest, i);
  f->read(f->ctx, n->cfg.layout.slot_a + i * MCOTA_PAGE_SIZE, n->page, len);
  for (k = len; k < MCOTA_PAGE_SIZE; k++) n->page[k] = 0xFFu;
  f->erase(f->ctx, n->cfg.layout.slot_b + i * MCOTA_PAGE_SIZE);
  f->write(f->ctx, n->cfg.layout.slot_b + i * MCOTA_PAGE_SIZE, n->page, MCOTA_PAGE_SIZE);
  if (!page_valid(n, i)) { n->state = MCOTA_NODE_FAILED; return true; }
  map_set(n->have_map, i);
  n->have++;
  finish_if_complete(n);
  return true;
}

static bool send_hello(mcota_node_t *n) {
  uint8_t p[64];
  uint32_t i = header(n, p, MCOTA_MSG_HELLO), k;
  copy(p + i, n->cfg.self, 4u); i += 4u;
  copy(p + i, n->key_ids[n->hello_key], 8u); i += 8u;
  board_hash(n->cfg.board, p + i); i += 4u;
  put32(p + i, n->cfg.image_size); i += 4u;
  copy(p + i, running_hash(n, n->cfg.image_size), 8u); i += 8u;
  for (k = 0; k < 15u && n->cfg.tag[k]; k++) p[i++] = (uint8_t)n->cfg.tag[k];
  p[i++] = 0u;
  p[i++] = (uint8_t)(n->cand_active ? MCOTA_NODE_FETCHING : n->state);
  p[i++] = (uint8_t)mcota_state(n->cfg.flash, &n->cfg.layout);
  {
    mcota_request_t job;
    bool known = mcota_request_read(n->cfg.flash, &n->cfg.layout, &job);
    for (k = 0; k < 4u; k++) p[i++] = known ? job.new_hash[k] : 0u;
  }
  return transmit(n, p, i, n->hello_routed ? &n->hello_route : 0);
}

void mcota_node_loop(mcota_node_t *n) {
  uint32_t now = now_ms(n), i;

  if (copy_same(n)) return;
  if (n->hello_due && (int32_t)(now - n->hello_at) >= 0 && send_hello(n)) n->hello_due = false;

  for (i = 0; i < MCOTA_PEERS; i++) {
    if (n->peers[i].used && now - n->peers[i].at > 12u * quiet_ms(n)) n->peers[i].used = false;
  }

  serve_step(n);

  for (i = 0; i < MCOTA_SLOTS && !src_ok(n, (n->offer_turn + i) % MCOTA_SLOTS); i++) { }
  if (i < MCOTA_SLOTS && (int32_t)(now - n->offer_at) >= 0) {
    uint32_t src = (n->offer_turn + i) % MCOTA_SLOTS;
    if (send_offer(n, src)) {
      n->offer_turn = src + 1u;
      n->offer_interval = n->offer_interval * 2u > n->cfg.offer_max_ms ? n->cfg.offer_max_ms : n->offer_interval * 2u;
      n->offer_at = now + n->offer_interval / 2u + rnd(n, n->offer_interval / 2u + 1u);
    }
  }

  if (n->cand_active && !n->provider_set && now - n->cand_at > 8u * quiet_ms(n)) n->cand_active = false;
  if (!n->cand_active && !fetching(n) && !filling(n)) return;

  if (n->provider_set) {
    if (now - n->quiet_at < n->wait_ms) return;
    if (n->retries >= MCOTA_TRIES) {
      n->provider_set = false;
      n->retries = 0;
      if (n->cand_active) n->cand_active = false;
      n->seek_interval = n->cfg.offer_min_ms;
      n->seek_at = now + rnd(n, 2000u);
    } else if (send_request(n)) {
      n->retries++;
      arm_quiet(n);
    }
  } else if (fetching(n) && (int32_t)(now - n->seek_at) >= 0) {
    if (send_seek(n)) {
      n->seek_interval = n->seek_interval * 2u > n->cfg.offer_max_ms ? n->cfg.offer_max_ms : n->seek_interval * 2u;
      n->seek_at = now + n->seek_interval / 2u + rnd(n, n->seek_interval / 2u + 1u);
    }
  }
}

bool mcota_node_seed_manifest(mcota_node_t *n, const uint8_t *raw, uint32_t len) {
  if (len > MCOTA_MANIFEST_MAX || busy_swapping(n)) return false;
  if (!adopt(n, raw, len, false)) return false;
  while (copy_same(n)) { }
  return true;
}

bool mcota_node_seed_block(mcota_node_t *n, uint32_t index, const uint8_t *data, uint32_t len) {
  if (!takes_blocks(n) || index >= n->manifest.pages) return false;
  if (mcota_manifest_same(&n->manifest, index)) return len == 0u;
  if (len != mcota_manifest_block_len(&n->manifest, index)) return false;
  if (!needs(n, index)) return true;
  return block_done(n, index, data, len);
}

bool mcota_node_apply(mcota_node_t *n) {
  mcota_request_t r;
  if (!n->has_manifest || n->state != MCOTA_NODE_COMPLETE || busy_swapping(n)) return false;
  if (n->cfg.compact && n->plan == MCOTA_PLAN_NONE) return false;
  r.compact = n->cfg.compact;
  r.direct = n->cfg.direct;
  r.store = n->cfg.compact ? n->store_base : 0u;
  r.pages = swap_pages(&n->manifest);
  r.new_size = n->manifest.new_size;
  r.old_size = n->manifest.old_size;
  copy(r.new_hash, n->manifest.new_hash, 32u);
  copy(r.old_hash, n->manifest.old_hash, 32u);
  return mcota_request(n->cfg.flash, &n->cfg.layout, &r);
}

bool mcota_node_no_rollback(const mcota_node_t *n) {
  return n->has_manifest && n->state == MCOTA_NODE_COMPLETE && n->cfg.compact && n->plan == MCOTA_PLAN_INPLACE;
}

bool mcota_node_needs_command(const mcota_node_t *n) {
  if (!n->has_manifest || n->state != MCOTA_NODE_COMPLETE) return false;
  if (n->cfg.compact && n->plan == MCOTA_PLAN_NONE) return false;
  if (!(n->manifest.flags & MCOTA_MANIFEST_AUTO_APPLY)) return true;
  return mcota_node_no_rollback(n) && !(n->manifest.flags & MCOTA_MANIFEST_NO_ROLLBACK);
}

bool mcota_node_apply_due(mcota_node_t *n) {
  uint32_t i, page, src;
  if (!n->has_manifest || n->state != MCOTA_NODE_COMPLETE || !(n->manifest.flags & MCOTA_MANIFEST_AUTO_APPLY)) return false;
  if (mcota_node_needs_command(n)) return false;
  if (n->repair_count > 0u) return false;
  if (n->cfg.compact && n->plan == MCOTA_PLAN_NONE) return false;
  if (next_page(n, &src, &page)) return false;
  for (i = 0; i < MCOTA_PARTIALS; i++) {
    if (n->partial[i].used) return false;
  }
  return now_ms(n) - n->busy_at >= (n->survives ? n->cfg.apply_idle_ms : n->cfg.apply_long_ms) + n->apply_jitter;
}

bool mcota_node_runs_requested(mcota_node_t *n) {
  mcota_request_t r;
  if (!mcota_request_read(n->cfg.flash, &n->cfg.layout, &r)) return false;
  return same(running_hash(n, r.new_size), r.new_hash, 32u);
}

bool mcota_node_has_page(const mcota_node_t *n, uint32_t index) {
  return n->has_manifest && index < n->manifest.pages && map_get(n->have_map, index);
}

uint32_t mcota_node_peer_wants(const mcota_node_t *n, uint32_t peer) {
  uint32_t i, count = 0;
  if (peer >= MCOTA_PEERS || !n->peers[peer].used) return 0;
  for (i = 0; i < MCOTA_MAX_PAGES; i++) {
    if (map_get(n->peers[peer].want, i)) count++;
  }
  return count;
}

void mcota_node_drop(mcota_node_t *n) {
  if (n->has_manifest && n->can_store) store_flash(n)->erase(store_flash(n)->ctx, n->store_base);
  n->has_manifest = false;
  n->cand_active = false;
  n->provider_set = false;
  n->state = MCOTA_NODE_IDLE;
  n->have = 0;
  n->serve_count = 0;
  n->stored_blocks = 0;
  reset_transfer(n);
}

uint32_t mcota_node_archives(const mcota_node_t *n) {
  uint32_t i, count = 0;
  for (i = 0; i < MCOTA_SLOTS - 1u; i++) {
    if (n->archives[i].used) count++;
  }
  return count;
}
