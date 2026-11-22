#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/mcota_manifest.h"
#include "../core/mcota_patch.h"

static uint8_t *old_img;
static uint32_t old_len;
static mcota_manifest_t m;
static uint8_t cache[MCOTA_PAGE_SIZE];
static uint32_t cached = 0xFFFFFFFFu;

static uint8_t *load(const char *path, size_t *len) {
  FILE *fh = fopen(path, "rb");
  uint8_t *data;
  if (!fh) { printf("ERROR: %s is missing\n", path); exit(2); }
  fseek(fh, 0, SEEK_END);
  *len = (size_t)ftell(fh);
  fseek(fh, 0, SEEK_SET);
  data = malloc(*len + MCOTA_PAGE_SIZE);
  if (fread(data, 1, *len, fh) != *len) exit(2);
  fclose(fh);
  return data;
}

static void old_read(void *ctx, uint32_t pos, uint8_t *out, uint32_t len) {
  (void)ctx;
  while (len > 0u) {
    uint32_t page = pos / MCOTA_PAGE_SIZE, offset = pos % MCOTA_PAGE_SIZE, take = MCOTA_PAGE_SIZE - offset;
    if (take > len) take = len;
    if (cached != page) {
      uint32_t start = page * MCOTA_PAGE_SIZE, size = old_len - start;
      if (size > MCOTA_PAGE_SIZE) size = MCOTA_PAGE_SIZE;
      memcpy(cache, old_img + start, size);
      mcota_view_predict(&m.view, cache, size, start);
      cached = page;
    }
    memcpy(out, cache + offset, take);
    out += take;
    pos += take;
    len -= take;
  }
}

int main(int argc, char **argv) {
  static uint8_t work[MCOTA_BLOCK_MAX], page[MCOTA_PAGE_SIZE];
  size_t n, pkg_len, new_len;
  uint8_t *pkg, *new_img;
  uint32_t i, offset, size, bad = 0, changed = 0;
  if (argc < 4) return 2;
  old_img = load(argv[1], &n);
  old_len = (uint32_t)n;
  pkg = load(argv[2], &pkg_len);
  new_img = load(argv[3], &new_len);
  size = (uint32_t)pkg[0] | ((uint32_t)pkg[1] << 8);
  if (!mcota_manifest_parse(pkg + 2, size, &m) || m.new_size != new_len || m.old_size != old_len) { printf("ERROR: manifest does not match\n"); return 1; }
  offset = 2u + size;
  for (i = 0; i < m.pages; i++) {
    uint32_t len = mcota_manifest_page_len(&m, i), block = mcota_manifest_block_len(&m, i);
    if (block == 0u) {
      if (memcmp(old_img + i * MCOTA_PAGE_SIZE, new_img + i * MCOTA_PAGE_SIZE, len) != 0) bad++;
      continue;
    }
    changed++;
    if (!mcota_block_apply(old_read, 0, old_len, pkg + offset, block, work, sizeof(work), page, len, i * MCOTA_PAGE_SIZE)) { bad++; offset += block; continue; }
    mcota_view_restore(&m.view, page, len, i * MCOTA_PAGE_SIZE);
    if (memcmp(page, new_img + i * MCOTA_PAGE_SIZE, len) != 0) bad++;
    offset += block;
  }
  printf("cross-check with the device code: %u pages, %u changed, %u errors\n", m.pages, changed, bad);
  return bad ? 1 : 0;
}
