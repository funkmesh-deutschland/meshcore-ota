#pragma once

#include <Arduino.h>

extern "C" {
#include "mcota_node.h"
}

#define MCOTA_LOCAL_SIZE 128u

bool mcota_port_begin(mcota_config_t *cfg);
bool mcota_port_busy();
bool mcota_port_blocked();
void mcota_port_keys_read(uint8_t image[MCOTA_LOCAL_SIZE]);
void mcota_port_keys_write(const uint8_t image[MCOTA_LOCAL_SIZE]);
bool mcota_port_confirmed(mcota_node_t *node);
bool mcota_port_peek(uint32_t addr, uint8_t out[24]);
