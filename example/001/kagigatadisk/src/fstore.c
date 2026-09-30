#include "fstore.h"

#include <string.h>

#ifdef FSTORE_HOST_TEST
#include "fstore_host.h"  // test harness: RAM-backed "flash"
#else
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
#endif

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2u * 1024u * 1024u)
#endif

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
#define FS_OFFSET FS_FLASH_OFFSET
#define FS_BLOCK 4096u
#define FS_BLOCKS ((PICO_FLASH_SIZE_BYTES - FS_OFFSET) / FS_BLOCK)
#define FS_SPB 7u  // sectors per block, behind one header page
#define FS_SLOTS (FS_BLOCKS * FS_SPB)
// Blocks kept free so reclaiming always has somewhere to move sectors to.
#define FS_RESERVE_BLOCKS 2u
// Stored sectors are capped at 90% of the slots, so there is always stale
// space to reclaim and a reclaim pass never has to move a full block.
#define FS_LIVE_MAX (FS_SLOTS * 9u / 10u)

_Static_assert(FS_BLOCKS * 8u < 0xFFFFu, "slot numbers must fit 16 bits");

#define BLK_MAGIC 0x3142534Bu   // "KSB1"
#define SLOT_MAGIC 0x4C53534Bu  // "KSSL"

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
// Index: key -> slot, open addressing with linear probing. Deletion shifts
// entries back instead of leaving tombstones.
#define FS_HASH 8192u
#define KEY_EMPTY 0xFFFFFFFFu
static uint32_t h_key[FS_HASH];
static uint16_t h_loc[FS_HASH];

enum { B_ERASE = 0, B_ERASED, B_USED };  // needs erase / erased / has slots
static uint8_t blk_state[FS_BLOCKS];
static uint8_t blk_used[FS_BLOCKS];  // slots programmed (or unusable)
static uint8_t blk_live[FS_BLOCKS];  // slots the index points at
static uint32_t blk_erases[FS_BLOCKS];
static int32_t head = -1;  // block being filled
static uint32_t next_seq = 1;
static uint32_t live_total;
static uint32_t erases_since_boot;

static uint8_t page[256];
static uint8_t move_buf[FS_SECTOR];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static uint32_t crc_tab[256];

static void crc_init(void) {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_tab[i] = c;
  }
}

static uint32_t crc32(const uint8_t *p, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

static inline uint32_t hhome(uint32_t key) { return (key * 2654435761u) >> 19; }  // 13 bits

static int32_t hfind(uint32_t key) {
  uint32_t i = hhome(key);
  for (uint32_t n = 0; n < FS_HASH; n++) {
    if (h_key[i] == key) return (int32_t)i;
    if (h_key[i] == KEY_EMPTY) return -1;
    i = (i + 1u) & (FS_HASH - 1u);
  }
  return -1;
}

static void hdel(uint32_t i) {
  uint32_t j = i;
  for (;;) {
    h_key[i] = KEY_EMPTY;
    for (;;) {
      j = (j + 1u) & (FS_HASH - 1u);
      if (h_key[j] == KEY_EMPTY) return;
      uint32_t k = hhome(h_key[j]);
      bool stays = (i <= j) ? (i < k && k <= j) : (i < k || k <= j);
      if (!stays) break;
    }
    h_key[i] = h_key[j];
    h_loc[i] = h_loc[j];
    i = j;
  }
}

// Points `key` at `loc`; returns the slot it pointed at before, or -1.
static int32_t hset(uint32_t key, uint16_t loc) {
  uint32_t i = hhome(key);
  for (;;) {
    if (h_key[i] == key) {
      int32_t old = h_loc[i];
      h_loc[i] = loc;
      return old;
    }
    if (h_key[i] == KEY_EMPTY) {
      h_key[i] = key;
      h_loc[i] = loc;
      return -1;
    }
    i = (i + 1u) & (FS_HASH - 1u);
  }
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
  erases_since_boot++;
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
  int32_t old = hset(key, LOC(b, s));
  blk_used[b]++;
  blk_live[b]++;
  if (old >= 0) {
    blk_live[LOC_BLK(old)]--;
  } else {
    live_total++;
  }
  return true;
}

// Moves the live sectors out of the fullest-of-stale block and marks it for
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
    int32_t i = hfind(sh->key);
    if (i < 0 || h_loc[i] != LOC(v, s)) continue;  // stale
    memcpy(move_buf, FS_XIP(data_off(v, s)), FS_SECTOR);
    if (!put_slot(sh->key, move_buf, next_seq++)) return false;
  }
  blk_state[v] = B_ERASE;  // erased when next needed (or by fstore_idle)
  blk_used[v] = 0;
  blk_live[v] = 0;
  return true;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------
