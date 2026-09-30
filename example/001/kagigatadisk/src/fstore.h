// Flash sector store: everything the object store (objstore.c) and the USB
// drive keep across a reset, as 512-byte sectors under 32-bit keys, in the
// flash above the firmware.
//
// Log-structured, so that no sector of flash is erased every time a host
// rewrites the same FAT or directory sector: a write always goes to the next
// free slot, the RAM index points at the newest copy, and whole 4 KiB blocks
// are reclaimed (live copies moved, block erased) only when free blocks run
// low - the erases spread over the whole region. Each block holds 7 sectors
// behind a header page; a sector's header (key, sequence number, CRC) is
// programmed after its data, so a write cut short by a reset is simply not
// there on the next boot.
//
// Owned by core0 alone. core1 (the SPI link) never touches flash, so it keeps
// running while a sector is programmed or a block erased.
#ifndef FSTORE_H
#define FSTORE_H

#include <stdbool.h>
#include <stdint.h>

#define FS_SECTOR 512u
// The store starts here in flash; the firmware must end below it (main.c).
#define FS_FLASH_OFFSET (192u * 1024u)

// Keys: namespace (4 bits) | layout version (4 bits) | index (24 bits: the
// USB drive's sector number, or an object's id and page). Bumping
// a version makes every sector stored under the old layout unreachable; boot
// then trims it.
#define FS_NS_USB 2u
#define FS_NS_OBJ 3u
#define FS_USB_VERSION 3u  // 3: FAT16, one data area shared with the SPI side
#define FS_OBJ_VERSION 1u
#define FS_KEY(ns, ver, idx) (((uint32_t)(ns) << 28) | ((uint32_t)(ver) << 24) | (uint32_t)(idx))
#define FS_USB_KEY(lba) FS_KEY(FS_NS_USB, FS_USB_VERSION, lba)
#define FS_OBJ_KEY(v) FS_KEY(FS_NS_OBJ, FS_OBJ_VERSION, v)
#define FS_KEY_INDEX(key) ((key) & 0x00FFFFFFu)
#define FS_KEY_SPACE(key) ((key) >> 24)  // namespace + version

// Scans flash and rebuilds the index. Call once on core0 before core1 starts.
// Sectors whose key is under neither current layout are dropped.
void fstore_init(void);

// The newest copy of `key` (a pointer into flash), or NULL.
const uint8_t *fstore_lookup(uint32_t key);

// Stores a sector (`data` in RAM), reclaiming blocks as needed. False
// when the store is full.
bool fstore_write(uint32_t key, const uint8_t *data);

// Forgets every sector for which keep(key, seq, ctx) returns false.
// Forgetting writes nothing: after a reset the sector's newest copy is back
// until trimmed again, so callers re-trim from their FAT at every boot.
// `seq` grows with every write, so comparing it against fstore_seq() taken
// earlier tells whether a sector was rewritten since.
typedef bool (*fstore_keep_fn)(uint32_t key, uint32_t seq, void *ctx);
void fstore_trim(fstore_keep_fn keep, void *ctx);
uint32_t fstore_seq(void);

// Erases one block ahead of need, if a spare erased block is wanted.
// Cheap to call often; each erase blocks for ~50 ms.
void fstore_idle(void);

uint32_t fstore_live(void);      // sectors stored
uint32_t fstore_capacity(void);  // sectors that can be stored
uint32_t fstore_erases(void);    // block erases since boot

#endif  // FSTORE_H
