---
name: kagigata-emu
description: Develop, build, flash and test KagigataDisk - an RP2354A board in an M5Stack Core2's TF-card-shaped slot, used as a plain 4-wire SPI peripheral that serves named objects (files) over the custom Kagigata protocol, and shows the same files to a PC as a USB drive. Use for building or flashing either board, running the protocol/device/bridge tests, changing the protocol (protocol/emu_protocol_defs.h), the firmware (emu_link, spi_slave_transport, emu_server, objstore, fstore, usbdisk) or the host library (lib/emu_storage), using the USB drive (log.txt, device_info.txt, spi_virtual_device/, command/), or diagnosing SPI link failures.
---

# KagigataDisk

KagigataDisk (RP2354A) sits in the M5Stack Core2's TF-card-shaped slot, which
is used purely as a 4-wire SPI connector (mode 0). Over it runs the Kagigata
protocol: small CRC-checked frames with which the host lists, reads, writes,
appends, truncates and removes named objects. The same objects appear on the
board's USB mass-storage drive for a PC. Both boards are plugged into **this
PC** over USB. The SPI side deals only in named objects; keep block/sector
concepts off it.

| | board | project | USB (VID:PID) | port |
|---|---|---|---|---|
| device | KagigataDisk (RP2354A) | `example/001/kagigatadisk/` (pico-sdk, target `kagigatadisk`) | `cafe:4002` mass storage (TinyUSB example IDs and strings: `TinyUSB Device`, SCSI `TinyUSB Mass Storage`) | `/dev/disk/by-id/usb-TinyUSB_Mass_Storage_<serial>-0:0` (udev: `/dev/kagigatadisk`), label `TinyUSB MSC` (mount path has a space) |
| (bootloader) | same, in BOOTSEL | - | `2e8a:000f` UF2 drive | label `RP2350` |
| host | M5Stack Core2 (ESP32) | `example/001/host/` (PlatformIO, envs `core2`, `native`) | `10c4:ea60` CP2104 | `/dev/ttyUSB*` |

Protocol definition: `protocol/emu_protocol_defs.h`. Host library usage:
`example/001/host/lib/emu_storage/README.md`.

## Layout

```
protocol/emu_protocol_defs.h        THE wire definition (frame, commands, status, caps, CRC16)
example/001/kagigatadisk/src/
  board_config.h                    pins: CS=0 MOSI=1 SCK=2 MISO=7, LEDs 19/14/12, pio0 SM0
  spi_slave_transport.{pio,c,h}     PIO SPI slave bytes; spis_arm/spis_go/spis_stop       (core1)
  emu_link.{c,h}                    windows, request collection, response streaming      (core1, RAM only)
  emu_server.{c,h}                  decode/validate/dispatch/encode, resend dedup        (core0)
  objstore.{c,h}                    objects: host objects, mirrors, device_info, README  (core0)
  fstore.{c,h}                      log-structured flash sector store                    (core0)
  usbdisk.c, usb_device.c           USB drive (FAT16) + TinyUSB MSC                      (core0)
  devinfo.c, led.c, main.c          /device_info.txt, LEDs, init + core0 loop
  SKILL.md, README_spi.md           drive's /SKILL.md and spi_virtual_device/README.md (= host's /README.md)
example/001/host/
  lib/emu_storage/src/              EmuStorage (API) > EmuProtocol (exchange) > EmuTransport/EmuSpiTransport
                                    + emu_protocol_defs.h = COPY made by tools/sync_protocol.sh
  lib/emu_storage/examples/basic/   minimal sketch
  src/main.cpp                      the Core2 app (uses only EmuStorage)
  test/test_protocol|test_device|test_usb_bridge
tools/                              env.sh (defaults), *_build/_flash.sh, boot0.sh, sync_protocol.sh, install.sh
```

## One-time setup

```bash
./tools/install.sh   # asks for sudo once; --uninstall removes
```

Installs a udev rule (`/dev/kagigatadisk`; plugdev + uaccess on the BOOTSEL
device) and a polkit rule (plugdev may mount the RPI UF2 drive and the
KagigataDisk drive without auth, also over ssh), and adds the user to plugdev.

