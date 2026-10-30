#include "fstore.h"

#include <string.h>

#include "board_config.h"
#include "hardware/flash.h"
#include "hardware/regs/addressmap.h"
#include "hardware/sync.h"
#include "pico.h"

#define FS_XIP(off) ((const uint8_t *)(XIP_BASE + (off)))

static void flash_prog(uint32_t off, const uint8_t *buf, uint32_t len) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_program(off, buf, len);
  restore_interrupts(irq);
}

static void flash_erase4k(uint32_t off) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(off, 4096u);
  restore_interrupts(irq);
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
#define FS_OFFSET STORE_FLASH_OFFSET
#define FS_BLOCK 4096u
#define FS_BLOCKS (STORE_FLASH_SIZE / FS_BLOCK)
#define FS_SPB 7u  // sectors per block, behind one header page
#define FS_SLOTS (FS_BLOCKS * FS_SPB)
// Blocks kept free so reclaiming always has somewhere to move sectors to.
#define FS_RESERVE_BLOCKS 2u
// Stored sectors are capped so a reclaim pass always finds stale space and
// never has to move a full block.
#define FS_LIVE_MAX ((FS_BLOCKS - FS_RESERVE_BLOCKS - 1u) * FS_SPB)

_Static_assert(STORE_FLASH_SIZE % FS_BLOCK == 0, "the store is whole blocks");

#define BLK_MAGIC 0x3146464Bu   // "KFF1": not 001's "KSB1", so neither reads the other's blocks
#define SLOT_MAGIC 0x4C53464Bu  // "KFSL"

typedef struct {
  uint32_t magic;
  uint32_t erase_count;
  uint32_t rsv[2];
} blk_hdr_t;

typedef struct {
  uint32_t key;
  uint32_t seq;
  uint32_t crc;
  uint32_t magic;
} slot_hdr_t;

static inline uint32_t blk_off(uint32_t b) { return FS_OFFSET + b * FS_BLOCK; }
static inline uint32_t hdr_off(uint32_t b, uint32_t s) {
  return blk_off(b) + sizeof(blk_hdr_t) + s * sizeof(slot_hdr_t);
}
static inline uint32_t data_off(uint32_t b, uint32_t s) {
  return blk_off(b) + (s + 1u) * FS_SECTOR;
}
#define LOC(b, s) ((uint16_t)((b) * 8u + (s)))
#define LOC_BLK(l) ((uint32_t)(l) >> 3)
#define LOC_SLOT(l) ((uint32_t)(l) & 7u)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
// Index: key -> slot. The store holds at most FS_LIVE_MAX keys, so a plain
// table searched linearly is plenty.
#define KEY_EMPTY 0xFFFFFFFFu
static uint32_t idx_key[FS_LIVE_MAX];
static uint16_t idx_loc[FS_LIVE_MAX];

enum { B_ERASE = 0, B_ERASED, B_USED };  // needs erase / erased / has slots
static uint8_t blk_state[FS_BLOCKS];
static uint8_t blk_used[FS_BLOCKS];  // slots programmed (or unusable)
static uint8_t blk_live[FS_BLOCKS];  // slots the index points at
static uint32_t blk_erases[FS_BLOCKS];
static int32_t head = -1;  // block being filled
static uint32_t next_seq = 1;
static uint32_t live_total;

static uint8_t page[256];
static uint8_t move_buf[FS_SECTOR];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t crc32(const uint8_t *p, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
  }
  return c ^ 0xFFFFFFFFu;
}

static int32_t ifind(uint32_t key) {
  for (uint32_t i = 0; i < FS_LIVE_MAX; i++) {
    if (idx_key[i] == key) return (int32_t)i;
  }
  return -1;
}

// Points `key` at `loc`; returns the slot it pointed at before, or -1.
// The caller has checked there is room for a new key.
static int32_t iset(uint32_t key, uint16_t loc) {
  int32_t i = ifind(key);
  if (i >= 0) {
    int32_t old = idx_loc[i];
    idx_loc[i] = loc;
    return old;
  }
  i = ifind(KEY_EMPTY);
  idx_key[i] = key;
  idx_loc[i] = loc;
  return -1;
}

