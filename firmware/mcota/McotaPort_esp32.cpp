#if defined(ESP32) || defined(ARDUINO_ARCH_ESP32)

#include "McotaPort.h"

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <nvs.h>

#define STATUS_ADDR 0xE0000000u
#define STATUS_WORDS 40u
#define TAIL_MIN 0x4000u
#define TAIL_MAX 0x100000u
#define APP_ROOM (MCOTA_MAX_PAGES * MCOTA_PAGE_SIZE)
#define PAUSE_EVERY 0x10000u

static const esp_partition_t *running;
static const esp_partition_t *other;
static const mcota_layout_t *layout;
static uint32_t tail;
static uint32_t status[STATUS_WORDS];
static nvs_handle_t values;
static bool values_open;
static bool flash_blocked;
static uint32_t read_since_pause;

extern "C" bool verifyRollbackLater() {
  return true;
}

static void save_status() {
  if (!values_open || nvs_set_blob(values, "status", status, sizeof(status)) != ESP_OK || nvs_commit(values) != ESP_OK) flash_blocked = true;
}

static const esp_partition_t *inside(uint32_t addr, uint32_t len) {
  const esp_partition_t *parts[2] = { running, other };
  for (uint32_t i = 0; i < 2u; i++) {
    const esp_partition_t *p = parts[i];
    if (p && addr >= p->address && len <= p->size && addr - p->address <= p->size - len) return p;
  }
  return NULL;
}

static void flash_read(void *, uint32_t addr, void *out, uint32_t len) {
  if (addr >= STATUS_ADDR) {
    uint32_t offset = addr - STATUS_ADDR;
    for (uint32_t i = 0; i < len; i++) ((uint8_t *)out)[i] = offset + i < sizeof(status) ? ((const uint8_t *)status)[offset + i] : 0xFFu;
    return;
  }
  const esp_partition_t *p = inside(addr, len);
  if (!p || esp_partition_read(p, addr - p->address, out, len) != ESP_OK) memset(out, 0xFF, len);
  read_since_pause += len;
  if (read_since_pause >= PAUSE_EVERY) {
    read_since_pause = 0;
    delay(1);
  }
}

static void flash_write(void *, uint32_t addr, const void *data, uint32_t len) {
  if (addr >= STATUS_ADDR) {
    uint32_t offset = addr - STATUS_ADDR;
    bool changed = false;
    for (uint32_t i = 0; i < len && offset + i < sizeof(status); i++) {
      uint8_t *at = (uint8_t *)status + offset + i;
      uint8_t value = *at & ((const uint8_t *)data)[i];
      changed = changed || value != *at;
      *at = value;
    }
    if (changed) save_status();
    return;
  }
  if (inside(addr, len) != other || esp_partition_write(other, addr - other->address, data, len) != ESP_OK) flash_blocked = true;
}

static void flash_erase(void *, uint32_t addr) {
  if (addr >= STATUS_ADDR) {
    bool changed = false;
    if (addr - STATUS_ADDR >= sizeof(status)) return;
    for (uint32_t i = 0; i < STATUS_WORDS; i++) {
      changed = changed || status[i] != 0xFFFFFFFFu;
      status[i] = 0xFFFFFFFFu;
    }
    if (changed) save_status();
    return;
  }
  if ((addr & (MCOTA_PAGE_SIZE - 1u)) || inside(addr, MCOTA_PAGE_SIZE) != other ||
      esp_partition_erase_range(other, addr - other->address, MCOTA_PAGE_SIZE) != ESP_OK) flash_blocked = true;
}

static const mcota_flash_t flash = { 0, flash_read, flash_write, flash_erase };

static void activate(void *, bool) {
  if (esp_ota_set_boot_partition(other) != ESP_OK) return;
  delay(200);
  esp_restart();
}

static uint32_t image_size() {
  uint8_t head[24], segment[8];
  uint32_t pos = sizeof(head);
  if (esp_partition_read(running, 0, head, sizeof(head)) != ESP_OK || head[0] != 0xE9u || head[1] == 0u || head[1] > 16u) return 0;
  for (uint32_t i = 0; i < head[1]; i++) {
    uint32_t len;
    if (pos + sizeof(segment) > running->size || esp_partition_read(running, pos, segment, sizeof(segment)) != ESP_OK) return 0;
    memcpy(&len, segment + 4, 4);
    if (len > running->size - pos - sizeof(segment)) return 0;
    pos += sizeof(segment) + len;
  }
  pos = (pos + 16u) & ~15u;
  if (head[23] == 1u) pos += 32u;
  return pos <= running->size ? pos : 0;
}

static bool is_ota(const esp_partition_t *p) {
  return p && p->type == ESP_PARTITION_TYPE_APP && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX;
}

