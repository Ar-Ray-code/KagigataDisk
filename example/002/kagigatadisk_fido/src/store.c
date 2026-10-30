#include "store.h"

#include <string.h>

#include "crypto.h"
#include "fstore.h"

#define KEY_STATE FS_FIDO_KEY(0)
#define KEY_COUNTER FS_FIDO_KEY(1)
#define KEY_RK(i) FS_FIDO_KEY(0x100u + (uint32_t)(i))

#define STATE_MAGIC 0x3154534Bu  // "KST1"

typedef struct {
  uint32_t magic;
  uint8_t master[32];
  uint8_t pin_set;
  uint8_t pin_retries;
  uint8_t pin_hash[16];
} state_t;

_Static_assert(sizeof(state_t) <= FS_SECTOR, "state fits a sector");
_Static_assert(sizeof(store_rk_t) <= FS_SECTOR, "a resident credential fits a sector");
_Static_assert(STORE_RK_MAX + 2 <= 64, "state, counter and every slot fit fstore (91 sectors)");

static state_t st;  // RAM copy of KEY_STATE
static uint32_t counter;
static bool inited;  // the region is ours (store_init ran)
static bool ready;
static uint8_t sector[FS_SECTOR];

static bool save(uint32_t key, const void *data, size_t n) {
  memset(sector, 0, sizeof(sector));
  memcpy(sector, data, n);
  bool ok = fstore_write(key, sector);
  crypto_wipe(sector, sizeof(sector));
  return ok;
}

static bool save_state(void) { return save(KEY_STATE, &st, sizeof(st)); }

static bool create(void) {
  memset(&st, 0, sizeof(st));
  st.magic = STATE_MAGIC;
  if (!crypto_reseed() || !crypto_random(st.master, sizeof(st.master))) return false;
  st.pin_retries = STORE_PIN_RETRIES;
  counter = 0;
  return save_state();
}

bool store_init(void) {
  inited = true;
  fstore_init();
  const state_t *s = (const state_t *)fstore_lookup(KEY_STATE);
  if (s && s->magic == STATE_MAGIC) {
    memcpy(&st, s, sizeof(st));
    const uint32_t *c = (const uint32_t *)fstore_lookup(KEY_COUNTER);
    counter = c ? *c : 0;
    ready = true;
  } else {
    // First boot (or the region held something else): start clean.
    fstore_format();
    ready = create();
  }
  return ready;
}

bool store_ready(void) { return ready; }

const uint8_t *store_master(void) { return st.master; }
bool store_pin_is_set(void) { return st.pin_set; }
uint8_t store_pin_retries(void) { return st.pin_retries; }
const uint8_t *store_pin_hash(void) { return st.pin_hash; }

bool store_set_pin(const uint8_t hash[16]) {
  memcpy(st.pin_hash, hash, 16);
  st.pin_set = 1;
  st.pin_retries = STORE_PIN_RETRIES;
  return save_state();
}

bool store_set_pin_retries(uint8_t n) {
  st.pin_retries = n;
  return save_state();
}

uint32_t store_counter_next(void) {
  uint32_t next = counter + 1u;
  if (!save(KEY_COUNTER, &next, sizeof(next))) return 0;
  counter = next;
  return counter;
}

uint32_t store_counter(void) { return counter; }

const store_rk_t *store_rk(int i) {
  const store_rk_t *rk = (const store_rk_t *)fstore_lookup(KEY_RK(i));
  return rk && rk->magic == STORE_RK_MAGIC ? rk : NULL;
}

int store_rk_slot_for(const uint8_t rp_id_hash[32], const uint8_t *user_id, size_t user_id_len) {
  int slot = -1;
  for (int i = 0; i < STORE_RK_MAX; i++) {
    const store_rk_t *o = store_rk(i);
    if (!o) {
      if (slot < 0) slot = i;
    } else if (memcmp(o->rp_id_hash, rp_id_hash, 32) == 0 && o->user_id_len == user_id_len &&
               memcmp(o->user_id, user_id, user_id_len) == 0) {
      return i;  // same account at the same RP: replaced
    }
  }
  return slot;
}

bool store_rk_write(int slot, const store_rk_t *rk) {
  return slot >= 0 && slot < STORE_RK_MAX && save(KEY_RK(slot), rk, sizeof(*rk));
}

int store_rk_count(void) {
  int n = 0;
  for (int i = 0; i < STORE_RK_MAX; i++) n += store_rk(i) != NULL;
  return n;
}

bool store_wipe(void) {
  crypto_wipe(&st, sizeof(st));
  if (!inited) return false;
  fstore_format();
  ready = create();
  return ready;
}
