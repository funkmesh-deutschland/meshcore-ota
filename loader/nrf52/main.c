#include <stdint.h>

#include "../../core/mcota_boot.h"
#include "layout.h"
#include "xflash.h"
#ifdef MCOTA_COMPACT
#include "../../core/mcota_compact.h"
#endif

#define NVMC_READY (*(volatile uint32_t *)0x4001E400u)
#define NVMC_CONFIG (*(volatile uint32_t *)0x4001E504u)
#define NVMC_ERASEPAGE (*(volatile uint32_t *)0x4001E508u)
#define WDT_RR0 (*(volatile uint32_t *)0x40010600u)

extern uint32_t __stack_top;
extern uint32_t __bss_start;
extern uint32_t __bss_end;
void reset_handler(void);
void fault_handler(void);

__attribute__((section(".vectors"), used))
static void *const vectors[16] = {
  &__stack_top, reset_handler, fault_handler, fault_handler, fault_handler, fault_handler, fault_handler, 0,
  0, 0, 0, fault_handler, fault_handler, 0, fault_handler, fault_handler
};

void *memcpy(void *to, const void *from, unsigned n) {
  uint8_t *d = to;
  const uint8_t *s = from;
  while (n--) *d++ = *s++;
  return to;
}

void *memset(void *to, int value, unsigned n) {
  uint8_t *d = to;
  while (n--) *d++ = (uint8_t)value;
  return to;
}

static void wait_ready(void) {
  while (NVMC_READY == 0u) { }
}

static int allowed(uint32_t addr, uint32_t len) {
  return addr >= MCOTA_WRITE_MIN && addr + len <= MCOTA_WRITE_MAX && addr + len > addr;
}

#ifdef MCOTA_EXT_SIZE
static int ext_allowed(uint32_t addr, uint32_t len) {
  uint32_t offset = addr - MCOTA_EXT_FLAG;
  return offset + len <= MCOTA_EXT_SIZE && offset + len > offset;
}
#endif

static void flash_read(void *ctx, uint32_t addr, void *out, uint32_t len) {
  (void)ctx;
#ifdef MCOTA_EXT_SIZE
  if (addr & MCOTA_EXT_FLAG) {
    if (ext_allowed(addr, len)) xf_read(addr - MCOTA_EXT_FLAG, out, len);
    return;
  }
#endif
  memcpy(out, (const void *)addr, len);
}

static void flash_write(void *ctx, uint32_t addr, const void *data, uint32_t len) {
  uint32_t i;
  (void)ctx;
#ifdef MCOTA_EXT_SIZE
  if (addr & MCOTA_EXT_FLAG) {
    WDT_RR0 = 0x6E524635u;
    if (ext_allowed(addr, len)) xf_write(addr - MCOTA_EXT_FLAG, data, len);
    return;
  }
#endif
  if (!allowed(addr, len) || (addr & 3u) || (len & 3u)) return;
  WDT_RR0 = 0x6E524635u;
  NVMC_CONFIG = 1u;
  wait_ready();
  for (i = 0; i < len; i += 4u) {
    uint32_t value;
    memcpy(&value, (const uint8_t *)data + i, 4u);
    *(volatile uint32_t *)(addr + i) = value;
    wait_ready();
  }
  NVMC_CONFIG = 0u;
  wait_ready();
}

static void flash_erase(void *ctx, uint32_t addr) {
  (void)ctx;
#ifdef MCOTA_EXT_SIZE
  if (addr & MCOTA_EXT_FLAG) {
    WDT_RR0 = 0x6E524635u;
    if (ext_allowed(addr, MCOTA_FLASH_PAGE) && !(addr & (MCOTA_FLASH_PAGE - 1u))) xf_erase(addr - MCOTA_EXT_FLAG);
    return;
  }
#endif
  if (!allowed(addr, MCOTA_FLASH_PAGE) || (addr & (MCOTA_FLASH_PAGE - 1u))) return;
  WDT_RR0 = 0x6E524635u;
  NVMC_CONFIG = 2u;
  wait_ready();
  NVMC_ERASEPAGE = addr;
  wait_ready();
  NVMC_CONFIG = 0u;
  wait_ready();
}

__attribute__((naked, noreturn)) static void start_app(uint32_t base __attribute__((unused))) {
  __asm volatile(
    "mov r4, r0\n"
    "svc 0x13\n"
    "ldr r0, [r4]\n"
    "msr msp, r0\n"
    "ldr r1, [r4, #4]\n"
    "mov r0, #0\n"
    "sub r0, #1\n"
    "mov lr, r0\n"
    "bx r1\n");
}

void fault_handler(void) {
  for (;;) { }
}

void reset_handler(void) {
  static const mcota_flash_t flash = { 0, flash_read, flash_write, flash_erase };
#ifdef MCOTA_COMPACT
  static mcota_compact_t engine;
  static const mcota_layout_t layout = { MCOTA_SLOT_A_ADDR, 0u, 0u, MCOTA_STATUS_ADDR, MCOTA_SLOT_PAGES, MCOTA_POOL_END, &engine, 0, 0 };
#else
  static const mcota_layout_t layout = { MCOTA_SLOT_A_ADDR, MCOTA_SLOT_B_ADDR, MCOTA_SCRATCH_ADDR, MCOTA_STATUS_ADDR, MCOTA_SLOT_PAGES, 0u, 0, 0, 0 };
#endif
  uint32_t *zero;
  for (zero = &__bss_start; zero < &__bss_end; zero++) *zero = 0u;
#ifdef MCOTA_EXT_SIZE
  if (xf_init()) mcota_boot(&flash, &layout);
#else
  mcota_boot(&flash, &layout);
#endif
  start_app(MCOTA_SLOT_A_ADDR);
}
