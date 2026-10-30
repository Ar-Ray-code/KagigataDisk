#include "cbor.h"

#include <string.h>

#define CBOR_MAX_DEPTH 8

// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
void cbw_init(cbor_w *w, uint8_t *buf, size_t cap) {
  w->buf = buf;
  w->cap = cap;
  w->len = 0;
  w->err = false;
}

static void put(cbor_w *w, const void *p, size_t n) {
  if (w->err || n > w->cap - w->len) {
    w->err = true;
    return;
  }
  if (n) memcpy(w->buf + w->len, p, n);
  w->len += n;
}

static void cbw_head(cbor_w *w, uint8_t major, uint64_t v) {
  uint8_t b[9];
  size_t n;
  uint8_t m = (uint8_t)(major << 5);
  if (v < 24) {
    b[0] = (uint8_t)(m | v);
    n = 1;
  } else if (v <= 0xFFu) {
    b[0] = m | 24;
    n = 2;
  } else if (v <= 0xFFFFu) {
    b[0] = m | 25;
    n = 3;
  } else if (v <= 0xFFFFFFFFu) {
    b[0] = m | 26;
    n = 5;
  } else {
    b[0] = m | 27;
    n = 9;
  }
  for (size_t i = n - 1; i > 0; i--) {  // big-endian argument after the initial byte
    b[i] = (uint8_t)v;
    v >>= 8;
  }
  put(w, b, n);
}

void cbw_uint(cbor_w *w, uint64_t v) { cbw_head(w, CBOR_UINT, v); }

void cbw_int(cbor_w *w, int64_t v) {
  if (v >= 0) {
    cbw_head(w, CBOR_UINT, (uint64_t)v);
  } else {
    cbw_head(w, CBOR_NINT, (uint64_t)(-1 - v));
  }
}

void cbw_bytes(cbor_w *w, const void *p, size_t n) {
  cbw_head(w, CBOR_BYTES, n);
  put(w, p, n);
}

void cbw_textn(cbor_w *w, const char *s, size_t n) {
  cbw_head(w, CBOR_TEXT, n);
  put(w, s, n);
}

void cbw_text(cbor_w *w, const char *s) { cbw_textn(w, s, strlen(s)); }

void cbw_bool(cbor_w *w, bool b) {
  uint8_t v = b ? 0xF5 : 0xF4;
  put(w, &v, 1);
}

void cbw_map(cbor_w *w, size_t pairs) { cbw_head(w, CBOR_MAP, pairs); }
void cbw_array(cbor_w *w, size_t items) { cbw_head(w, CBOR_ARRAY, items); }

// ---------------------------------------------------------------------------
// Reader
// ---------------------------------------------------------------------------
void cbr_init(cbor_r *r, const uint8_t *p, size_t n) {
  r->p = p;
  r->end = p + n;
  r->status = 0;
}

static bool fail(cbor_r *r, uint8_t status) {
  if (!r->status) r->status = status;
  return false;
}

static size_t left(const cbor_r *r) { return (size_t)(r->end - r->p); }

// Reads an item's initial byte and argument.
static bool head(cbor_r *r, uint8_t *major, uint64_t *v) {
  if (r->status) return false;
  if (r->p >= r->end) return fail(r, CBOR_ERR_INVALID);
  uint8_t ib = *r->p++;
  uint8_t ai = ib & 31u;
  *major = ib >> 5;
  if (ai < 24) {
    *v = ai;
    return true;
  }
  if (ai > 27) return fail(r, CBOR_ERR_INVALID);  // indefinite length or reserved
  size_t n = (size_t)1 << (ai - 24);
  if (left(r) < n) return fail(r, CBOR_ERR_INVALID);
  uint64_t x = 0;
  for (size_t i = 0; i < n; i++) x = (x << 8) | *r->p++;
  *v = x;
  return true;
}

uint8_t cbr_peek(const cbor_r *r) { return r->p < r->end ? (uint8_t)(*r->p >> 5) : 0xFF; }

bool cbr_uint(cbor_r *r, uint64_t *v) {
  uint8_t m;
  if (!head(r, &m, v)) return false;
  return m == CBOR_UINT || fail(r, CBOR_ERR_TYPE);
}

bool cbr_int(cbor_r *r, int64_t *v) {
  uint8_t m;
  uint64_t x;
  if (!head(r, &m, &x)) return false;
  if (m != CBOR_UINT && m != CBOR_NINT) return fail(r, CBOR_ERR_TYPE);
  if (x > (uint64_t)INT64_MAX) return fail(r, CBOR_ERR_INVALID);
  *v = m == CBOR_UINT ? (int64_t)x : -1 - (int64_t)x;
  return true;
}

static bool string(cbor_r *r, uint8_t want, const uint8_t **p, size_t *n) {
  uint8_t m;
  uint64_t len;
  if (!head(r, &m, &len)) return false;
  if (m != want) return fail(r, CBOR_ERR_TYPE);
  if (len > left(r)) return fail(r, CBOR_ERR_INVALID);
  *p = r->p;
  *n = (size_t)len;
  r->p += len;
  return true;
}

bool cbr_bytes(cbor_r *r, const uint8_t **p, size_t *n) { return string(r, CBOR_BYTES, p, n); }

bool cbr_text(cbor_r *r, const char **s, size_t *n) {
  return string(r, CBOR_TEXT, (const uint8_t **)s, n);
}

bool cbr_bool(cbor_r *r, bool *b) {
  uint8_t m;
  uint64_t v;
  if (!head(r, &m, &v)) return false;
  if (m != CBOR_SIMPLE || (v != 20 && v != 21)) return fail(r, CBOR_ERR_TYPE);
  *b = v == 21;
  return true;
}

// Containers: every entry takes at least one byte, which bounds the count.
static bool container(cbor_r *r, uint8_t want, size_t per, size_t *n) {
  uint8_t m;
  uint64_t v;
  if (!head(r, &m, &v)) return false;
  if (m != want) return fail(r, CBOR_ERR_TYPE);
  if (v > left(r) / per) return fail(r, CBOR_ERR_INVALID);
  *n = (size_t)v;
  return true;
}

bool cbr_map(cbor_r *r, size_t *pairs) { return container(r, CBOR_MAP, 2, pairs); }
bool cbr_array(cbor_r *r, size_t *items) { return container(r, CBOR_ARRAY, 1, items); }

static bool skip(cbor_r *r, int depth) {
  if (depth > CBOR_MAX_DEPTH) return fail(r, CBOR_ERR_INVALID);
  uint8_t m;
  uint64_t v;
  if (!head(r, &m, &v)) return false;
  switch (m) {
    case CBOR_BYTES:
    case CBOR_TEXT:
      if (v > left(r)) return fail(r, CBOR_ERR_INVALID);
      r->p += v;
      return true;
    case CBOR_MAP:
      if (v > left(r) / 2) return fail(r, CBOR_ERR_INVALID);
      v *= 2;
      // fall through
    case CBOR_ARRAY:
      if (v > left(r)) return fail(r, CBOR_ERR_INVALID);
      for (uint64_t i = 0; i < v; i++) {
        if (!skip(r, depth + 1)) return false;
      }
      return true;
    case CBOR_TAG:
      return skip(r, depth + 1);
    default:  // integers, simple values, floats: the head was all of it
      return true;
  }
}

bool cbr_skip(cbor_r *r) { return skip(r, 0); }

bool cbor_text_is(const char *s, size_t n, const char *z) {
  return n == strlen(z) && memcmp(s, z, n) == 0;
}
