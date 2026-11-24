#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("Os")
#endif

#include "McotaService.h"

#include "McotaPort.h"

extern "C" {
#include "mcota_sha256.h"
}
#include "mcota_config.h"

#ifndef MCOTA_BUDGET_PPM
#define MCOTA_BUDGET_PPM 5000
#endif
#ifndef MCOTA_BOARD
#error "MCOTA_BOARD missing"
#endif

#ifndef MCOTA_TAG
#define MCOTA_TAG "-"
#endif

#define MCOTA_KEYS_MAGIC 0x4B544F4Cu
#define MCOTA_LOCAL_MAGIC 0x54534C4Cu
#define LOCAL_AT 96u
#define PRESET_NAME 16u
#define PRESET_AFTER_MS 4000u
#define TRIAL_MIN_MS 60000u
#define TRIAL_MAX_MS 600000u
#ifdef MCOTA_TEST
#define PING_EVERY_MS 20000u
#else
#define PING_EVERY_MS 120000u
#endif
#define PROBE_EVERY_MS 60000u
#define REPLY_MAX 160

McotaService mcota_service;

static McotaHost *host;
static mcota_node_t node;
static mcota_config_t cfg;
static bool ready;
static uint8_t load_buf[MCOTA_BLOCK_MAX];
static uint32_t load_len;
static uint32_t reboot_at;
static uint32_t next_ping;
static uint32_t started_at;
static uint32_t next_probe;
static bool neighbour_seen;

static uint32_t h_now(void *) { return millis(); }
static uint32_t h_airtime(void *, uint32_t len) { return host->mcotaAirtime(len); }
static bool h_send(void *, const uint8_t *data, uint32_t len, const mcota_route_t *route) {
  return route ? host->mcotaSend(data, len, route->bytes, route->hop_size, route->hops) : host->mcotaSend(data, len, NULL, 0, 0);
}
static bool h_verify(void *, const uint8_t *pub, const uint8_t *sig, const uint8_t *msg, uint32_t len) { return host->mcotaVerify(pub, sig, msg, len); }
static uint32_t h_random(void *) { return host->mcotaRandom(); }

#ifdef MCOTA_BUILTIN_KEY
static const uint8_t builtin_key[32] = MCOTA_BUILTIN_KEY;
#endif
#ifdef MCOTA_RETIRED_COUNT
static const uint8_t retired_keys[MCOTA_RETIRED_COUNT][32] = MCOTA_RETIRED_KEYS;
#endif
#if MCOTA_PRESET_COUNT > 0
static const char *const preset_names[MCOTA_PRESET_COUNT] = MCOTA_PRESET_NAMES;
static const char *const preset_commands[MCOTA_PRESET_COUNT] = MCOTA_PRESET_COMMANDS;
#endif

static bool settings_locked;
static bool preset_checked;
static uint32_t preset_applied;
static uint32_t preset_failed;
static char preset_name[PRESET_NAME];

static void load_keys() {
  uint8_t page[MCOTA_LOCAL_SIZE];
  uint32_t magic, local;
  mcota_port_keys_read(page);
  memcpy(&magic, page, 4);
  for (uint32_t i = 0; i < MCOTA_KEYS; i++) {
    uint32_t flag;
    memcpy(&flag, page + 4u + i * 36u, 4);
    cfg.key_set[i] = magic == MCOTA_KEYS_MAGIC && flag == 0u;
    memcpy(cfg.keys[i], page + 8u + i * 36u, 32);
  }
#ifdef MCOTA_RETIRED_COUNT
  for (uint32_t i = 0; i < MCOTA_KEYS; i++) {
    for (uint32_t k = 0; k < MCOTA_RETIRED_COUNT; k++) {
      if (cfg.key_set[i] && memcmp(cfg.keys[i], retired_keys[k], 32) == 0) cfg.key_set[i] = false;
    }
  }
#endif
#ifdef MCOTA_BUILTIN_KEY
  {
    bool present = false;
    for (uint32_t i = 0; i < MCOTA_KEYS; i++) present = present || (cfg.key_set[i] && memcmp(cfg.keys[i], builtin_key, 32) == 0);
    for (uint32_t i = 0; i < MCOTA_KEYS && !present; i++) {
      if (cfg.key_set[i]) continue;
      memcpy(cfg.keys[i], builtin_key, 32);
      cfg.key_set[i] = true;
      present = true;
    }
  }
#endif
  memcpy(&local, page + LOCAL_AT, 4);
  settings_locked = false;
  preset_applied = 0;
  memset(preset_name, 0, sizeof(preset_name));
  if (local == MCOTA_LOCAL_MAGIC) {
    uint32_t lock;
    memcpy(&lock, page + LOCAL_AT + 4u, 4);
    memcpy(&preset_applied, page + LOCAL_AT + 8u, 4);
    memcpy(preset_name, page + LOCAL_AT + 12u, PRESET_NAME - 1u);
    settings_locked = lock == 1u;
  }
}

