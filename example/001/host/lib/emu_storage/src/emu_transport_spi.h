// SPI transport for the M5Stack Core2 (ESP32, Arduino): SPI transactions and
// the device's chip select. The bus may be shared with other devices (the
// Core2's LCD is on it), so every transaction takes the bus with
// SPI.beginTransaction() and gives it back.
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "emu_transport.h"

// Default SPI clock. The link is specified up to (not including) 16 MHz.
#ifndef EMU_SPI_DEFAULT_HZ
#define EMU_SPI_DEFAULT_HZ 8000000u
#endif

class EmuSpiTransport : public EmuTransport {
 public:
  // Pins < 0: leave the bus as it is (already begun by someone else).
  bool begin(SPIClass &spi, int sckPin, int misoPin, int mosiPin, int csPin,
             uint32_t frequency = EMU_SPI_DEFAULT_HZ);
  void end();
  void setFrequency(uint32_t frequency) { settings_ = SPISettings(frequency, MSBFIRST, SPI_MODE0); hz_ = frequency; }
  uint32_t frequency() const { return hz_; }

  EmuTransportResult select() override;
  EmuTransportResult deselect() override;
  EmuTransportResult transfer(const uint8_t *tx, uint8_t *rx, size_t length) override;

 private:
  SPIClass *spi_ = nullptr;
  int cs_ = -1;
  uint32_t hz_ = EMU_SPI_DEFAULT_HZ;
  SPISettings settings_{EMU_SPI_DEFAULT_HZ, MSBFIRST, SPI_MODE0};
  bool selected_ = false;
};
