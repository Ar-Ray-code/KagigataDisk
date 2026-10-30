// The cryptography CTAP2 needs, on Mbed TLS: P-256 (ECDSA, ECDH), SHA-256,
// HMAC-SHA-256, AES-256-CBC and random numbers. Private keys are 32-byte
// big-endian scalars; public keys are their affine x and y.
//
// Random numbers come from an AES-256 CTR-DRBG whose entropy is read straight
// from the RP2350's hardware TRNG with all its health tests on (von Neumann
// balancer, CRNGT, autocorrelation) plus a check for repeated output. If the
// TRNG keeps failing, crypto_ok() turns false for good and nothing that
// needs randomness may be used (ctap2.c then answers every command with an
// error).
#ifndef CRYPTO_H
#define CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void crypto_init(void);
bool crypto_ok(void);
// Fills out with random bytes; false (and crypto_ok() false) on a TRNG failure.
bool crypto_random(void *out, size_t n);
// Mixes fresh TRNG entropy into the DRBG; done before making long-lived
// secrets (the master secret, a new credential).
bool crypto_reseed(void);

void crypto_sha256(const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32]);
void crypto_hmac_sha256(const uint8_t *key, size_t klen, const void *a, size_t alen,
                        const void *b, size_t blen, uint8_t out[32]);

// True if d is a valid private key (1 <= d < n).
bool crypto_p256_valid(const uint8_t d[32]);
bool crypto_p256_keygen(uint8_t d[32], uint8_t x[32], uint8_t y[32]);
bool crypto_p256_pubkey(const uint8_t d[32], uint8_t x[32], uint8_t y[32]);
// ECDSA over a SHA-256 hash; writes the DER signature (at most 72 bytes) and
// returns its length, 0 on failure.
#define CRYPTO_SIG_MAX 72
size_t crypto_p256_sign(const uint8_t d[32], const uint8_t hash[32], uint8_t sig[CRYPTO_SIG_MAX]);
// x coordinate of d * (px, py); false if the point is not on the curve.
bool crypto_p256_ecdh(const uint8_t d[32], const uint8_t px[32], const uint8_t py[32],
                      uint8_t z[32]);

// n must be a multiple of 16. in and out may be the same buffer.
void crypto_aes256_cbc(bool encrypt, const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *in, uint8_t *out, size_t n);

bool crypto_equal(const void *a, const void *b, size_t n);  // constant time
void crypto_wipe(void *p, size_t n);

#endif  // CRYPTO_H
