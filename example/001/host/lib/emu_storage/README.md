# emu_storage

emu_storage provides a host-side object/file interface for KagigataDisk.
KagigataDisk sits in a TF-card-shaped slot, used as a four-wire SPI
connector: what travels over it is the Kagigata protocol, and what the device
holds are named objects (files).

## Purpose

Give an application on the SPI host (an M5Stack Core2 here) one place to talk
to KagigataDisk: list, read, write, append, truncate and remove objects by
path, and ask the device about itself. The application never touches the SPI
bus, the chip select, frames or CRCs.

## Architecture

```
application (src/main.cpp)
   |  EmuStorage: paths, buffers, EmuResult          emu_storage.h / .cpp
   v
protocol: frames, CRC, sequence numbers,           emu_protocol.h / .cpp
          polling, timeouts, resends
   |  EmuTransport: select / transfer / deselect     emu_transport.h
   v
SPI transport: SPI transactions, chip select       emu_transport_spi.h / .cpp
   |
   v
KagigataDisk firmware (protocol server, object store)
```

The wire definitions (opcodes, status codes, frame layout, CRC) are defined
once, in `protocol/emu_protocol_defs.h` at the repository root, which the
firmware includes. `src/emu_protocol_defs.h` is a copy of it (so the library
also builds on its own), made by `tools/sync_protocol.sh`; the host build
(`check_protocol.py`) and the firmware build both stop while the copy differs,
so the two sides cannot drift apart. Edit the original, then run the script.

## Public API

Everything an application needs is in `emu_storage.h` (types in
`emu_types.h`):

| call | what it does |
|---|---|
| `begin()` / `begin(EmuStorageConfig)` | take the bus, greet the device (HELLO), check the protocol version, read its info; `beginResult()` says why it failed |
| `end()` | release the chip select |
| `isConnected()` | ping the device |
| `getInfo(EmuDeviceInfo&)` | protocol version, capabilities, largest object, free bytes, device name, firmware version |
| `list(entries, capacity, count)` | all objects (name, size, read-only) |
| `stat(path, EmuFileInfo&)` | one object's size and attributes |
| `read(path, offset, buffer, length, bytesRead)` | read; short at the end of the object |
| `write(path, offset, data, length, bytesWritten)` | write, creating the object if needed; a gap past the end reads as zeros |
| `append(path, data, length, bytesWritten)` | add to the end, creating the object if needed |
| `truncate(path, size)` | shrink or grow (zero-filled) |
| `remove(path)` | delete |
| `sync()` | return once everything written is in the device's flash |
| `readText` / `writeText` / `appendText` | text conveniences on top of the above |
| `setFrequency(hz)`, `linkStats()`, `clearLinkStats()` | diagnostics |

`EmuStorageConfig` defaults to the Core2's slot pins (SCK 18, MISO 38,
MOSI 23, CS 4) and 8 MHz. Buffers are the caller's; the library allocates
nothing per call. Read and write data are split into chunks of at most 256
bytes internally; the application never sees the chunking.

Paths are object names with an optional leading `/`: at most 31 printable
ASCII characters, no `/` inside, no `\ : * ? " < > |`. There are no
directories (`isDirectory` is always false). Names are compared without
regard to case.

## Transport

`EmuSpiTransport` wraps every transaction in `SPI.beginTransaction()` / CS low
... CS high / `SPI.endTransaction()`, so the bus can be shared (on the Core2
the LCD is on it; KagigataDisk releases MISO while deselected). SPI mode 0,
MSB first. The default clock is 8 MHz (`EMU_SPI_DEFAULT_HZ`); the link is
specified below 16 MHz. Measured on a Core2: error-free at 4, 8, 11.43 and
13.33 MHz (the ESP32 cannot make 15 MHz; 13.33 MHz is its fastest clock below
16 MHz). Nothing else in the application may drive the KagigataDisk chip
select.

## Protocol version

`EMU_PROTOCOL_VERSION` is 1. `begin()` sends HELLO carrying the host's version;
the device answers with its own, and a mismatch makes `begin()` fail with
`EmuResult::Unsupported`.

## Supported operations

HELLO, GET_INFO, PING, LIST, STAT, READ, WRITE (with create), APPEND,
TRUNCATE, REMOVE, SYNC. The device reports them as capability bits.

## Unsupported operations

- directories
- streams (`EMU_CAP_STREAM` is reserved for them)
- raw flash, sector or block access of any kind
- writing the device's own read-only objects (`README.md`, `device_info.txt`)
  and the files a PC placed on the device's USB drive (they are read-only to
  the SPI host)

## Example

```cpp
#include <M5Unified.h>
#include <emu_storage.h>

EmuStorage storage;

void setup() {
  M5.begin();
  if (!storage.begin()) {
    Serial.printf("KagigataDisk: %s\n", emuResultName(storage.beginResult()));
    return;
  }
  storage.appendText("/log.txt", "boot\n");

  EmuDirectoryEntry entries[16];
  size_t n = 0;
  if (storage.list(entries, 16, n) == EmuResult::Ok) {
    for (size_t i = 0; i < n; i++) Serial.printf("%s %lu\n", entries[i].name, (unsigned long)entries[i].size);
  }

  char text[256];
  size_t len;
  storage.readText("/device_info.txt", text, sizeof(text), len);
}
```

`examples/basic/basic.ino` is the same as a sketch.

## Error handling

Every call returns an `EmuResult`; `emuResultName()` gives a short English
name for logs or the UI.

| result | meaning | what to do |
|---|---|---|
| `Ok` | done | |
| `NotFound`, `AlreadyExists`, `ReadOnly`, `NoSpace`, `InvalidArgument` | the device (or the library, for a bad path) refused the request | fix the request |
| `Unsupported` | unknown command or protocol version | update firmware or library |
| `Timeout`, `CrcError` | the device did not answer, or its answers kept arriving damaged | the link is marked down: call `begin()` again |
| `NotConnected` | an earlier timeout marked the link down | call `begin()` again |
| `NotInitialized` | `begin()` never succeeded | call `begin()` |
| `ProtocolError`, `IoError` | an answer that makes no sense / a device-side failure | report it |

What the library retries by itself: a damaged answer is read again (the
device repeats it until the next request); a request that got no answer, or
that the device says arrived damaged, is sent again - at most twice, with the
same sequence number, and the device answers such a resend from its last
response instead of carrying it out a second time, so a resent write or
append never happens twice. Every request has a timeout (1 s; 3 s for writes
and sync; 0.3 s for the HELLO probe), so nothing waits forever, device or not.

Set `-DEMU_STORAGE_DEBUG=1` to have the library log its retries on `Serial`;
it is silent otherwise.

## Tests

From `example/001/host`:

```bash
pio test -e native      # frame codec, CRC, length checks, sequence handling, resends, error mapping (PC, no hardware)
pio test -e core2 -v    # against a real KagigataDisk: all operations, recovery, and a 4/8/12/13.33 MHz benchmark
```