static const slot_hdr_t *slot_hdr(uint32_t b, uint32_t s) {
  return (const slot_hdr_t *)FS_XIP(hdr_off(b, s));
}

static bool all_ff(const uint8_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (p[i] != 0xFFu) return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Blocks
// ---------------------------------------------------------------------------
static uint32_t free_blocks(void) {
  uint32_t n = 0;
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    n += blk_state[b] != B_USED && (int32_t)b != head;
  }
  return n;
}

static void erase_block(uint32_t b) {
  flash_erase4k(blk_off(b));
  blk_erases[b]++;
  blk_state[b] = B_ERASED;
  blk_used[b] = 0;
  blk_live[b] = 0;
}

// Least-worn free block becomes the new head.
static bool open_head(void) {
  int32_t best = -1;
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if (blk_state[b] == B_USED || (int32_t)b == head) continue;
    if (best < 0 || blk_erases[b] < blk_erases[best] ||
        (blk_erases[b] == blk_erases[best] && blk_state[b] == B_ERASED &&
         blk_state[best] != B_ERASED)) {
      best = (int32_t)b;
    }
  }
  if (best < 0) return false;
  if (blk_state[best] != B_ERASED) erase_block((uint32_t)best);
  blk_hdr_t h = {.magic = BLK_MAGIC, .erase_count = blk_erases[best], .rsv = {0xFFFFFFFFu, 0xFFFFFFFFu}};
  memset(page, 0xFF, sizeof(page));
  memcpy(page, &h, sizeof(h));
  flash_prog(blk_off((uint32_t)best), page, sizeof(page));
  blk_state[best] = B_USED;
  blk_used[best] = 0;
  blk_live[best] = 0;
  head = best;
  return true;
}

// Programs one sector into the head block and points the index at it.
static bool put_slot(uint32_t key, const uint8_t *data, uint32_t seq) {
  if (head < 0 || blk_used[head] >= FS_SPB) {
    if (!open_head()) return false;
  }
  uint32_t b = (uint32_t)head, s = blk_used[b];
  slot_hdr_t sh = {.key = key, .seq = seq, .crc = crc32(data, FS_SECTOR), .magic = SLOT_MAGIC};
  memset(page, 0xFF, sizeof(page));
  memcpy(page + sizeof(blk_hdr_t) + s * sizeof(slot_hdr_t), &sh, sizeof(sh));
  flash_prog(data_off(b, s), data, FS_SECTOR);
  flash_prog(blk_off(b), page, sizeof(page));  // only this header's bytes are not 0xFF
  int32_t old = iset(key, LOC(b, s));
  blk_used[b]++;
  blk_live[b]++;
  if (old >= 0) {
    blk_live[LOC_BLK(old)]--;
  } else {
    live_total++;
  }
  return true;
}

// Moves the live sectors out of the most-stale block and marks it for
// erasing. False if there is nothing worth reclaiming.
static bool reclaim_one(void) {
  int32_t victim = -1;
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if (blk_state[b] != B_USED || (int32_t)b == head || blk_live[b] >= blk_used[b]) continue;
    if (victim < 0 || blk_live[b] < blk_live[victim]) victim = (int32_t)b;
  }
  if (victim < 0) return false;
  uint32_t v = (uint32_t)victim;
  for (uint32_t s = 0; s < blk_used[v] && blk_live[v]; s++) {
    const slot_hdr_t *sh = slot_hdr(v, s);
    if (sh->magic != SLOT_MAGIC) continue;
    int32_t i = ifind(sh->key);
    if (i < 0 || idx_loc[i] != LOC(v, s)) continue;  // stale
    memcpy(move_buf, FS_XIP(data_off(v, s)), FS_SECTOR);
    if (!put_slot(sh->key, move_buf, next_seq++)) return false;
  }
  blk_state[v] = B_ERASE;  // erased when next needed
  blk_used[v] = 0;
  blk_live[v] = 0;
  return true;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
