// CTAP 2.0 authenticator API (FIDO_2_0): authenticatorMakeCredential,
// GetAssertion, GetNextAssertion, GetInfo, ClientPIN (PIN protocol 1) and
// Reset. ES256 (P-256) credentials only, no extensions, no built-in user
// verification (the PIN is the only UV).
//
// Credentials are not stored unless they are discoverable (rk): the
// credential ID itself says how to re-derive the private key,
//   credential ID = 0x01 | nonce (32) | LEFT(HMAC(mac_key, 0x01|nonce|rpIdHash), 16)
//   private key   = HMAC(seed_key, rpIdHash | nonce | i), first i that is a valid scalar
// with mac_key and seed_key derived from the master secret (store.c), so an
// ID only works for the RP it was made for, and only on this key until reset.
#ifndef CTAP2_H
#define CTAP2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CTAP2_MSG_MAX 2048  // maxMsgSize

// Status codes (CTAP 2.0 section 6.3).
#define CTAP2_OK 0x00
#define CTAP1_ERR_INVALID_COMMAND 0x01
#define CTAP1_ERR_INVALID_PARAMETER 0x02
#define CTAP1_ERR_INVALID_LENGTH 0x03
#define CTAP2_ERR_CBOR_UNEXPECTED_TYPE 0x11
#define CTAP2_ERR_INVALID_CBOR 0x12
#define CTAP2_ERR_MISSING_PARAMETER 0x14
#define CTAP2_ERR_CREDENTIAL_EXCLUDED 0x19
#define CTAP2_ERR_UNSUPPORTED_ALGORITHM 0x26
#define CTAP2_ERR_KEY_STORE_FULL 0x28
#define CTAP2_ERR_UNSUPPORTED_OPTION 0x2B
#define CTAP2_ERR_INVALID_OPTION 0x2C
#define CTAP2_ERR_KEEPALIVE_CANCEL 0x2D
#define CTAP2_ERR_NO_CREDENTIALS 0x2E
#define CTAP2_ERR_USER_ACTION_TIMEOUT 0x2F
#define CTAP2_ERR_NOT_ALLOWED 0x30
#define CTAP2_ERR_PIN_INVALID 0x31
#define CTAP2_ERR_PIN_BLOCKED 0x32
#define CTAP2_ERR_PIN_AUTH_INVALID 0x33
#define CTAP2_ERR_PIN_AUTH_BLOCKED 0x34
#define CTAP2_ERR_PIN_NOT_SET 0x35
#define CTAP2_ERR_PIN_REQUIRED 0x36
#define CTAP2_ERR_PIN_POLICY_VIOLATION 0x37
#define CTAP1_ERR_OTHER 0x7F

// Derives the credential keys from the store and makes this power-up's PIN
// token and key agreement key. Call after store_init() (and after a reset).
void ctap2_init(void);

// True for the commands that act on credentials or the PIN (makeCredential,
// getAssertion, getNextAssertion, clientPIN, reset) - not getInfo.
bool ctap2_is_user_action(uint8_t cmd);

// One authenticator API request: req = command byte + CBOR parameters.
// Writes status byte + CBOR response into resp and returns its length.
// May block waiting for a touch (ctaphid_wait_up()).
size_t ctap2_handle(const uint8_t *req, size_t len, uint8_t *resp, size_t cap);

#endif  // CTAP2_H
