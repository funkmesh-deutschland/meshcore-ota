#ifndef MCOTA_XFLASH_H
#define MCOTA_XFLASH_H

#include <stdbool.h>
#include <stdint.h>

#include "layout.h"

#ifdef MCOTA_EXT_SIZE

#define XF_OUTSET (*(volatile uint32_t *)0x50000508u)
#define XF_OUTCLR (*(volatile uint32_t *)0x5000050Cu)
#define XF_IN (*(volatile uint32_t *)0x50000510u)
#define XF_PIN_CNF(pin) (*(volatile uint32_t *)(0x50000700u + 4u * (pin)))
#define XF_POLLS 4000000u

static inline void xf_high(uint32_t pin) { XF_OUTSET = 1u << pin; }
static inline void xf_low(uint32_t pin) { XF_OUTCLR = 1u << pin; }

static uint8_t xf_byte(uint8_t out) {
  uint8_t in = 0;
  int bit;
  for (bit = 7; bit >= 0; bit--) {
    if (out & (1u << bit)) xf_high(MCOTA_EXT_PIN_IO0); else xf_low(MCOTA_EXT_PIN_IO0);
    xf_high(MCOTA_EXT_PIN_SCK);
    in = (uint8_t)((in << 1) | ((XF_IN >> MCOTA_EXT_PIN_IO1) & 1u));
    xf_low(MCOTA_EXT_PIN_SCK);
  }
  return in;
}

static void xf_start(uint8_t command) {
  xf_low(MCOTA_EXT_PIN_CS);
  xf_byte(command);
}

static void xf_stop(void) {
  xf_high(MCOTA_EXT_PIN_CS);
}

static void xf_address(uint8_t command, uint32_t addr) {
  xf_start(command);
  xf_byte((uint8_t)(addr >> 16));
  xf_byte((uint8_t)(addr >> 8));
  xf_byte((uint8_t)addr);
}

static bool xf_wait(void) {
  uint32_t polls;
  for (polls = 0; polls < XF_POLLS; polls++) {
    uint8_t status;
    xf_start(0x05);
    status = xf_byte(0);
    xf_stop();
    if (!(status & 1u)) return true;
  }
  return false;
}

static bool xf_init(void) {
  uint8_t id[3];
  volatile uint32_t pause;
  int i;
  xf_high(MCOTA_EXT_PIN_CS);
  XF_PIN_CNF(MCOTA_EXT_PIN_CS) = 3u;
  xf_low(MCOTA_EXT_PIN_SCK);
  XF_PIN_CNF(MCOTA_EXT_PIN_SCK) = 3u;
  XF_PIN_CNF(MCOTA_EXT_PIN_IO0) = 3u;
  XF_PIN_CNF(MCOTA_EXT_PIN_IO1) = 0xCu;
  xf_high(MCOTA_EXT_PIN_IO2);
  XF_PIN_CNF(MCOTA_EXT_PIN_IO2) = 3u;
  xf_high(MCOTA_EXT_PIN_IO3);
  XF_PIN_CNF(MCOTA_EXT_PIN_IO3) = 3u;
  xf_start(0xAB);
  xf_stop();
  for (pause = 0; pause < 20000u; pause++) { }
  xf_start(0x9F);
  for (i = 0; i < 3; i++) id[i] = xf_byte(0);
  xf_stop();
  if (id[0] == 0x00u || id[0] == 0xFFu || id[2] < 10u || id[2] > 31u) return false;
  if ((1u << id[2]) < MCOTA_EXT_SIZE) return false;
  return xf_wait();
}

static void xf_read(uint32_t addr, uint8_t *out, uint32_t len) {
  uint32_t i;
  xf_address(0x03, addr);
  for (i = 0; i < len; i++) out[i] = xf_byte(0);
  xf_stop();
}

static bool xf_write(uint32_t addr, const uint8_t *data, uint32_t len) {
  while (len > 0u) {
    uint32_t take = 256u - (addr & 255u), i;
    if (take > len) take = len;
    xf_start(0x06);
    xf_stop();
    xf_address(0x02, addr);
    for (i = 0; i < take; i++) xf_byte(data[i]);
    xf_stop();
    if (!xf_wait()) return false;
    addr += take;
    data += take;
    len -= take;
  }
  return true;
}

static bool xf_erase(uint32_t addr) {
  xf_start(0x06);
  xf_stop();
  xf_address(0x20, addr);
  xf_stop();
  return xf_wait();
}

#endif
#endif
