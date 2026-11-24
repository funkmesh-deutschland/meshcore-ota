#pragma once

#include <Arduino.h>

class McotaHost {
public:
  virtual bool mcotaSend(const uint8_t *data, uint32_t len, const uint8_t *path, uint8_t hop_size, uint8_t hops) = 0;
  virtual uint32_t mcotaAirtime(uint32_t len) = 0;
  virtual uint32_t mcotaRandom() = 0;
  virtual bool mcotaVerify(const uint8_t *pub_key, const uint8_t *sig, const uint8_t *msg, uint32_t len) = 0;
  virtual void mcotaReboot() = 0;
  virtual void mcotaProbe() = 0;
  virtual void mcotaCommand(char *command, char *reply) = 0;
};

class McotaService {
public:
  void begin(McotaHost *host, const uint8_t *self_pub_key);
  void loop();
  void onPacket(const uint8_t *data, uint32_t len);
  void onNeighbourAnswer();
  bool pending();
  bool handleCommand(bool usb, const char *command, char *reply);
};

extern McotaService mcota_service;