static void save_keys() {
  uint8_t page[MCOTA_LOCAL_SIZE];
  uint8_t image[MCOTA_LOCAL_SIZE];
  uint32_t magic = MCOTA_KEYS_MAGIC, local = MCOTA_LOCAL_MAGIC, lock = settings_locked ? 1u : 0u, old;
  mcota_port_keys_read(page);
  memset(image, 0xFF, sizeof(image));
  memcpy(image, &magic, 4);
  memcpy(&old, page, 4);
  for (uint32_t i = 0; i < MCOTA_KEYS; i++) {
    if (!cfg.key_set[i]) continue;
#ifdef MCOTA_BUILTIN_KEY
    uint32_t flag;
    memcpy(&flag, page + 4u + i * 36u, 4);
    if (memcmp(cfg.keys[i], builtin_key, 32) == 0 && !(old == MCOTA_KEYS_MAGIC && flag == 0u)) continue;
#endif
    memset(image + 4u + i * 36u, 0, 4);
    memcpy(image + 8u + i * 36u, cfg.keys[i], 32);
  }
  memcpy(image + LOCAL_AT, &local, 4);
  memcpy(image + LOCAL_AT + 4u, &lock, 4);
  memcpy(image + LOCAL_AT + 8u, &preset_applied, 4);
  memcpy(image + LOCAL_AT + 12u, preset_name, PRESET_NAME);
  mcota_port_keys_write(image);
}

