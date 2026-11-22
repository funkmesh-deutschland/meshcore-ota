#if defined(NRF52_PLATFORM) || defined(ARDUINO_ARCH_NRF52)

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "McotaPort.h"

#include <nrf_sdm.h>

#include "layout.h"
extern "C" {
#include "xflash.h"
}

static bool flash_blocked;

#define NVMC_READY (*(volatile uint32_t *)0x4001E400u)
#define NVMC_CONFIG (*(volatile uint32_t *)0x4001E504u)
#define NVMC_ERASEPAGE (*(volatile uint32_t *)0x4001E508u)

static bool softdevice_on() {
  uint8_t enabled = 0;
  sd_softdevice_is_enabled(&enabled);
  return enabled != 0;
}

extern "C" uint32_t __etext;
extern "C" uint32_t __data_start__;
extern "C" uint32_t __data_end__;

static uint32_t image_size() {
  return ((uint32_t)&__etext - MCOTA_SLOT_A_ADDR) + ((uint32_t)&__data_end__ - (uint32_t)&__data_start__);
}

static bool allowed(uint32_t addr, uint32_t len) {
  uint32_t end = addr + len;
  if (end <= addr) return false;
#ifdef MCOTA_COMPACT
  if (addr >= MCOTA_SLOT_A_ADDR + ((image_size() + MCOTA_FLASH_PAGE - 1u) & ~(MCOTA_FLASH_PAGE - 1u)) && end <= MCOTA_POOL_END) return true;
#endif
  if (addr >= MCOTA_STATUS_ADDR && end <= MCOTA_STATUS_ADDR + MCOTA_STATUS_PAGES * MCOTA_FLASH_PAGE) return true;
  return addr >= MCOTA_KEYS_ADDR && end <= MCOTA_KEYS_ADDR + MCOTA_FLASH_PAGE;
}

#ifdef MCOTA_EXT_SIZE
static bool ext_ok;

static bool ext_allowed(uint32_t addr, uint32_t len) {
  uint32_t offset = addr - MCOTA_EXT_FLAG;
  return ext_ok && offset + len <= MCOTA_EXT_SIZE && offset + len > offset;
}
#endif

static void wait_ready() {
  while (NVMC_READY == 0u) { }
}

static void flash_read(void *, uint32_t addr, void *out, uint32_t len) {
#ifdef MCOTA_EXT_SIZE
  if (addr & MCOTA_EXT_FLAG) {
    if (ext_allowed(addr, len)) xf_read(addr - MCOTA_EXT_FLAG, (uint8_t *)out, len);
    else memset(out, 0xFF, len);
    return;
  }
#endif
  memcpy(out, (const void *)addr, len);
}

static void flash_write(void *, uint32_t addr, const void *data, uint32_t len) {
#ifdef MCOTA_EXT_SIZE
  if (addr & MCOTA_EXT_FLAG) {
    if (!ext_allowed(addr, len) || !xf_write(addr - MCOTA_EXT_FLAG, (const uint8_t *)data, len)) flash_blocked = true;
    return;
  }
#endif
  if (!allowed(addr, len) || (addr & 3u) || (len & 3u) || softdevice_on()) { flash_blocked = true; return; }
  NVMC_CONFIG = 1u;
  wait_ready();
  for (uint32_t i = 0; i < len; i += 4u) {
    uint32_t value;
    memcpy(&value, (const uint8_t *)data + i, 4u);
    *(volatile uint32_t *)(addr + i) = value;
    wait_ready();
  }
  NVMC_CONFIG = 0u;
  wait_ready();
}

static void flash_erase(void *, uint32_t addr) {
#ifdef MCOTA_EXT_SIZE
  if (addr & MCOTA_EXT_FLAG) {
    if (!ext_allowed(addr, MCOTA_FLASH_PAGE) || (addr & (MCOTA_FLASH_PAGE - 1u)) || !xf_erase(addr - MCOTA_EXT_FLAG)) flash_blocked = true;
    return;
  }
#endif
  if (!allowed(addr, MCOTA_FLASH_PAGE) || (addr & (MCOTA_FLASH_PAGE - 1u)) || softdevice_on()) { flash_blocked = true; return; }
  NVMC_CONFIG = 2u;
  wait_ready();
  NVMC_ERASEPAGE = addr;
  wait_ready();
  NVMC_CONFIG = 0u;
  wait_ready();
}

static const mcota_flash_t flash = { 0, flash_read, flash_write, flash_erase };

bool mcota_port_begin(mcota_config_t *cfg) {
  cfg->flash = &flash;
  cfg->layout.slot_a = MCOTA_SLOT_A_ADDR;
  cfg->layout.status = MCOTA_STATUS_ADDR;
  cfg->layout.slot_pages = MCOTA_SLOT_PAGES;
#ifdef MCOTA_COMPACT
  cfg->compact = true;
  cfg->layout.pool_end = MCOTA_POOL_END;
#else
  cfg->layout.slot_b = MCOTA_SLOT_B_ADDR;
  cfg->layout.scratch = MCOTA_SCRATCH_ADDR;
#endif
  cfg->image_size = image_size();
#ifdef MCOTA_EXT_SIZE
  ext_ok = xf_init();
  if (!ext_ok) flash_blocked = true;
  cfg->store = &flash;
  cfg->store_addr = MCOTA_STORE_ADDR;
  cfg->store_size = MCOTA_STORE_SIZE;
#endif
  return true;
}

bool mcota_port_busy() {
  return softdevice_on();
}

bool mcota_port_blocked() {
  return flash_blocked;
}

void mcota_port_keys_read(uint8_t image[MCOTA_LOCAL_SIZE]) {
  memcpy(image, (const void *)MCOTA_KEYS_ADDR, MCOTA_LOCAL_SIZE);
}

void mcota_port_keys_write(const uint8_t image[MCOTA_LOCAL_SIZE]) {
  flash_erase(0, MCOTA_KEYS_ADDR);
  flash_write(0, MCOTA_KEYS_ADDR, image, MCOTA_LOCAL_SIZE);
}

bool mcota_port_confirmed(mcota_node_t *) {
  return false;
}

bool mcota_port_peek(uint32_t addr, uint8_t out[24]) {
  if (addr > 0xFFFE8u) return false;
  memcpy(out, (const void *)addr, 24);
  return true;
}

#endif