## Build, flash, test

```bash
./tools/kagigata_build.sh              # cmake (if needed) + ninja -> example/001/kagigatadisk/build/kagigatadisk.uf2
./tools/kagigata_build.sh --clean      # wipe build/ first
./tools/kagigata_flash.sh [--build]    # boot0 -> mount UF2 drive -> copy -> wait for the drive (~6.7 s)
./tools/boot0.sh                       # only reboot into the UF2 bootloader (deletes command/update-firmware)
./tools/core2_flash.sh [--build-only | /dev/ttyUSB0]

cd example/001/host
pio test -e native                        # 13 protocol unit tests, fake device, no hardware
pio test -e core2 -v                      # 27 tests on the Core2 against the real board (-v prints PERF lines)
pio test -e core2 -f test_usb_bridge -v   # manual: follow "STEP A/B done" prompts on the PC
pio ci lib/emu_storage/examples/basic/basic.ino --lib lib/emu_storage --board m5stack-core2 \
  -O "lib_deps=m5stack/M5Unified @ ^0.2.21"
```

`pio test -e core2` leaves test firmware on the Core2: re-flash the app with
`./tools/core2_flash.sh`. `test_device` creates/removes `t_*.bin` and
`perf.bin` only. clk_sys is fixed at 150 MHz; there is no clock option.
`-DKAGIGATA_FW_VERSION=x.y.z` sets the string GET_INFO reports (cached).

## Changing the protocol

1. Edit `protocol/emu_protocol_defs.h` (never the copy in `lib/emu_storage/src/`).
2. `./tools/sync_protocol.sh`.
3. Rebuild both. Until synced, CMake's `protocol_check` target and
   PlatformIO's `check_protocol.py` stop the build ("run tools/sync_protocol.sh").
4. Update `emu_server.c` (device), `emu_storage.cpp` (host),
   and the fake device in `test/test_protocol` if the exchange changes.
   Changing the frame or an existing payload: bump `EMU_PROTOCOL_VERSION`.
   Adding a feature: add a capability bit.

## Protocol rules (keep them)

- Frame: `'K' 'G' | ver | cmd | seq | flags | u16 len LE | payload (<=320) | CRC16 LE`.
  CRC-16/CCITT-FALSE (0x1021, init 0xFFFF, no reflect/xorout) over header+payload.
  All fields little-endian, encoded byte by byte. Data chunk <= 256. Names <= 31
  printable ASCII, flat, case-insensitive.
- Commands: HELLO 01, GET_INFO 02, PING 03, LIST 10, STAT 11, READ 20, WRITE 21
  (flag CREATE), TRUNCATE 22, REMOVE 23, APPEND 24, SYNC 30. Response cmd =
  request | 0x80; payload starts with a status byte (OK, INVALID_ARGUMENT,
  NOT_FOUND, ALREADY_EXISTS, READ_ONLY, NO_SPACE, UNSUPPORTED, IO_ERROR, BAD_FRAME).
- Exchange = chip-select windows. Window 1: the host clocks the request in
  (MISO ignored). Windows 2..n: the host clocks EMU_FILL (0x00) and scans MISO
  for 'K'; the device sends 0x00 until ready, then the response from its first
  byte. Every new window restarts the response from byte 0 until the next
  request, so a damaged response is simply re-read, not re-requested.
  First byte of a window: 'K' = request, anything else = poll.
- A resend (same seq + cmd + CRC as the last executed request, HELLO excluded)
  is answered from the saved response with the REPLAY flag - never executed twice.
  A damaged/truncated request (CRC, length, CS cut) is answered BAD_FRAME and
  the host resends it. A request arriving while core0 is still busy is dropped;
  the host times out and resends.
- Host timeouts: 1000 ms default, 3000 ms for write/append/truncate/sync,
  300 ms for HELLO (1 resend); 2 resends otherwise. Nothing waits forever;
  after Timeout/CrcError the library returns NotConnected until `begin()`.
- Deselected, the device stops its PIO SM and releases MISO (the Core2's LCD
  shares SCK/MOSI/MISO). SPI clock < 16 MHz; default 8 MHz; 13.33 MHz (80/6)
  is the fastest verified.