void fstore_init(void) {
  for (uint32_t i = 0; i < FS_LIVE_MAX; i++) idx_key[i] = KEY_EMPTY;
  live_total = 0;
  uint32_t max_seq = 0;
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    const blk_hdr_t *bh = (const blk_hdr_t *)FS_XIP(blk_off(b));
    blk_live[b] = 0;
    blk_used[b] = 0;
    if (bh->magic != BLK_MAGIC) {
      blk_state[b] = B_ERASE;
      blk_erases[b] = 0;
      continue;
    }
    blk_state[b] = B_USED;
    blk_erases[b] = bh->erase_count;
    for (uint32_t s = 0; s < FS_SPB; s++) {
      const slot_hdr_t *sh = slot_hdr(b, s);
      if (all_ff((const uint8_t *)sh, sizeof(*sh))) {
        // Unwritten - unless a reset cut in between data and header.
        if (!all_ff(FS_XIP(data_off(b, s)), FS_SECTOR)) blk_used[b] = (uint8_t)(s + 1u);
        continue;
      }
      blk_used[b] = (uint8_t)(s + 1u);
      if (sh->magic != SLOT_MAGIC || (sh->key >> 24) != (FS_FIDO_KEY(0) >> 24)) continue;
      if (crc32(FS_XIP(data_off(b, s)), FS_SECTOR) != sh->crc) continue;
      if (sh->seq > max_seq) max_seq = sh->seq;
      int32_t i = ifind(sh->key);
      if (i >= 0) {
        const slot_hdr_t *cur = slot_hdr(LOC_BLK(idx_loc[i]), LOC_SLOT(idx_loc[i]));
        if (cur->seq >= sh->seq) continue;
      } else if (ifind(KEY_EMPTY) < 0) {
        continue;  // more keys than the index holds: cannot happen with this firmware
      }
      iset(sh->key, LOC(b, s));
    }
    // A block cut short is not appended to again: the next write opens a
    // fresh head, and reclaiming recovers the unused slots.
    if (blk_used[b] < FS_SPB) blk_used[b] = FS_SPB;
  }
  for (uint32_t i = 0; i < FS_LIVE_MAX; i++) {
    if (idx_key[i] == KEY_EMPTY) continue;
    blk_live[LOC_BLK(idx_loc[i])]++;
    live_total++;
  }
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if (blk_state[b] == B_USED && blk_live[b] == 0) blk_state[b] = B_ERASE;
  }
  next_seq = max_seq + 1u;
  head = -1;
}

const uint8_t *fstore_lookup(uint32_t key) {
  int32_t i = ifind(key);
  if (i < 0) return NULL;
  uint16_t l = idx_loc[i];
  return FS_XIP(data_off(LOC_BLK(l), LOC_SLOT(l)));
}

bool fstore_write(uint32_t key, const uint8_t *data) {
  int32_t i = ifind(key);
  if (i >= 0 && memcmp(FS_XIP(data_off(LOC_BLK(idx_loc[i]), LOC_SLOT(idx_loc[i]))), data,
                       FS_SECTOR) == 0) {
    return true;  // unchanged: no flash wear at all
  }
  if (i < 0 && live_total >= FS_LIVE_MAX) return false;
  // Before opening a new head, make sure a free block will remain for the
  // next reclaim to move into.
  if (head < 0 || blk_used[head] >= FS_SPB) {
    while (free_blocks() <= FS_RESERVE_BLOCKS) {
      if (!reclaim_one()) break;
    }
  }
  return put_slot(key, data, next_seq++);
}

void fstore_format(void) {
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if (!all_ff(FS_XIP(blk_off(b)), FS_BLOCK)) flash_erase4k(blk_off(b));
  }
  fstore_init();
  for (uint32_t b = 0; b < FS_BLOCKS; b++) blk_state[b] = B_ERASED;
}