static bool key_current(uint32_t key) {
  uint32_t sp = FS_KEY_SPACE(key);
  return sp == FS_KEY_SPACE(FS_OBJ_KEY(0)) || sp == FS_KEY_SPACE(FS_USB_KEY(0));
}

void fstore_init(void) {
  crc_init();
  for (uint32_t i = 0; i < FS_HASH; i++) h_key[i] = KEY_EMPTY;
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
      if (sh->magic != SLOT_MAGIC || !key_current(sh->key)) continue;
      if (crc32(FS_XIP(data_off(b, s)), FS_SECTOR) != sh->crc) continue;
      if (sh->seq > max_seq) max_seq = sh->seq;
      int32_t i = hfind(sh->key);
      if (i >= 0) {
        const slot_hdr_t *cur = slot_hdr(LOC_BLK(h_loc[i]), LOC_SLOT(h_loc[i]));
        if (cur->seq >= sh->seq) continue;
      }
      hset(sh->key, LOC(b, s));
    }
    // A block cut short is not appended to again: the next write opens a
    // fresh head, and reclaiming recovers the unused slots.
    if (blk_used[b] < FS_SPB) blk_used[b] = FS_SPB;
  }
  for (uint32_t i = 0; i < FS_HASH; i++) {
    if (h_key[i] == KEY_EMPTY) continue;
    blk_live[LOC_BLK(h_loc[i])]++;
    live_total++;
  }
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if (blk_state[b] == B_USED && blk_live[b] == 0) blk_state[b] = B_ERASE;
  }
  next_seq = max_seq + 1u;
  head = -1;
}

const uint8_t *fstore_lookup(uint32_t key) {
  int32_t i = hfind(key);
  if (i < 0) return NULL;
  uint16_t l = h_loc[i];
  return FS_XIP(data_off(LOC_BLK(l), LOC_SLOT(l)));
}

bool fstore_write(uint32_t key, const uint8_t *data) {
  int32_t i = hfind(key);
  if (i >= 0 && memcmp(FS_XIP(data_off(LOC_BLK(h_loc[i]), LOC_SLOT(h_loc[i]))), data,
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

void fstore_trim(fstore_keep_fn keep, void *ctx) {
  for (uint32_t i = 0; i < FS_HASH;) {
    uint32_t key = h_key[i];
    if (key != KEY_EMPTY) {
      uint16_t l = h_loc[i];
      const slot_hdr_t *sh = slot_hdr(LOC_BLK(l), LOC_SLOT(l));
      if (!keep(key, sh->seq, ctx)) {
        blk_live[LOC_BLK(l)]--;
        live_total--;
        hdel(i);
        continue;  // an entry may have shifted into i
      }
    }
    i++;
  }
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if (blk_state[b] == B_USED && (int32_t)b != head && blk_live[b] == 0) {
      blk_state[b] = B_ERASE;
      blk_used[b] = 0;
    }
  }
}

uint32_t fstore_seq(void) { return next_seq; }

void fstore_idle(void) {
  uint32_t erased = 0;
  int32_t cand = -1;
  for (uint32_t b = 0; b < FS_BLOCKS; b++) {
    if ((int32_t)b == head) continue;
    if (blk_state[b] == B_ERASED) erased++;
    if (blk_state[b] == B_ERASE && (cand < 0 || blk_erases[b] < blk_erases[cand])) {
      cand = (int32_t)b;
    }
  }
  if (erased < 4u && cand >= 0) erase_block((uint32_t)cand);
}

uint32_t fstore_live(void) { return live_total; }
uint32_t fstore_capacity(void) { return FS_LIVE_MAX; }
uint32_t fstore_erases(void) { return erases_since_boot; }