#if MCOTA_PRESET_COUNT > 0
static uint32_t preset_hash(uint32_t index) {
  uint32_t h = 2166136261u;
  for (const char *p = preset_names[index]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  for (const char *p = preset_commands[index]; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
  return h ? h : 1u;
}

static int preset_index(const char *name) {
  for (uint32_t i = 0; i < MCOTA_PRESET_COUNT; i++) {
    if (strcmp(preset_names[i], name) == 0) return (int)i;
  }
  return -1;
}

static uint32_t preset_active() {
  int found = preset_index(preset_name);
  return found < 0 ? 0u : (uint32_t)found;
}

static void preset_apply(uint32_t index) {
  const char *p = preset_commands[index];
  preset_failed = 0;
  while (*p) {
    char line[96], out[256];
    uint32_t n = 0;
    while (*p && *p != '\n') {
      if (n + 1u < sizeof(line)) line[n++] = *p;
      p++;
    }
    if (*p) p++;
    line[n] = 0;
    if (n == 0 || memcmp(line, "ota", 3) == 0) continue;
    out[0] = 0;
    host->mcotaCommand(line, out);
    if (memcmp(out, "OK", 2) != 0 && memcmp(out, "ok", 2) != 0) preset_failed++;
  }
  memset(preset_name, 0, sizeof(preset_name));
  strncpy(preset_name, preset_names[index], PRESET_NAME - 1u);
  preset_applied = preset_hash(index);
  save_keys();
}
#endif

static void start_node() {
  mcota_node_init(&node, &cfg);
}

static void confirm() {
  mcota_confirm(cfg.flash, &cfg.layout);
  if (mcota_port_confirmed(&node)) start_node();
}

void McotaService::begin(McotaHost *h, const uint8_t *self_pub_key) {
  host = h;
  memset(&cfg, 0, sizeof(cfg));
  if (!mcota_port_begin(&cfg)) return;
  strncpy(cfg.tag, MCOTA_TAG, sizeof(cfg.tag) - 1);
  cfg.now_ms = h_now;
  cfg.airtime_ms = h_airtime;
  cfg.send = h_send;
  cfg.verify = h_verify;
  cfg.random = h_random;
  memcpy(cfg.self, self_pub_key, 4);
  strncpy(cfg.board, MCOTA_BOARD, sizeof(cfg.board) - 1);
  cfg.budget_ppm = MCOTA_BUDGET_PPM;
#ifdef MCOTA_TEST
  cfg.offer_min_ms = 15000u;
  cfg.offer_max_ms = 120000u;
  cfg.apply_idle_ms = 120000u;
  cfg.apply_long_ms = 600000u;
#else
  cfg.offer_min_ms = 60000u;
  cfg.offer_max_ms = 3600000u;
  cfg.apply_idle_ms = 1800000u;
  cfg.apply_long_ms = 86400000u;
#endif
  load_keys();
  start_node();
  if (mcota_state(cfg.flash, &cfg.layout) == MCOTA_STATE_TRIAL && !mcota_node_runs_requested(&node) && mcota_clear(cfg.flash, &cfg.layout)) start_node();
  if (mcota_port_confirmed(&node)) start_node();
  started_at = millis();
  next_ping = started_at + 5000u;
  next_probe = started_at + 15000u;
  ready = true;
}

void McotaService::onPacket(const uint8_t *data, uint32_t len) {
  if (ready && !mcota_port_busy()) mcota_node_receive(&node, data, len);
}

bool McotaService::pending() {
  if (!ready) return false;
  if (reboot_at) return true;
  if (mcota_port_busy()) return false;
  return mcota_node_pending(&node) || mcota_state(cfg.flash, &cfg.layout) == MCOTA_STATE_TRIAL;
}

void McotaService::onNeighbourAnswer() {
  neighbour_seen = true;
}

void McotaService::loop() {
  if (!ready) return;
  uint32_t now = millis();
  if (reboot_at && (int32_t)(now - reboot_at) >= 0) host->mcotaReboot();
  if (mcota_port_busy()) return;
  mcota_node_loop(&node);
#if MCOTA_PRESET_COUNT > 0
  if (!preset_checked && now - started_at >= PRESET_AFTER_MS) {
    preset_checked = true;
    if (!settings_locked && preset_applied != preset_hash(preset_active())) preset_apply(preset_active());
  }
#endif

  if (!reboot_at && mcota_node_apply_due(&node) && !mcota_port_blocked() && mcota_node_apply(&node) && !mcota_port_blocked()) reboot_at = now + 3000u;

  if (mcota_state(cfg.flash, &cfg.layout) == MCOTA_STATE_TRIAL) {
    uint32_t up = now - started_at;
    bool alive = node.pong_seen || neighbour_seen;
    if (!alive && (int32_t)(now - next_ping) >= 0) {
      mcota_node_ping(&node);
      next_ping = now + PING_EVERY_MS;
    }
    if (!alive && (int32_t)(now - next_probe) >= 0) {
      host->mcotaProbe();
      next_probe = now + PROBE_EVERY_MS;
    }
    if (up >= TRIAL_MIN_MS && alive && !mcota_port_blocked() && mcota_node_runs_requested(&node)) {
      confirm();
    } else if (up >= TRIAL_MAX_MS && !reboot_at) {
      reboot_at = now + 1000u;
    }
  }
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int parse_hex(const char *text, uint8_t *out, uint32_t max) {
  uint32_t n = 0;
  while (text[0] && text[1]) {
    int hi = hex_value(text[0]), lo = hex_value(text[1]);
    if (hi < 0 || lo < 0 || n >= max) return -1;
    out[n++] = (uint8_t)((hi << 4) | lo);
    text += 2;
  }
  return text[0] ? -1 : (int)n;
}

static void hex(char *out, const uint8_t *data, uint32_t len) {
  static const char digits[] = "0123456789abcdef";
  for (uint32_t i = 0; i < len; i++) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 15];
  }
  out[2 * len] = 0;
}

static const char *state_name() {
  switch (node.state) {
    case MCOTA_NODE_FETCHING: return "loading";
    case MCOTA_NODE_COMPLETE: return "ready";
    case MCOTA_NODE_APPLIED: return "applied";
    case MCOTA_NODE_FAILED: return "failed";
    default: return node.cand_active ? "manifest" : "idle";
  }
}

static const char *loader_name() {
  switch (mcota_state(cfg.flash, &cfg.layout)) {
    case MCOTA_STATE_ARMED: return "swap";
    case MCOTA_STATE_TRIAL: return "trial";
    case MCOTA_STATE_CONFIRMED: return "confirmed";
    case MCOTA_STATE_REVERTED: return "rollback";
    case MCOTA_STATE_REJECTED: return "rejected";
    default: return "idle";
  }
}

static bool writes(const char *arg) {
  static const char *const starts[] = { "key set ", "key clear ", "load ", "apply", "drop", "reset", "confirm", "preset ", "locksettings " };
  for (uint32_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
    if (strncmp(arg, starts[i], strlen(starts[i])) == 0) return true;
  }
  return false;
}

bool McotaService::handleCommand(bool usb, const char *command, char *reply) {
  const char *prefix = NULL;
  int room = REPLY_MAX;
  while (*command == ' ') command++;
  if (strlen(command) > 4 && command[2] == '|') {
    prefix = command;
    command += 3;
  }
  if (memcmp(command, "ota", 3) != 0 || (command[3] != ' ' && command[3] != 0)) return false;
  const char *arg = command[3] ? command + 4 : "";
  char id[17];
  if (prefix) {
    memcpy(reply, prefix, 3);
    reply += 3;
    room -= 3;
  }

  if (!ready) {
    strcpy(reply, "ERR not ready");
  } else if (writes(arg) && mcota_port_busy()) {
    strcpy(reply, "ERR Bluetooth is active, try later");
  } else if (strcmp(arg, "info") == 0 || arg[0] == 0) {
    char from[9] = "-";
    bool loading = node.cand_active || (node.has_manifest && node.state == MCOTA_NODE_FETCHING);
    hex(id, node.campaign, 8);
    if (node.provider_set && loading) hex(from, node.provider, 4);
    snprintf(reply, room, "OK%s fw=%s %s %s %u/%u w=%u old=%u id=%s loader=%s from=%s age=%us budget=%u.%u%% tx=%u rx=%u%s%s%s%s",
             mcota_port_blocked() ? " FLASH-BLOCKED" : mcota_port_busy() ? " PAUSED" : "", MCOTA_TAG, state_name(), node.has_manifest ? node.manifest.version : "-", (unsigned)node.have,
             node.has_manifest ? (unsigned)node.manifest.pages : 0u, (unsigned)node.serve_count, (unsigned)mcota_node_archives(&node), node.has_manifest ? id : "-", loader_name(),
             from, (unsigned)((millis() - node.useful_at) / 1000u),
             (unsigned)(MCOTA_BUDGET_PPM / 10000), (unsigned)(MCOTA_BUDGET_PPM % 10000 / 1000), (unsigned)node.tx_packets, (unsigned)node.rx_packets,
             node.has_manifest && (node.manifest.flags & MCOTA_MANIFEST_AUTO_APPLY) ? " auto" : "",
             mcota_node_no_rollback(&node) ? " no-rollback" : "", mcota_node_needs_command(&node) ? " awaiting-command" : "",
             node.has_manifest && !node.can_serve ? " no-sharing" : cfg.direct ? " direct" : cfg.store ? " ext" : "");
  } else if (strcmp(arg, "peers") == 0) {
    int used = snprintf(reply, room, "OK");
    bool any = false;
    for (uint32_t i = 0; i < MCOTA_PEERS; i++) {
      char peer[9];
      if (!node.peers[i].used || used >= room - 1) continue;
      hex(peer, node.peers[i].id, 4);
      used += snprintf(reply + used, room - used, " %s:%u:%us", peer, (unsigned)mcota_node_peer_wants(&node, i), (unsigned)((millis() - node.peers[i].at) / 1000u));
      any = true;
    }
    if (!any) snprintf(reply + used, room - used, " none");
  } else if (strcmp(arg, "key") == 0) {
    char a[17] = "-", b[17] = "-";
    if (cfg.key_set[0]) hex(a, node.key_ids[0], 8);
    if (cfg.key_set[1]) hex(b, node.key_ids[1], 8);
    sprintf(reply, "OK 1=%s 2=%s", a, b);
  } else if (memcmp(arg, "key set ", 8) == 0 || memcmp(arg, "key clear ", 10) == 0) {
    bool set = arg[4] == 's';
    const char *rest = arg + (set ? 8 : 10);
    int slot = rest[0] - '1';
    uint8_t key[32];
    if (!usb) {
      strcpy(reply, "ERR USB only");
    } else if (slot < 0 || slot >= (int)MCOTA_KEYS || (set && (rest[1] != ' ' || parse_hex(rest + 2, key, 32) != 32))) {
      strcpy(reply, "ERR usage: ota key set <1|2> <64 hex digits> or ota key clear <1|2>");
    } else {
      cfg.key_set[slot] = set;
      if (set) memcpy(cfg.keys[slot], key, 32);
      save_keys();
      load_keys();
      start_node();
      strcpy(reply, mcota_port_blocked() ? "ERR flash blocked" : "OK");
    }
  } else if (memcmp(arg, "hash ", 5) == 0) {
    uint32_t size = (uint32_t)atol(arg + 5);
    if (size == 0 || size > cfg.layout.slot_pages * MCOTA_PAGE_SIZE) {
      strcpy(reply, "ERR size");
    } else {
      uint8_t h[32];
      char text[65];
      mcota_hash_region(cfg.flash, cfg.layout.slot_a, size, h);
      hex(text, h, 32);
      sprintf(reply, "OK %s", text);
    }
  } else if (memcmp(arg, "load ", 5) == 0) {
    const char *sub = arg + 5;
    if (!usb) {
      strcpy(reply, "ERR USB only");
    } else if (strcmp(sub, "begin") == 0) {
      load_len = 0;
      strcpy(reply, "OK");
    } else if (memcmp(sub, "data ", 5) == 0) {
      int n = parse_hex(sub + 5, load_buf + load_len, sizeof(load_buf) - load_len);
      if (n < 0) strcpy(reply, "ERR data");
      else { load_len += (uint32_t)n; sprintf(reply, "OK %u", (unsigned)load_len); }
    } else if (strcmp(sub, "manifest") == 0) {
      bool ok = mcota_node_seed_manifest(&node, load_buf, load_len);
      load_len = 0;
      strcpy(reply, ok ? "OK" : "ERR manifest rejected (signature, device or running firmware mismatch)");
    } else if (memcmp(sub, "block ", 6) == 0) {
      bool ok = mcota_node_seed_block(&node, (uint32_t)atol(sub + 6), load_buf, load_len);
      load_len = 0;
      if (ok) sprintf(reply, "OK %u/%u", (unsigned)node.have, (unsigned)node.manifest.pages);
      else strcpy(reply, "ERR block rejected");
    } else {
      strcpy(reply, "ERR unknown");
    }
  } else if (memcmp(arg, "apply", 5) == 0) {
    hex(id, node.campaign, 8);
    if (!usb && (arg[5] != ' ' || strcmp(arg + 6, id) != 0)) {
      strcpy(reply, "ERR over radio use the id: ota apply <id>");
    } else if (!mcota_node_apply(&node)) {
      strcpy(reply, "ERR no complete update");
    } else if (mcota_port_blocked()) {
      strcpy(reply, "ERR flash blocked");
    } else {
      reboot_at = millis() + 3000u;
      strcpy(reply, "OK reboot in 3 s");
    }
  } else if (strcmp(arg, "drop") == 0) {
    mcota_state_t state = mcota_state(cfg.flash, &cfg.layout);
    if (state == MCOTA_STATE_ARMED || state == MCOTA_STATE_TRIAL) {
      strcpy(reply, "ERR update is being applied");
    } else {
      mcota_node_drop(&node);
      strcpy(reply, mcota_port_blocked() ? "ERR flash blocked" : "OK");
    }
  } else if (strcmp(arg, "reset") == 0) {
    if (!usb && mcota_state(cfg.flash, &cfg.layout) == MCOTA_STATE_TRIAL) strcpy(reply, "ERR during a trial boot USB only");
    else if (!mcota_clear(cfg.flash, &cfg.layout)) strcpy(reply, mcota_port_blocked() ? "ERR flash blocked" : "ERR swap in progress");
    else { start_node(); strcpy(reply, "OK"); }
  } else if (strcmp(arg, "confirm") == 0) {
    if (!usb) strcpy(reply, "ERR USB only");
    else { confirm(); sprintf(reply, "OK loader=%s", loader_name()); }
#ifdef MCOTA_TEST
  } else if (memcmp(arg, "peek ", 5) == 0) {
    uint32_t addr = (uint32_t)strtoul(arg + 5, NULL, 16) & ~3u;
    uint8_t data[24];
    char text[49];
    if (!usb) strcpy(reply, "ERR USB only");
    else if (!mcota_port_peek(addr, data)) strcpy(reply, "ERR address");
    else { hex(text, data, 24); sprintf(reply, "OK %05x %s", (unsigned)addr, text); }
#endif
  } else if (strcmp(arg, "ping") == 0) {
    node.pong_seen = false;
    node.ping_nonce = 0;
    neighbour_seen = false;
    host->mcotaProbe();
    strcpy(reply, mcota_node_ping(&node) ? "OK sent" : "ERR budget");
  } else if (strcmp(arg, "pong") == 0) {
    strcpy(reply, node.pong_seen ? "OK reply from MeshCore-OTA neighbour" : neighbour_seen ? "OK reply from repeater" : "OK no reply");
  } else if (strcmp(arg, "preset") == 0) {
#if MCOTA_PRESET_COUNT > 0
    int used = snprintf(reply, room, "OK active=%s state=%s lock=%s errors=%u list=", preset_names[preset_active()],
                        preset_applied == preset_hash(preset_active()) ? "applied" : "pending", settings_locked ? "on" : "off", (unsigned)preset_failed);
    for (uint32_t i = 0; i < MCOTA_PRESET_COUNT && used < room; i++) used += snprintf(reply + used, room - used, "%s%s", i ? "," : "", preset_names[i]);
#else
    strcpy(reply, "OK no presets in this firmware");
#endif
  } else if (memcmp(arg, "preset ", 7) == 0) {
#if MCOTA_PRESET_COUNT > 0
    int found = preset_index(arg + 7);
    if (found < 0) {
      strcpy(reply, "ERR unknown preset, list with ota preset");
    } else {
      preset_apply((uint32_t)found);
      snprintf(reply, room, "OK %s applied, %u errors", preset_names[found], (unsigned)preset_failed);
    }
#else
    strcpy(reply, "ERR no presets in this firmware");
#endif
  } else if (strcmp(arg, "locksettings") == 0) {
    strcpy(reply, settings_locked ? "OK on" : "OK off");
  } else if (strcmp(arg, "locksettings on") == 0 || strcmp(arg, "locksettings off") == 0) {
    settings_locked = arg[14] == 'n';
    save_keys();
    strcpy(reply, mcota_port_blocked() ? "ERR flash blocked" : settings_locked ? "OK on" : "OK off");
  } else {
    strcpy(reply, "ERR ota info|peers|key|preset|locksettings|hash|load|apply|drop|reset|confirm|ping|pong");
  }
  return true;
}
