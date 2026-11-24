#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_boot.h"

#define SLOT_PAGES 6u
#define FLASH_PAGES (2u * SLOT_PAGES + 1u + MCOTA_STATUS_PAGES)
#define FLASH_SIZE (FLASH_PAGES * MCOTA_PAGE_SIZE)

static uint8_t mem[FLASH_SIZE];
static long ops, fail_at;
static jmp_buf power_cut;
static unsigned seed = 1;

static unsigned rnd(void) {
  seed = seed * 1103515245u + 12345u;
  return (seed >> 16) & 0x7FFFu;
}

static void sim_read(void *ctx, uint32_t addr, void *out, uint32_t len) {
  (void)ctx;
  memcpy(out, mem + addr, len);
}

static void sim_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  uint32_t i, done = len;
  (void)ctx;
  if (addr % 4u || len % 4u || addr + len > FLASH_SIZE) { printf("ERROR: invalid write\n"); exit(2); }
  ops++;
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
  ops++;
  if (ops == fail_at) {
    for (i = 0; i < MCOTA_PAGE_SIZE; i++) mem[addr + i] = (uint8_t)rnd();
    longjmp(power_cut, 1);
  }
  memset(mem + addr, 0xFF, MCOTA_PAGE_SIZE);
}

static const mcota_flash_t flash = { 0, sim_read, sim_write, sim_erase };
static const mcota_layout_t layout = {
  0u, SLOT_PAGES * MCOTA_PAGE_SIZE, 2u * SLOT_PAGES * MCOTA_PAGE_SIZE, (2u * SLOT_PAGES + 1u) * MCOTA_PAGE_SIZE, SLOT_PAGES, 0u, 0, 0, 0
};

static uint8_t old_image[4u * MCOTA_PAGE_SIZE + 1234u];
static uint8_t new_image[5u * MCOTA_PAGE_SIZE + 77u];
static uint8_t start[FLASH_SIZE];
static int failures;

static void check(int ok, const char *what, long point) {
  if (!ok) {
    failures++;
    if (failures < 20) printf("ERROR: %s (failure point %ld)\n", what, point);
  }
}

static void fill(uint8_t *p, size_t n, unsigned s) {
  size_t i;
  seed = s;
  for (i = 0; i < n; i++) p[i] = (uint8_t)rnd();
}

static void prepare(int bad_hash) {
  mcota_request_t r;
  memset(&r, 0, sizeof(r));
  memset(mem, 0xFF, sizeof(mem));
  memcpy(mem + layout.slot_a, old_image, sizeof(old_image));
  memcpy(mem + layout.slot_b, new_image, sizeof(new_image));
  r.pages = SLOT_PAGES;
  r.new_size = sizeof(new_image);
  r.old_size = sizeof(old_image);
  mcota_hash_region(&flash, layout.slot_b, r.new_size, r.new_hash);
  mcota_hash_region(&flash, layout.slot_a, r.old_size, r.old_hash);
  if (bad_hash) r.new_hash[3] ^= 1u;
  fail_at = 0;
  if (!mcota_request(&flash, &layout, &r)) { printf("ERROR: request rejected\n"); exit(2); }
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
  while (!boot(rnd() % 3u == 0 ? (long)(1 + rnd() % 400u) : 0)) {
    if (++guard > 1000) { printf("ERROR: boot never finishes\n"); exit(2); }
  }
}

static int a_is(const uint8_t *img, size_t n) { return memcmp(mem + layout.slot_a, img, n) == 0; }
static int b_is(const uint8_t *img, size_t n) { return memcmp(mem + layout.slot_b, img, n) == 0; }

