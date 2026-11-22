#ifndef MCOTA_NODE_H
#define MCOTA_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "mcota_boot.h"
#include "mcota_compact.h"
#include "mcota_manifest.h"
#include "mcota_patch.h"

#define MCOTA_CHUNK 152u
#define MCOTA_PACKET_MAX 176u
#define MCOTA_HEADER 4u
#define MCOTA_KEYS 2u
#define MCOTA_MAP_BYTES (MCOTA_MAX_PAGES / 8u)
#define MCOTA_PEERS 4u
#define MCOTA_PARTIALS 2u
#define MCOTA_REPAIRS 8u
#define MCOTA_ASK_PARTS 6u
#define MCOTA_ROUTE_MAX 6u
#define MCOTA_TRIES 6u
#define MCOTA_SLOTS 3u
#define MCOTA_SLOT_MIN (8u * MCOTA_PAGE_SIZE)

#define MCOTA_MSG_OFFER 1u
#define MCOTA_MSG_REQUEST 2u
#define MCOTA_MSG_DATA 3u
#define MCOTA_MSG_PING 4u
#define MCOTA_MSG_PONG 5u
#define MCOTA_MSG_SEEK 6u
#define MCOTA_MSG_QUERY 7u
#define MCOTA_MSG_HELLO 8u

#define MCOTA_OFFER_ROUTED 1u

typedef struct {
  uint8_t hop_size;
  uint8_t hops;
  uint8_t bytes[MCOTA_ROUTE_MAX];
} mcota_route_t;

typedef struct {
  const mcota_flash_t *flash;
  mcota_layout_t layout;
  const mcota_flash_t *store;
  uint32_t store_addr;
  uint32_t store_size;
  void *ctx;
  uint32_t (*now_ms)(void *ctx);
  uint32_t (*airtime_ms)(void *ctx, uint32_t len);
  bool (*send)(void *ctx, const uint8_t *data, uint32_t len, const mcota_route_t *route);
  bool (*verify)(void *ctx, const uint8_t *pub_key, const uint8_t *sig, const uint8_t *msg, uint32_t len);
  uint32_t (*random)(void *ctx);
  uint8_t self[4];
  char board[16];
  uint8_t keys[MCOTA_KEYS][32];
  bool key_set[MCOTA_KEYS];
  uint32_t budget_ppm;
  uint32_t offer_min_ms;
  uint32_t offer_max_ms;
  uint32_t apply_idle_ms;
  uint32_t apply_long_ms;
  bool compact;
  bool direct;
  uint32_t image_size;
  char tag[16];
} mcota_config_t;

typedef enum {
  MCOTA_NODE_IDLE = 0,
  MCOTA_NODE_FETCHING,
  MCOTA_NODE_COMPLETE,
  MCOTA_NODE_APPLIED,
  MCOTA_NODE_FAILED
} mcota_node_state_t;

typedef struct {
  bool used;
  uint8_t kind;
  uint16_t index;
  uint32_t len;
  uint32_t mask;
  uint32_t at;
  uint8_t data[MCOTA_ITEM_MAX];
} mcota_partial_t;

typedef struct {
  bool used;
  uint8_t src;
  uint8_t id[4];
  uint8_t want[MCOTA_MAP_BYTES];
  uint32_t at;
} mcota_peer_t;

typedef struct {
  uint8_t src;
  uint8_t kind;
  uint16_t index;
  uint32_t mask;
} mcota_repair_t;

typedef struct {
  bool used;
  uint32_t base;
  uint32_t marks_base;
  uint32_t blocks_base;
  uint32_t old_size;
  uint16_t pages;
  uint16_t manifest_len;
  uint8_t campaign[8];
  uint8_t key_id[8];
  uint8_t old_hash[8];
  char version[16];
} mcota_archive_t;

