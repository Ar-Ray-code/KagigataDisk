#include "crypto.h"

#include <string.h>

#include "mbedtls/aes.h"
#include "mbedtls/constant_time.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/sha256.h"
#include "hardware/resets.h"
#include "hardware/structs/trng.h"
#include "pico/time.h"
#include "pico/unique_id.h"

static mbedtls_ctr_drbg_context drbg;
static mbedtls_ecp_group p256;
static bool rng_ok;

// ---------------------------------------------------------------------------
// TRNG
// ---------------------------------------------------------------------------
#define TRNG_ISR_ERRORS 0x0Eu  // VN_ERR | CRNGT_ERR | AUTOCORR_ERR
#define TRNG_ISR_EHR_VALID 0x01u
#define TRNG_SAMPLE_START 0x400u  // rng_clk cycles between ROSC samples
#define TRNG_SAMPLE_MAX 0x10000u  // slowest tried before giving up
#define TRNG_TIMEOUT_US 1000000u

static uint32_t trng_sample = TRNG_SAMPLE_START;
static uint32_t trng_last[6];

// Configures and starts sampling. after_error: the TRNG's own reset first,
// the only way to clear AUTOCORR_ERR.
static void trng_restart(bool after_error) {
  trng_hw->rnd_source_enable = 0;
  if (after_error) {
    trng_hw->trng_sw_reset = 1;
    busy_wait_us_32(10);
  }
  trng_hw->rng_imr = 0xFu;  // polled, no interrupts
  trng_hw->trng_config = 0;
  trng_hw->sample_cnt1 = trng_sample;
  trng_hw->trng_debug_control = 0;  // every health test on
  trng_hw->rng_icr = 0xFu;
  trng_hw->rnd_source_enable = 1;
}

// One 192-bit EHR block that passed the hardware tests and differs word for
// word from the previous one. A failure slows the sampling down (adjacent
// samples correlate less) and retries; false once even the slowest fails.
static bool trng_block(uint32_t out[6]) {
  while (trng_sample <= TRNG_SAMPLE_MAX) {
    absolute_time_t until = make_timeout_time_us(TRNG_TIMEOUT_US);
    uint32_t isr;
    while (!((isr = trng_hw->rng_isr) & (TRNG_ISR_EHR_VALID | TRNG_ISR_ERRORS)) &&
           !time_reached(until)) {
    }
    if (isr & TRNG_ISR_EHR_VALID && !(isr & TRNG_ISR_ERRORS)) {
      bool fresh = true;
      for (int i = 0; i < 6; i++) {
        out[i] = trng_hw->ehr_data[i];  // reading the last word restarts collection
        fresh = fresh && out[i] != trng_last[i];
      }
      trng_hw->rng_icr = TRNG_ISR_EHR_VALID;
      if (fresh) {
        memcpy(trng_last, out, sizeof(trng_last));
        return true;
      }
    }
    trng_sample *= 2;
    trng_restart(true);
  }
  return false;
}

static int entropy(void *ctx, unsigned char *out, size_t len) {
  (void)ctx;
  uint32_t block[6];
  while (len) {
    if (!trng_block(block)) {
      crypto_wipe(block, sizeof(block));
      return MBEDTLS_ERR_CTR_DRBG_ENTROPY_SOURCE_FAILED;
    }
    size_t n = len < sizeof(block) ? len : sizeof(block);
    memcpy(out, block, n);
    out += n;
    len -= n;
  }
  crypto_wipe(block, sizeof(block));
  return 0;
}

static int rng(void *ctx, unsigned char *out, size_t len) {
  (void)ctx;
  int r = mbedtls_ctr_drbg_random(&drbg, out, len);
  if (r) rng_ok = false;
  return r;
}

void crypto_init(void) {
  reset_unreset_block_num_wait_blocking(RESET_TRNG);  // whatever the boot ROM left
  trng_restart(false);
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  mbedtls_ctr_drbg_init(&drbg);
  // CTR-DRBG with its derivation function conditions the raw TRNG bits.
  rng_ok = mbedtls_ctr_drbg_seed(&drbg, entropy, NULL, id.id, sizeof(id.id)) == 0;
  mbedtls_ecp_group_init(&p256);
  mbedtls_ecp_group_load(&p256, MBEDTLS_ECP_DP_SECP256R1);
}

bool crypto_ok(void) { return rng_ok; }

bool crypto_reseed(void) {
  if (rng_ok && mbedtls_ctr_drbg_reseed(&drbg, NULL, 0) != 0) rng_ok = false;
  return rng_ok;
}

bool crypto_random(void *out, size_t n) {
  uint8_t *p = out;
  for (size_t left = n; left && rng_ok;) {
    size_t k = left < MBEDTLS_CTR_DRBG_MAX_REQUEST ? left : MBEDTLS_CTR_DRBG_MAX_REQUEST;
    rng(NULL, p, k);
    p += k;
    left -= k;
  }
  if (!rng_ok) memset(out, 0, n);  // never hand out a partly random buffer
  return rng_ok;
}

void crypto_sha256(const void *a, size_t alen, const void *b, size_t blen, uint8_t out[32]) {
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, a, alen);
  if (b) mbedtls_sha256_update(&c, b, blen);
  mbedtls_sha256_finish(&c, out);
  mbedtls_sha256_free(&c);
}

