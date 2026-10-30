// Minimal CBOR (RFC 8949) for CTAP2: a bounded writer and a cursor reader.
//
// The writer emits exactly what it is told; callers write map entries in
// CTAP2 canonical order (integer keys ascending, text keys shorter first and
// then bytewise). The reader accepts definite lengths only (CTAP2 canonical
// CBOR has no indefinite ones) and never reads past `end`.
#ifndef CBOR_H
#define CBOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Major types.
enum { CBOR_UINT, CBOR_NINT, CBOR_BYTES, CBOR_TEXT, CBOR_ARRAY, CBOR_MAP, CBOR_TAG, CBOR_SIMPLE };

// Reader status codes; they are the CTAP2 status codes for the same failures.
#define CBOR_ERR_TYPE 0x11     // CTAP2_ERR_CBOR_UNEXPECTED_TYPE
#define CBOR_ERR_INVALID 0x12  // CTAP2_ERR_INVALID_CBOR

typedef struct {
  uint8_t *buf;
  size_t cap, len;
  bool err;  // ran out of room; everything after is dropped
} cbor_w;

void cbw_init(cbor_w *w, uint8_t *buf, size_t cap);
void cbw_uint(cbor_w *w, uint64_t v);
void cbw_int(cbor_w *w, int64_t v);
void cbw_bytes(cbor_w *w, const void *p, size_t n);
void cbw_text(cbor_w *w, const char *s);
void cbw_textn(cbor_w *w, const char *s, size_t n);
void cbw_bool(cbor_w *w, bool b);
void cbw_map(cbor_w *w, size_t pairs);
void cbw_array(cbor_w *w, size_t items);

typedef struct {
  const uint8_t *p, *end;
  uint8_t status;  // 0, or the first CBOR_ERR_* hit; every later call fails
} cbor_r;

void cbr_init(cbor_r *r, const uint8_t *p, size_t n);
// Major type of the next item, or 0xFF at the end.
uint8_t cbr_peek(const cbor_r *r);
bool cbr_uint(cbor_r *r, uint64_t *v);
bool cbr_int(cbor_r *r, int64_t *v);
bool cbr_bytes(cbor_r *r, const uint8_t **p, size_t *n);
bool cbr_text(cbor_r *r, const char **s, size_t *n);
bool cbr_bool(cbor_r *r, bool *b);
bool cbr_map(cbor_r *r, size_t *pairs);
bool cbr_array(cbor_r *r, size_t *items);
// Skips one complete item (nested up to 8 levels).
bool cbr_skip(cbor_r *r);

// True if the text (s, n) equals the C string z.
bool cbor_text_is(const char *s, size_t n, const char *z);

#endif  // CBOR_H
