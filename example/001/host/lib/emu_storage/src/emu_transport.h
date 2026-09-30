// The byte transport under the protocol: selecting the device and moving
// bytes. It knows nothing about frames, commands or objects.
#pragma once

#include <stddef.h>
#include <stdint.h>

enum class EmuTransportResult : uint8_t {
  Ok = 0,
  NotInitialized,
  InvalidArgument,
};

class EmuTransport {
 public:
  virtual ~EmuTransport() {}
  // One transaction: select() ... transfers ... deselect(). The transport
  // owns whatever exclusive access the bus needs meanwhile.
  virtual EmuTransportResult select() = 0;
  virtual EmuTransportResult deselect() = 0;
  // Full duplex. `tx` NULL: send the idle fill byte. `rx` NULL: discard.
  virtual EmuTransportResult transfer(const uint8_t *tx, uint8_t *rx, size_t length) = 0;
  EmuTransportResult write(const uint8_t *data, size_t length) { return transfer(data, nullptr, length); }
  EmuTransportResult read(uint8_t *data, size_t length) { return transfer(nullptr, data, length); }
};