## Firmware invariants

- **core1 never touches flash** - no flash code, no `const` tables, no
  initializer templates in `.rodata`, no SDK calls that live in flash (e.g.
  write IO_BANK0 directly instead of `gpio_set_oeover()`). core1 is never
  paused (no `flash_safe_execute`, no `pico_flash`); core0 programs/erases
  flash with only its own IRQs off. core1 only moves bytes: PIO FIFOs <->
  double rx buffers / response buffer. After touching `emu_link.c`,
  `spi_slave_transport.*`, `led.h` or anything core1 reaches, make sure
  everything reachable from `emu_link_run` still sits in RAM.
- **Never let the TX FIFO run dry while selected** (`SPIS_TX_PRIME` = 3):
  an `out` stall delays the next `in` and the bit stream loses alignment.
  The per-byte budget is ~2.4 us at 13.33 MHz. Keep `rx_byte()`/`next_tx()` trivial.
- **Arm at window end, only go at CS assert.** `spis_arm()` (SM restart, FIFOs
  cleared, 3 bytes primed) runs right after deselect; `spis_go()` (MISO on + SM
  enable) is all that happens on assert. Resyncing on assert was too late: the
  ESP32 clocks within ~1 us and responses arrived 1-2 bits shifted.
- **SCK bypasses the PIO input synchroniser** (`input_sync_bypass`): 2 clk_sys
  cycles less between falling SCK and the next MISO bit; without it 13.33 MHz
  failed with CRC errors on MISO only. MOSI/CS stay synchronised. MISO is 12 mA,
  fast slew. MISO changes after the falling edge, never after the rising one.
- CS assert is acted on immediately; deassert only after 8 consecutive polls
  (`CS_DEASSERT_CONFIRM_POLLS`).
- core0 main loop: `tud_task`, `emu_server_task`, `usbdisk_task`,
  `objstore_task`, `emu_server_task`, `fstore_idle`, `led_update`,
  `devinfo_update`. The core0<->core1 handover is `link_state`
  (L_IDLE/L_PENDING/L_READY) with `__dmb()`; no locks.
- Object persistence: object pages (512 B, fstore namespace `FS_NS_OBJ`, key
  `(id<<16)|page`) belong to an object only if below its size and written
  after its `create_seq`; the table (3 sectors, `(0xFF<<16)|k`) is written
  before the pages. This keeps deleted/truncated data from resurfacing after a
  reboot (fstore trim only edits the RAM index). Growth/truncate-up zero-fills.
  Page cache: 64 slots, flushed at 5 s age or 32 dirty pages; SYNC flushes;
  NO_SPACE when shared flash is exhausted.
- Limits: 32 host objects, 32 mirrors (1440 KiB total), 1440 KiB per object,
  `device_info.txt` 512 B fixed. Reserved names: `device_info.txt`, `README.md`.
- Flash: firmware below 192 KiB (`FS_FLASH_OFFSET`; `main.c` blinks all LEDs
  otherwise), store 192 KiB..2 MiB (464 x 4 KiB blocks, 2923 sectors).
  fstore drops keys outside `FS_NS_USB`/v3 and `FS_NS_OBJ`/v1 at boot (the old
  namespace 1 data is gone after the first boot). Bump a version to drop a layout.

## Where to change what

| change | where |
|---|---|
| a new command | `protocol/emu_protocol_defs.h` (+ sync), `emu_server.c` `serve()` switch + `do_*()`, `emu_storage.{h,cpp}`, tests |
| object semantics, limits, names | `objstore.{c,h}` (`OBJ_*`, `objstore_name_ok()`) |
| response streaming, window handling | `emu_link.c` (core1 rules apply) |
| pins, PIO allocation | `board_config.h` (4 SPI pins need not be adjacent; SCK < 32) |
| bit-level timing | `spi_slave_transport.{pio,c}` (re-run `test_device` perf) |
| USB drive layout, views, commands | `usbdisk.c` (`commands[]` row = new command; actions run from `usbdisk_task()`) |
| device_info keys | `devinfo.c` (stay within 512 B) |
| host timeouts, polling | `emu_protocol.h/.cpp` |
| host defaults (pins, clock) | `EmuStorageConfig` in `emu_storage.h`, `EMU_SPI_DEFAULT_HZ` |
| drive's SKILL.md / folder README | `src/SKILL.md`, `src/README_spi.md` (rebuild; `OBJECT_DEPENDS` picks them up) |

