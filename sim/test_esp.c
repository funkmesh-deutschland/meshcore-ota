#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_node.h"
#include "ed_25519.h"

#define NODES 5
#define STORE_SIZE (160u * MCOTA_PAGE_SIZE)

typedef struct {
  uint8_t *mem;
  uint8_t *store;
  mcota_flash_t flash;
  mcota_flash_t store_flash;
  mcota_node_t node;
  mcota_config_t cfg;
  int index;
  int switches;
} sim_t;

static sim_t sims[NODES];
static int link_up[NODES][NODES];
static uint32_t now_ms, slot_pages, tail_pages, flash_size;
static unsigned seed = 5;
static int loss_percent = 10, failures;

static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFFu; }
static void f_read(void *ctx, uint32_t addr, void *out, uint32_t len) { memcpy(out, ((sim_t *)ctx)->mem + addr, len); }
static void guard(sim_t *s, uint32_t addr, uint32_t len) {
  uint32_t area = (slot_pages + tail_pages) * MCOTA_PAGE_SIZE;
  if (addr + len > flash_size) { printf("ERROR: write out of range\n"); exit(2); }
  if (tail_pages && addr + len > s->cfg.layout.slot_a && addr < s->cfg.layout.slot_a + area) { printf("ERROR: write into the running area\n"); exit(2); }
}
static void f_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  uint32_t i;
  guard((sim_t *)ctx, addr, len);
  for (i = 0; i < len; i++) ((sim_t *)ctx)->mem[addr + i] &= ((const uint8_t *)data)[i];
}
static void f_erase(void *ctx, uint32_t addr) {
  guard((sim_t *)ctx, addr, MCOTA_PAGE_SIZE);
  memset(((sim_t *)ctx)->mem + addr, 0xFF, MCOTA_PAGE_SIZE);
}
static void x_read(void *ctx, uint32_t addr, void *out, uint32_t len) { memcpy(out, ((sim_t *)ctx)->store + addr, len); }
static void x_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  uint32_t i;
  if (addr + len > STORE_SIZE) { printf("ERROR: write outside the package store\n"); exit(2); }
  for (i = 0; i < len; i++) ((sim_t *)ctx)->store[addr + i] &= ((const uint8_t *)data)[i];
}
static void x_erase(void *ctx, uint32_t addr) { memset(((sim_t *)ctx)->store + addr, 0xFF, MCOTA_PAGE_SIZE); }
static uint32_t h_now(void *ctx) { (void)ctx; return now_ms; }
static uint32_t h_airtime(void *ctx, uint32_t len) { (void)ctx; return 80u + len * 56u / 10u; }
static uint32_t h_random(void *ctx) { (void)ctx; return rnd() | (rnd() << 15); }
static bool h_verify(void *ctx, const uint8_t *pub, const uint8_t *sig, const uint8_t *msg, uint32_t len) {
  (void)ctx;
  return ed25519_verify(sig, msg, len, pub) == 1;
}
static bool h_send(void *ctx, const uint8_t *data, uint32_t len, const mcota_route_t *route) {
  sim_t *from = (sim_t *)ctx;
  int i;
  (void)route;
  for (i = 0; i < NODES; i++) {
    if (!link_up[from->index][i] || (int)(rnd() % 100u) < loss_percent) continue;
    mcota_node_receive(&sims[i].node, data, len);
  }
  return true;
}

static void activate(void *ctx, bool new_image) {
  sim_t *s = (sim_t *)ctx;
  uint32_t a = s->cfg.layout.slot_a;
  (void)new_image;
  s->cfg.layout.slot_a = s->cfg.layout.slot_b;
  s->cfg.layout.slot_b = a;
  if (tail_pages) s->cfg.store_addr = a + slot_pages * MCOTA_PAGE_SIZE;
  s->switches++;
}

