// The USB side: a mass-storage drive (volume label "TinyUSB MSC") that a PC reads
// and writes like a USB stick. It is a FAT volume of its own; the SPI host
// never sees it as such, only the objects in objstore.c:
//
//   /SKILL.md                read-only, how to use the drive (English)
//   /command/                an empty file per action, run on deletion:
//       update-firmware      reboot into the UF2 bootloader
//   /spi_virtual_device/     the objects the SPI host sees:
//       log.txt etc.         live views of the SPI host's objects; edits and
//                            deletions go back to them
//       device_info.txt      read-only, live view of /device_info.txt
//       README.md            read-only
//       <anything else>      read-write, shown to the SPI host as a read-only
//                            object
//
// The volume lives in the flash store (fstore.c): what the PC writes stays
// across resets. Writes reach flash in batches - once the oldest unsaved
// sector is 5 s old or 64 sectors are waiting. All of this runs on core0.
#ifndef USBDISK_H
#define USBDISK_H

#include <stdbool.h>
#include <stdint.h>

// Loads the volume from the flash store (building it on a fresh store). Call
// once on core0 after fstore_init() and objstore_init(), before tusb_init().
void usbdisk_init(void);

// Call from core0's main loop after tud_task(): saves what the PC wrote into
// /spi_virtual_device/ once the writes settle, keeps the live files' sizes
// current, and runs deleted commands.
void usbdisk_task(void);

// The volume as 512-byte sectors, for the mass-storage class (usb_device.c).
// usbdisk_write() fails for an out-of-range LBA or when RAM is full.
uint32_t usbdisk_sectors(void);
bool usbdisk_read(uint32_t lba, uint8_t *buf);
bool usbdisk_write(uint32_t lba, const uint8_t *buf);

// For device_info.txt.
const char *usbdisk_status(void);   // "ok" or the latest problem
uint32_t usbdisk_mirrored(void);    // PC files the SPI host sees
uint32_t usbdisk_unsaved(void);     // PC-written sectors waiting for flash
uint32_t usbdisk_flash_free(void);  // sectors both sides may still fill
uint32_t usbdisk_ram_used(void);
uint32_t usbdisk_ram_total(void);

#endif  // USBDISK_H
