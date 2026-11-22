#ifndef MCOTA_BOOT_H
#define MCOTA_BOOT_H

#include <stdbool.h>
#include <stdint.h>

#define MCOTA_PAGE_SIZE 4096u
#define MCOTA_MAGIC 0x41544F4Cu
#define MCOTA_FORMAT 2u
#ifndef MCOTA_MAX_PAGES
#define MCOTA_MAX_PAGES 256u
#endif
#define MCOTA_STATUS_PAGES ((35u + 6u * MCOTA_MAX_PAGES + 1023u) / 1024u)
#define MCOTA_MODE_SWAP 0u
#define MCOTA_MODE_COMPACT 1u
#define MCOTA_MODE_DIRECT 2u
#define MCOTA_MAX_TRIALS 3u

#define MCOTA_W_MAGIC 0u
#define MCOTA_W_FORMAT 1u
#define MCOTA_W_PAGES 2u
#define MCOTA_W_NEW_SIZE 3u
#define MCOTA_W_OLD_SIZE 4u
#define MCOTA_W_NEW_HASH 5u
#define MCOTA_W_OLD_HASH 13u
#define MCOTA_W_COMPACT 21u
#define MCOTA_W_STORE 22u
#define MCOTA_W_CHECK 23u
#define MCOTA_W_ARMED 24u
#define MCOTA_W_SWAP_DONE 25u
#define MCOTA_W_TRIAL 26u
#define MCOTA_W_CONFIRMED 29u
#define MCOTA_W_REVERT_STARTED 30u
#define MCOTA_W_REVERT_DONE 31u
#define MCOTA_W_REJECTED 32u
#define MCOTA_W_STARTED 33u
#define MCOTA_W_INPLACE 34u
#define MCOTA_W_JOURNAL_SWAP 35u
#define MCOTA_W_JOURNAL_REVERT (MCOTA_W_JOURNAL_SWAP + 3u * MCOTA_MAX_PAGES)

typedef struct {
  void *ctx;
  void (*read)(void *ctx, uint32_t addr, void *out, uint32_t len);
  void (*write)(void *ctx, uint32_t addr, const void *data, uint32_t len);
  void (*erase)(void *ctx, uint32_t addr);
} mcota_flash_t;

typedef struct {
  uint32_t slot_a;
  uint32_t slot_b;
  uint32_t scratch;
  uint32_t status;
  uint32_t slot_pages;
  uint32_t pool_end;
  void *compact;
  void (*activate)(void *ctx, bool new_image);
  void *activate_ctx;
} mcota_layout_t;

typedef enum {
  MCOTA_STATE_NONE = 0,
  MCOTA_STATE_ARMED,
  MCOTA_STATE_TRIAL,
  MCOTA_STATE_CONFIRMED,
  MCOTA_STATE_REVERTED,
  MCOTA_STATE_REJECTED
} mcota_state_t;

typedef struct {
  uint32_t pages;
  uint32_t new_size;
  uint32_t old_size;
  uint8_t new_hash[32];
  uint8_t old_hash[32];
  bool compact;
  bool direct;
  uint32_t store;
} mcota_request_t;

void mcota_boot(const mcota_flash_t *flash, const mcota_layout_t *layout);
mcota_state_t mcota_state(const mcota_flash_t *flash, const mcota_layout_t *layout);
uint32_t mcota_trials(const mcota_flash_t *flash, const mcota_layout_t *layout);
bool mcota_request(const mcota_flash_t *flash, const mcota_layout_t *layout, const mcota_request_t *request);
bool mcota_request_read(const mcota_flash_t *flash, const mcota_layout_t *layout, mcota_request_t *out);
void mcota_confirm(const mcota_flash_t *flash, const mcota_layout_t *layout);
void mcota_hash_region(const mcota_flash_t *flash, uint32_t addr, uint32_t len, uint8_t out[32]);

#endif
