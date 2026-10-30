// Flash sector store for the credential store (store.c): 512-byte sectors
// under 32-bit keys in the last STORE_FLASH_SIZE bytes of flash.
//
// The same log-structured design as example/001's fstore.c, cut down to a
// 64 KiB region: a write always goes to the next free slot, the RAM index
// points at the newest copy, and whole 4 KiB blocks are reclaimed (live
// copies moved, block erased) only when free blocks run low, so the sign
// counter written at every signature wears the region evenly. A sector's
// header (key, sequence number, CRC) is programmed after its data, so a write
// cut short by a reset is simply not there on the next boot.
//
// Old copies of a sector stay in flash until their block is reclaimed;
// fstore_format() erases the whole region, which is how secrets are destroyed.
#ifndef FSTORE_H
#define FSTORE_H

#include <stdbool.h>
#include <stdint.h>

#define FS_SECTOR 512u

// Keys: namespace (4 bits) | layout version (4 bits) | index (24 bits).
// Sectors under any other namespace/version are ignored at boot (e.g. what
// example/001 left in this part of flash).
#define FS_NS_FIDO 4u
#define FS_FIDO_VERSION 1u
#define FS_KEY(ns, ver, idx) (((uint32_t)(ns) << 28) | ((uint32_t)(ver) << 24) | (uint32_t)(idx))
#define FS_FIDO_KEY(idx) FS_KEY(FS_NS_FIDO, FS_FIDO_VERSION, idx)

// Scans flash and rebuilds the index.
void fstore_init(void);

// The newest copy of `key` (a pointer into flash, valid until the next
// write), or NULL.
const uint8_t *fstore_lookup(uint32_t key);

// Stores a sector (`data` in RAM), reclaiming blocks as needed. False when
// the store is full.
bool fstore_write(uint32_t key, const uint8_t *data);

// Erases the whole region (~16 block erases) and empties the index.
void fstore_format(void);

#endif  // FSTORE_H