## USB drive

```
/SKILL.md                    read-only usage guide
/command/                    empty files; deleting one (rm, or a file manager's trash) runs it 1 s later
    update-firmware          -> UF2 bootloader (flushes both sides first)
/spi_virtual_device/
    README.md                read-only; also the host's /README.md
    log.txt, <host objects>  live views of the host's objects (up to 15); edits/deletes go back to the objects;
                             new host objects appear at the next mount; host growth visible up to +16 KiB until remount
    device_info.txt          live view of /device_info.txt
    <other files>            read-write; shown to the host as read-only objects (<=31-char ASCII names)
```

```bash
DEV=$(readlink -f /dev/disk/by-id/usb-TinyUSB_Mass_Storage_*-0:0)
MNT=$(findmnt -no TARGET "$DEV") || { udisksctl mount -b "$DEV"; MNT=$(findmnt -no TARGET "$DEV"); }
udisksctl unmount -b "$DEV" && udisksctl mount -b "$DEV"   # the PC caches: remount to see host changes
tail "$MNT/spi_virtual_device/log.txt"
cp memo.txt "$MNT/spi_virtual_device/" && sync             # host sees memo.txt ~5-7 s later
grep -E '^(spi_|flash_|usb_|mirrored)' "$MNT/spi_virtual_device/device_info.txt"
```

`device_info.txt` keys: `unique_id uptime_s clk_sys_hz clk_peri_hz flash_clkdiv
flash_sectors flash_erases spi_unsaved usb_unsaved mirrored_files usb_sync`.
The device keeps no link counters; link health is measured on the host:
`EmuStorage::linkStats()` (resends, crcErrors, timeouts, replays), e.g. the
PERF lines of `pio test -e core2 -v`.

Flash (~1.4 MiB) is shared by PC and host with no fixed split. Writes on both
sides sit in RAM up to ~5 s (`spi_unsaved` / `usb_unsaved`); last writer wins
when both edit the same object.

## When something fails

| symptom | where to look |
|---|---|
| `no cafe:4002 drive found` | firmware not running: board in BOOTSEL (rerun `kagigata_flash.sh`) or unplugged |
| `could not mount ... (run tools/install.sh?)` | polkit rule missing (non-desktop sessions) |
| build stops: `... differs from ...: run tools/sync_protocol.sh` | protocol copy out of date: `./tools/sync_protocol.sh` |
| all three LEDs blink together from boot | firmware > 192 KiB; shrink it |
| Core2 FILES dot stays gray | `begin()` fails: serial shows `KagigataDisk: <result>`; timeout = wiring/GND/board not running; RX LED (GPIO12) dark = no request arrives |
| `unsupported` from `begin()` | protocol versions differ: rebuild both from the same header |
| intermittent `crc error` / `timeout` | host `linkStats()`: `crcErrors` = damaged answers (MISO side), `resends` without `crcErrors` = requests lost or damaged (MOSI side) or core0 busy past the timeout; lower the clock; >= 16 MHz is unsupported |
| `no space` | flash full (`flash_sectors` near 2923), object > 1440 KiB, or 32 objects exist |
| `read only` | `README.md`, `device_info.txt` and PC-provided files are read-only |
| PC file not visible to the host | `sync`? waited 5-7 s? read `usb_sync` (e.g. `skipped: name too long for the SPI side`, `the SPI host has a file of that name`, `too many files`) |
| PC edit of a host object not applied | `sync`? 5-7 s? `usb_sync` `not passed to the SPI side: ...`; a later host write wins |
| new host object missing on the drive | PC cache: remount (max 15 views) |
| writes missing after a reset | they were still in RAM: call `sync()` on the host; `*_unsaved` = 0 means saved |

Not verified: Windows / macOS PCs, file managers other than GNOME Files,
fsck of the drive image, power loss mid-write, long-term flash wear, LEDs by eye.
