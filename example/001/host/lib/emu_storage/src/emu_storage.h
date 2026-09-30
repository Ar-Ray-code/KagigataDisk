// emu_storage: the host-side object/file interface to a KagigataDisk.
//
// The device holds named objects (files) and this library reads and writes them by path over the Kagigata
// SPI protocol. Paths are object names with an optional leading '/', e.g.
// "/log.txt"; there are no directories.
//
//   EmuStorage storage;
//   if (storage.begin()) storage.appendText("/log.txt", "hello\n");
//
// Every call is synchronous and bounded by a timeout; none waits forever,
// whether or not a device is attached.
#pragma once

#include <Arduino.h>
#include <SPI.h>

#include "emu_types.h"

struct EmuStorageConfig {
  SPIClass *spi = &SPI;
  // M5Stack Core2: the TF-card-shaped slot's SPI bus, shared with the LCD.
  int sckPin = 18;
  int misoPin = 38;
  int mosiPin = 23;
  int csPin = 4;
  uint32_t frequency = 8000000u;  // < 16 MHz
};

class EmuStorage {
 public:
  EmuStorage();
  ~EmuStorage();

  // Takes the bus, greets the device and checks its protocol version.
  bool begin();
  bool begin(const EmuStorageConfig &config);
  void end();
  // Why the last begin() failed (Ok after a successful one).
  EmuResult beginResult() const { return beginResult_; }

  // Asks the device (a ping); false if it does not answer.
  bool isConnected();

  EmuResult getInfo(EmuDeviceInfo &info);

  // All objects, up to `capacity` of them.
  EmuResult list(EmuDirectoryEntry *entries, size_t capacity, size_t &count);
  EmuResult stat(const char *path, EmuFileInfo &info);

  // Reads up to `length` bytes at `offset`; fewer at the end of the object.
  EmuResult read(const char *path, uint32_t offset, void *buffer, size_t length,
                 size_t &bytesRead);
  // Writes `length` bytes at `offset`, creating the object if needed. A gap
  // past the current end reads as zeros.
  EmuResult write(const char *path, uint32_t offset, const void *data, size_t length,
                  size_t &bytesWritten);
  // Adds to the end, creating the object if needed.
  EmuResult append(const char *path, const void *data, size_t length, size_t &bytesWritten);
  EmuResult truncate(const char *path, uint32_t size);
  EmuResult remove(const char *path);
  // Returns once everything written so far is in the device's flash (it
  // otherwise saves within about 5 seconds by itself).
  EmuResult sync();

  // Text conveniences on top of the primitives above.
  EmuResult readText(const char *path, char *buffer, size_t capacity, size_t &length);  // NUL-terminated
  EmuResult writeText(const char *path, const char *text);  // replaces the content
  EmuResult appendText(const char *path, const char *text);

  // SPI clock for the following requests (< 16 MHz).
  void setFrequency(uint32_t hz);
  EmuLinkStats linkStats() const;
  void clearLinkStats();

 private:
  struct Impl;
  Impl *impl_;
  EmuResult beginResult_ = EmuResult::NotInitialized;
  bool started_ = false;
  bool connected_ = false;
  uint16_t chunk_ = 0;

  EmuResult call(uint8_t command, const uint8_t *payload, size_t length, const uint8_t *&data,
                 size_t &dataLen, uint32_t timeoutMs);
};
