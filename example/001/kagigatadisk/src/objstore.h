// The objects (files) the SPI host sees through the Kagigata protocol:
//
//   * its own objects - /log.txt and whatever else it creates: read-write,
//     kept in the flash store (fstore.c) as 512-byte pages under their id
//   * the files a PC saved in the USB drive's /spi_virtual_device/
//     ("mirrors", published by usbdisk.c) - read-only, read straight from the
//     drive's own sectors
//   * /device_info.txt (devinfo.c) and /README.md (the firmware's text) -
//     read-only
//
// There is one flat namespace, names compared without regard to case (the
// USB drive shows the same files on FAT). Writes go through a RAM page cache
// and reach flash in batches (objstore_task()): once the oldest unsaved page
// is OBJ_FLUSH_AGE_MS old, or OBJ_FLUSH_PAGES pages are waiting, so a host
// appending a line at a time wears flash only once per batch.
//
// Everything here runs on core0.
#ifndef OBJSTORE_H
#define OBJSTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "emu_protocol_defs.h"

#define OBJ_PAGE 512u
#define OBJ_HOST_MAX 32u    // objects the SPI host may have
#define OBJ_MIRROR_MAX 32u  // PC files shown to it
#define OBJ_MAX_BYTES (1440u * 1024u)
#define OBJ_FLUSH_AGE_MS 5000u
#define OBJ_FLUSH_PAGES 32u

#define OBJ_LOG_NAME "log.txt"
#define OBJ_DEVINFO_NAME "device_info.txt"
#define OBJ_README_NAME "README.md"

typedef struct {
  char name[EMU_NAME_MAX + 1];
  uint32_t size;
  uint8_t attr;  // EMU_ATTR_*
} obj_info_t;

// Call once after fstore_init() (and devinfo_init()).
void objstore_init(void);
// The firmware's README text, shown as /README.md.
void objstore_set_readme(const uint8_t *text, uint32_t len);

// Batched saving. Call objstore_task() from the main loop.
void objstore_task(void);
void objstore_flush(void);        // save everything now (before a reboot)
uint32_t objstore_unsaved(void);  // pages waiting for flash
bool objstore_flash_full(void);   // the last save ran out of flash

// ---- The protocol's view (emu_server.c) -----------------------------------
// `name` is a bare object name (no '/'), NUL-terminated.
uint32_t objstore_count(void);
bool objstore_entry(uint32_t index, obj_info_t *out);
emu_status_t objstore_stat(const char *name, obj_info_t *out);
emu_status_t objstore_read(const char *name, uint32_t off, uint8_t *buf, uint32_t len,
                           uint32_t *got);
emu_status_t objstore_write(const char *name, uint32_t off, const uint8_t *data, uint32_t len,
                            bool create, uint32_t *size_out);
emu_status_t objstore_append(const char *name, const uint8_t *data, uint32_t len,
                             uint32_t *size_out);
emu_status_t objstore_truncate(const char *name, uint32_t size);
emu_status_t objstore_remove(const char *name);
uint32_t objstore_available(void);  // bytes the SPI host may still add

// ---- The USB drive's view (usbdisk.c) -------------------------------------
// The SPI host's own objects, by index (false past the last).
bool objstore_host_object(uint32_t index, char name_out[EMU_NAME_MAX + 1], uint32_t *size_out);
// One of them by name: its size (false if there is none), one page of it at
// byte offset `off` (a multiple of OBJ_PAGE; zero past its end).
bool objstore_host_find(const char *name, uint32_t *size_out);
void objstore_host_page(const char *name, uint32_t off, uint8_t *buf);
// The PC deleted one / saved a new version of one (or a new one): `size`
// bytes that fill(ctx, off, buf) supplies a page at a time. The object takes
// the PC's spelling of the name. False if flash has no room for it (the old
// version is kept then).
bool objstore_host_delete(const char *name);
typedef void (*obj_fill_fn)(void *ctx, uint32_t off, uint8_t *buf);
bool objstore_host_replace(const char *name, uint32_t size, obj_fill_fn fill, void *ctx);

// Replaces the set of PC files shown read-only. Each file's content is the
// given USB-drive sectors (usb_lbas[k] holds bytes k*512 ...), read through
// `read`. Returns how many were taken.
typedef struct {
  char name[EMU_NAME_MAX + 1];
  uint32_t size;
  const uint16_t *usb_lbas;
} obj_mirror_t;
typedef void (*obj_sector_fn)(uint32_t lba, uint8_t *buf);
uint32_t objstore_mirror_publish(const obj_mirror_t *files, uint32_t n, obj_sector_fn read);

// Names: printable ASCII without / \ : * ? " < > |, 1..EMU_NAME_MAX long,
// not "." or "..".
bool objstore_name_ok(const char *name);
bool objstore_name_eq(const char *a, const char *b);  // ignoring case
bool objstore_reserved_name(const char *name);        // device_info.txt, README.md

#endif  // OBJSTORE_H
