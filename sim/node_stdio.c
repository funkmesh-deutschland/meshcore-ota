#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_node.h"
#include "ed_25519.h"

#define SLOT_PAGES 20u
#define FLASH_SIZE ((2u * SLOT_PAGES + 1u + MCOTA_STATUS_PAGES) * MCOTA_PAGE_SIZE)

static uint8_t mem[FLASH_SIZE];
static uint32_t now_ms;
static unsigned seed = 9;
static mcota_node_t node;

static unsigned rnd(void) { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7FFFu; }
static void f_read(void *ctx, uint32_t addr, void *out, uint32_t len) { (void)ctx; memcpy(out, mem + addr, len); }
static void f_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  uint32_t i;
  (void)ctx;
  for (i = 0; i < len; i++) mem[addr + i] &= ((const uint8_t *)data)[i];
}
static void f_erase(void *ctx, uint32_t addr) { (void)ctx; memset(mem + addr, 0xFF, MCOTA_PAGE_SIZE); }
static uint32_t h_now(void *ctx) { (void)ctx; return now_ms; }
static uint32_t h_airtime(void *ctx, uint32_t len) { (void)ctx; return 80u + len * 56u / 10u; }
static uint32_t h_random(void *ctx) { (void)ctx; return rnd() | (rnd() << 15); }
static bool h_verify(void *ctx, const uint8_t *pub, const uint8_t *sig, const uint8_t *msg, uint32_t len) {
  (void)ctx;
  return ed25519_verify(sig, msg, len, pub) == 1;
}
static uint8_t saved[MCOTA_SETTINGS_MAX];
static uint32_t saved_len;
static uint32_t h_load(void *ctx, uint8_t *out, uint32_t max) {
  (void)ctx;
  if (saved_len > max) return 0;
  memcpy(out, saved, saved_len);
  return saved_len;
}
static bool h_save(void *ctx, const uint8_t *data, uint32_t len) {
  (void)ctx;
  memcpy(saved, data, len);
  saved_len = len;
  return true;
}
static bool h_send(void *ctx, const uint8_t *data, uint32_t len, const mcota_route_t *route) {
  uint32_t i;
  (void)ctx;
  if (route) {
    printf("V %u ", (unsigned)route->hop_size);
    for (i = 0; i < (uint32_t)route->hop_size * route->hops; i++) printf("%02x", route->bytes[i]);
    printf(" ");
  } else {
    printf("S ");
  }
  for (i = 0; i < len; i++) printf("%02x", data[i]);
  printf("\n");
  return true;
}

static int unhex(const char *text, uint8_t *out, int max) {
  int n = 0;
  unsigned v;
  while (text[0] && text[1] && text[0] != '\n' && n < max && sscanf(text, "%2x", &v) == 1) { out[n++] = (uint8_t)v; text += 2; }
  return n;
}

int main(int argc, char **argv) {
  static mcota_flash_t flash = { 0, f_read, f_write, f_erase };
  static mcota_config_t cfg;
  static char line[1024];
  static uint8_t buf[512];
  FILE *fh;
  size_t n;
  if (argc < 4) return 2;
  memset(mem, 0xFF, sizeof(mem));
  fh = fopen(argv[1], "rb");
  if (!fh) return 2;
  n = fread(mem, 1, SLOT_PAGES * MCOTA_PAGE_SIZE, fh);
  fclose(fh);
  (void)n;
  fh = fopen(argv[2], "rb");
  if (!fh || fread(cfg.keys[0], 1, 32, fh) != 32) return 2;
  fclose(fh);
  cfg.key_set[0] = true;
  cfg.flash = &flash;
  cfg.layout.slot_a = 0;
  cfg.layout.slot_b = SLOT_PAGES * MCOTA_PAGE_SIZE;
  cfg.layout.scratch = 2u * SLOT_PAGES * MCOTA_PAGE_SIZE;
  cfg.layout.status = (2u * SLOT_PAGES + 1u) * MCOTA_PAGE_SIZE;
  cfg.layout.slot_pages = SLOT_PAGES;
  cfg.now_ms = h_now; cfg.airtime_ms = h_airtime; cfg.send = h_send; cfg.verify = h_verify; cfg.random = h_random;
  cfg.self[0] = 0x77; cfg.self[1] = 0x01; cfg.self[2] = 0x02; cfg.self[3] = 0x03;
  strncpy(cfg.board, argv[3], 15);
  cfg.budget_ppm = 50000u;
  cfg.offer_min_ms = 10000u;
  cfg.offer_max_ms = 120000u;
  cfg.apply_idle_ms = 300000u;
  cfg.apply_long_ms = 3600000u;
  cfg.image_size = (uint32_t)n;
  cfg.settings_load = h_load;
  cfg.settings_save = h_save;
  cfg.settings_note_ms = 600000u;
  strncpy(cfg.tag, "1.0.0", 15);
  mcota_node_init(&node, &cfg);
  setvbuf(stdout, NULL, _IOLBF, 0);
  while (fgets(line, sizeof(line), stdin)) {
    if (line[0] == 'T') {
      uint32_t end = now_ms + (uint32_t)atol(line + 2);
      while (now_ms < end) { mcota_node_loop(&node); now_ms += 100u; }
      printf("OK\n");
    } else if (line[0] == 'R') {
      int len = unhex(line + 2, buf, sizeof(buf));
      mcota_node_receive(&node, buf, (uint32_t)len);
      printf("OK\n");
    } else if (line[0] == 'P') {
      node.pong_seen = false;
      node.ping_nonce = 0;
      mcota_node_ping(&node);
      printf("OK\n");
    } else if (line[0] == 'Q') {
      fh = fopen(argv[4], "wb");
      if (fh) { fwrite(mem + cfg.layout.slot_b, 1, SLOT_PAGES * MCOTA_PAGE_SIZE, fh); fclose(fh); }
      printf("STATE %d %u %u %u %d %u %u %u %u\n", (int)node.state, node.have, node.has_manifest ? node.manifest.pages : 0u, node.bundles, (int)node.pong_seen, node.requests_sent, node.seeks_sent,
             (unsigned)node.settings_rev, (unsigned)node.tx_packets);
    }
  }
  return 0;
}