void crypto_hmac_sha256(const uint8_t *key, size_t klen, const void *a, size_t alen,
                        const void *b, size_t blen, uint8_t out[32]) {
  mbedtls_md_context_t c;
  mbedtls_md_init(&c);
  mbedtls_md_setup(&c, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&c, key, klen);
  mbedtls_md_hmac_update(&c, a, alen);
  if (b) mbedtls_md_hmac_update(&c, b, blen);
  mbedtls_md_hmac_finish(&c, out);
  mbedtls_md_free(&c);
}

bool crypto_p256_valid(const uint8_t d[32]) {
  mbedtls_mpi m;
  mbedtls_mpi_init(&m);
  bool ok = mbedtls_mpi_read_binary(&m, d, 32) == 0 && mbedtls_ecp_check_privkey(&p256, &m) == 0;
  mbedtls_mpi_free(&m);
  return ok;
}

// x, y of k * P.
static bool mul(const mbedtls_mpi *k, const mbedtls_ecp_point *p, uint8_t x[32], uint8_t y[32]) {
  mbedtls_ecp_point q;
  mbedtls_ecp_point_init(&q);
  bool ok = mbedtls_ecp_mul(&p256, &q, k, p, rng, NULL) == 0 &&
            mbedtls_mpi_write_binary(&q.MBEDTLS_PRIVATE(X), x, 32) == 0 &&
            (!y || mbedtls_mpi_write_binary(&q.MBEDTLS_PRIVATE(Y), y, 32) == 0);
  mbedtls_ecp_point_free(&q);
  return ok;
}

bool crypto_p256_pubkey(const uint8_t d[32], uint8_t x[32], uint8_t y[32]) {
  mbedtls_mpi m;
  mbedtls_mpi_init(&m);
  bool ok = mbedtls_mpi_read_binary(&m, d, 32) == 0 &&
            mbedtls_ecp_check_privkey(&p256, &m) == 0 && mul(&m, &p256.G, x, y);
  mbedtls_mpi_free(&m);
  return ok;
}

bool crypto_p256_keygen(uint8_t d[32], uint8_t x[32], uint8_t y[32]) {
  for (int i = 0; i < 16; i++) {
    crypto_random(d, 32);
    if (crypto_p256_pubkey(d, x, y)) return true;
  }
  return false;
}

// One DER INTEGER (r or s): minimal length, positive.
static size_t der_int(uint8_t *out, const uint8_t v[32]) {
  size_t i = 0;
  while (i < 31 && v[i] == 0) i++;
  size_t n = 32 - i;
  bool pad = v[i] & 0x80u;
  out[0] = 0x02;
  out[1] = (uint8_t)(n + pad);
  out[2] = 0;
  memcpy(out + 2 + pad, v + i, n);
  return 2 + pad + n;
}

size_t crypto_p256_sign(const uint8_t d[32], const uint8_t hash[32], uint8_t sig[CRYPTO_SIG_MAX]) {
  mbedtls_mpi m, r, s;
  mbedtls_mpi_init(&m);
  mbedtls_mpi_init(&r);
  mbedtls_mpi_init(&s);
  uint8_t rb[32], sb[32];
  size_t len = 0;
  if (mbedtls_mpi_read_binary(&m, d, 32) == 0 &&
      mbedtls_ecdsa_sign(&p256, &r, &s, &m, hash, 32, rng, NULL) == 0 &&
      mbedtls_mpi_write_binary(&r, rb, 32) == 0 && mbedtls_mpi_write_binary(&s, sb, 32) == 0) {
    size_t n = der_int(sig + 2, rb);
    n += der_int(sig + 2 + n, sb);
    sig[0] = 0x30;  // SEQUENCE { r, s }: at most 2 * 35 bytes, short form length
    sig[1] = (uint8_t)n;
    len = 2 + n;
  }
  mbedtls_mpi_free(&m);
  mbedtls_mpi_free(&r);
  mbedtls_mpi_free(&s);
  return len;
}

bool crypto_p256_ecdh(const uint8_t d[32], const uint8_t px[32], const uint8_t py[32],
                      uint8_t z[32]) {
  mbedtls_mpi m;
  mbedtls_ecp_point p;
  mbedtls_mpi_init(&m);
  mbedtls_ecp_point_init(&p);
  bool ok = mbedtls_mpi_read_binary(&m, d, 32) == 0 &&
            mbedtls_mpi_read_binary(&p.MBEDTLS_PRIVATE(X), px, 32) == 0 &&
            mbedtls_mpi_read_binary(&p.MBEDTLS_PRIVATE(Y), py, 32) == 0 &&
            mbedtls_mpi_lset(&p.MBEDTLS_PRIVATE(Z), 1) == 0 &&
            mbedtls_ecp_check_pubkey(&p256, &p) == 0 && mul(&m, &p, z, NULL);
  mbedtls_mpi_free(&m);
  mbedtls_ecp_point_free(&p);
  return ok;
}

void crypto_aes256_cbc(bool encrypt, const uint8_t key[32], const uint8_t iv[16],
                       const uint8_t *in, uint8_t *out, size_t n) {
  mbedtls_aes_context c;
  uint8_t v[16];
  memcpy(v, iv, 16);
  mbedtls_aes_init(&c);
  if (encrypt) {
    mbedtls_aes_setkey_enc(&c, key, 256);
  } else {
    mbedtls_aes_setkey_dec(&c, key, 256);
  }
  mbedtls_aes_crypt_cbc(&c, encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, n, v, in, out);
  mbedtls_aes_free(&c);
}

bool crypto_equal(const void *a, const void *b, size_t n) { return mbedtls_ct_memcmp(a, b, n) == 0; }

void crypto_wipe(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }
