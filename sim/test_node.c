#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_node.h"
#include "../core/mcota_sha256.h"
#include "ed_25519.h"

#define NODES 8
#define MAX_PAGES 20u
#define FLASH_SIZE ((2u * MAX_PAGES + 1u + MCOTA_STATUS_PAGES) * MCOTA_PAGE_SIZE)
#define EXT_SIZE (49u * MCOTA_PAGE_SIZE)

typedef struct {
  uint8_t mem[FLASH_SIZE];
  uint8_t writes[FLASH_SIZE / 4u];
  uint8_t ext[EXT_SIZE];
  uint8_t ext_writes[EXT_SIZE / 4u];
  mcota_flash_t flash;
  mcota_flash_t ext_flash;
  mcota_compact_t engine;
  mcota_node_t node;
  mcota_config_t cfg;
  int index;
  int off;
} sim_t;

static sim_t sims[NODES];
static int link_up[NODES][NODES];
static uint32_t now_ms;
static unsigned seed = 5;
static int loss_percent = 20;
static int corrupt_percent = 0;
static long delivered, dropped, corrupted;
static int failures;

static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFFu; }

static uint32_t seen[NODES][128];
static int seen_at[NODES];

static int duplicate(int node, const uint8_t *data, uint32_t len) {
  uint32_t h = 2166136261u, i;
  for (i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
  for (i = 0; i < 128u; i++) {
    if (seen[node][i] == h) return 1;
  }
  seen[node][seen_at[node]] = h;
  seen_at[node] = (seen_at[node] + 1) % 128;
  return 0;
}

static void f_read(void *ctx, uint32_t addr, void *out, uint32_t len) { memcpy(out, ((sim_t *)ctx)->mem + addr, len); }
static void f_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  sim_t *s = (sim_t *)ctx;
  uint32_t i;
  if (addr % 4u || len % 4u || addr + len > FLASH_SIZE) { printf("ERROR: invalid write %u %u\n", addr, len); exit(2); }
  for (i = 0; i < len; i++) s->mem[addr + i] &= ((const uint8_t *)data)[i];
  for (i = 0; i < len; i += 4u) {
    if (++s->writes[(addr + i) / 4u] > 2u) { printf("ERROR: word %u written more than twice\n", addr + i); exit(2); }
  }
}
static void f_erase(void *ctx, uint32_t addr) {
  sim_t *s = (sim_t *)ctx;
  if (addr % MCOTA_PAGE_SIZE || addr + MCOTA_PAGE_SIZE > FLASH_SIZE) { printf("ERROR: invalid erase\n"); exit(2); }
  memset(s->mem + addr, 0xFF, MCOTA_PAGE_SIZE);
  memset(s->writes + addr / 4u, 0, MCOTA_PAGE_SIZE / 4u);
}
static void x_read(void *ctx, uint32_t addr, void *out, uint32_t len) { memcpy(out, ((sim_t *)ctx)->ext + addr, len); }
static void x_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  sim_t *s = (sim_t *)ctx;
  uint32_t i;
  if (addr % 4u || len % 4u || addr + len > EXT_SIZE) { printf("ERROR: invalid write in external flash %u %u\n", addr, len); exit(2); }
  for (i = 0; i < len; i++) s->ext[addr + i] &= ((const uint8_t *)data)[i];
  for (i = 0; i < len; i += 4u) {
    if (++s->ext_writes[(addr + i) / 4u] > 2u) { printf("ERROR: word %u in external flash written more than twice\n", addr + i); exit(2); }
  }
}
static void x_erase(void *ctx, uint32_t addr) {
  sim_t *s = (sim_t *)ctx;
  if (addr % MCOTA_PAGE_SIZE || addr + MCOTA_PAGE_SIZE > EXT_SIZE) { printf("ERROR: invalid erase in external flash\n"); exit(2); }
  memset(s->ext + addr, 0xFF, MCOTA_PAGE_SIZE);
  memset(s->ext_writes + addr / 4u, 0, MCOTA_PAGE_SIZE / 4u);
}
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
  if (from->off) return true;
  for (i = 0; i < NODES; i++) {
    uint8_t copy[256];
    if (!link_up[from->index][i] || sims[i].off) continue;
    if ((int)(rnd() % 100u) < loss_percent) { dropped++; continue; }
    memcpy(copy, data, len);
    if ((int)(rnd() % 100u) < corrupt_percent && len > 21u) { copy[20u + rnd() % (len - 20u)] ^= (uint8_t)(1u + rnd() % 255u); corrupted++; }
    if (duplicate(i, copy, len)) { dropped++; continue; }
    delivered++;
    mcota_node_receive(&sims[i].node, copy, len);
  }
  return true;
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

