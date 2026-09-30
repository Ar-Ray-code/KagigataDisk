// Kagigata SPI protocol: the wire definitions shared by the KagigataDisk
// firmware (example/001/kagigatadisk, pico-sdk) and the host library
// (example/001/host/lib/emu_storage, PlatformIO). Both builds include this one
// file, so opcodes, status codes and frame layout cannot drift apart.
//
// Plain C, no structs on the wire: every field is encoded byte by byte,
// little-endian.
//
// Frame (request and response alike):
//
//   offset  size  field
//   0       2     magic 'K' 'G'
//   2       1     protocol version (EMU_PROTOCOL_VERSION)
//   3       1     command; a response carries the request's | EMU_RESPONSE_BIT
//   4       1     sequence number, echoed by the response
//   5       1     flags
//   6       2     payload length, little-endian
//   8       n     payload
//   8+n     2     frame integrity CRC16 over bytes 0 .. 8+n-1, little-endian
//
// One exchange is two chip-select windows: the host selects the device and
// clocks a request frame in, deselects, then selects it again and clocks
// filler bytes (EMU_FILL) out until the response frame's magic appears. While
// the device is still working it sends EMU_FILL. The device answers every
// window with the same response, from its first byte, until the next request
// arrives, so a response damaged on the wire is simply read again.
#ifndef EMU_PROTOCOL_DEFS_H
#define EMU_PROTOCOL_DEFS_H

#include <stdint.h>

#define EMU_PROTOCOL_VERSION 1u

#define EMU_MAGIC0 0x4Bu  // 'K'
#define EMU_MAGIC1 0x47u  // 'G'

// What each side clocks out while it has nothing to say: the host while it
// polls, the device while no response is ready.
#define EMU_FILL 0x00u

#define EMU_HEADER_SIZE 8u
#define EMU_CRC_SIZE 2u
// Largest payload either side sends. Read and write data are split into
// chunks of at most EMU_MAX_DATA bytes by the host library.
#define EMU_MAX_PAYLOAD 320u
#define EMU_MAX_DATA 256u
#define EMU_FRAME_MAX (EMU_HEADER_SIZE + EMU_MAX_PAYLOAD + EMU_CRC_SIZE)

// Object names: printable ASCII, no '/', at most EMU_NAME_MAX characters.
// A path on the wire is the name, optionally with one leading '/'.
#define EMU_NAME_MAX 31u

#define EMU_RESPONSE_BIT 0x80u

// Commands. Payloads (request -> response); every response payload starts
// with a status byte (emu_status_t), followed by the listed fields when the
// status is EMU_ST_OK. `path` is u8 length + that many bytes.
enum {
  EMU_CMD_HELLO = 0x01,     // u16 host version -> u16 device version, u32 caps, u16 max data
  EMU_CMD_GET_INFO = 0x02,  // - -> u16 version, u32 caps, u32 max object size,
                            //      u32 available bytes, u8 len + device name,
                            //      u8 len + firmware version
  EMU_CMD_PING = 0x03,      // - -> -

  EMU_CMD_LIST = 0x10,  // u16 first index -> u16 total, u8 count, count x
                        //   (u32 size, u8 attr, u8 len + name)
  EMU_CMD_STAT = 0x11,  // path -> u32 size, u8 attr

  EMU_CMD_READ = 0x20,      // u32 offset, u16 length, path -> data (may be short at EOF)
  EMU_CMD_WRITE = 0x21,     // u32 offset, u8 flags (EMU_WRITE_*), path, data -> u32 new size
  EMU_CMD_TRUNCATE = 0x22,  // u32 size, path -> -
  EMU_CMD_REMOVE = 0x23,    // path -> -
  EMU_CMD_APPEND = 0x24,    // path, data -> u32 new size

  EMU_CMD_SYNC = 0x30,  // - -> -   (everything written so far is in flash)
};

// Response status codes.
typedef enum {
  EMU_ST_OK = 0,
  EMU_ST_INVALID_ARGUMENT = 1,
  EMU_ST_NOT_FOUND = 2,
  EMU_ST_ALREADY_EXISTS = 3,
  EMU_ST_READ_ONLY = 4,
  EMU_ST_NO_SPACE = 5,
  EMU_ST_UNSUPPORTED = 6,  // unknown command or protocol version
  EMU_ST_IO_ERROR = 7,
  EMU_ST_BAD_FRAME = 8,  // request failed its CRC or length check: send it again
} emu_status_t;

// Capability bits (HELLO, GET_INFO).
#define EMU_CAP_READ (1u << 0)
#define EMU_CAP_WRITE (1u << 1)
#define EMU_CAP_REMOVE (1u << 2)
#define EMU_CAP_TRUNCATE (1u << 3)
#define EMU_CAP_STREAM (1u << 4)  // reserved: not implemented yet
#define EMU_CAP_APPEND (1u << 5)

// Object attributes (LIST, STAT).
#define EMU_ATTR_READ_ONLY (1u << 0)
#define EMU_ATTR_DIRECTORY (1u << 1)  // reserved: there are no directories yet

// WRITE flags.
#define EMU_WRITE_CREATE (1u << 0)  // create the object if it does not exist

// Response header flags.
#define EMU_FLAG_REPLAY (1u << 0)  // a repeated request: answered from the last response

// Frame integrity CRC16: CCITT polynomial 0x1021, initial value 0xFFFF,
// MSB first, no final XOR.
static inline uint16_t emu_crc16_update(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)b << 8;
  for (int i = 0; i < 8; i++) crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
  return crc;
}

static inline uint16_t emu_crc16(const uint8_t *p, uint32_t n) {
  uint16_t crc = 0xFFFFu;
  for (uint32_t i = 0; i < n; i++) crc = emu_crc16_update(crc, p[i]);
  return crc;
}

static inline void emu_put_u16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static inline void emu_put_u32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t emu_get_u16(const uint8_t *p) {
  return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t emu_get_u32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#endif  // EMU_PROTOCOL_DEFS_H
