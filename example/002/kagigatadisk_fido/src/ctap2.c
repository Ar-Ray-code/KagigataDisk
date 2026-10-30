#include "ctap2.h"

#include <string.h>

#include "board_config.h"
#include "cbor.h"
#include "crypto.h"
#include "ctaphid.h"
#include "pico/time.h"
#include "store.h"

#define CMD_MAKE_CREDENTIAL 0x01
#define CMD_GET_ASSERTION 0x02
#define CMD_GET_INFO 0x04
#define CMD_CLIENT_PIN 0x06
#define CMD_RESET 0x07
#define CMD_GET_NEXT_ASSERTION 0x08

#define PIN_GET_RETRIES 1
#define PIN_GET_KEY_AGREEMENT 2
#define PIN_SET 3
#define PIN_CHANGE 4
#define PIN_GET_TOKEN 5

#define FLAG_UP 0x01
#define FLAG_UV 0x04
#define FLAG_AT 0x40

#define COSE_ES256 (-7)
#define COSE_ECDH_ES_HKDF_256 (-25)

#define CRED_ID_VERSION 0x01
#define CRED_ID_LEN STORE_CRED_ID_LEN  // version (1) | nonce (32) | tag (16)

#define RESET_WINDOW_MS 10000      // authenticatorReset only this soon after power-up
#define NEXT_ASSERTION_MS 30000
#define PIN_AUTH_FAILS_MAX 3       // wrong PINs in a row before a power cycle is needed

// Returns the reader's status from the enclosing function when a read fails.
#define CB(expr)                   \
  do {                             \
    if (!(expr)) return r->status; \
  } while (0)

static const uint8_t aaguid[16] = {KGFIDO_AAGUID_BYTES};
#if KGFIDO_ATTEST_BATCH
static const uint8_t attest_key[32] = {KGFIDO_ATTEST_KEY_BYTES};
static const uint8_t attest_cert[KGFIDO_ATTEST_CERT_LEN] = {KGFIDO_ATTEST_CERT_BYTES};
#endif
static const uint8_t zero_iv[16];

static uint8_t mac_key[32];   // credential ID tags
static uint8_t seed_key[32];  // credential private keys
static uint8_t pin_token[32];
static uint8_t ka_d[32], ka_x[32], ka_y[32];  // ClientPIN key agreement (this power-up)
static uint8_t pin_fails;                     // wrong PINs in a row since power-up

// The rest of a getAssertion that found several discoverable credentials.
static struct {
  bool active;
  absolute_time_t until;
  uint8_t rp_id_hash[32];
  uint8_t cdh[32];
  uint8_t flags;
  uint8_t slots[STORE_RK_MAX];
  uint8_t n, pos;
} next;

// ---------------------------------------------------------------------------
// Keys and credential IDs
// ---------------------------------------------------------------------------
static void derive_keys(void) {
  static const char id_label[] = "kgfido credential id";
  static const char key_label[] = "kgfido credential key";
  crypto_hmac_sha256(store_master(), 32, id_label, sizeof(id_label) - 1, NULL, 0, mac_key);
  crypto_hmac_sha256(store_master(), 32, key_label, sizeof(key_label) - 1, NULL, 0, seed_key);
}

void ctap2_init(void) {
  if (store_ready()) derive_keys();
  crypto_random(pin_token, sizeof(pin_token));
  crypto_p256_keygen(ka_d, ka_x, ka_y);
  pin_fails = 0;
  next.active = false;
}

static void cred_tag(const uint8_t rp_id_hash[32], const uint8_t *id, uint8_t tag[32]) {
  crypto_hmac_sha256(mac_key, 32, id, 33, rp_id_hash, 32, tag);
}

static void cred_make_id(const uint8_t rp_id_hash[32], const uint8_t nonce[32],
                         uint8_t id[CRED_ID_LEN]) {
  uint8_t tag[32];
  id[0] = CRED_ID_VERSION;
  memcpy(id + 1, nonce, 32);
  cred_tag(rp_id_hash, id, tag);
  memcpy(id + 33, tag, 16);
}

// True if id is one of ours, made for this RP.
static bool cred_check(const uint8_t rp_id_hash[32], const uint8_t *id, size_t len) {
  if (len != CRED_ID_LEN || id[0] != CRED_ID_VERSION) return false;
  uint8_t tag[32];
  cred_tag(rp_id_hash, id, tag);
  return crypto_equal(tag, id + 33, 16);
}