int main(void) {
  long total, k, cases = 0;
  int i;

  fill(old_image, sizeof(old_image), 11);
  fill(new_image, sizeof(new_image), 22);

  prepare(0);
  memcpy(start, mem, sizeof(mem));
  check(mcota_state(&flash, &layout) == MCOTA_STATE_ARMED, "request is not armed", 0);
  check(boot(0), "clean swap aborts", 0);
  total = ops;
  check(a_is(new_image, sizeof(new_image)) && b_is(old_image, sizeof(old_image)), "swap without failure", 0);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_TRIAL && mcota_trials(&flash, &layout) == 1u, "trial boot 1", 0);

  for (k = 1; k <= total; k++) {
    seed = (unsigned)(k * 7919u);
    memcpy(mem, start, sizeof(mem));
    check(!boot(k), "failure was not triggered", k);
    boot_until_done();
    check(a_is(new_image, sizeof(new_image)), "A does not hold the new image after a failure", k);
    check(b_is(old_image, sizeof(old_image)), "B does not hold the old image after a failure", k);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_TRIAL, "state after swap is not trial boot", k);
    cases++;
  }

  memcpy(mem, start, sizeof(mem));
  boot(0);
  boot(0);
  boot(0);
  check(mcota_trials(&flash, &layout) == 3u && a_is(new_image, sizeof(new_image)), "three trial boots", 0);
  memcpy(start, mem, sizeof(mem));
  check(boot(0), "clean rollback aborts", 0);
  total = ops;
  check(a_is(old_image, sizeof(old_image)) && b_is(new_image, sizeof(new_image)), "rollback without failure", 0);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_REVERTED, "state after rollback", 0);
  for (k = 1; k <= total; k++) {
    seed = (unsigned)(k * 104729u);
    memcpy(mem, start, sizeof(mem));
    check(!boot(k), "failure during rollback was not triggered", k);
    boot_until_done();
    check(a_is(old_image, sizeof(old_image)), "A does not hold the old image after a rollback with failure", k);
    check(b_is(new_image, sizeof(new_image)), "B does not hold the new image after a rollback with failure", k);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_REVERTED, "state after rollback with failure", k);
    cases++;
  }
  for (i = 0; i < 5; i++) boot(0);
  check(a_is(old_image, sizeof(old_image)) && mcota_state(&flash, &layout) == MCOTA_STATE_REVERTED, "further boots after the rollback change nothing", 0);

  prepare(0);
  boot(0);
  mcota_confirm(&flash, &layout);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_CONFIRMED, "confirmation", 0);
  for (i = 0; i < 6; i++) boot(0);
  check(a_is(new_image, sizeof(new_image)) && mcota_state(&flash, &layout) == MCOTA_STATE_CONFIRMED, "confirmed image stays", 0);

  prepare(1);
  boot(0);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_REJECTED, "wrong hash is rejected", 0);
  check(a_is(old_image, sizeof(old_image)) && b_is(new_image, sizeof(new_image)), "rejection does not change the slots", 0);

  prepare(0);
  mem[layout.slot_a + 100u] ^= 0x10u;
  boot(0);
  check(mcota_state(&flash, &layout) == MCOTA_STATE_REJECTED, "changed running image is rejected", 0);

  for (i = 1; i <= 3; i++) {
    int t;
    prepare(0);
    for (t = 0; t < i; t++) boot(0);
    fill(mem + layout.slot_a, sizeof(new_image), 33);
    memcpy(start, mem, sizeof(mem));
    for (t = 0; t < 4; t++) boot(0);
    check(memcmp(mem + layout.slot_a, start + layout.slot_a, sizeof(new_image)) == 0, "a firmware installed during the trial is changed by the loader", i);
    check(mcota_state(&flash, &layout) == MCOTA_STATE_REJECTED, "a firmware installed during the trial leaves the trial state", i);
    check(mcota_clear(&flash, &layout) && mcota_state(&flash, &layout) == MCOTA_STATE_NONE, "the status cannot be cleared", i);
  }
  prepare(0);
  check(!mcota_clear(&flash, &layout) && mcota_state(&flash, &layout) == MCOTA_STATE_ARMED, "the status is cleared before the swap", 0);

  memset(mem, 0xFF, sizeof(mem));
  memcpy(mem + layout.slot_a, old_image, sizeof(old_image));
  boot(0);
  check(ops == 0 && mcota_state(&flash, &layout) == MCOTA_STATE_NONE, "nothing is written without a request", 0);
  for (k = 0; k < 200; k++) {
    fill(mem + layout.status, MCOTA_PAGE_SIZE, (unsigned)(900 + k));
    boot(0);
    check(ops == 0 && a_is(old_image, sizeof(old_image)), "random data in the status page triggers something", k);
  }

  {
    mcota_request_t r;
    prepare(0);
    boot(0);
    memset(&r, 0, sizeof(r));
    r.pages = 1; r.new_size = 4; r.old_size = 4;
    check(!mcota_request(&flash, &layout, &r), "new request during the trial boot is accepted", 0);
  }

  printf("%ld failure points checked, %d errors\n", cases, failures);
  return failures ? 1 : 0;
}
