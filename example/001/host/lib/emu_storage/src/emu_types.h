// Public types of the emu_storage library: results, device and object
// information. Nothing here describes how the bytes travel.
#pragma once

#include <stddef.h>
#include <stdint.h>

// Longest object name, not counting the terminating NUL.
#define EMU_STORAGE_NAME_MAX 31

enum class EmuResult : uint8_t {
  Ok = 0,
  NotInitialized,   // begin() has not succeeded
  NotConnected,     // the device stopped answering; call begin() again
  Timeout,          // no answer in time
  ProtocolError,    // the device answered, but not with what was asked
  CrcError,         // answers kept arriving damaged
  InvalidArgument,  // bad path, size or buffer
  NotFound,
  AlreadyExists,
  ReadOnly,
  NoSpace,
  Unsupported,  // the device does not support this (or this protocol version)
  IoError,
};

// Short English name of a result, for logs and the UI.
const char *emuResultName(EmuResult r);

// Capability bits reported by the device.
enum EmuCapability : uint32_t {
  EMU_CAP_READ_BIT = 1u << 0,
  EMU_CAP_WRITE_BIT = 1u << 1,
  EMU_CAP_REMOVE_BIT = 1u << 2,
  EMU_CAP_TRUNCATE_BIT = 1u << 3,
  EMU_CAP_STREAM_BIT = 1u << 4,  // reserved
  EMU_CAP_APPEND_BIT = 1u << 5,
};

struct EmuDeviceInfo {
  uint16_t protocolVersion;
  uint32_t capabilities;  // EmuCapability bits
  uint32_t maxObjectSize;
  uint32_t availableBytes;
  uint16_t maxChunk;  // largest read/write piece per request (the library splits for you)
  char deviceName[32];
  char firmwareVersion[16];
};

struct EmuFileInfo {
  uint32_t size;
  bool isDirectory;
  bool readOnly;
};

struct EmuDirectoryEntry {
  char name[EMU_STORAGE_NAME_MAX + 1];
  uint32_t size;
  bool isDirectory;  // always false for now: there are no directories
  bool readOnly;
};

// Link counters, for diagnostics.
struct EmuLinkStats {
  uint32_t requests;    // requests sent (resends not counted)
  uint32_t resends;     // requests sent again after a timeout or a damaged request
  uint32_t crcErrors;   // responses that arrived damaged (read again)
  uint32_t timeouts;    // requests that got no answer at all
  uint32_t replays;     // answers the device repeated for a resent request
};
