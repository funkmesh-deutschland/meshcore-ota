#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_boot.h"
#include "../core/mcota_compact.h"
#include "../core/mcota_manifest.h"

#define FLASH_SIZE (256u * MCOTA_PAGE_SIZE)

static uint8_t mem[FLASH_SIZE], start[FLASH_SIZE];
static uint8_t writes[FLASH_SIZE / 4u];
static long ops, fail_at;
static jmp_buf power_cut;
static unsigned seed = 1;
static int failures, strict;
static mcota_compact_t engine;
static mcota_layout_t layout;

static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFFu; }

static void sim_read(void *ctx, uint32_t addr, void *out, uint32_t len) {
  (void)ctx;
  if (addr + len > FLASH_SIZE) { printf("ERROR: read out of range %u\n", addr); exit(2); }
  memcpy(out, mem + addr, len);
}

static void sim_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  uint32_t i, done = len;
  (void)ctx;
  if (addr % 4u || len % 4u || addr + len > FLASH_SIZE) { printf("ERROR: invalid write %u %u\n", addr, len); exit(2); }
  ops++;
  for (i = 0; i < len; i += 4u) {
    if (++writes[(addr + i) / 4u] > (strict ? 2u : 4u)) { printf("ERROR: word %u written too often\n", addr + i); exit(2); }
  }
  if (ops == fail_at) done = rnd() % (len + 1u);
  for (i = 0; i < done; i++) mem[addr + i] &= ((const uint8_t *)data)[i];
  if (ops == fail_at) {
    if (done < len) mem[addr + done] &= (uint8_t)(((const uint8_t *)data)[done] | rnd());
    longjmp(power_cut, 1);
  }
}

static void sim_erase(void *ctx, uint32_t addr) {
  uint32_t i;
  (void)ctx;
  if (addr % MCOTA_PAGE_SIZE || addr + MCOTA_PAGE_SIZE > FLASH_SIZE) { printf("ERROR: invalid erase\n"); exit(2); }
  if (addr >= layout.pool_end && addr < layout.status) { printf("ERROR: erase behind the pool\n"); exit(2); }
  ops++;
  memset(writes + addr / 4u, 0, MCOTA_PAGE_SIZE / 4u);
  if (ops == fail_at) {
    for (i = 0; i < MCOTA_PAGE_SIZE; i++) mem[addr + i] = (uint8_t)rnd();
    longjmp(power_cut, 1);
  }
  memset(mem + addr, 0xFF, MCOTA_PAGE_SIZE);
}

static const mcota_flash_t flash = { 0, sim_read, sim_write, sim_erase };

static void check(int ok, const char *what, long point) {
  if (!ok) {
    failures++;
    if (failures < 20) printf("ERROR: %s (failure point %ld)\n", what, point);
  }
}

static uint8_t *load(const char *dir, const char *name, size_t *len) {
  char path[512];
  FILE *fh;
  uint8_t *buf;
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  fh = fopen(path, "rb");
  if (!fh) { printf("ERROR: %s is missing\n", path); exit(2); }
  fseek(fh, 0, SEEK_END);
  *len = (size_t)ftell(fh);
  fseek(fh, 0, SEEK_SET);
  buf = malloc(*len + 1u);
  if (fread(buf, 1, *len, fh) != *len) exit(2);
  fclose(fh);
  return buf;
}

static int boot(long fail) {
  ops = 0;
  fail_at = fail;
  if (setjmp(power_cut)) return 0;
  mcota_boot(&flash, &layout);
  return 1;
}

static void boot_until_done(void) {
  int guard = 0;
  while (!boot(rnd() % 3u == 0 ? (long)(1 + rnd() % 3000u) : 0)) {
    if (++guard > 1000) { printf("ERROR: boot never finishes\n"); exit(2); }
  }
}