static bool cred_key(const uint8_t rp_id_hash[32], const uint8_t *id, uint8_t d[32]) {
  uint8_t msg[32 + 32 + 1];
  memcpy(msg, rp_id_hash, 32);
  memcpy(msg + 32, id + 1, 32);
  for (uint8_t i = 0; i < 8; i++) {
    msg[64] = i;
    crypto_hmac_sha256(seed_key, 32, msg, sizeof(msg), NULL, 0, d);
    if (crypto_p256_valid(d)) return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// Request parsing
// ---------------------------------------------------------------------------
typedef struct {
  bool rk, uv, up;
  bool has_rk, has_uv, has_up;
} options_t;

typedef struct {
  const uint8_t *id;
  size_t id_len;
  const char *name, *display_name;
  size_t name_len, display_name_len;
} user_t;

static uint8_t parse_rp(cbor_r *r, const char **id, size_t *id_len) {
  size_t n;
  CB(cbr_map(r, &n));
  *id = NULL;
  while (n--) {
    const char *k;
    size_t kl;
    CB(cbr_text(r, &k, &kl));
    if (cbor_text_is(k, kl, "id")) {
      CB(cbr_text(r, id, id_len));
    } else {
      CB(cbr_skip(r));
    }
  }
  return *id ? CTAP2_OK : CTAP2_ERR_MISSING_PARAMETER;
}

static uint8_t parse_user(cbor_r *r, user_t *u) {
  size_t n;
  CB(cbr_map(r, &n));
  memset(u, 0, sizeof(*u));
  while (n--) {
    const char *k;
    size_t kl;
    CB(cbr_text(r, &k, &kl));
    if (cbor_text_is(k, kl, "id")) {
      CB(cbr_bytes(r, &u->id, &u->id_len));
    } else if (cbor_text_is(k, kl, "name")) {
      CB(cbr_text(r, &u->name, &u->name_len));
    } else if (cbor_text_is(k, kl, "displayName")) {
      CB(cbr_text(r, &u->display_name, &u->display_name_len));
    } else {
      CB(cbr_skip(r));
    }
  }
  if (!u->id) return CTAP2_ERR_MISSING_PARAMETER;
  return u->id_len <= STORE_USER_ID_MAX ? CTAP2_OK : CTAP1_ERR_INVALID_LENGTH;
}

// pubKeyCredParams: *es256 is set if ES256 is among them.
static uint8_t parse_params(cbor_r *r, bool *es256) {
  size_t n;
  CB(cbr_array(r, &n));
  *es256 = false;
  while (n--) {
    size_t m;
    CB(cbr_map(r, &m));
    bool have_alg = false, have_type = false, public_key = false;
    int64_t alg = 0;
    while (m--) {
      const char *k;
      size_t kl;
      CB(cbr_text(r, &k, &kl));
      if (cbor_text_is(k, kl, "alg")) {
        CB(cbr_int(r, &alg));
        have_alg = true;
      } else if (cbor_text_is(k, kl, "type")) {
        const char *t;
        size_t tl;
        CB(cbr_text(r, &t, &tl));
        have_type = true;
        public_key = cbor_text_is(t, tl, "public-key");
      } else {
        CB(cbr_skip(r));
      }
    }
    if (!have_alg || !have_type) return CTAP2_ERR_MISSING_PARAMETER;
    if (public_key && alg == COSE_ES256) *es256 = true;
  }
  return CTAP2_OK;
}

static uint8_t parse_options(cbor_r *r, options_t *o) {
  size_t n;
  CB(cbr_map(r, &n));
  while (n--) {
    const char *k;
    size_t kl;
    CB(cbr_text(r, &k, &kl));
    if (cbor_text_is(k, kl, "rk")) {
      CB(cbr_bool(r, &o->rk));
      o->has_rk = true;
    } else if (cbor_text_is(k, kl, "uv")) {
      CB(cbr_bool(r, &o->uv));
      o->has_uv = true;
    } else if (cbor_text_is(k, kl, "up")) {
      CB(cbr_bool(r, &o->up));
      o->has_up = true;
    } else {
      CB(cbr_skip(r));
    }
  }
  return CTAP2_OK;
}

// One PublicKeyCredentialDescriptor; *id is NULL if its type is not
// "public-key" (to be ignored).
static uint8_t parse_cred_desc(cbor_r *r, const uint8_t **id, size_t *id_len) {
  size_t n;
  CB(cbr_map(r, &n));
  const uint8_t *p = NULL;
  size_t len = 0;
  bool have_type = false, public_key = false;
  while (n--) {
    const char *k;
    size_t kl;
    CB(cbr_text(r, &k, &kl));
    if (cbor_text_is(k, kl, "id")) {
      CB(cbr_bytes(r, &p, &len));
    } else if (cbor_text_is(k, kl, "type")) {
      const char *t;
      size_t tl;
      CB(cbr_text(r, &t, &tl));
      have_type = true;
      public_key = cbor_text_is(t, tl, "public-key");
    } else {
      CB(cbr_skip(r));
    }
  }
  if (!p || !have_type) return CTAP2_ERR_MISSING_PARAMETER;
  *id = public_key ? p : NULL;
  *id_len = len;
  return CTAP2_OK;
}

// extensions: none supported, but it has to be a map.
static uint8_t skip_extensions(cbor_r *r) {
  if (cbr_peek(r) != CBOR_MAP) return CTAP2_ERR_CBOR_UNEXPECTED_TYPE;
  CB(cbr_skip(r));
  return CTAP2_OK;
}

// Records where an array starts (for a second pass) and skips over it.
static uint8_t take_array(cbor_r *r, cbor_r *start, size_t *n) {
  CB(cbr_array(r, n));
  *start = *r;
  for (size_t i = 0; i < *n; i++) CB(cbr_skip(r));
  return CTAP2_OK;
}

// A COSE_Key from the platform (ClientPIN keyAgreement): its x and y.
static uint8_t parse_cose_key(cbor_r *r, const uint8_t **x, const uint8_t **y) {
  size_t n;
  CB(cbr_map(r, &n));
  *x = *y = NULL;
  while (n--) {
    int64_t k;
    CB(cbr_int(r, &k));
    const uint8_t *p;
    size_t len;
    if (k == -2 || k == -3) {
      CB(cbr_bytes(r, &p, &len));
      if (len != 32) return CTAP1_ERR_INVALID_PARAMETER;
      *(k == -2 ? x : y) = p;
    } else {
      CB(cbr_skip(r));
    }
  }
  return *x && *y ? CTAP2_OK : CTAP1_ERR_INVALID_PARAMETER;
}

// ---------------------------------------------------------------------------
// Response pieces
// ---------------------------------------------------------------------------
static void put_cose_key(cbor_w *w, int64_t alg, const uint8_t x[32], const uint8_t y[32]) {
  cbw_map(w, 5);
  cbw_int(w, 1);  // kty: EC2
  cbw_int(w, 2);
  cbw_int(w, 3);  // alg
  cbw_int(w, alg);
  cbw_int(w, -1);  // crv: P-256
  cbw_int(w, 1);
  cbw_int(w, -2);
  cbw_bytes(w, x, 32);
  cbw_int(w, -3);
  cbw_bytes(w, y, 32);
}

static void put_cred_desc(cbor_w *w, const uint8_t *id) {
  cbw_map(w, 2);
  cbw_text(w, "id");
  cbw_bytes(w, id, CRED_ID_LEN);
  cbw_text(w, "type");
  cbw_text(w, "public-key");
}

// rpIdHash | flags | signCount (big-endian)
static size_t put_auth_data(uint8_t *out, const uint8_t rp_id_hash[32], uint8_t flags,
                            uint32_t counter) {
  memcpy(out, rp_id_hash, 32);
  out[32] = flags;
  out[33] = (uint8_t)(counter >> 24);
  out[34] = (uint8_t)(counter >> 16);
  out[35] = (uint8_t)(counter >> 8);
  out[36] = (uint8_t)counter;
  return 37;
}

// Length of s[0..n) cut to at most max bytes without splitting a UTF-8 character.
static size_t utf8_cut(const char *s, size_t n, size_t max) {
  if (n <= max) return n;
  while (max > 0 && ((uint8_t)s[max] & 0xC0u) == 0x80u) max--;
  return max;
}

// ---------------------------------------------------------------------------
// PIN
// ---------------------------------------------------------------------------
// pinAuth of makeCredential/getAssertion: LEFT(HMAC(pinToken, clientDataHash), 16).
// *uv is set when it checks out. An empty pinAuth is a platform asking for a
// touch to pick this authenticator.
static uint8_t check_pin_auth(bool have_auth, const uint8_t *auth, size_t auth_len,
                              bool have_proto, uint64_t proto, const uint8_t cdh[32], bool *uv) {
  *uv = false;
  if (!have_auth) return CTAP2_OK;
  if (auth_len == 0) {
    uint8_t st = ctaphid_wait_up();
    if (st != CTAP2_OK) return st;
    return store_pin_is_set() ? CTAP2_ERR_PIN_INVALID : CTAP2_ERR_PIN_NOT_SET;
  }
  if (!have_proto) return CTAP2_ERR_MISSING_PARAMETER;
  if (proto != 1) return CTAP2_ERR_PIN_AUTH_INVALID;
  if (!store_pin_is_set()) return CTAP2_ERR_PIN_NOT_SET;
  uint8_t mac[32];
  crypto_hmac_sha256(pin_token, sizeof(pin_token), cdh, 32, NULL, 0, mac);
  if (auth_len != 16 || !crypto_equal(mac, auth, 16)) return CTAP2_ERR_PIN_AUTH_INVALID;
  *uv = true;
  return CTAP2_OK;
}

// sharedSecret = SHA-256((ka_d * platform key).x)
static uint8_t shared_secret(const uint8_t *x, const uint8_t *y, uint8_t out[32]) {
  uint8_t z[32];
  if (!crypto_p256_ecdh(ka_d, x, y, z)) return CTAP1_ERR_INVALID_PARAMETER;
  crypto_sha256(z, 32, NULL, 0, out);
  crypto_wipe(z, sizeof(z));
  return CTAP2_OK;
}

// LEFT(HMAC(sharedSecret, a | b), 16) == auth
static bool platform_auth_ok(const uint8_t shared[32], const uint8_t *auth, size_t auth_len,
                             const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
  uint8_t mac[32];
  crypto_hmac_sha256(shared, 32, a, alen, b, blen, mac);
  return auth_len == 16 && crypto_equal(mac, auth, 16);
}

// newPinEnc -> stored LEFT(SHA-256(PIN), 16).
static uint8_t set_new_pin(const uint8_t shared[32], const uint8_t *enc, size_t len) {
  uint8_t pin[256];
  if (len < 64 || len > sizeof(pin) || len % 16) return CTAP1_ERR_INVALID_PARAMETER;
  crypto_aes256_cbc(false, shared, zero_iv, enc, pin, len);
  size_t n = 0, chars = 0;
  for (; n < len && pin[n]; n++) chars += (pin[n] & 0xC0u) != 0x80u;
  uint8_t st = CTAP2_OK;
  if (chars < 4 || n > 63) {
    st = CTAP2_ERR_PIN_POLICY_VIOLATION;
  } else {
    uint8_t h[32];
    crypto_sha256(pin, n, NULL, 0, h);
    if (!store_set_pin(h)) st = CTAP1_ERR_OTHER;
    crypto_wipe(h, sizeof(h));
  }
  crypto_wipe(pin, sizeof(pin));
  return st;
}

// pinHashEnc against the stored PIN hash, spending a retry first.
static uint8_t check_pin_hash(const uint8_t shared[32], const uint8_t *enc, size_t len) {
  if (len != 16) return CTAP1_ERR_INVALID_PARAMETER;
  if (!store_set_pin_retries((uint8_t)(store_pin_retries() - 1u))) return CTAP1_ERR_OTHER;
  uint8_t h[16];
  crypto_aes256_cbc(false, shared, zero_iv, enc, h, 16);
  bool ok = crypto_equal(h, store_pin_hash(), 16);
  crypto_wipe(h, sizeof(h));
  if (!ok) {
    crypto_p256_keygen(ka_d, ka_x, ka_y);  // the platform has to fetch a new one
    pin_fails++;
    if (store_pin_retries() == 0) return CTAP2_ERR_PIN_BLOCKED;
    if (pin_fails >= PIN_AUTH_FAILS_MAX) return CTAP2_ERR_PIN_AUTH_BLOCKED;
    return CTAP2_ERR_PIN_INVALID;
  }
  pin_fails = 0;
  return store_set_pin_retries(STORE_PIN_RETRIES) ? CTAP2_OK : CTAP1_ERR_OTHER;
}

static uint8_t pin_usable(void) {
  if (!store_pin_is_set()) return CTAP2_ERR_PIN_NOT_SET;
  if (store_pin_retries() == 0) return CTAP2_ERR_PIN_BLOCKED;
  if (pin_fails >= PIN_AUTH_FAILS_MAX) return CTAP2_ERR_PIN_AUTH_BLOCKED;
  return CTAP2_OK;
}

static uint8_t cmd_client_pin(cbor_r *r, cbor_w *w) {
  uint64_t proto = 0, sub = 0;
  bool have_proto = false, have_sub = false;
  const uint8_t *kx = NULL, *ky = NULL, *auth = NULL, *new_enc = NULL, *hash_enc = NULL;
  size_t auth_len = 0, new_len = 0, hash_len = 0;
  uint8_t st;
  size_t n;
  CB(cbr_map(r, &n));
  while (n--) {
    int64_t key;
    CB(cbr_int(r, &key));
    switch (key) {
      case 1:
        CB(cbr_uint(r, &proto));
        have_proto = true;
        break;
      case 2:
        CB(cbr_uint(r, &sub));
        have_sub = true;
        break;
      case 3:
        if ((st = parse_cose_key(r, &kx, &ky)) != CTAP2_OK) return st;
        break;
      case 4:
        CB(cbr_bytes(r, &auth, &auth_len));
        break;
      case 5:
        CB(cbr_bytes(r, &new_enc, &new_len));
        break;
      case 6:
        CB(cbr_bytes(r, &hash_enc, &hash_len));
        break;
      default:
        CB(cbr_skip(r));
        break;
    }
  }
  if (!have_proto || !have_sub) return CTAP2_ERR_MISSING_PARAMETER;
  if (proto != 1) return CTAP1_ERR_INVALID_PARAMETER;

  uint8_t shared[32];
  switch (sub) {
    case PIN_GET_RETRIES:
      cbw_map(w, 1);
      cbw_uint(w, 3);
      cbw_uint(w, store_pin_retries());
      return CTAP2_OK;

    case PIN_GET_KEY_AGREEMENT:
      cbw_map(w, 1);
      cbw_uint(w, 1);
      put_cose_key(w, COSE_ECDH_ES_HKDF_256, ka_x, ka_y);
      return CTAP2_OK;

    case PIN_SET:
      if (!kx || !auth || !new_enc) return CTAP2_ERR_MISSING_PARAMETER;
      if (store_pin_is_set()) return CTAP2_ERR_NOT_ALLOWED;
      if ((st = shared_secret(kx, ky, shared)) != CTAP2_OK) return st;
      if (!platform_auth_ok(shared, auth, auth_len, new_enc, new_len, NULL, 0)) {
        st = CTAP2_ERR_PIN_AUTH_INVALID;
      } else {
        st = set_new_pin(shared, new_enc, new_len);
      }
      break;

    case PIN_CHANGE:
      if (!kx || !auth || !new_enc || !hash_enc) return CTAP2_ERR_MISSING_PARAMETER;
      if ((st = pin_usable()) != CTAP2_OK) return st;
      if ((st = shared_secret(kx, ky, shared)) != CTAP2_OK) return st;
      if (!platform_auth_ok(shared, auth, auth_len, new_enc, new_len, hash_enc, hash_len)) {
        st = CTAP2_ERR_PIN_AUTH_INVALID;
      } else if ((st = check_pin_hash(shared, hash_enc, hash_len)) == CTAP2_OK) {
        st = set_new_pin(shared, new_enc, new_len);
        if (st == CTAP2_OK) crypto_random(pin_token, sizeof(pin_token));  // old tokens die
      }
      break;

    case PIN_GET_TOKEN:
      if (!kx || !hash_enc) return CTAP2_ERR_MISSING_PARAMETER;
      if ((st = pin_usable()) != CTAP2_OK) return st;
      if ((st = shared_secret(kx, ky, shared)) != CTAP2_OK) return st;
      if ((st = check_pin_hash(shared, hash_enc, hash_len)) == CTAP2_OK) {
        uint8_t enc[sizeof(pin_token)];
        crypto_aes256_cbc(true, shared, zero_iv, pin_token, enc, sizeof(enc));
        cbw_map(w, 1);
        cbw_uint(w, 2);
        cbw_bytes(w, enc, sizeof(enc));
      }
      break;

    default:
      return CTAP1_ERR_INVALID_PARAMETER;
  }
  crypto_wipe(shared, sizeof(shared));
  return st;
}

// ---------------------------------------------------------------------------
// makeCredential
// ---------------------------------------------------------------------------
static uint8_t cmd_make_credential(cbor_r *r, cbor_w *w) {
  const uint8_t *cdh = NULL, *pin_auth = NULL;
  size_t cdh_len = 0, pin_auth_len = 0;
  const char *rp_id = NULL;
  size_t rp_id_len = 0;
  user_t user;
  bool have_user = false, have_params = false, es256 = false;
  bool have_pin_auth = false, have_pin_proto = false;
  uint64_t pin_proto = 0;
  cbor_r excl;
  size_t excl_n = 0;
  options_t opt = {0};
  uint8_t st;

  size_t n;
  CB(cbr_map(r, &n));
  while (n--) {
    int64_t key;
    CB(cbr_int(r, &key));
    switch (key) {
      case 1:
        CB(cbr_bytes(r, &cdh, &cdh_len));
        break;
      case 2:
        if ((st = parse_rp(r, &rp_id, &rp_id_len)) != CTAP2_OK) return st;
        break;
      case 3:
        if ((st = parse_user(r, &user)) != CTAP2_OK) return st;
        have_user = true;
        break;
      case 4:
        if ((st = parse_params(r, &es256)) != CTAP2_OK) return st;
        have_params = true;
        break;
      case 5:
        if ((st = take_array(r, &excl, &excl_n)) != CTAP2_OK) return st;
        break;
      case 6:
        if ((st = skip_extensions(r)) != CTAP2_OK) return st;
        break;
      case 7:
        if ((st = parse_options(r, &opt)) != CTAP2_OK) return st;
        break;
      case 8:
        CB(cbr_bytes(r, &pin_auth, &pin_auth_len));
        have_pin_auth = true;
        break;
      case 9:
        CB(cbr_uint(r, &pin_proto));
        have_pin_proto = true;
        break;
      default:
        CB(cbr_skip(r));
        break;
    }
  }
  if (!cdh || !rp_id || !have_user || !have_params) return CTAP2_ERR_MISSING_PARAMETER;
  if (cdh_len != 32) return CTAP1_ERR_INVALID_LENGTH;
  if (!es256) return CTAP2_ERR_UNSUPPORTED_ALGORITHM;
  if (opt.has_up && !opt.up) return CTAP2_ERR_INVALID_OPTION;
  if (opt.uv) return CTAP2_ERR_UNSUPPORTED_OPTION;  // no built-in user verification

  bool uv;
  st = check_pin_auth(have_pin_auth, pin_auth, pin_auth_len, have_pin_proto, pin_proto, cdh, &uv);
  if (st != CTAP2_OK) return st;
  if (!have_pin_auth && store_pin_is_set()) return CTAP2_ERR_PIN_REQUIRED;

  uint8_t rp_id_hash[32];
  crypto_sha256(rp_id, rp_id_len, NULL, 0, rp_id_hash);

  for (size_t i = 0; i < excl_n; i++) {
    const uint8_t *id;
    size_t len;
    if ((st = parse_cred_desc(&excl, &id, &len)) != CTAP2_OK) return st;
    if (id && cred_check(rp_id_hash, id, len)) {
      st = ctaphid_wait_up();  // the user confirms before learning it is excluded
      return st == CTAP2_OK ? CTAP2_ERR_CREDENTIAL_EXCLUDED : st;
    }
  }
  int slot = opt.rk ? store_rk_slot_for(rp_id_hash, user.id, user.id_len) : -1;
  if (opt.rk && slot < 0) return CTAP2_ERR_KEY_STORE_FULL;

  if ((st = ctaphid_wait_up()) != CTAP2_OK) return st;

  // The new credential.
  uint8_t nonce[32], id[CRED_ID_LEN], d[32], x[32], y[32];
  bool made = false;
  for (int i = 0; i < 4 && !made && crypto_reseed(); i++) {
    crypto_random(nonce, sizeof(nonce));
    cred_make_id(rp_id_hash, nonce, id);
    made = cred_key(rp_id_hash, id, d) && crypto_p256_pubkey(d, x, y);
  }
  uint32_t counter = made ? store_counter_next() : 0;
  if (!counter) {
    crypto_wipe(d, sizeof(d));
    return CTAP1_ERR_OTHER;
  }
  if (opt.rk) {
    static store_rk_t rk;  // 512 bytes: kept off the stack
    memset(&rk, 0, sizeof(rk));
    rk.magic = STORE_RK_MAGIC;
    rk.created = counter;
    memcpy(rk.rp_id_hash, rp_id_hash, 32);
    memcpy(rk.cred_id, id, CRED_ID_LEN);
    rk.user_id_len = (uint8_t)user.id_len;
    memcpy(rk.user_id, user.id, user.id_len);
    if (user.name) {
      rk.name_len = (uint8_t)utf8_cut(user.name, user.name_len, STORE_NAME_MAX);
      memcpy(rk.name, user.name, rk.name_len);
    }
    if (user.display_name) {
      rk.display_name_len =
          (uint8_t)utf8_cut(user.display_name, user.display_name_len, STORE_NAME_MAX);
      memcpy(rk.display_name, user.display_name, rk.display_name_len);
    }
    if (!store_rk_write(slot, &rk)) {
      crypto_wipe(d, sizeof(d));
      return CTAP2_ERR_KEY_STORE_FULL;
    }
  }

  // authData with the attested credential data.
  uint8_t ad[256];
  size_t al = put_auth_data(ad, rp_id_hash, FLAG_UP | FLAG_AT | (uv ? FLAG_UV : 0), counter);
  memcpy(ad + al, aaguid, 16);
  al += 16;
  ad[al++] = 0;
  ad[al++] = CRED_ID_LEN;
  memcpy(ad + al, id, CRED_ID_LEN);
  al += CRED_ID_LEN;
  cbor_w kw;
  cbw_init(&kw, ad + al, sizeof(ad) - al);
  put_cose_key(&kw, COSE_ES256, x, y);
  al += kw.len;

  // "packed" attestation over authData | clientDataHash.
  uint8_t hash[32], sig[CRYPTO_SIG_MAX];
  crypto_sha256(ad, al, cdh, 32, hash);
#if KGFIDO_ATTEST_BATCH
  size_t sig_len = crypto_p256_sign(attest_key, hash, sig);
#else
  size_t sig_len = crypto_p256_sign(d, hash, sig);  // self attestation
#endif
  crypto_wipe(d, sizeof(d));
  if (!sig_len) return CTAP1_ERR_OTHER;

  cbw_map(w, 3);
  cbw_uint(w, 1);
  cbw_text(w, "packed");
  cbw_uint(w, 2);
  cbw_bytes(w, ad, al);
  cbw_uint(w, 3);
  cbw_map(w, KGFIDO_ATTEST_BATCH ? 3 : 2);
  cbw_text(w, "alg");
  cbw_int(w, COSE_ES256);
  cbw_text(w, "sig");
  cbw_bytes(w, sig, sig_len);
#if KGFIDO_ATTEST_BATCH
  cbw_text(w, "x5c");
  cbw_array(w, 1);
  cbw_bytes(w, attest_cert, sizeof(attest_cert));
#endif
  return CTAP2_OK;
}

// ---------------------------------------------------------------------------
// getAssertion / getNextAssertion
// ---------------------------------------------------------------------------
// One assertion: with cred_id (from the allowList), or with the resident
// credential in `slot` (then the user entity is included). count > 0 adds
// numberOfCredentials.
static uint8_t put_assertion(cbor_w *w, const uint8_t rp_id_hash[32], const uint8_t cdh[32],
                             uint8_t flags, const uint8_t *cred_id, int slot, size_t count) {
  // The counter write may move sectors around: read the slot after it.
  uint32_t counter = store_counter_next();
  if (!counter) return CTAP1_ERR_OTHER;
  static store_rk_t rk;
  if (!cred_id) {
    const store_rk_t *p = store_rk(slot);
    if (!p) return CTAP2_ERR_NO_CREDENTIALS;
    rk = *p;
    cred_id = rk.cred_id;
  }

  uint8_t d[32];
  if (!cred_key(rp_id_hash, cred_id, d)) return CTAP1_ERR_OTHER;
  uint8_t ad[37], hash[32], sig[CRYPTO_SIG_MAX];
  put_auth_data(ad, rp_id_hash, flags, counter);
  crypto_sha256(ad, sizeof(ad), cdh, 32, hash);
  size_t sig_len = crypto_p256_sign(d, hash, sig);
  crypto_wipe(d, sizeof(d));
  if (!sig_len) return CTAP1_ERR_OTHER;

  bool user = slot >= 0;
  cbw_map(w, 3u + user + (count > 0));
  cbw_uint(w, 1);
  put_cred_desc(w, cred_id);
  cbw_uint(w, 2);
  cbw_bytes(w, ad, sizeof(ad));
  cbw_uint(w, 3);
  cbw_bytes(w, sig, sig_len);
  if (user) {
    // Names only after user verification (CTAP 2.0 5.2 step 9).
    bool ident = flags & FLAG_UV;
    bool name = ident && rk.name_len, display = ident && rk.display_name_len;
    cbw_uint(w, 4);
    cbw_map(w, 1u + name + display);
    cbw_text(w, "id");
    cbw_bytes(w, rk.user_id, rk.user_id_len);
    if (name) {
      cbw_text(w, "name");
      cbw_textn(w, rk.name, rk.name_len);
    }
    if (display) {
      cbw_text(w, "displayName");
      cbw_textn(w, rk.display_name, rk.display_name_len);
    }
  }
  if (count) {
    cbw_uint(w, 5);
    cbw_uint(w, count);
  }
  return CTAP2_OK;
}

// Slots of the resident credentials for this RP, newest first.
static size_t find_resident(const uint8_t rp_id_hash[32], uint8_t slots[STORE_RK_MAX]) {
  uint32_t created[STORE_RK_MAX];
  size_t n = 0;
  for (int i = 0; i < STORE_RK_MAX; i++) {
    const store_rk_t *rk = store_rk(i);
    if (!rk || memcmp(rk->rp_id_hash, rp_id_hash, 32) != 0) continue;
    size_t j = n++;
    for (; j > 0 && created[j - 1] < rk->created; j--) {
      created[j] = created[j - 1];
      slots[j] = slots[j - 1];
    }
    created[j] = rk->created;
    slots[j] = (uint8_t)i;
  }
  return n;
}

static uint8_t cmd_get_assertion(cbor_r *r, cbor_w *w) {
  const char *rp_id = NULL;
  size_t rp_id_len = 0;
  const uint8_t *cdh = NULL, *pin_auth = NULL;
  size_t cdh_len = 0, pin_auth_len = 0;
  bool have_pin_auth = false, have_pin_proto = false;
  uint64_t pin_proto = 0;
  cbor_r allow;
  size_t allow_n = 0;
  options_t opt = {.up = true};
  uint8_t st;

  size_t n;
  CB(cbr_map(r, &n));
  while (n--) {
    int64_t key;
    CB(cbr_int(r, &key));
    switch (key) {
      case 1:
        CB(cbr_text(r, &rp_id, &rp_id_len));
        break;
      case 2:
        CB(cbr_bytes(r, &cdh, &cdh_len));
        break;
      case 3:
        if ((st = take_array(r, &allow, &allow_n)) != CTAP2_OK) return st;
        break;
      case 4:
        if ((st = skip_extensions(r)) != CTAP2_OK) return st;
        break;
      case 5:
        if ((st = parse_options(r, &opt)) != CTAP2_OK) return st;
        break;
      case 6:
        CB(cbr_bytes(r, &pin_auth, &pin_auth_len));
        have_pin_auth = true;
        break;
      case 7:
        CB(cbr_uint(r, &pin_proto));
        have_pin_proto = true;
        break;
      default:
        CB(cbr_skip(r));
        break;
    }
  }
  if (!rp_id || !cdh) return CTAP2_ERR_MISSING_PARAMETER;
  if (cdh_len != 32) return CTAP1_ERR_INVALID_LENGTH;
  if (opt.has_rk) return CTAP2_ERR_INVALID_OPTION;
  if (opt.uv) return CTAP2_ERR_UNSUPPORTED_OPTION;

  bool uv;
  st = check_pin_auth(have_pin_auth, pin_auth, pin_auth_len, have_pin_proto, pin_proto, cdh, &uv);
  if (st != CTAP2_OK) return st;

  uint8_t rp_id_hash[32];
  crypto_sha256(rp_id, rp_id_len, NULL, 0, rp_id_hash);

  const uint8_t *cred_id = NULL;
  uint8_t slots[STORE_RK_MAX];
  size_t count = 0;
  if (allow_n) {
    for (size_t i = 0; i < allow_n && !cred_id; i++) {
      const uint8_t *id;
      size_t len;
      if ((st = parse_cred_desc(&allow, &id, &len)) != CTAP2_OK) return st;
      if (id && cred_check(rp_id_hash, id, len)) cred_id = id;
    }
    if (!cred_id) return CTAP2_ERR_NO_CREDENTIALS;
  } else {
    count = find_resident(rp_id_hash, slots);
    if (!count) return CTAP2_ERR_NO_CREDENTIALS;
  }

  if (opt.up && (st = ctaphid_wait_up()) != CTAP2_OK) return st;
  uint8_t flags = (uint8_t)((opt.up ? FLAG_UP : 0) | (uv ? FLAG_UV : 0));

  if (cred_id) return put_assertion(w, rp_id_hash, cdh, flags, cred_id, -1, 0);
  st = put_assertion(w, rp_id_hash, cdh, flags, NULL, slots[0], count);
  if (st == CTAP2_OK && count > 1) {
    next.active = true;
    next.until = make_timeout_time_ms(NEXT_ASSERTION_MS);
    memcpy(next.rp_id_hash, rp_id_hash, 32);
    memcpy(next.cdh, cdh, 32);
    next.flags = flags;
    memcpy(next.slots, slots, count);
    next.n = (uint8_t)count;
    next.pos = 1;
  }
  return st;
}

static uint8_t cmd_get_next_assertion(cbor_w *w) {
  if (!next.active || time_reached(next.until) || next.pos >= next.n) {
    next.active = false;
    return CTAP2_ERR_NOT_ALLOWED;
  }
  uint8_t st = put_assertion(w, next.rp_id_hash, next.cdh, next.flags, NULL,
                             next.slots[next.pos++], 0);
  next.until = make_timeout_time_ms(NEXT_ASSERTION_MS);
  if (next.pos >= next.n) next.active = false;
  return st;
}

// ---------------------------------------------------------------------------
// getInfo / reset
// ---------------------------------------------------------------------------
static uint8_t cmd_get_info(cbor_w *w) {
  cbw_map(w, 5);
  cbw_uint(w, 1);  // versions
  cbw_array(w, 1);
  cbw_text(w, "FIDO_2_0");
  cbw_uint(w, 3);  // aaguid
  cbw_bytes(w, aaguid, sizeof(aaguid));
  cbw_uint(w, 4);  // options (no "uv": the PIN is the only user verification)
  cbw_map(w, 4);
  cbw_text(w, "rk");
  cbw_bool(w, true);
  cbw_text(w, "up");
  cbw_bool(w, true);
  cbw_text(w, "plat");
  cbw_bool(w, false);
  cbw_text(w, "clientPin");
  cbw_bool(w, store_pin_is_set());
  cbw_uint(w, 5);  // maxMsgSize
  cbw_uint(w, CTAP2_MSG_MAX);
  cbw_uint(w, 6);  // pinProtocols
  cbw_array(w, 1);
  cbw_uint(w, 1);
  return CTAP2_OK;
}

static uint8_t cmd_reset(void) {
  if (to_ms_since_boot(get_absolute_time()) > RESET_WINDOW_MS) return CTAP2_ERR_NOT_ALLOWED;
  uint8_t st = ctaphid_wait_up();
  if (st != CTAP2_OK) return st;
  if (!store_wipe()) return CTAP1_ERR_OTHER;
  ctap2_init();
  return CTAP2_OK;
}

// ---------------------------------------------------------------------------
bool ctap2_is_user_action(uint8_t cmd) {
  return cmd == CMD_MAKE_CREDENTIAL || cmd == CMD_GET_ASSERTION ||
         cmd == CMD_GET_NEXT_ASSERTION || cmd == CMD_CLIENT_PIN || cmd == CMD_RESET;
}

size_t ctap2_handle(const uint8_t *req, size_t len, uint8_t *resp, size_t cap) {
  cbor_r rd;
  cbor_w w;
  cbr_init(&rd, req + 1, len - 1);
  cbw_init(&w, resp + 1, cap - 1);
  cbor_r *r = &rd;
  uint8_t cmd = req[0], st;
  if (cmd != CMD_GET_NEXT_ASSERTION) next.active = false;
  if ((!store_ready() || !crypto_ok()) && cmd != CMD_GET_INFO) {
    st = CTAP1_ERR_OTHER;
  } else {
    switch (cmd) {
      case CMD_MAKE_CREDENTIAL:
        st = cmd_make_credential(r, &w);
        break;
      case CMD_GET_ASSERTION:
        st = cmd_get_assertion(r, &w);
        break;
      case CMD_GET_NEXT_ASSERTION:
        st = cmd_get_next_assertion(&w);
        break;
      case CMD_GET_INFO:
        st = cmd_get_info(&w);
        break;
      case CMD_CLIENT_PIN:
        st = cmd_client_pin(r, &w);
        break;
      case CMD_RESET:
        st = cmd_reset();
        break;
      default:
        st = CTAP1_ERR_INVALID_COMMAND;
        break;
    }
  }
  if (st == CTAP2_OK && w.err) st = CTAP1_ERR_OTHER;  // response did not fit
  if (!crypto_ok()) st = CTAP1_ERR_OTHER;  // the TRNG failed during the command
  resp[0] = st;
  return st == CTAP2_OK ? 1 + w.len : 1;
}