static int with_ext;
static int compact;

static void setup(int i, const uint8_t *image, size_t image_len, const uint8_t *key, const char *board, uint32_t slot_pages) {
  sim_t *s = &sims[i];
  int k;
  memset(s, 0, sizeof(*s));
  memset(seen[i], 0, sizeof(seen[i]));
  for (k = 0; k < NODES; k++) link_up[i][k] = link_up[k][i] = 0;
  s->index = i;
  memset(s->mem, 0xFF, sizeof(s->mem));
  memcpy(s->mem, image, image_len);
  s->flash.ctx = s; s->flash.read = f_read; s->flash.write = f_write; s->flash.erase = f_erase;
  memset(s->ext, 0xFF, sizeof(s->ext));
  s->ext_flash.ctx = s; s->ext_flash.read = x_read; s->ext_flash.write = x_write; s->ext_flash.erase = x_erase;
  if (with_ext) { s->cfg.store = &s->ext_flash; s->cfg.store_addr = MCOTA_PAGE_SIZE; s->cfg.store_size = EXT_SIZE - MCOTA_PAGE_SIZE; }
  s->cfg.flash = &s->flash;
  s->cfg.layout.slot_a = 0;
  s->cfg.layout.slot_b = slot_pages * MCOTA_PAGE_SIZE;
  s->cfg.layout.scratch = 2u * slot_pages * MCOTA_PAGE_SIZE;
  s->cfg.layout.status = (2u * slot_pages + 1u) * MCOTA_PAGE_SIZE;
  s->cfg.layout.slot_pages = slot_pages;
  if (compact) {
    s->cfg.compact = true;
    s->cfg.layout.slot_b = 0;
    s->cfg.layout.scratch = 0;
    s->cfg.layout.pool_end = slot_pages * MCOTA_PAGE_SIZE;
    s->cfg.layout.status = (slot_pages + 1u) * MCOTA_PAGE_SIZE;
    s->cfg.layout.compact = &s->engine;
  }
  s->cfg.ctx = s; s->cfg.now_ms = h_now; s->cfg.airtime_ms = h_airtime; s->cfg.send = h_send; s->cfg.verify = h_verify; s->cfg.random = h_random;
  s->cfg.self[0] = (uint8_t)(0x10 + i); s->cfg.self[1] = 0xAB; s->cfg.self[2] = (uint8_t)i; s->cfg.self[3] = 0x01;
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

static void connect(int a, int b) { link_up[a][b] = link_up[b][a] = 1; }

static void run(uint32_t ms) {
  uint32_t end = now_ms + ms;
  int i;
  while (now_ms < end) {
    for (i = 0; i < NODES; i++) {
      if (!sims[i].off) mcota_node_loop(&sims[i].node);
    }
    now_ms += 100u;
  }
}

static int slot_b_is(int i, const uint8_t *img, size_t len) { return memcmp(sims[i].mem + sims[i].cfg.layout.slot_b, img, len) == 0; }
static int slot_a_is(int i, const uint8_t *img, size_t len) { return memcmp(sims[i].mem, img, len) == 0; }

static uint8_t *pkg;
static uint32_t manifest_len;

static void seed_all(int i) {
  mcota_manifest_t m;
  uint32_t pos = 2u + manifest_len, k;
  check(mcota_node_seed_manifest(&sims[i].node, pkg + 2, manifest_len), "valid manifest is rejected");
  mcota_manifest_parse(pkg + 2, manifest_len, &m);
  for (k = 0; k < m.pages; k++) {
    uint32_t len = mcota_manifest_block_len(&m, k);
    check(mcota_node_seed_block(&sims[i].node, k, pkg + pos, len), "valid block is rejected");
    pos += len;
  }
}

static void wait_complete(int i, int rounds) {
  int k;
  for (k = 0; k < rounds && sims[i].node.state != MCOTA_NODE_COMPLETE; k++) run(60u * 1000u);
}

int main(int argc, char **argv) {
  const char *dir = argc > 1 ? argv[1] : "fixture";
  size_t old_len, new_len, pkg_len, n;
  uint8_t *old_img = load(dir, "old.bin", &old_len), *new_img = load(dir, "new.bin", &new_len);
  uint8_t *good = load(dir, "good.pub", &n), *other = load(dir, "other.pub", &n);
  uint32_t pos, i, start, before, single, shared;
  int tampered = 0;
  mcota_manifest_t m;
  int k;
  pkg = load(dir, "update.mcota", &pkg_len);
  manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);

  for (k = 0; k < NODES; k++) setup(k, old_img, old_len, k == 3 ? other : good, k == 4 ? "other_board" : "sim_board", MAX_PAGES);
  connect(0, 1); connect(1, 2); connect(0, 3); connect(0, 4);

  check(mcota_manifest_parse(pkg + 2, manifest_len, &m), "manifest is not readable");
  pkg[2 + 60] ^= 1u;
  check(!mcota_node_seed_manifest(&sims[0].node, pkg + 2, manifest_len), "tampered manifest is accepted");
  pkg[2 + 60] ^= 1u;
  check(!mcota_node_seed_manifest(&sims[3].node, pkg + 2, manifest_len), "manifest with a foreign key is accepted");
  check(!mcota_node_seed_manifest(&sims[4].node, pkg + 2, manifest_len), "manifest for another device is accepted");
  check(mcota_node_seed_manifest(&sims[0].node, pkg + 2, manifest_len), "valid manifest is rejected");
  pos = 2u + manifest_len;
  for (i = 0; i < m.pages; i++) pos += mcota_manifest_block_len(&m, i);
  for (i = m.pages; i-- > 0u; ) {
    uint32_t len = mcota_manifest_block_len(&m, i);
    pos -= len;
    if (len > 0u && !tampered) {
      tampered = 1;
      pkg[pos + 5u] ^= 0x40u;
      check(!mcota_node_seed_block(&sims[0].node, i, pkg + pos, len), "tampered block is accepted");
      pkg[pos + 5u] ^= 0x40u;
    }
    check(mcota_node_seed_block(&sims[0].node, i, pkg + pos, len), "valid block in reverse order is rejected");
  }
  check(sims[0].node.state == MCOTA_NODE_COMPLETE && slot_b_is(0, new_img, new_len), "first node is not complete");
  check(sims[0].node.can_serve && sims[0].node.serve_count == m.pages, "first node cannot share");

  corrupt_percent = 10;
  start = now_ms;
  run(20u * 60u * 1000u);
  check(sims[1].node.has_manifest && sims[1].node.have > 0u, "node 1 fetched nothing after 20 minutes");
  before = sims[1].node.have;
  mcota_node_init(&sims[1].node, &sims[1].cfg);
  check(sims[1].node.has_manifest && sims[1].node.have == before, "progress is lost on reboot");

  wait_complete(2, 4000);
  wait_complete(1, 4000);
  check(sims[1].node.state == MCOTA_NODE_COMPLETE && slot_b_is(1, new_img, new_len), "node 1 is not complete");
  check(sims[2].node.state == MCOTA_NODE_COMPLETE && slot_b_is(2, new_img, new_len), "node 2 (via node 1) is not complete");
  check(slot_a_is(1, old_img, old_len) && slot_a_is(2, old_img, old_len), "running firmware was changed before apply");
  check(!sims[3].node.has_manifest && sims[3].node.tx_packets == 0u, "node with a foreign key took part");
  check(!sims[4].node.has_manifest && sims[4].node.tx_packets == 0u, "node with another device took part");
  for (i = 0; i < FLASH_SIZE - MAX_PAGES * MCOTA_PAGE_SIZE; i++) {
    if (sims[3].mem[MAX_PAGES * MCOTA_PAGE_SIZE + i] != 0xFF) { check(0, "flash of the foreign node was written"); break; }
  }
  for (k = 0; k < 3; k++) {
    uint64_t allowed = (uint64_t)(now_ms - start) * sims[k].cfg.budget_ppm / 1000000u + 2000u;
    if (sims[k].node.sent_ms > allowed) { check(0, "airtime budget exceeded"); printf("  node %d: sent for %u ms, allowed %u ms\n", k, sims[k].node.sent_ms, (unsigned)allowed); }
  }
  check(sims[1].node.bundles > 0u && sims[2].node.bundles > 0u, "small blocks were not bundled");
  printf("chain over 2 stations: %u min, %ld packets delivered, %ld lost, %ld corrupted, requests node 1 %u, node 2 %u\n",
         (now_ms - start) / 60000u, delivered, dropped, corrupted, sims[1].node.requests_sent, sims[2].node.requests_sent);

  corrupt_percent = 0;
  check(!mcota_node_apply_due(&sims[2].node), "automatic apply without release in the manifest");
  check(mcota_node_apply(&sims[2].node), "apply is rejected");
  check(!mcota_node_apply(&sims[2].node), "second apply is accepted");
  mcota_boot(&sims[2].flash, &sims[2].cfg.layout);
  check(slot_a_is(2, new_img, new_len), "the new firmware does not run after the swap");
  mcota_node_init(&sims[2].node, &sims[2].cfg);
  check(mcota_node_runs_requested(&sims[2].node), "running firmware is not recognised as the requested one");
  mcota_confirm(&sims[2].flash, &sims[2].cfg.layout);
  check(sims[2].node.state == MCOTA_NODE_APPLIED && sims[2].node.can_serve && sims[2].node.serve_count == m.pages, "updated node no longer shares");
  connect(2, 5);
  wait_complete(5, 4000);
  check(sims[5].node.state == MCOTA_NODE_COMPLETE && slot_b_is(5, new_img, new_len), "late node does not get the update from the updated node");

  check(mcota_node_seed_manifest(&sims[2].node, pkg + 2, manifest_len), "loading again on an updated node is rejected");
  check(sims[2].node.serve_count == m.pages, "loading the same manifest again erases the stored blocks");

  check(mcota_node_apply(&sims[1].node), "apply on node 1 is rejected");
  for (k = 0; k < 4; k++) mcota_boot(&sims[1].flash, &sims[1].cfg.layout);
  check(slot_a_is(1, old_img, old_len) && mcota_state(&sims[1].flash, &sims[1].cfg.layout) == MCOTA_STATE_REVERTED, "rollback without confirmation does not happen");
  mcota_node_init(&sims[1].node, &sims[1].cfg);
  check(sims[1].node.state == MCOTA_NODE_FAILED, "failed update is not blocked");
  check(!mcota_node_apply(&sims[1].node), "failed update can be applied again");
  before = sims[1].node.tx_packets;
  run(30u * 60u * 1000u);
  check(sims[1].node.tx_packets == before, "node keeps offering a failed update");

  for (k = 0; k < 2; k++) {
    uint32_t pages = k == 0 ? m.pages + 1u : m.pages;
    setup(6, old_img, old_len, good, "sim_board", pages);
    seed_all(6);
    check(sims[6].node.state == MCOTA_NODE_COMPLETE && !sims[6].node.can_serve, "node without room counts as able to share");
    check(sims[6].node.can_store == (k == 0), "room for the manifest detected wrongly");
    check(mcota_node_apply(&sims[6].node), "apply without room for sharing is rejected");
    mcota_boot(&sims[6].flash, &sims[6].cfg.layout);
    mcota_node_init(&sims[6].node, &sims[6].cfg);
    check(slot_a_is(6, new_img, new_len) && mcota_node_runs_requested(&sims[6].node), "update without room for sharing cannot be confirmed");
    check((sims[6].node.state == MCOTA_NODE_APPLIED) == (k == 0), "manifest does not survive the swap");
    before = sims[6].node.tx_packets;
    run(10u * 60u * 1000u);
    check(sims[6].node.tx_packets == before, "node without stored blocks offers");
  }

  with_ext = 1;
  setup(6, old_img, old_len, good, "sim_board", m.pages);
  setup(7, old_img, old_len, good, "sim_board", m.pages);
  with_ext = 0;
  seed_all(6);
  check(sims[6].node.can_serve && sims[6].node.serve_count == m.pages, "external flash is not used for sharing");
  for (i = 0; i < FLASH_SIZE - 2u * m.pages * MCOTA_PAGE_SIZE; i++) {
    if (sims[6].mem[2u * m.pages * MCOTA_PAGE_SIZE + i] != 0xFF) { check(0, "with external flash there are writes behind area B"); break; }
  }
  check(mcota_node_apply(&sims[6].node), "apply with external flash is rejected");
  mcota_boot(&sims[6].flash, &sims[6].cfg.layout);
  mcota_node_init(&sims[6].node, &sims[6].cfg);
  mcota_confirm(&sims[6].flash, &sims[6].cfg.layout);
  check(sims[6].node.state == MCOTA_NODE_APPLIED && sims[6].node.serve_count == m.pages, "external flash does not survive the swap");
  connect(6, 7);
  loss_percent = 10;
  wait_complete(7, 4000);
  check(sims[7].node.state == MCOTA_NODE_COMPLETE && slot_b_is(7, new_img, new_len), "sharing from external flash fails");
  loss_percent = 20;

  setup(6, new_img, new_len, good, "sim_board", MAX_PAGES);
  check(mcota_node_seed_manifest(&sims[6].node, pkg + 2, manifest_len) && sims[6].node.state == MCOTA_NODE_APPLIED, "manifest on an already updated node is not recognised");
  before = sims[6].node.tx_packets;
  run(10u * 60u * 1000u);
  check(sims[6].node.tx_packets == before && sims[6].node.stored_blocks == 0u, "updated node without blocks offers");

  loss_percent = 0;
  for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
  seed_all(0);
  connect(0, 1);
  wait_complete(1, 4000);
  single = sims[0].node.tx_packets;
  check(sims[1].node.state == MCOTA_NODE_COMPLETE, "single receiver does not become complete");

  for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
  seed_all(0);
  connect(0, 1); connect(0, 2); connect(0, 3);
  sims[2].off = 1; sims[3].off = 1;
  run(3u * 60u * 1000u);
  sims[2].off = 0;
  run(3u * 60u * 1000u);
  sims[3].off = 0;
  wait_complete(1, 4000); wait_complete(2, 4000); wait_complete(3, 4000);
  shared = sims[0].node.tx_packets;
  for (k = 1; k < 4; k++) printf("  node %d: requests %u, overheard %u, bundles %u, received %u\n", k, sims[k].node.requests_sent, sims[k].node.overheard, sims[k].node.bundles, sims[k].node.rx_packets);
  check(sims[1].node.state == MCOTA_NODE_COMPLETE && sims[2].node.state == MCOTA_NODE_COMPLETE && sims[3].node.state == MCOTA_NODE_COMPLETE, "three receivers do not become complete");
  check(slot_b_is(2, new_img, new_len) && slot_b_is(3, new_img, new_len), "overheard firmware is wrong");
  check(shared < 2u * single, "overhearing saves no packets");
  printf("overhearing: one receiver %u packets, three staggered receivers together %u packets (separately it would be %u)\n", single, shared, 3u * single);

  loss_percent = 10;
  for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
  seed_all(0); seed_all(2);
  connect(0, 1); connect(2, 1);
  sims[2].off = 1;
  for (k = 0; k < 600 && sims[1].node.have < 3u; k++) run(10u * 1000u);
  check(sims[1].node.have >= 3u && sims[1].node.state == MCOTA_NODE_FETCHING, "node 1 does not load from the first provider");
  sims[0].off = 1; sims[2].off = 0;
  sims[2].node.offer_at = now_ms + 100u * 60u * 60u * 1000u;
  wait_complete(1, 4000);
  check(sims[1].node.state == MCOTA_NODE_COMPLETE && slot_b_is(1, new_img, new_len), "no other provider is found after the provider fails");
  check(sims[1].node.seeks_sent > 0u, "no seek was sent into the mesh");
  printf("provider change: %u seeks, %u requests\n", sims[1].node.seeks_sent, sims[1].node.requests_sent);

  loss_percent = 10;
  for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
  seed_all(0);
  memcpy(sims[1].mem + sims[1].cfg.layout.slot_b, new_img, new_len);
  connect(0, 1);
  for (k = 0; k < 600 && sims[1].node.serve_count < m.pages; k++) run(10u * 1000u);
  check(sims[1].node.state == MCOTA_NODE_COMPLETE && sims[1].node.serve_count == m.pages, "blocks for pages already present are not fetched afterwards");
  printf("catching up: %u requests for %u blocks\n", sims[1].node.requests_sent, m.pages);

  {
    size_t len2, newer_len;
    uint8_t *pkg2 = load(dir, "update2.mcota", &len2), *newer_img = load(dir, "newer.bin", &newer_len), *keep = pkg;
    uint32_t keep_len = manifest_len;
    with_ext = 1;
    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
    with_ext = 0;
    seed_all(0);
    check(mcota_node_apply(&sims[0].node), "chain: apply of the first stage is rejected");
    mcota_boot(&sims[0].flash, &sims[0].cfg.layout);
    mcota_node_init(&sims[0].node, &sims[0].cfg);
    mcota_confirm(&sims[0].flash, &sims[0].cfg.layout);
    pkg = pkg2;
    manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);
    seed_all(0);
    check(sims[0].node.state == MCOTA_NODE_COMPLETE && mcota_node_archives(&sims[0].node) == 1u, "chain: older package is not kept");
    mcota_node_init(&sims[0].node, &sims[0].cfg);
    check(sims[0].node.state == MCOTA_NODE_COMPLETE && mcota_node_archives(&sims[0].node) == 1u, "chain: older package is lost on reboot");
    connect(0, 1);
    wait_complete(1, 4000);
    check(sims[1].node.state == MCOTA_NODE_COMPLETE && slot_b_is(1, new_img, new_len), "chain: latecomer does not get the older package");
    check(mcota_node_apply(&sims[1].node), "chain: latecomer cannot apply the first stage");
    mcota_boot(&sims[1].flash, &sims[1].cfg.layout);
    mcota_node_init(&sims[1].node, &sims[1].cfg);
    mcota_confirm(&sims[1].flash, &sims[1].cfg.layout);
    check(sims[1].node.state == MCOTA_NODE_APPLIED, "chain: latecomer does not see the first stage as applied");
    wait_complete(1, 4000);
    check(sims[1].node.state == MCOTA_NODE_COMPLETE && slot_b_is(1, newer_img, newer_len), "chain: latecomer does not get the second stage");
    check(mcota_node_archives(&sims[1].node) == 1u, "chain: latecomer does not keep the first stage");
    printf("chain of two packages from one repeater: latecomer is up to date\n");
    pkg = keep;
    manifest_len = keep_len;
  }

  {
    size_t other_len, big_len;
    uint8_t *other_img = load(dir, "switch.bin", &other_len), *big = load(dir, "switch.mcota", &big_len), *keep = pkg;
    uint32_t keep_len = manifest_len, fw_pages = m.pages;
    compact = 1;
    loss_percent = 10;
    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", fw_pages + 20u);
    seed_all(0);
    check(sims[0].node.state == MCOTA_NODE_COMPLETE && sims[0].node.survives, "without second area: first node not complete");
    check(slot_a_is(0, old_img, old_len), "without second area: running firmware changed while loading");
    connect(0, 1);
    wait_complete(1, 4000);
    check(sims[1].node.state == MCOTA_NODE_COMPLETE && slot_a_is(1, old_img, old_len), "without second area: neighbour does not load");
    check(mcota_node_apply(&sims[1].node), "without second area: apply is rejected");
    mcota_boot(&sims[1].flash, &sims[1].cfg.layout);
    check(slot_a_is(1, new_img, new_len), "without second area: the new firmware does not run after the swap");
    mcota_node_init(&sims[1].node, &sims[1].cfg);
    check(mcota_node_runs_requested(&sims[1].node), "without second area: new firmware is not recognised");
    mcota_confirm(&sims[1].flash, &sims[1].cfg.layout);
    check(sims[1].node.state == MCOTA_NODE_APPLIED && sims[1].node.serve_count == fw_pages, "without second area: package does not survive the swap");
    link_up[0][1] = link_up[1][0] = 0;
    connect(1, 2);
    wait_complete(2, 4000);
    check(sims[2].node.state == MCOTA_NODE_COMPLETE, "without second area: updated node does not share");
    check(mcota_node_apply(&sims[2].node), "without second area: apply on the third node");
    for (k = 0; k < 4; k++) mcota_boot(&sims[2].flash, &sims[2].cfg.layout);
    check(slot_a_is(2, old_img, old_len) && mcota_state(&sims[2].flash, &sims[2].cfg.layout) == MCOTA_STATE_REVERTED, "without second area: rollback without confirmation does not happen");

    pkg = big;
    manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);
    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", 16u + 18u);
    seed_all(0);
    check(sims[0].node.state == MCOTA_NODE_COMPLETE && !sims[0].node.survives, "full size: package must not survive the swap");
    connect(0, 1);
    wait_complete(1, 8000);
    check(sims[1].node.state == MCOTA_NODE_COMPLETE, "full size: neighbour does not become complete");
    run(20u * 60u * 1000u);
    check(!mcota_node_apply_due(&sims[0].node), "full size: applies although the package would be gone afterwards and the long period is running");
    run(80u * 60u * 1000u);
    check(mcota_node_apply_due(&sims[0].node) && mcota_node_apply_due(&sims[1].node), "full size: does not apply after the long period");
    check(mcota_node_apply(&sims[1].node), "full size: apply is rejected");
    mcota_boot(&sims[1].flash, &sims[1].cfg.layout);
    check(slot_a_is(1, other_img, other_len) && mcota_state(&sims[1].flash, &sims[1].cfg.layout) == MCOTA_STATE_TRIAL, "full size: the new firmware does not run after the swap");
    mcota_node_init(&sims[1].node, &sims[1].cfg);
    check(mcota_node_runs_requested(&sims[1].node), "full size: new firmware is not recognised");
    mcota_confirm(&sims[1].flash, &sims[1].cfg.layout);
    before = sims[1].node.tx_packets;
    run(10u * 60u * 1000u);
    check(sims[1].node.tx_packets == before && sims[1].node.stored_blocks == 0u, "full size: offers a package after the swap that it no longer has");
    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", 16u + 15u);
    seed_all(0);
    check(sims[0].node.state == MCOTA_NODE_COMPLETE && mcota_node_no_rollback(&sims[0].node), "no room for the rollback: node does not notice");
    check(mcota_node_needs_command(&sims[0].node), "no room for the rollback: node does not wait for the command");
    run(200u * 60u * 1000u);
    check(!mcota_node_apply_due(&sims[0].node), "no room for the rollback: applies automatically without command");
    check(mcota_node_apply(&sims[0].node), "no room for the rollback: command is rejected");
    mcota_boot(&sims[0].flash, &sims[0].cfg.layout);
    check(slot_a_is(0, other_img, other_len), "no room for the rollback: the new firmware does not run after the command");
    {
      size_t free_len;
      pkg = load(dir, "switch-norollback.mcota", &free_len);
      manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);
      for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", 16u + 15u);
      seed_all(0);
      check(mcota_node_no_rollback(&sims[0].node) && !mcota_node_needs_command(&sims[0].node), "release without rollback: node still waits for the command");
      run(200u * 60u * 1000u);
      check(mcota_node_apply_due(&sims[0].node), "release without rollback: does not apply automatically");
    }
    printf("without second area: small update is shared after the swap, full size update waits for the long period, without rollback copy only on command\n");
    compact = 0;
    pkg = keep;
    manifest_len = keep_len;
  }

  loss_percent = 0;
  pkg[2 + 5] |= 1u;
  check(!mcota_node_seed_manifest(&sims[3].node, pkg + 2, manifest_len), "manifest with release set afterwards is accepted");
  pkg[2 + 5] &= (uint8_t)~1u;
  {
    size_t auto_len;
    uint8_t *auto_pkg = load(dir, "auto.mcota", &auto_len), *keep = pkg;
    uint32_t keep_len = manifest_len;
    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
    pkg = auto_pkg;
    manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);
    seed_all(0);
    connect(0, 1);
    check(!mcota_node_apply_due(&sims[0].node), "apply without waiting time");
    run(2u * 60u * 1000u);
    check(!mcota_node_apply_due(&sims[0].node), "apply while a neighbour is loading");
    wait_complete(1, 4000);
    run(11u * 60u * 1000u);
    check(sims[1].node.state == MCOTA_NODE_COMPLETE && mcota_node_apply_due(&sims[0].node) && mcota_node_apply_due(&sims[1].node), "automatic apply does not happen");
    pkg = keep;
    manifest_len = keep_len;
  }

  {
    size_t other_len;
    uint8_t *other_pkg = load(dir, "auto.mcota", &other_len), *keep = pkg;
    uint32_t keep_len = manifest_len;
    compact = 0;
    with_ext = 0;
    loss_percent = 0;
    corrupt_percent = 0;
    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
    seed_all(0);
    pkg = other_pkg;
    manifest_len = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);
    seed_all(2);
    connect(0, 1);
    for (k = 0; k < 900 && !(sims[1].node.has_manifest && sims[1].node.have > 0u); k++) run(1000u);
    check(sims[1].node.state == MCOTA_NODE_FETCHING && sims[1].node.have < sims[1].node.manifest.pages
          && memcmp(sims[1].node.campaign, sims[0].node.campaign, 8) == 0, "the node is not loading the first update");
    link_up[0][1] = link_up[1][0] = 0;
    connect(2, 1);
    run(2u * 60u * 1000u);
    check(memcmp(sims[1].node.campaign, sims[0].node.campaign, 8) == 0, "a loading node gives up its update at once");
    run(20u * 60u * 1000u);
    check(sims[1].node.has_manifest && memcmp(sims[1].node.campaign, sims[2].node.campaign, 8) == 0, "a stalled node does not take another update");
    wait_complete(1, 400);
    check(sims[1].node.state == MCOTA_NODE_COMPLETE, "the node does not finish the update it switched to");

    for (k = 0; k < NODES; k++) setup(k, old_img, old_len, good, "sim_board", MAX_PAGES);
    seed_all(2);
    {
      mcota_sha256_t h;
      uint8_t ghost[96], digest[32];
      uint32_t pages = sims[2].node.manifest.pages, mb = (pages + 7u) / 8u, i = 0, j;
      ghost[i++] = 'L'; ghost[i++] = '2'; ghost[i++] = MCOTA_MSG_OFFER; ghost[i++] = 7;
      ghost[i++] = 0x66; ghost[i++] = 0x66; ghost[i++] = 0x66; ghost[i++] = 0x66;
      memcpy(ghost + i, sims[2].node.campaign, 8); ghost[i] ^= 0xFFu; i += 8u;
      memcpy(ghost + i, sims[2].node.key_ids[0], 8); i += 8u;
      mcota_sha256_init(&h);
      mcota_sha256_update(&h, (const uint8_t *)"sim_board", 9);
      mcota_sha256_final(&h, digest);
      memcpy(ghost + i, digest, 4); i += 4u;
      ghost[i++] = (uint8_t)sims[2].node.manifest.old_size; ghost[i++] = (uint8_t)(sims[2].node.manifest.old_size >> 8);
      ghost[i++] = (uint8_t)(sims[2].node.manifest.old_size >> 16); ghost[i++] = (uint8_t)(sims[2].node.manifest.old_size >> 24);
      memcpy(ghost + i, sims[2].node.manifest.old_hash, 8); i += 8u;
      ghost[i++] = (uint8_t)pages; ghost[i++] = (uint8_t)(pages >> 8);
      ghost[i++] = (uint8_t)manifest_len; ghost[i++] = (uint8_t)(manifest_len >> 8);
      ghost[i++] = 0;
      for (j = 0; j < mb; j++) ghost[i++] = 0xFFu;
      mcota_node_receive(&sims[1].node, ghost, i);
    }
    check(sims[1].node.cand_active && !sims[1].node.has_manifest, "an offer for an unknown update does not make the node a candidate");
    check(mcota_node_pending(&sims[1].node) && !mcota_node_pending(&sims[3].node), "pending work is not reported for a candidate only");
    {
      uint8_t query[16] = { 'L', '2', MCOTA_MSG_QUERY, 9, 0x55, 0x55, 0x55, 0x55 };
      memcpy(query + 8, sims[3].node.key_ids[0], 8);
      sims[3].node.cfg.image_size = (uint32_t)old_len;
      mcota_node_receive(&sims[3].node, query, sizeof(query));
      check(mcota_node_pending(&sims[3].node), "a waiting hello is not reported as pending work");
      run(10u * 1000u);
      check(!mcota_node_pending(&sims[3].node), "pending work stays reported after the hello");
    }
    run(60u * 1000u);
    connect(2, 1);
    run(3u * 60u * 1000u);
    check(sims[1].node.has_manifest && memcmp(sims[1].node.campaign, sims[2].node.campaign, 8) == 0, "an unanswered offer blocks a real one");
    pkg = keep;
    manifest_len = keep_len;
  }

  if (failures) {
    for (k = 0; k < NODES; k++)
      printf("  node %d: state %d, manifest %d, pages %u, provider %d, sent %u, received %u\n", k, (int)sims[k].node.state,
             (int)sims[k].node.has_manifest, sims[k].node.have, (int)sims[k].node.provider_set, sims[k].node.tx_packets, sims[k].node.rx_packets);
  }
  printf("node test: %d errors\n", failures);
  return failures ? 1 : 0;
}