static void check(int ok, const char *what) {
  if (!ok) { failures++; printf("ERROR: %s\n", what); }
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

static void setup(int i, const uint8_t *image, size_t image_len, const uint8_t *key, const char *board) {
  sim_t *s = &sims[i];
  memset(s, 0, sizeof(*s));
  s->index = i;
  s->mem = malloc(flash_size);
  s->store = malloc(STORE_SIZE);
  memset(s->mem, 0xFF, flash_size);
  memset(s->store, 0xFF, STORE_SIZE);
  memcpy(s->mem, image, image_len);
  s->flash.ctx = s; s->flash.read = f_read; s->flash.write = f_write; s->flash.erase = f_erase;
  s->store_flash.ctx = s; s->store_flash.read = x_read; s->store_flash.write = x_write; s->store_flash.erase = x_erase;
  s->cfg.flash = &s->flash;
  s->cfg.store = tail_pages ? &s->flash : &s->store_flash;
  s->cfg.store_addr = tail_pages ? (2u * slot_pages + tail_pages) * MCOTA_PAGE_SIZE : 0;
  s->cfg.store_size = tail_pages ? tail_pages * MCOTA_PAGE_SIZE : STORE_SIZE;
  s->cfg.layout.slot_a = 0;
  s->cfg.layout.slot_b = (slot_pages + tail_pages) * MCOTA_PAGE_SIZE;
  s->cfg.layout.scratch = 0;
  s->cfg.layout.status = 2u * (slot_pages + tail_pages) * MCOTA_PAGE_SIZE;
  s->cfg.layout.slot_pages = slot_pages;
  s->cfg.layout.activate = activate;
  s->cfg.layout.activate_ctx = s;
  s->cfg.direct = true;
  s->cfg.ctx = s; s->cfg.now_ms = h_now; s->cfg.airtime_ms = h_airtime; s->cfg.send = h_send; s->cfg.verify = h_verify; s->cfg.random = h_random;
  s->cfg.self[0] = (uint8_t)(0x30 + i); s->cfg.self[1] = 0xE5; s->cfg.self[2] = (uint8_t)i; s->cfg.self[3] = 0x32;
  strncpy(s->cfg.board, board, 15);
  memcpy(s->cfg.keys[0], key, 32);
  s->cfg.key_set[0] = true;
  s->cfg.budget_ppm = 50000u;
  s->cfg.offer_min_ms = 10000u;
  s->cfg.offer_max_ms = 120000u;
  s->cfg.apply_idle_ms = 300000u;
  s->cfg.apply_long_ms = 3600000u;
  mcota_node_init(&s->node, &s->cfg);
}

static void run(uint32_t ms) {
  uint32_t end = now_ms + ms;
  int i;
  while (now_ms < end) {
    for (i = 0; i < NODES; i++) mcota_node_loop(&sims[i].node);
    now_ms += 100u;
  }
}

static void reboot(int i) {
  mcota_boot(&sims[i].flash, &sims[i].cfg.layout);
  mcota_node_init(&sims[i].node, &sims[i].cfg);
}

static void confirm(int i) {
  sim_t *s = &sims[i];
  mcota_confirm(&s->flash, &s->cfg.layout);
  if (tail_pages && mcota_state(&s->flash, &s->cfg.layout) == MCOTA_STATE_CONFIRMED && mcota_node_runs_requested(&s->node)) {
    uint32_t from = s->cfg.layout.slot_a + slot_pages * MCOTA_PAGE_SIZE, k;
    for (k = 0; k < tail_pages; k++) {
      f_erase(s, s->cfg.store_addr + k * MCOTA_PAGE_SIZE);
      f_write(s, s->cfg.store_addr + k * MCOTA_PAGE_SIZE, s->mem + from + k * MCOTA_PAGE_SIZE, MCOTA_PAGE_SIZE);
    }
    mcota_node_init(&s->node, &s->cfg);
  }
}

static int runs(int i, const uint8_t *img, size_t len) { return memcmp(sims[i].mem + sims[i].cfg.layout.slot_a, img, len) == 0; }

static void fill(int i, const uint8_t *pkg, uint32_t manifest_len, const mcota_manifest_t *m) {
  uint32_t pos = 2u + manifest_len, k;
  check(mcota_node_seed_manifest(&sims[i].node, pkg + 2, manifest_len), "valid manifest is rejected");
  for (k = 0; k < m->pages; k++) {
    uint32_t len = mcota_manifest_block_len(m, k);
    check(mcota_node_seed_block(&sims[i].node, k, pkg + pos, len), "valid block is rejected");
    pos += len;
  }
}

static void scenario(const char *dir, const char *board, uint32_t tails) {
  size_t old_len, new_len, pkg_len, n;
  uint8_t *old_img = load(dir, "old.bin", &old_len), *new_img = load(dir, "new.bin", &new_len), *pkg = load(dir, "update.mcota", &pkg_len), *good = load(dir, "good.pub", &n);
  uint32_t manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8), start;
  mcota_manifest_t m;
  int k;

  check(mcota_manifest_parse(pkg + 2, manifest_len, &m), "manifest is not readable");
  slot_pages = (uint32_t)(((old_len > new_len ? old_len : new_len) + MCOTA_PAGE_SIZE - 1u) / MCOTA_PAGE_SIZE) + 8u;
  tail_pages = tails;
  flash_size = (2u * (slot_pages + tail_pages) + MCOTA_STATUS_PAGES) * MCOTA_PAGE_SIZE;
  memset(link_up, 0, sizeof(link_up));
  for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, board);
  link_up[0][1] = link_up[1][0] = 1;

  fill(0, pkg, manifest_len, &m);
  check(sims[0].node.state == MCOTA_NODE_COMPLETE, "first node is not complete");
  start = now_ms;
  for (k = 0; k < 20000 && sims[1].node.state != MCOTA_NODE_COMPLETE; k++) run(60000u);
  check(sims[1].node.state == MCOTA_NODE_COMPLETE, "neighbour does not become complete");
  printf("%u pages transferred in %u min, %u packets from the provider, %u requests\n", m.pages, (now_ms - start) / 60000u, sims[0].node.tx_packets, sims[1].node.requests_sent);

  check(mcota_node_apply(&sims[1].node), "apply is rejected");
  reboot(1);
  check(sims[1].switches == 1 && runs(1, new_img, new_len), "switch to the other area does not happen");
  reboot(1);
  check(mcota_state(&sims[1].flash, &sims[1].cfg.layout) == MCOTA_STATE_TRIAL && mcota_node_runs_requested(&sims[1].node), "trial boot after the switch");
  confirm(1);
  check(sims[1].node.state == MCOTA_NODE_APPLIED && sims[1].node.stored_blocks > 0u, "the package is missing right after the confirmation");
  reboot(1);
  check(runs(1, new_img, new_len) && sims[1].node.state == MCOTA_NODE_APPLIED && sims[1].node.stored_blocks > 0u, "after the confirmation the new firmware does not run or the package is missing");

  link_up[0][1] = link_up[1][0] = 0;
  link_up[1][2] = link_up[2][1] = 1;
  for (k = 0; k < 20000 && sims[2].node.state != MCOTA_NODE_COMPLETE; k++) run(60000u);
  check(sims[2].node.state == MCOTA_NODE_COMPLETE, "updated node does not share");
  check(mcota_node_apply(&sims[2].node), "apply on the third node");
  for (k = 0; k < 7; k++) reboot(2);
  check(runs(2, old_img, old_len) && mcota_state(&sims[2].flash, &sims[2].cfg.layout) == MCOTA_STATE_REVERTED, "rollback without confirmation does not happen");
  check(sims[2].node.state == MCOTA_NODE_FAILED, "failed update is not blocked");

  fill(3, pkg, manifest_len, &m);
  check(mcota_node_apply(&sims[3].node), "apply on the fourth node");
  reboot(3);
  check(sims[3].switches == 1 && runs(3, new_img, new_len), "switch on the fourth node does not happen");
  activate(&sims[3], false);
  for (k = 0; k < 3; k++) reboot(3);
  check(sims[3].switches == 2 && runs(3, old_img, old_len), "switches again after the bootloader rollback before the first run");
  check(mcota_state(&sims[3].flash, &sims[3].cfg.layout) == MCOTA_STATE_REVERTED && sims[3].node.state == MCOTA_NODE_FAILED, "bootloader rollback before the first run is not recorded as failed");

  fill(4, pkg, manifest_len, &m);
  check(mcota_node_apply(&sims[4].node), "apply on the fifth node");
  reboot(4);
  reboot(4);
  check(sims[4].switches == 1 && mcota_state(&sims[4].flash, &sims[4].cfg.layout) == MCOTA_STATE_TRIAL, "trial boot on the fifth node");
  activate(&sims[4], false);
  for (k = 0; k < 3; k++) reboot(4);
  check(sims[4].switches == 2 && runs(4, old_img, old_len), "switches again after the bootloader rollback during the trial boot");
  check(mcota_state(&sims[4].flash, &sims[4].cfg.layout) == MCOTA_STATE_REVERTED && sims[4].node.state == MCOTA_NODE_FAILED, "bootloader rollback during the trial boot is not recorded as failed");

  printf("switch without loader (%s, %u pages, %s): %d errors\n", board, m.pages, tails ? "package at the end of the other area" : "separate package store", failures);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "fixture", *board = argc > 2 ? argv[2] : "sim_board";
  scenario(dir, board, 0u);
  scenario(dir, board, argc > 3 ? (uint32_t)atoi(argv[3]) : 16u);
  return failures ? 1 : 0;
}
