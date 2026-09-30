#include "emu_transport_spi.h"

#include "emu_protocol_defs.h"

bool EmuSpiTransport::begin(SPIClass &spi, int sckPin, int misoPin, int mosiPin, int csPin,
                            uint32_t frequency) {
  if (csPin < 0) return false;
  spi_ = &spi;
  cs_ = csPin;
  setFrequency(frequency);
  pinMode(cs_, OUTPUT);
  digitalWrite(cs_, HIGH);  // idle: deselected
  if (sckPin >= 0) spi_->begin(sckPin, misoPin, mosiPin, -1);
  selected_ = false;
  return true;
}

void EmuSpiTransport::end() {
  if (selected_) deselect();
  if (cs_ >= 0) digitalWrite(cs_, HIGH);
  spi_ = nullptr;
}

EmuTransportResult EmuSpiTransport::select() {
  if (!spi_) return EmuTransportResult::NotInitialized;
  if (selected_) return EmuTransportResult::Ok;
  spi_->beginTransaction(settings_);
  digitalWrite(cs_, LOW);
  selected_ = true;
  return EmuTransportResult::Ok;
}

EmuTransportResult EmuSpiTransport::deselect() {
  if (!spi_) return EmuTransportResult::NotInitialized;
  if (!selected_) return EmuTransportResult::Ok;
  digitalWrite(cs_, HIGH);
  spi_->endTransaction();
  selected_ = false;
  return EmuTransportResult::Ok;
}

EmuTransportResult EmuSpiTransport::transfer(const uint8_t *tx, uint8_t *rx, size_t length) {
  if (!spi_ || !selected_) return EmuTransportResult::NotInitialized;
  static uint8_t fill[64];  // EMU_FILL (0) bytes to send while only reading
  static_assert(EMU_FILL == 0, "fill[] is zero-initialised");
  static uint8_t sink[64];
  while (length) {
    size_t n = length < sizeof(fill) ? length : sizeof(fill);
    spi_->transferBytes(tx ? tx : fill, rx ? rx : sink, n);
    if (tx) tx += n;
    if (rx) rx += n;
    length -= n;
  }
  return EmuTransportResult::Ok;
}