static uint32_t prepare(const uint8_t *old_img, size_t old_len, const uint8_t *pkg, uint32_t pool_pages, int arm) {
  uint32_t manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8), pages, old_pages, store, at, i, pos;
  mcota_manifest_t m;
  mcota_request_t r;
  uint8_t head[8];
  memset(&r, 0, sizeof(r));
  if (!mcota_manifest_parse(pkg + 2, manifest_len, &m)) { printf("ERROR: manifest unreadable\n"); exit(2); }
  old_pages = (uint32_t)((old_len + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE);
  pages = old_pages > m.pages ? old_pages : m.pages;
  memset(mem, 0xFF, sizeof(mem));
  memset(writes, 0, sizeof(writes));
  memcpy(mem, old_img, old_len);
  store = pages * MCOTA_PAGE_SIZE;
  layout.slot_a = 0;
  layout.pool_end = store + pool_pages * MCOTA_PAGE_SIZE;
  layout.status = layout.pool_end + MCOTA_PAGE_SIZE;
  layout.slot_pages = pages + pool_pages;
  layout.compact = &engine;
  for (i = store + pool_pages * MCOTA_PAGE_SIZE - 64u; i < layout.pool_end; i++) mem[i] = (uint8_t)(i * 7u);
  head[0] = 0x4C; head[1] = 0x4F; head[2] = 0x54; head[3] = 0x32;
  head[4] = (uint8_t)manifest_len; head[5] = (uint8_t)(manifest_len >> 8); head[6] = 0; head[7] = 0;
  memcpy(mem + store, head, 8);
  memcpy(mem + store + 8u, pkg + 2, manifest_len);
  at = store + 8u + ((manifest_len + 3u) & ~3u);
  memset(mem + at, 0, 4u * m.pages);
  at += 4u * m.pages;
  pos = 2u + manifest_len;
  for (i = 0; i < m.pages; i++) {
    uint32_t len = mcota_manifest_block_len(&m, i);
    memcpy(mem + at, pkg + pos, len);
    at += (len + 3u) & ~3u;
    pos += len;
  }
  if (at > layout.pool_end) return 0u;
  if (!arm) return at;
  r.pages = pages;
  r.new_size = m.new_size;
  r.old_size = m.old_size;
  memcpy(r.new_hash, m.new_hash, 32);
  memcpy(r.old_hash, m.old_hash, 32);
  r.compact = true;
  r.store = store;
  fail_at = 0;
  if (!mcota_request(&flash, &layout, &r)) { printf("ERROR: request rejected\n"); exit(2); }
  return at;
}

static long run_case(const char *name, const uint8_t *old_img, size_t old_len, const uint8_t *new_img, size_t new_len, const uint8_t *pkg, uint32_t pool_pages, long step) {
  long total, k, cases = 0;
  uint32_t used = prepare(old_img, old_len, pkg, pool_pages, 1);
  int wrapped;
  if (!used) { printf("ERROR: %s does not fit\n", name); exit(2); }
  memcpy(start, mem, sizeof(mem));
  strict = 1;
  check(boot(0), "clean swap aborts", 0);
  total = ops;
  check(memcmp(mem, new_img, new_len) == 0, "swap without failure does not yield the new firmware", 0);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_TRIAL, "state after swap is not trial boot", 0);
  wrapped = engine.saved_at[0] != 0u;
  for (k = 0, wrapped = 0; k < (long)engine.pages; k++) {
    if (engine.saved[k] && engine.saved_at[k] < used) wrapped = 1;
  }
  strict = 0;
  for (k = 1; k <= total; k += step) {
    seed = (unsigned)(k * 7919u);
    memcpy(mem, start, sizeof(mem));
    memset(writes, 0, sizeof(writes));
    check(!boot(k), "failure was not triggered", k);
    boot_until_done();
    check(memcmp(mem, new_img, new_len) == 0, "the new firmware does not run after a failure", k);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_TRIAL, "state after failure is not trial boot", k);
    cases++;
  }
  memcpy(mem, start, sizeof(mem));
  memset(writes, 0, sizeof(writes));
  boot(0); boot(0); boot(0);
  check(mcota_trials(&flash, &layout) == 3u && memcmp(mem, new_img, new_len) == 0, "three trial boots", 0);
  memcpy(start, mem, sizeof(mem));
  strict = 1;
  check(boot(0), "clean rollback aborts", 0);
  total = ops;
  check(memcmp(mem, old_img, old_len) == 0 && mcota_state(&flash, &layout) == MCOTA_STATE_REVERTED, "rollback without failure", 0);
  strict = 0;
  for (k = 1; k <= total; k += step) {
    seed = (unsigned)(k * 104729u);
    memcpy(mem, start, sizeof(mem));
    memset(writes, 0, sizeof(writes));
    check(!boot(k), "failure during rollback was not triggered", k);
    boot_until_done();
    check(memcmp(mem, old_img, old_len) == 0, "the old firmware does not run after a rollback with failure", k);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_REVERTED, "state after rollback with failure", k);
    cases++;
  }
  printf("%s: %ld failure points, pool %u pages, rollback copy %s\n", name, cases, pool_pages, wrapped ? "uses freed room" : "fits beside it");
  return cases;
}

