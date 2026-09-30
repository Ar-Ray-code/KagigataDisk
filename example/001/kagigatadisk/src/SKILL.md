---
name: kagigatadisk
description: Use when this USB drive (RP2350-KagigataDisk, label KAGIGATADSK) is mounted - reading what its SPI host logged, adding or removing files the SPI host can read, checking device health, or putting the board into its UF2 bootloader to update the firmware.
---

# KagigataDisk USB drive

KagigataDisk is an RP2350 board that plugs into a microcontroller's
TF-card-shaped slot (the "SPI host", e.g. an M5Stack Core2) and uses it purely
as an SPI connector: the host reads and writes named files on the board with
KagigataDisk's own protocol (the emu_storage library on the Core2). This USB
drive is the PC's window into the same files. It is not a real USB stick: the
SPI host's files and this drive are two views of the same data, kept in sync
by the board.

## Finding the drive

On Linux it is `/dev/disk/by-id/usb-RP2350_KagigataDisk_<serial>-0:0`, and a
desktop mounts it as `KAGIGATADSK` (`/run/media/$USER/KAGIGATADSK` or
`/media/$USER/KAGIGATADSK`). Below, `$DEV` is the device and `$MNT` the mount
point:

```bash
DEV=$(ls /dev/disk/by-id/usb-RP2350_KagigataDisk_*-0:0)
MNT=$(findmnt -rno TARGET "$DEV") || { udisksctl mount -b "$DEV"; MNT=$(findmnt -rno TARGET "$DEV"); }
```

## Layout

```
/SKILL.md                       this file (read-only)
/command/                       one empty file per action, run on deletion
    update-firmware             enter the UF2 bootloader
/spi_virtual_device/            the files the SPI host sees
    README.md                   read-only; a short note, the SPI host sees it too
    log.txt                     the SPI host's files: read, edit or delete them
    <the SPI host's other files>
    device_info.txt             read-only; board status, refreshed every second
    <your files>                read-write; the SPI host can read them
```

Everything on the drive except `command/` is kept in the board's flash and
survives resets.

## Space

The board's flash (about 1.4 MiB) is shared: your files and the SPI host's
files come out of the same room, with no fixed split. The PC is shown the
flash that is still free when it mounts the drive, so the free space you see
can be more than what is left once the SPI host has written since. If flash
really runs out, writes fail there and then (on the PC: "No space left on
device"; on the SPI host: a "no space" result).

## How writes reach flash

Writes - from the PC and from the SPI host alike - are collected in RAM and
written to flash in batches: when the oldest unsaved sector is 5 seconds old,
or when enough sectors are waiting (64 from the PC, 32 from the SPI host).
Rewriting the same sectors within a batch costs flash nothing extra. Always
`sync` (or eject) after writing, or the PC itself may hold writes back.

## The SPI host's files

`spi_virtual_device/` shows every file the SPI host has: `log.txt` and
whatever else it has created. The PC caches the drive, so what you
see is a snapshot from when it was mounted; a file the SPI host created since
shows up at the next mount. To see new content, unmount and mount again:

```bash
udisksctl unmount -b "$DEV" && udisksctl mount -b "$DEV"
```

You can edit these files here: once your writes settle (after `sync`, about
5-7 s), the SPI host's file is replaced with what you saved - edited in
place, rewritten, truncated alike. Both you and the SPI host write them, and
whoever writes last wins: remount first, so you start from the SPI host's
latest content, and expect what it adds while you edit to be lost. Saving a
new `log.txt` here also creates the SPI host's `/log.txt` for it to append
to.

Deleting one here deletes it for the SPI host too - e.g. to start a fresh
log. The SPI host writes a new `log.txt` when it next logs something; it shows
up here after a remount.

The SPI host asks for files by name every time, so it sees such a change as
soon as it lands - it has nothing cached to throw away.

## Giving the SPI host a file

Copy it into `spi_virtual_device/` and flush:

```bash
cp config.txt "$MNT/spi_virtual_device/" && sync
```

Once the batch is in flash and the PC has been quiet for 1.5 s (about 5-7 s
after the copy), the SPI host sees the file (as `/config.txt`) as a read-only
file. Editing the file replaces it; deleting it takes it away from the SPI
host.

Limits:
- up to 32 of your files; the SPI host's files show up to 15
- names of at most 31 printable ASCII characters, without `/ \ : * ? " < > |`;
  no subfolders (a folder inside `spi_virtual_device/` is ignored)
- names do not tell case apart: a file named like one of the SPI host's own
  files (`LOG.TXT` next to its `log.txt`) is skipped
- `device_info.txt` and `README.md` are the board's own names and cannot be
  used
- keep file contents to plain ASCII: the SPI host handles ASCII text only
- when the SPI host grows one of its files, this drive shows at most 16 KiB
  beyond the size it had at the last mount; remount to see the rest
- while flash is being written, the SPI host's requests wait (a few hundred
  milliseconds at worst instead of well under one); they do not fail

Files outside `spi_virtual_device/` are kept too, but the SPI host never sees
them.

## Checking the board

`spi_virtual_device/device_info.txt` (remount to refresh):

| key | meaning |
|---|---|
| `uptime_s`, `clk_sys_hz`, `flash_clkdiv` | uptime and clocks |
| `flash_sectors` | sectors stored in flash / how many fit |
| `flash_erases` | flash block erases since boot |
| `spi_unsaved`, `usb_unsaved` | sectors waiting for the next batch |
| `mirrored_files` | your files the SPI host can see |
| `usb_sync` | `ok`, or why the last file could not be shown to the SPI host |

## Updating the firmware

`command/update-firmware` is the one command: deleting it (renaming it away
counts too) is the request. Deleting it from a desktop file manager (moving it
to the trash) works the same: the board spots the trash record the file
manager writes, without waiting for the PC to finish the move. With `rm`,
flush after deleting:

```bash
rm "$MNT/command/update-firmware" && sync
```

(or move it to the trash in a file manager). One second later the board
writes out anything unsaved and reboots into the RP2350 UF2 bootloader: this
drive disappears - the PC drops its mount by itself - and a drive labelled
`RP2350` appears. Copy the new `.uf2`
file onto it; the board restarts with the new firmware. Unsaved writes are
written to flash before the reboot, and the drive's contents are kept.

## Notes

- Always `sync` (or eject) after writing: the PC may otherwise hold the writes
  back for many seconds.
- `command/` is rebuilt at every boot: a deleted `update-firmware` is back
  after the next reset.
- A file named `update-firmware` moved to the trash from
  anywhere on this drive counts as the command, until the record says where it
  came from.
- Writes from the last few seconds before an unexpected power loss may be
  lost; what reached flash is intact.
