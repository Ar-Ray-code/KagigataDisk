# spi_virtual_device

This folder holds the files KagigataDisk shows to its SPI host (an M5Stack
Core2, for example). The SPI host reads and writes them by name with
KagigataDisk's own protocol (the emu_storage library on the Core2). They are
kept in KagigataDisk's flash and survive power-off.

| file | what it is | what the PC can do |
|---|---|---|
| `log.txt` etc. | files the SPI host created | read, edit, delete |
| `device_info.txt` | KagigataDisk status, refreshed every second | read |
| `README.md` | this file (the SPI host sees it too) | read |
| anything else | files placed by the PC | anything; the SPI host sees them read-only |

## Usage

- After placing a file, run `sync` (or eject). The SPI host sees it 5-7
  seconds later.
- Editing one of the SPI host's files (`log.txt` etc.) replaces its content
  for the SPI host; deleting it deletes it for the SPI host too.
- The PC caches the drive: remount it to see what the SPI host wrote or
  created since.

## Notes

- The SPI host's files are written by both the PC and the SPI host; whoever
  writes last wins. Remount before editing, to start from the latest content.
- Writes are collected in RAM and saved to flash in batches (after 5 seconds,
  or once enough are waiting). A power cut right after writing can lose the
  last few seconds.
- The flash (about 1.4 MiB) is shared by the PC and the SPI host. The free
  space shown is as of the mount, so it shrinks by whatever the SPI host
  writes afterwards; when flash is really full, writes fail then and there.
- Up to 32 files from the PC. Names: at most 31 printable ASCII characters,
  without `/ \ : * ? " < > |`. Case does not count, so a file named like one
  of the SPI host's files is not shown to the SPI host.
- Keep file contents to plain ASCII: the SPI host handles ASCII text only.

See `SKILL.md` at the top of the drive for details.