static int plan_only(char **argv) {
  size_t old_len, new_len, n;
  uint8_t *old_img = load("", argv[2], &old_len), *pkg = load("", argv[3], &n), *new_img = load("", argv[4], &new_len);
  uint32_t total = (uint32_t)atoi(argv[5]), pages = (uint32_t)(((old_len > new_len ? old_len : new_len) + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE);
  uint32_t used;
  const char *names[] = { "does not fit", "with rollback copy, package is kept", "with rollback copy, package is consumed", "in place without rollback copy, package is kept" };
  uint32_t plan;
  if (total <= pages) { printf("no room\n"); return 1; }
  used = prepare(old_img, old_len, pkg, total - pages, 0);
  if (!used) { printf("firmware %u pages, pool %u pages: the package itself does not fit\n", pages, total - pages); return 0; }
  mcota_compact_open(&engine, &flash, &layout, pages * MCOTA_PAGE_SIZE);
  plan = mcota_compact_plan(&flash, 0, layout.pool_end, pages * MCOTA_PAGE_SIZE, &engine.manifest, engine.page, engine.packed, sizeof(engine.packed));
  printf("firmware %u pages, pool %u pages, package takes %u pages: %s\n", pages, total - pages, (used - pages * MCOTA_PAGE_SIZE + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE, names[plan]);
  if (plan != MCOTA_PLAN_NONE) {
    prepare(old_img, old_len, pkg, total - pages, 1);
    strict = 1;
    boot(0);
    printf("  trial run: %s, %ld write and erase operations\n", memcmp(mem, new_img, new_len) == 0 ? "new firmware matches" : "ERROR", ops);
  }
  return 0;
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "fixture";
  if (argc > 5 && strcmp(argv[1], "--plan") == 0) return plan_only(argv);
  size_t old_len, new_len, other_len, n;
  uint8_t *old_img = load(dir, "old.bin", &old_len), *new_img = load(dir, "new.bin", &new_len), *other = load(dir, "switch.bin", &other_len);
  uint8_t *pkg = load(dir, "update.mcota", &n), *big = load(dir, "switch.mcota", &n);
  long cases = 0;
  uint32_t pool;

  cases += run_case("small update", old_img, old_len, new_img, new_len, pkg, 20u, 1);
  cases += run_case("full size update, plenty of room", old_img, old_len, other, other_len, big, 40u, 3);
  for (pool = 16u; pool < 40u; pool++) {
    prepare(old_img, old_len, big, pool, 1);
    if (boot(0) && mcota_state(&flash, &layout) == MCOTA_STATE_TRIAL) break;
    check((memcmp(mem, old_img, old_len) == 0 && mcota_state(&flash, &layout) == MCOTA_STATE_REJECTED) ||
          (memcmp(mem, other, other_len) == 0 && mcota_state(&flash, &layout) == MCOTA_STATE_CONFIRMED), "pool too tight: neither cleanly rejected nor rebuilt in place", pool);
  }
  cases += run_case("full size update, tight room", old_img, old_len, other, other_len, big, pool, 1);

  {
    uint8_t *free_pkg = load(dir, "switch-norollback.mcota", &n);
    uint32_t tight = pool - 2u, used;
    long total, k, points = 0;
    used = prepare(old_img, old_len, free_pkg, tight, 1);
    check(used != 0u, "package without rollback does not fit into the tight pool", 0);
    memcpy(start, mem, sizeof(mem));
    strict = 1;
    check(boot(0), "in-place rebuild aborts", 0);
    total = ops;
    check(memcmp(mem, other, other_len) == 0 && mcota_state(&flash, &layout) == MCOTA_STATE_CONFIRMED, "in-place rebuild does not yield the new firmware", 0);
    check(memcmp(mem + 16u * MCOTA_PAGE_SIZE, start + 16u * MCOTA_PAGE_SIZE, used - 16u * MCOTA_PAGE_SIZE) == 0, "in-place rebuild changes the stored package", 0);
    strict = 0;
    for (k = 1; k <= total; k++) {
      seed = (unsigned)(k * 7919u);
      memcpy(mem, start, sizeof(mem));
      memset(writes, 0, sizeof(writes));
      check(!boot(k), "failure was not triggered", k);
      boot_until_done();
      check(memcmp(mem, other, other_len) == 0, "the new firmware does not run after a failure during the in-place rebuild", k);
      check(mcota_state(&flash, &layout) == MCOTA_STATE_CONFIRMED, "state after in-place rebuild", k);
      points++;
    }
    cases += points;
    printf("in-place rebuild without rollback copy: %ld failure points, pool %u pages\n", points, tight);
    prepare(old_img, old_len, big, tight, 1);
    boot(0);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_CONFIRMED && memcmp(mem, other, other_len) == 0, "on command it is not applied without rollback copy", 0);
  }

  prepare(old_img, old_len, pkg, 20u, 1);
  mem[100] ^= 0x10u;
  boot(0);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_REJECTED, "changed running firmware is not rejected", 0);
  {
    uint32_t used = prepare(old_img, old_len, pkg, 20u, 1);
    mem[used - 9u] ^= 0x04u;
    memcpy(start, mem, sizeof(mem));
    boot(0);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_REJECTED && memcmp(mem, old_img, old_len) == 0, "damaged block is not rejected", 0);
  }

  printf("swap without second area: %ld failure points checked, %d errors\n", cases, failures);
  return failures ? 1 : 0;
}
