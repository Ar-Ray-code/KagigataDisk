#include "emu_protocol.h"

#include <string.h>

// Time, the only thing this file needs from the platform. (Also builds
// without Arduino, for the protocol unit tests on the PC.)
#if defined(ARDUINO)
#include <Arduino.h>
static uint32_t nowMs() { return millis(); }
static void pauseUs(uint32_t us) { delayMicroseconds(us); }
static void pauseMs(uint32_t ms) { delay(ms); }
#else
#include <chrono>
#include <thread>
static uint32_t nowMs() {
  using namespace std::chrono;
  return (uint32_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
static void pauseUs(uint32_t us) { std::this_thread::sleep_for(std::chrono::microseconds(us)); }
static void pauseMs(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif

size_t emuEncodeFrame(uint8_t *out, uint8_t command, uint8_t sequence, uint8_t flags,
                      const uint8_t *payload, size_t length) {
  if (length > EMU_MAX_PAYLOAD) return 0;
  out[0] = EMU_MAGIC0;
  out[1] = EMU_MAGIC1;
  out[2] = EMU_PROTOCOL_VERSION;
  out[3] = command;
  out[4] = sequence;
  out[5] = flags;
  emu_put_u16(&out[6], (uint16_t)length);
  if (length) memcpy(&out[EMU_HEADER_SIZE], payload, length);
  emu_put_u16(&out[EMU_HEADER_SIZE + length], emu_crc16(out, EMU_HEADER_SIZE + length));
  return EMU_HEADER_SIZE + length + EMU_CRC_SIZE;
}

EmuDecode emuDecodeFrame(const uint8_t *in, size_t length, EmuFrame &frame) {
  if (length < EMU_HEADER_SIZE + EMU_CRC_SIZE) return EmuDecode::Short;
  if (in[0] != EMU_MAGIC0 || in[1] != EMU_MAGIC1) return EmuDecode::BadMagic;
  uint16_t n = emu_get_u16(&in[6]);
  if (n > EMU_MAX_PAYLOAD) return EmuDecode::BadLength;
  if (length < EMU_HEADER_SIZE + n + EMU_CRC_SIZE) return EmuDecode::Short;
  if (length > EMU_HEADER_SIZE + n + EMU_CRC_SIZE) return EmuDecode::BadLength;
  if (emu_get_u16(&in[EMU_HEADER_SIZE + n]) != emu_crc16(in, EMU_HEADER_SIZE + n)) {
    return EmuDecode::BadCrc;
  }
  frame.version = in[2];
  frame.command = in[3];
  frame.sequence = in[4];
  frame.flags = in[5];
  frame.payload = &in[EMU_HEADER_SIZE];
  frame.length = n;
  return EmuDecode::Ok;
}

EmuResult emuResultFromStatus(uint8_t status) {
  switch (status) {
    case EMU_ST_OK: return EmuResult::Ok;
    case EMU_ST_INVALID_ARGUMENT: return EmuResult::InvalidArgument;
    case EMU_ST_NOT_FOUND: return EmuResult::NotFound;
    case EMU_ST_ALREADY_EXISTS: return EmuResult::AlreadyExists;
    case EMU_ST_READ_ONLY: return EmuResult::ReadOnly;
    case EMU_ST_NO_SPACE: return EmuResult::NoSpace;
    case EMU_ST_UNSUPPORTED: return EmuResult::Unsupported;
    case EMU_ST_IO_ERROR: return EmuResult::IoError;
    default: return EmuResult::ProtocolError;
  }
}

EmuResult EmuProtocol::sendRequest(size_t frameLen) {
  if (transport_->select() != EmuTransportResult::Ok) return EmuResult::NotInitialized;
  transport_->write(req_, frameLen);
  transport_->deselect();
  return EmuResult::Ok;
}

// Bytes read per poll window while looking for the response's magic.
static constexpr size_t POLL_WINDOW = 16;

EmuResult EmuProtocol::awaitResponse(uint8_t command, uint8_t sequence, uint32_t deadlineMs,
                                     size_t &respLen) {
  uint8_t win[POLL_WINDOW];
  uint32_t polls = 0;
  bool damaged = false;  // a response came, but damaged: worth reading again
  for (;;) {
    transport_->select();
    transport_->read(win, sizeof(win));
    size_t k = 0;
    while (k < sizeof(win) && win[k] != EMU_MAGIC0) k++;
#if EMU_STORAGE_DEBUG
    if (polls == 0) {
      EMU_LOG("first poll:");
      for (size_t i = 0; i < sizeof(win); i++) Serial.printf(" %02x", win[i]);
      Serial.printf("\n");
    }
#endif
    if (k < sizeof(win)) {
      // A response is streaming: collect it within this same window (the
      // device starts over from its first byte in the next one).
      size_t have = sizeof(win) - k;
      memcpy(resp_, &win[k], have);
      if (have < EMU_HEADER_SIZE) {
        transport_->read(&resp_[have], EMU_HEADER_SIZE - have);
        have = EMU_HEADER_SIZE;
      }
      uint16_t n = emu_get_u16(&resp_[6]);
      bool plausible = resp_[1] == EMU_MAGIC1 && n <= EMU_MAX_PAYLOAD;
      size_t total = EMU_HEADER_SIZE + (size_t)n + EMU_CRC_SIZE;
      if (plausible && have < total) {
        transport_->read(&resp_[have], total - have);
        have = total;
      }
      transport_->deselect();
      if (plausible) {
        EmuFrame f;
        EmuDecode d = emuDecodeFrame(resp_, total, f);
        if (d == EmuDecode::Ok) {
          if (f.command == (command | EMU_RESPONSE_BIT) && f.sequence == sequence) {
            if (f.flags & EMU_FLAG_REPLAY) stats_.replays++;
            respLen = total;
            return EmuResult::Ok;
          }
          // Someone else's (an earlier request's): keep waiting for ours.
        } else {
          stats_.crcErrors++;
          damaged = true;
          EMU_LOG("damaged response (%d)\n", (int)d);
        }
      } else {
        stats_.crcErrors++;
        damaged = true;
      }
    } else {
      transport_->deselect();
    }
    if ((int32_t)(nowMs() - deadlineMs) >= 0) return damaged ? EmuResult::CrcError : EmuResult::Timeout;
    // Give the device a moment; after the first few polls, give the CPU away.
    if (++polls < 20) {
      pauseUs(50);
    } else {
      pauseMs(1);
    }
  }
}

EmuResult EmuProtocol::exchange(uint8_t command, const uint8_t *payload, size_t length,
                                uint8_t &status, const uint8_t *&data, size_t &dataLen,
                                uint32_t timeoutMs, uint8_t resends) {
  if (!transport_) return EmuResult::NotInitialized;
  uint8_t seq = ++seq_;
  size_t frameLen = emuEncodeFrame(req_, command, seq, 0, payload, length);
  if (!frameLen) return EmuResult::InvalidArgument;
  stats_.requests++;
  EmuResult last = EmuResult::Timeout;
  for (uint8_t attempt = 0; attempt <= resends; attempt++) {
    if (attempt) stats_.resends++;
    EmuResult r = sendRequest(frameLen);
    if (r != EmuResult::Ok) return r;
    size_t respLen = 0;
    r = awaitResponse(command, seq, nowMs() + timeoutMs, respLen);
    if (r == EmuResult::Ok) {
      EmuFrame f;
      emuDecodeFrame(resp_, respLen, f);
      if (f.length < 1) return EmuResult::ProtocolError;
      status = f.payload[0];
      if (status == EMU_ST_BAD_FRAME) {
        last = EmuResult::CrcError;  // our request arrived damaged: send it again
        EMU_LOG("request %u arrived damaged\n", seq);
        continue;
      }
      data = f.payload + 1;
      dataLen = f.length - 1u;
      return EmuResult::Ok;
    }
    if (r == EmuResult::Timeout) stats_.timeouts++;
    last = r;
    EMU_LOG("request %u: %s\n", seq, emuResultName(r));
  }
  return last;
}