static bool prepare(mcota_config_t *cfg) {
  uint32_t count = 0, size;
  size_t len = sizeof(status);
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
  while (it) {
    if (is_ota(esp_partition_get(it))) count++;
    it = esp_partition_next(it);
  }
  running = esp_ota_get_running_partition();
  other = esp_ota_get_next_update_partition(NULL);
  if (count != 2u || !is_ota(running) || !is_ota(other) || other == running) return false;
  size = running->size < other->size ? running->size : other->size;
  if (size < 16u * TAIL_MIN) return false;
  tail = size > APP_ROOM + TAIL_MIN ? size - APP_ROOM : TAIL_MIN;
  if (tail > TAIL_MAX) tail = TAIL_MAX;

  memset(status, 0xFF, sizeof(status));
  values_open = nvs_open("meshcore-ota", NVS_READWRITE, &values) == ESP_OK;
  if (!values_open) flash_blocked = true;
  else if (nvs_get_blob(values, "status", status, &len) != ESP_OK || len != sizeof(status)) memset(status, 0xFF, sizeof(status));

  cfg->flash = &flash;
  cfg->layout.slot_a = running->address;
  cfg->layout.slot_b = other->address;
  cfg->layout.status = STATUS_ADDR;
  cfg->layout.slot_pages = (size - tail) / MCOTA_PAGE_SIZE;
  cfg->layout.activate = activate;
  cfg->direct = true;
  cfg->store = &flash;
  cfg->store_addr = other->address + other->size - tail;
  cfg->store_size = tail;
  cfg->image_size = image_size();
  layout = &cfg->layout;
  return true;
}

bool mcota_port_begin(mcota_config_t *cfg) {
  bool ok = prepare(cfg);
  esp_ota_img_states_t state;
  if (ok) mcota_boot(&flash, &cfg->layout);
  if (running && esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) esp_ota_mark_app_valid_cancel_rollback();
  return ok;
}

bool mcota_port_busy() {
  return false;
}

bool mcota_port_blocked() {
  return flash_blocked;
}

void mcota_port_keys_read(uint8_t image[MCOTA_LOCAL_SIZE]) {
  size_t len = MCOTA_LOCAL_SIZE;
  memset(image, 0xFF, MCOTA_LOCAL_SIZE);
  if (!values_open || nvs_get_blob(values, "local", image, &len) != ESP_OK || len != MCOTA_LOCAL_SIZE) memset(image, 0xFF, MCOTA_LOCAL_SIZE);
}

void mcota_port_keys_write(const uint8_t image[MCOTA_LOCAL_SIZE]) {
  if (!values_open || nvs_set_blob(values, "local", image, MCOTA_LOCAL_SIZE) != ESP_OK || nvs_commit(values) != ESP_OK) flash_blocked = true;
}

bool mcota_port_confirmed(mcota_node_t *node) {
  mcota_request_t request;
  uint8_t done[8];
  size_t len = sizeof(done);
  uint8_t *from, *to;
  uint32_t source, target;
  bool ok = true;
  if (!layout || !values_open || mcota_state(&flash, layout) != MCOTA_STATE_CONFIRMED || !mcota_request_read(&flash, layout, &request)) return false;
  if (nvs_get_blob(values, "mirror", done, &len) == ESP_OK && len == sizeof(done) && memcmp(done, request.new_hash, sizeof(done)) == 0) return false;
  if (!mcota_node_runs_requested(node)) return false;
  from = (uint8_t *)malloc(2u * MCOTA_PAGE_SIZE);
  if (!from) return false;
  to = from + MCOTA_PAGE_SIZE;
  source = running->size - tail;
  target = other->size - tail;
  for (uint32_t offset = 0; offset < tail && ok; offset += MCOTA_PAGE_SIZE) {
    bool blank = true, clean = true;
    if ((offset & (PAUSE_EVERY - 1u)) == 0u) delay(1);
    ok = esp_partition_read(running, source + offset, from, MCOTA_PAGE_SIZE) == ESP_OK && esp_partition_read(other, target + offset, to, MCOTA_PAGE_SIZE) == ESP_OK;
    if (!ok || memcmp(from, to, MCOTA_PAGE_SIZE) == 0) continue;
    for (uint32_t i = 0; i < MCOTA_PAGE_SIZE; i++) {
      blank = blank && from[i] == 0xFFu;
      clean = clean && to[i] == 0xFFu;
    }
    if (!clean) ok = esp_partition_erase_range(other, target + offset, MCOTA_PAGE_SIZE) == ESP_OK;
    if (ok && !blank) ok = esp_partition_write(other, target + offset, from, MCOTA_PAGE_SIZE) == ESP_OK;
  }
  free(from);
  if (!ok) {
    flash_blocked = true;
    return false;
  }
  if (nvs_set_blob(values, "mirror", request.new_hash, sizeof(done)) != ESP_OK || nvs_commit(values) != ESP_OK) flash_blocked = true;
  return true;
}

bool mcota_port_peek(uint32_t addr, uint8_t out[24]) {
  if (addr < STATUS_ADDR && !inside(addr, 24)) return false;
  flash_read(0, addr, out, 24);
  return true;
}

#endif