typedef struct {
  mcota_config_t cfg;
  uint8_t key_ids[MCOTA_KEYS][8];

  mcota_node_state_t state;
  bool has_manifest;
  bool can_store;
  bool can_serve;
  bool survives;
  uint32_t plan;
  mcota_manifest_t manifest;
  uint8_t manifest_raw[MCOTA_MANIFEST_MAX];
  uint8_t campaign[8];
  uint8_t have_map[MCOTA_MAP_BYTES];
  uint32_t have;
  uint8_t serve_map[MCOTA_MAP_BYTES];
  uint32_t serve_count;
  uint32_t stored_blocks;
  uint32_t store_base;
  uint32_t marks_base;
  uint32_t blocks_base;

  bool cand_active;
  uint8_t cand_campaign[8];
  uint32_t cand_manifest_len;
  uint32_t cand_at;
  uint8_t ignored[8];
  bool ignored_set;

  bool provider_set;
  bool provider_routed;
  uint8_t provider[4];
  uint8_t provider_map[MCOTA_MAP_BYTES];
  mcota_route_t provider_route;

  mcota_partial_t partial[MCOTA_PARTIALS];
  uint32_t chunk_mask[MCOTA_MAX_PAGES];
  uint8_t bad_map[MCOTA_MAP_BYTES];
  uint32_t open_page;
  uint32_t direct_chunks;
  uint8_t work[MCOTA_BLOCK_MAX];
  uint8_t page[MCOTA_PAGE_SIZE];
  uint8_t old_page[MCOTA_PAGE_SIZE];
  uint32_t old_cached;
  bool old_valid;

  uint32_t quiet_at;
  uint32_t wait_ms;
  uint32_t retries;
  uint32_t useful_at;
  uint32_t asked_at;
  bool asked;
  uint32_t seek_at;
  uint32_t seek_interval;

  mcota_peer_t peers[MCOTA_PEERS];
  mcota_repair_t repairs[MCOTA_REPAIRS];
  uint32_t repair_count;
  mcota_archive_t archives[MCOTA_SLOTS - 1u];
  uint32_t adopt_base;
  uint32_t offer_turn;
  uint32_t offer_at;
  uint32_t offer_interval;
  uint32_t busy_at;
  uint32_t apply_jitter;

  uint32_t budget_us;
  uint32_t budget_at;
  uint32_t sent_ms;

  bool hash_cached;
  uint32_t hash_size;
  uint8_t hash[32];

  uint8_t sequence;
  uint32_t ping_nonce;
  bool pong_seen;
  uint32_t rx_packets;
  uint32_t bundles;
  uint32_t tx_packets;
  uint32_t requests_sent;
  uint32_t seeks_sent;
  bool hello_due;
  bool hello_routed;
  uint32_t hello_at;
  uint8_t hello_key;
  mcota_route_t hello_route;
  uint32_t overheard;
} mcota_node_t;

void mcota_node_init(mcota_node_t *n, const mcota_config_t *cfg);
void mcota_node_loop(mcota_node_t *n);
void mcota_node_receive(mcota_node_t *n, const uint8_t *data, uint32_t len);
bool mcota_node_seed_manifest(mcota_node_t *n, const uint8_t *raw, uint32_t len);
bool mcota_node_seed_block(mcota_node_t *n, uint32_t index, const uint8_t *data, uint32_t len);
bool mcota_node_apply(mcota_node_t *n);
bool mcota_node_apply_due(mcota_node_t *n);
bool mcota_node_no_rollback(const mcota_node_t *n);
bool mcota_node_needs_command(const mcota_node_t *n);
bool mcota_node_runs_requested(mcota_node_t *n);
void mcota_node_drop(mcota_node_t *n);
bool mcota_node_ping(mcota_node_t *n);
bool mcota_node_has_page(const mcota_node_t *n, uint32_t index);
uint32_t mcota_node_peer_wants(const mcota_node_t *n, uint32_t peer);
uint32_t mcota_node_archives(const mcota_node_t *n);
void mcota_key_id(const uint8_t pub_key[32], uint8_t out[8]);

#endif
