#include "emu_server.h"

#include <stdbool.h>
#include <string.h>

#include "emu_link.h"
#include "emu_protocol_defs.h"
#include "objstore.h"

#define CAPS (EMU_CAP_READ | EMU_CAP_WRITE | EMU_CAP_REMOVE | EMU_CAP_TRUNCATE | EMU_CAP_APPEND)
#define DEVICE_NAME "RP2350-KagigataDisk"
#ifndef KAGIGATA_FW_VERSION
#define KAGIGATA_FW_VERSION "dev"
#endif

static bool have_last;
static uint8_t last_cmd, last_seq;
static uint16_t last_crc;
// The last response to a request that was carried out, for answering a resend.
static uint8_t last_resp[EMU_FRAME_MAX];
static uint32_t last_resp_len;

void emu_server_init(void) {
  have_last = false;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
typedef struct {
  const uint8_t *p;
  uint32_t left;
  bool ok;
} rd_t;

static uint32_t rd_u8(rd_t *r) {
  if (r->left < 1) return r->ok = false, 0u;
  r->left--;
  return *r->p++;
}

static uint32_t rd_u16(rd_t *r) {
  if (r->left < 2) return r->ok = false, 0u;
  uint32_t v = emu_get_u16(r->p);
  r->p += 2;
  r->left -= 2;
  return v;
}

static uint32_t rd_u32(rd_t *r) {
  if (r->left < 4) return r->ok = false, 0u;
  uint32_t v = emu_get_u32(r->p);
  r->p += 4;
  r->left -= 4;
  return v;
}

// A path: u8 length + bytes, with at most one leading '/'. Out: the bare
// name, NUL-terminated.
static bool rd_path(rd_t *r, char name[EMU_NAME_MAX + 1]) {
  uint32_t n = rd_u8(r);
  if (!r->ok || n > r->left || n > EMU_NAME_MAX + 1u) return r->ok = false;
  const uint8_t *s = r->p;
  r->p += n;
  r->left -= n;
  if (n && s[0] == '/') {
    s++;
    n--;
  }
  if (n > EMU_NAME_MAX) return r->ok = false;
  memcpy(name, s, n);
  name[n] = '\0';
  if (memchr(name, '\0', n) != NULL) return r->ok = false;
  return true;
}

// ---------------------------------------------------------------------------
// Encoding: the response is built in place in the link's buffer.
// ---------------------------------------------------------------------------
static uint8_t *out;
static uint32_t out_n;  // payload bytes so far

static void put_u8(uint32_t v) {
  if (out_n < EMU_MAX_PAYLOAD) out[EMU_HEADER_SIZE + out_n++] = (uint8_t)v;
}
static void put_u16(uint32_t v) {
  put_u8(v);
  put_u8(v >> 8);
}
static void put_u32(uint32_t v) {
  put_u16(v);
  put_u16(v >> 16);
}
static void put_str(const char *s) {
  uint32_t n = (uint32_t)strlen(s);
  put_u8(n);
  for (uint32_t i = 0; i < n; i++) put_u8((uint8_t)s[i]);
}

static uint32_t finish(uint8_t cmd, uint8_t seq, uint8_t flags) {
  out[0] = EMU_MAGIC0;
  out[1] = EMU_MAGIC1;
  out[2] = EMU_PROTOCOL_VERSION;
  out[3] = (uint8_t)(cmd | EMU_RESPONSE_BIT);
  out[4] = seq;
  out[5] = flags;
  emu_put_u16(&out[6], (uint16_t)out_n);
  uint16_t crc = emu_crc16(out, EMU_HEADER_SIZE + out_n);
  emu_put_u16(&out[EMU_HEADER_SIZE + out_n], crc);
  return EMU_HEADER_SIZE + out_n + EMU_CRC_SIZE;
}

// ---------------------------------------------------------------------------
// Commands. Each writes its status byte and results.
// ---------------------------------------------------------------------------
static void status(emu_status_t st) { put_u8(st); }

static void do_hello(rd_t *r) {
  (void)rd_u16(r);  // the host's version: it compares, the header already matched ours
  status(EMU_ST_OK);
  put_u16(EMU_PROTOCOL_VERSION);
  put_u32(CAPS);
  put_u16(EMU_MAX_DATA);
}

static void do_get_info(void) {
  status(EMU_ST_OK);
  put_u16(EMU_PROTOCOL_VERSION);
  put_u32(CAPS);
  put_u32(OBJ_MAX_BYTES);
  put_u32(objstore_available());
  put_str(DEVICE_NAME);
  put_str(KAGIGATA_FW_VERSION);
}

static void do_list(rd_t *r) {
  uint32_t first = rd_u16(r);
  if (!r->ok) return status(EMU_ST_INVALID_ARGUMENT);
  uint32_t total = objstore_count();
  status(EMU_ST_OK);
  put_u16(total);
  uint32_t count_at = out_n;
  put_u8(0);
  uint32_t count = 0;
  obj_info_t e;
  for (uint32_t i = first; i < total && objstore_entry(i, &e); i++) {
    uint32_t need = 4u + 1u + 1u + (uint32_t)strlen(e.name);
    if (out_n + need > EMU_MAX_PAYLOAD) break;
    put_u32(e.size);
    put_u8(e.attr);
    put_str(e.name);
    count++;
  }
  out[EMU_HEADER_SIZE + count_at] = (uint8_t)count;
}

static void do_stat(rd_t *r) {
  char name[EMU_NAME_MAX + 1];
  if (!rd_path(r, name)) return status(EMU_ST_INVALID_ARGUMENT);
  obj_info_t e;
  emu_status_t st = objstore_stat(name, &e);
  status(st);
  if (st != EMU_ST_OK) return;
  put_u32(e.size);
  put_u8(e.attr);
}

static void do_read(rd_t *r) {
  char name[EMU_NAME_MAX + 1];
  uint32_t off = rd_u32(r), len = rd_u16(r);
  if (!r->ok || !rd_path(r, name) || len > EMU_MAX_DATA) return status(EMU_ST_INVALID_ARGUMENT);
  static uint8_t buf[EMU_MAX_DATA];
  uint32_t got = 0;
  emu_status_t st = objstore_read(name, off, buf, len, &got);
  status(st);
  for (uint32_t i = 0; st == EMU_ST_OK && i < got; i++) put_u8(buf[i]);
}

static void do_write(rd_t *r, bool append) {
  char name[EMU_NAME_MAX + 1];
  uint32_t off = 0, flags = 0;
  if (!append) {
    off = rd_u32(r);
    flags = rd_u8(r);
  }
  if (!r->ok || !rd_path(r, name) || r->left > EMU_MAX_DATA) return status(EMU_ST_INVALID_ARGUMENT);
  uint32_t size = 0;
  emu_status_t st = append ? objstore_append(name, r->p, r->left, &size)
                           : objstore_write(name, off, r->p, r->left,
                                            (flags & EMU_WRITE_CREATE) != 0, &size);
  status(st);
  if (st == EMU_ST_OK) put_u32(size);
}

static void do_truncate(rd_t *r) {
  char name[EMU_NAME_MAX + 1];
  uint32_t size = rd_u32(r);
  if (!r->ok || !rd_path(r, name)) return status(EMU_ST_INVALID_ARGUMENT);
  status(objstore_truncate(name, size));
}

static void do_remove(rd_t *r) {
  char name[EMU_NAME_MAX + 1];
  if (!rd_path(r, name)) return status(EMU_ST_INVALID_ARGUMENT);
  status(objstore_remove(name));
}

static void do_sync(void) {
  objstore_flush();
  status(objstore_flash_full() ? EMU_ST_NO_SPACE : EMU_ST_OK);
}

// ---------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------
static uint32_t serve(const uint8_t *f, uint32_t n, bool truncated) {
  out = emu_link_response_buf();
  out_n = 0;
  uint8_t cmd = n > 3 ? f[3] : 0, seq = n > 4 ? f[4] : 0;
  uint32_t len = n >= EMU_HEADER_SIZE ? emu_get_u16(&f[6]) : 0;

  if (truncated || n < EMU_HEADER_SIZE + EMU_CRC_SIZE || f[1] != EMU_MAGIC1 ||
      n != EMU_HEADER_SIZE + len + EMU_CRC_SIZE ||
      emu_get_u16(&f[EMU_HEADER_SIZE + len]) != emu_crc16(f, EMU_HEADER_SIZE + len)) {
    put_u8(EMU_ST_BAD_FRAME);
    return finish(cmd, seq, 0);
  }
  uint16_t crc = emu_get_u16(&f[EMU_HEADER_SIZE + len]);
  if (have_last && cmd != EMU_CMD_HELLO && cmd == last_cmd && seq == last_seq && crc == last_crc) {
    memcpy(out, last_resp, last_resp_len);
    uint32_t plen = emu_get_u16(&out[6]);
    out[5] |= EMU_FLAG_REPLAY;
    emu_put_u16(&out[EMU_HEADER_SIZE + plen], emu_crc16(out, EMU_HEADER_SIZE + plen));
    return last_resp_len;
  }
  have_last = true;
  last_cmd = cmd;
  last_seq = seq;
  last_crc = crc;

  rd_t r = {.p = f + EMU_HEADER_SIZE, .left = len, .ok = true};
  if (f[2] != EMU_PROTOCOL_VERSION) {
    status(EMU_ST_UNSUPPORTED);
    put_u16(EMU_PROTOCOL_VERSION);  // so the host can say what it found
  } else switch (cmd) {
    case EMU_CMD_HELLO: do_hello(&r); break;
    case EMU_CMD_GET_INFO: do_get_info(); break;
    case EMU_CMD_PING: status(EMU_ST_OK); break;
    case EMU_CMD_LIST: do_list(&r); break;
    case EMU_CMD_STAT: do_stat(&r); break;
    case EMU_CMD_READ: do_read(&r); break;
    case EMU_CMD_WRITE: do_write(&r, false); break;
    case EMU_CMD_APPEND: do_write(&r, true); break;
    case EMU_CMD_TRUNCATE: do_truncate(&r); break;
    case EMU_CMD_REMOVE: do_remove(&r); break;
    case EMU_CMD_SYNC: do_sync(); break;
    default: status(EMU_ST_UNSUPPORTED); break;
  }
  last_resp_len = finish(cmd, seq, 0);
  memcpy(last_resp, out, last_resp_len);
  return last_resp_len;
}

void emu_server_task(void) {
  uint32_t n;
  bool truncated;
  const uint8_t *f = emu_link_request(&n, &truncated);
  if (!f) return;
  emu_link_reply(serve(f, n, truncated));
}
