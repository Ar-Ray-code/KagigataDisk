// Kagigata protocol, host side: frame encoding and decoding, and the
// request/response exchange over an EmuTransport - polling, timeouts and
// resends. Wire constants come from protocol/emu_protocol_defs.h, shared with
// the firmware.
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "emu_protocol_defs.h"
#include "emu_transport.h"
#include "emu_types.h"

#ifndef EMU_STORAGE_DEBUG
#define EMU_STORAGE_DEBUG 0
#endif
#if EMU_STORAGE_DEBUG && defined(ARDUINO)
#include <Arduino.h>
#define EMU_LOG(...) Serial.printf("[emu] " __VA_ARGS__)
#else
#undef EMU_STORAGE_DEBUG
#define EMU_STORAGE_DEBUG 0
#define EMU_LOG(...) \
  do {               \
  } while (0)
#endif

// Time the device gets to answer one request. Flash housekeeping on the
// device can hold an answer back for a few hundred milliseconds.
constexpr uint32_t EMU_DEFAULT_TIMEOUT_MS = 1000;
constexpr uint32_t EMU_LONG_TIMEOUT_MS = 3000;  // SYNC, big writes
constexpr uint32_t EMU_HELLO_TIMEOUT_MS = 300;  // probing for the device
// Times a request is sent again after getting no answer, or after the device
// reports it arrived damaged. The device recognises a resend by its sequence
// number and answers it without carrying it out twice.
constexpr uint8_t EMU_DEFAULT_RESENDS = 2;

// ---- Frame codec (no I/O) ---------------------------------------------------
struct EmuFrame {
  uint8_t version;
  uint8_t command;
  uint8_t sequence;
  uint8_t flags;
  const uint8_t *payload;
  uint16_t length;
};

// Encodes a frame into `out` (EMU_FRAME_MAX bytes). Returns its length, or 0
// if the payload is too long.
size_t emuEncodeFrame(uint8_t *out, uint8_t command, uint8_t sequence, uint8_t flags,
                      const uint8_t *payload, size_t length);

enum class EmuDecode : uint8_t { Ok, Short, BadMagic, BadLength, BadCrc };
// Checks and decodes a complete frame. `frame.payload` points into `in`.
EmuDecode emuDecodeFrame(const uint8_t *in, size_t length, EmuFrame &frame);

// Maps a response status byte to the library's result.
EmuResult emuResultFromStatus(uint8_t status);

// ---- Exchange ---------------------------------------------------------------
class EmuProtocol {
 public:
  void attach(EmuTransport *transport) { transport_ = transport; }
  void reseed(uint8_t sequence) { seq_ = sequence; }

  // Sends `command` with `payload` and waits for its response. On Ok,
  // `status` is the response's status byte and `data`/`dataLen` the rest of
  // its payload (valid until the next call). Transport-level trouble is
  // retried here; the status is the caller's to interpret.
  EmuResult exchange(uint8_t command, const uint8_t *payload, size_t length, uint8_t &status,
                     const uint8_t *&data, size_t &dataLen,
                     uint32_t timeoutMs = EMU_DEFAULT_TIMEOUT_MS,
                     uint8_t resends = EMU_DEFAULT_RESENDS);

  const EmuLinkStats &stats() const { return stats_; }
  void clearStats() { stats_ = EmuLinkStats{}; }

 private:
  EmuResult sendRequest(size_t frameLen);
  EmuResult awaitResponse(uint8_t command, uint8_t sequence, uint32_t deadlineMs, size_t &respLen);

  EmuTransport *transport_ = nullptr;
  uint8_t seq_ = 0;
  uint8_t req_[EMU_FRAME_MAX];
  uint8_t resp_[EMU_FRAME_MAX];
  EmuLinkStats stats_{};
};
