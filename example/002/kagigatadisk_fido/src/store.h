// Everything the key keeps across a reset, on top of fstore.c:
//
//   state     the master secret (from which every credential's private key
//             is derived), the PIN hash and the PIN retry counter
//   counter   the signature counter, written at every signature
//   rk slots  discoverable (resident) credentials: rpIdHash, credential ID
//             and the user entity, one 512-byte sector each
//
// The master secret is made from the TRNG on first boot and after a reset
// (authenticatorReset or "wipe" before the bootloader), which erases the
// whole region so no copy of the old one survives.
#ifndef STORE_H
#define STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STORE_RK_MAX 32
#define STORE_USER_ID_MAX 64
#define STORE_NAME_MAX 64  // user name / displayName, bytes (UTF-8, cut at a character)
#define STORE_CRED_ID_LEN 49
#define STORE_PIN_RETRIES 8

typedef struct {
  uint32_t magic;    // STORE_RK_MAGIC, or 0 for a free slot
  uint32_t created;  // signature counter at creation: newest first in getAssertion
  uint8_t rp_id_hash[32];
  uint8_t cred_id[STORE_CRED_ID_LEN];
  uint8_t user_id_len;
  uint8_t user_id[STORE_USER_ID_MAX];
  uint8_t name_len;
  uint8_t display_name_len;
  char name[STORE_NAME_MAX];
  char display_name[STORE_NAME_MAX];
} store_rk_t;
#define STORE_RK_MAGIC 0x314B524Bu  // "KRK1"

// Loads the state, creating it on first boot. False if the store does not
// work (then store_ready() stays false and CTAP commands fail).
bool store_init(void);
bool store_ready(void);

const uint8_t *store_master(void);  // 32 bytes
bool store_pin_is_set(void);
uint8_t store_pin_retries(void);
const uint8_t *store_pin_hash(void);  // LEFT(SHA-256(PIN), 16)
bool store_set_pin(const uint8_t hash[16]);  // also resets the retries
bool store_set_pin_retries(uint8_t n);

// Increments and saves the signature counter; returns the new value, 0 on a
// flash failure.
uint32_t store_counter_next(void);
uint32_t store_counter(void);

// Slot i (0..STORE_RK_MAX-1): its credential, or NULL if free. The pointer
// is into flash and valid until the next store write.
const store_rk_t *store_rk(int i);
// The slot for a credential of this account: the one holding the same
// rpIdHash and user ID (it gets replaced), else a free one; -1 if all are
// taken.
int store_rk_slot_for(const uint8_t rp_id_hash[32], const uint8_t *user_id, size_t user_id_len);
bool store_rk_write(int slot, const store_rk_t *rk);
int store_rk_count(void);

// Erases the whole store and starts over with a new master secret.
bool store_wipe(void);

#endif  // STORE_H
