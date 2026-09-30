#include "usbdisk.h"

#include <stdio.h>
#include <string.h>

#include "devinfo.h"
#include "fstore.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "objstore.h"

// ---------------------------------------------------------------------------
// Geometry: a FAT16 "superfloppy" (no partition table), one 512-byte sector
// per cluster, fixed for good - the volume lives in the flash store and has
// to mean the same thing after every boot.
//
//   cluster  what                              stored
//   2        /command/                         RAM, rebuilt every boot
//   3..      /spi_virtual_device/ (16, grows)  flash
//   19..     /SKILL.md (up to 24),             firmware image
//   43..     spi_virtual_device/README.md (8)
//   51       device_info.txt view              generated (devinfo.c)
//   52..     data area (DATA_N), shared by:
//              views of the SPI host's objects live from the object store;
//                                              PC edits go back to it
//              the PC's own files              flash
//
// Flash is shared with the SPI side: neither has a fixed share. The free
// space the PC sees is set to what flash has left each time it mounts the
// drive (rebalance_data_area()); clusters beyond that are held back as bad.
// The SPI side is refused a write only when flash is really full
// (objstore.c). The data area is sized to the flash plus the views' growth
// room, so little is held back (and counted as used) on top.
// ---------------------------------------------------------------------------
#define UD_SECTOR 512u
#define UD_ROOT_ENTRIES 128u
#define UD_ROOT_SECTORS (UD_ROOT_ENTRIES * 32u / UD_SECTOR)

#define CMD_CL 2u
#define SPIDIR_CL (CMD_CL + 1u)
#define SPIDIR_N 16u
#define SKILL_CL (SPIDIR_CL + SPIDIR_N)
#define SKILL_N 32u
#define SKILL_MAX 24u                      // SKILL.md itself
#define README_CL (SKILL_CL + SKILL_MAX)   // the folder's README.md
#define README_N (SKILL_N - SKILL_MAX)
#define DEVINFO_CL (SKILL_CL + SKILL_N)
#define DATA_CL (DEVINFO_CL + 1u)
#define DATA_N 4100u  // flash (~2900) + views' growth room; >= 4085 in all: FAT16
#define N_CLUSTERS (DATA_CL - 2u + DATA_N)
#define END_CL (N_CLUSTERS + 2u)

#define UD_FAT_SECTORS (((N_CLUSTERS + 2u) * 2u + UD_SECTOR - 1u) / UD_SECTOR)
#define LBA_FAT1 1u
#define LBA_FAT2 (LBA_FAT1 + UD_FAT_SECTORS)
#define LBA_ROOT (LBA_FAT2 + UD_FAT_SECTORS)
#define LBA_DATA (LBA_ROOT + UD_ROOT_SECTORS)
#define TOTAL_SECTORS (LBA_DATA + N_CLUSTERS)

_Static_assert(N_CLUSTERS >= 4085u && N_CLUSTERS <= 65524u, "must be FAT16");

// Views: one per SPI-host object, laid out at boot and at every mount.
#define VIEWS_MAX 15u                       // fits the 4-bit tag in vmap[]
#define VIEW_MAX_N (OBJ_MAX_BYTES / UD_SECTOR)
#define VIEW_SLACK 32u  // growth room (16 KiB) until the next mount
_Static_assert(VIEW_MAX_N <= 0x0FFFu, "view offsets are kept in 12 bits");

// Write-back cache in front of the flash store.
#define UD_OVL_SLOTS 256u  // 128 KiB
#define FLUSH_AGE_MS OBJ_FLUSH_AGE_MS
#define FLUSH_SECTORS 64u
// The PC's files are published to the SPI side once its writes have been quiet
// this long and are all in flash.
#define SYNC_IDLE_MS 1500u
#define REFRESH_MS 1000u
#define REFRESH_QUIET_MS 3000u
#define TRIM_PERIOD_MS 60000u
// Delay between deleting a command file and acting on it, so the PC gets its
// write acknowledged first - and, before update-firmware's reboot, has time to
// unmount (tools/boot0.sh does).
#define CMD_DELAY_MS 1000u
// A file manager's trash note is looked for once the PC's writes pause this long.
#define TRASH_QUIET_MS 300u
// A mount (the PC reading LBA 0) lays the views out again - not twice for a
// probe and the mount right behind it.
#define LAYOUT_GAP_MS 5000u
// Flash kept out of both sides' free space, for metadata and reclaiming.
#define FLASH_MARGIN 64u
// PC files shown to the SPI host, in sectors, all together.
#define MIRROR_SECTORS (OBJ_MAX_BYTES / UD_SECTOR)

#define FAT_DATE ((46u << 9) | (8u << 5) | 31u)  // 2026-08-31
#define FAT_TIME (12u << 11)
#define FAT16_BAD 0xFFF7u
#define FAT16_EOC 0xFFFFu

#define ATTR_RO 0x01u
#define ATTR_HIDDEN 0x02u
#define ATTR_SYSTEM 0x04u
#define ATTR_LABEL 0x08u
#define ATTR_DIR 0x10u
#define ATTR_ARCHIVE 0x20u
#define ATTR_LFN 0x0Fu
#define NTCASE_LOWER_BASE 0x08u
#define NTCASE_LOWER_EXT 0x10u

#define UD_NAME_MAX 64u        // longest file name handled, ASCII
#define UD_DIR_MAX_SECTORS 64u

extern const uint8_t skill_md_start[];
extern const uint8_t skill_md_end[];
extern const uint8_t readme_md_start[];
extern const uint8_t readme_md_end[];

static uint32_t skill_len(void) { return (uint32_t)(skill_md_end - skill_md_start); }
static uint32_t readme_len(void) {
  uint32_t n = (uint32_t)(readme_md_end - readme_md_start);
  return n < README_N * 512u ? n : README_N * 512u;
}
static uint32_t readme_clusters(void) { return (readme_len() + 511u) / 512u; }

// ---------------------------------------------------------------------------
// Storage
// ---------------------------------------------------------------------------
static uint8_t bootsec[UD_SECTOR];
static uint8_t fat[UD_FAT_SECTORS * UD_SECTOR];  // FAT #2 mirrors this one
static uint8_t root[UD_ROOT_SECTORS * UD_SECTOR];
static uint8_t cmd_dir[UD_SECTOR];
static bool fat_dirty[UD_FAT_SECTORS];
static bool root_dirty[UD_ROOT_SECTORS];

// PC-written data sectors not yet in flash (or kept as a read cache).
#define SLOT_EMPTY 0xFFFFu
static uint16_t ovl_lba[UD_OVL_SLOTS];
static bool ovl_dirty[UD_OVL_SLOTS];
static uint8_t ovl_buf[UD_OVL_SLOTS][UD_SECTOR];
static uint32_t ovl_victim;

// A view put back later only counts once the PC has read the FAT sector
// holding it from the device (a fresh mount does; a PC still running on a
// cached FAT does not). Until then, the PC freeing it is just its stale FAT
// being written back, and must not delete anything on the SPI side.
typedef enum { VIEW_OFF, VIEW_PENDING, VIEW_ON } view_state_t;

// An SPI-host object shown in the folder. Its clusters (vcl[base..base+n))
// read as the object; what the PC writes into them is an edit, copied back
// to the object once the writes settle. When the PC frees them (deleted, or
// rewrote the file elsewhere) the view is off and they are plain storage for
// the PC until the next mount lays the views out again.
typedef struct {
  char name[EMU_NAME_MAX + 1];
  uint16_t base, n;
  uint8_t state;
  bool written;  // the PC wrote into the view: the object is to follow
  bool freed;    // the PC freed it: deleted or rewritten, sync_pass() tells
  bool pc_file;  // the folder holds the PC's own copy (already in the object)
} cview_t;
static cview_t views[VIEWS_MAX];
static uint32_t n_views;
static uint16_t vcl[DATA_N];   // the views' clusters, view by view
static uint16_t vmap[DATA_N];  // cluster -> (view + 1) << 12 | offset, 0 = none

static view_state_t devinfo_view, readme_view;
static bool views_missing;  // lay out again at the next mount
static uint32_t last_layout_ms;

static uint32_t unsaved_since_ms;
static bool have_unsaved;
static uint32_t last_write_ms;
static bool mirror_stale;
static uint32_t next_refresh_ms;
static uint32_t next_trim_ms;
static uint32_t trim_mark;
static uint32_t n_mirrored;
static bool write_failed;  // since the last sync pass
static bool flash_full;
static char status[48] = "ok";

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }

// ---------------------------------------------------------------------------
// Little helpers
// ---------------------------------------------------------------------------
static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}

static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (get16(p + 2) << 16); }

static uint32_t fat_get(uint32_t c) { return get16(&fat[c * 2u]); }
static void fat_set(uint32_t c, uint32_t v) {
  put16(&fat[c * 2u], (uint16_t)v);
  fat_dirty[c * 2u / UD_SECTOR] = true;
}

static void fat_chain(uint32_t first, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) fat_set(first + i, i + 1u < n ? first + i + 1u : FAT16_EOC);
}

static void fat_fill(uint32_t first, uint32_t n, uint32_t v) {
  for (uint32_t i = 0; i < n; i++) fat_set(first + i, v);
}

static bool cluster_ok(uint32_t c) { return c >= 2u && c < END_CL; }
static uint32_t cluster_lba(uint32_t c) { return LBA_DATA + (c - 2u); }
static bool in_range(uint32_t c, uint32_t first, uint32_t n) { return c >= first && c < first + n; }

static void set_status(const char *what, const char *name) {
  snprintf(status, sizeof(status), "%s%s%s", what, name ? " " : "", name ? name : "");
}

static void mark_unsaved(void) {
  if (!have_unsaved) {
    have_unsaved = true;
    unsaved_since_ms = now_ms();
  }
}

// ---------------------------------------------------------------------------
// Which cluster is what
// ---------------------------------------------------------------------------
// The view a cluster belongs to (-1: none) and its offset in the file.
static int view_of(uint32_t c, uint32_t *off) {
  if (!in_range(c, DATA_CL, DATA_N)) return -1;
  uint32_t m = vmap[c - DATA_CL];
  if (!m) return -1;
  if (off) *off = m & 0x0FFFu;
  return (int)(m >> 12) - 1;
}

static bool view_live(int v) { return v >= 0 && views[v].state != VIEW_OFF; }

// Clusters the PC stores data in: its folder, the user area, and anything
// the drive's own files let go of (the PC deleted or rewrote them - a PC
// reuses the lowest free clusters first).
static bool stored_cluster(uint32_t c) {
  if (in_range(c, SPIDIR_CL, SPIDIR_N)) return true;
  if (in_range(c, DATA_CL, DATA_N)) return !view_live(view_of(c, NULL));
  if (c == DEVINFO_CL) return devinfo_view == VIEW_OFF;
  if (in_range(c, README_CL, readme_clusters())) return readme_view == VIEW_OFF;
  return false;
}

// Clusters the drive shows files in (not the PC's own).
static bool own_cluster(uint32_t c) {
  if (c == DEVINFO_CL) return devinfo_view != VIEW_OFF;
  if (in_range(c, README_CL, README_N)) return readme_view != VIEW_OFF;
  return view_live(view_of(c, NULL));
}

// ---------------------------------------------------------------------------
// Write-back cache
// ---------------------------------------------------------------------------
static int ovl_find(uint32_t lba) {
  for (uint32_t i = 0; i < UD_OVL_SLOTS; i++) {
    if (ovl_lba[i] == lba) return (int)i;
  }
  return -1;
}

static uint32_t unsaved_count(void) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < UD_FAT_SECTORS; i++) n += fat_dirty[i];
  for (uint32_t i = 0; i < UD_ROOT_SECTORS; i++) n += root_dirty[i];
  for (uint32_t i = 0; i < UD_OVL_SLOTS; i++) n += ovl_dirty[i];
  return n;
}

static void flush(void) {
  bool full = false;
  for (uint32_t i = 0; i < UD_FAT_SECTORS; i++) {
    if (!fat_dirty[i]) continue;
    if (fstore_write(FS_USB_KEY(LBA_FAT1 + i), &fat[i * UD_SECTOR])) fat_dirty[i] = false;
    else full = true;
  }
  for (uint32_t i = 0; i < UD_ROOT_SECTORS; i++) {
    if (!root_dirty[i]) continue;
    if (fstore_write(FS_USB_KEY(LBA_ROOT + i), &root[i * UD_SECTOR])) root_dirty[i] = false;
    else full = true;
  }
  for (uint32_t i = 0; i < UD_OVL_SLOTS; i++) {
    if (!ovl_dirty[i]) continue;
    if (fstore_write(FS_USB_KEY(ovl_lba[i]), ovl_buf[i])) ovl_dirty[i] = false;
    else full = true;
  }
  flash_full = full;
  have_unsaved = false;
}

// A slot for `lba`: its own, an empty one, or a saved one to reuse. When
// every slot is waiting for flash, saves them all first.
static uint8_t *ovl_claim(uint32_t lba) {
  int i = ovl_find(lba);
  if (i < 0) {
    for (int pass = 0; pass < 2 && i < 0; pass++) {
      for (uint32_t n = 0; n < UD_OVL_SLOTS; n++) {
        uint32_t k = (ovl_victim + n) % UD_OVL_SLOTS;
        if (ovl_lba[k] == SLOT_EMPTY || !ovl_dirty[k]) {
          i = (int)k;
          break;
        }
      }
      if (i < 0) flush();
    }
    if (i < 0) return NULL;
    ovl_victim = (uint32_t)i + 1u;
    ovl_lba[i] = (uint16_t)lba;
  }
  ovl_dirty[i] = true;
  mark_unsaved();
  return ovl_buf[i];
}

static const uint8_t *stored_data(uint32_t lba) {
  int i = ovl_find(lba);
  return i >= 0 ? ovl_buf[i] : fstore_lookup(FS_USB_KEY(lba));
}

uint32_t usbdisk_ram_used(void) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < UD_OVL_SLOTS; i++) n += ovl_dirty[i];
  return n;
}

uint32_t usbdisk_ram_total(void) { return UD_OVL_SLOTS; }

// ---------------------------------------------------------------------------
// Sector access
// ---------------------------------------------------------------------------
static void read_data(uint32_t c, uint8_t *buf) {
  memset(buf, 0, UD_SECTOR);
  uint32_t lba = cluster_lba(c), off = 0;
  int v = view_of(c, &off);
  if (c == CMD_CL) {
    memcpy(buf, cmd_dir, UD_SECTOR);
  } else if (stored_cluster(c)) {
    const uint8_t *p = stored_data(lba);
    if (p) memcpy(buf, p, UD_SECTOR);
  } else if (in_range(c, SKILL_CL, SKILL_N)) {
    bool readme = c >= README_CL;
    const uint8_t *blob = readme ? readme_md_start : skill_md_start;
    uint32_t len = readme ? readme_len() : skill_len();
    uint32_t o = (c - (readme ? README_CL : SKILL_CL)) * UD_SECTOR;
    if (o < len) memcpy(buf, blob + o, len - o < UD_SECTOR ? len - o : UD_SECTOR);
  } else if (c == DEVINFO_CL) {
    memcpy(buf, devinfo_text(), DEVINFO_SIZE < UD_SECTOR ? DEVINFO_SIZE : UD_SECTOR);
  } else if (view_live(v)) {
    // What the PC wrote there (not yet in the object), else the object.
    const uint8_t *p = stored_data(lba);
    if (p) {
      memcpy(buf, p, UD_SECTOR);
    } else {
      objstore_host_page(views[v].name, off * UD_SECTOR, buf);
    }
  }
}

static void read_sector(uint32_t lba, uint8_t *buf) {
  if (lba == 0) {
    memcpy(buf, bootsec, UD_SECTOR);
  } else if (lba < LBA_FAT2) {
    memcpy(buf, &fat[(lba - LBA_FAT1) * UD_SECTOR], UD_SECTOR);
  } else if (lba < LBA_ROOT) {
    memcpy(buf, &fat[(lba - LBA_FAT2) * UD_SECTOR], UD_SECTOR);
  } else if (lba < LBA_DATA) {
    memcpy(buf, &root[(lba - LBA_ROOT) * UD_SECTOR], UD_SECTOR);
  } else {
    read_data(lba - LBA_DATA + 2u, buf);
  }
}

static void notice_freed_views(void);

// Stores a sector, as the PC or the drive itself writes it. False for the
// drive's read-only files, or when flash is full and the cache with it.
static bool store_sector(uint32_t lba, const uint8_t *buf) {
  if (lba == 0) {
    memcpy(bootsec, buf, UD_SECTOR);  // RAM only: rebuilt every boot
    return true;
  }
  if (lba < LBA_ROOT) {
    uint32_t i = (lba < LBA_FAT2 ? lba - LBA_FAT1 : lba - LBA_FAT2);
    uint8_t *dst = &fat[i * UD_SECTOR];
    if (memcmp(dst, buf, UD_SECTOR) != 0) {
      fat_dirty[i] = true;
      mark_unsaved();
    }
    memcpy(dst, buf, UD_SECTOR);
    notice_freed_views();
    return true;
  }
  if (lba < LBA_DATA) {
    uint32_t i = lba - LBA_ROOT;
    uint8_t *dst = &root[i * UD_SECTOR];
    if (memcmp(dst, buf, UD_SECTOR) != 0) {
      root_dirty[i] = true;
      mark_unsaved();
    }
    memcpy(dst, buf, UD_SECTOR);
    return true;
  }
  uint32_t c = lba - LBA_DATA + 2u;
  uint8_t *dst;
  int v = view_of(c, NULL);
  if (c == CMD_CL) {
    dst = cmd_dir;
  } else if (view_live(v)) {
    dst = ovl_claim(lba);  // an edit of the object (sync_pass())
    if (!dst) return false;
    views[v].written = true;
  } else if (stored_cluster(c)) {
    dst = ovl_claim(lba);
    if (!dst) return false;
  } else {
    return false;
  }
  memcpy(dst, buf, UD_SECTOR);
  return true;
}

// ---------------------------------------------------------------------------
// Directory entries
// ---------------------------------------------------------------------------
static uint8_t sfn_checksum(const uint8_t *sfn) {
  uint8_t s = 0;
  for (int i = 0; i < 11; i++) s = (uint8_t)(((s & 1u) << 7) + (s >> 1) + sfn[i]);
  return s;
}

// Byte offsets of the 13 UCS-2 characters inside an LFN entry.
static const uint8_t lfn_pos[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};

// Appends a directory entry (with long-name entries ahead of it when `lname`
// is given) at dir[*idx...]; returns false if it does not fit in `cap`.
static bool put_entry(uint8_t *dir, uint32_t *idx, uint32_t cap, const char *lname,
                      const char sfn[11], uint8_t attr, uint8_t ntcase, uint32_t cluster,
                      uint32_t size) {
  uint32_t len = lname ? (uint32_t)strlen(lname) : 0u;
  uint32_t nlfn = (len + 12u) / 13u;
  if (*idx + nlfn + 1u > cap) return false;
  uint8_t sum = sfn_checksum((const uint8_t *)sfn);
  for (uint32_t k = nlfn; k >= 1u; k--) {
    uint8_t *e = &dir[(*idx)++ * 32u];
    memset(e, 0, 32);
    e[0] = (uint8_t)(k | (k == nlfn ? 0x40u : 0u));
    e[11] = ATTR_LFN;
    e[13] = sum;
    for (uint32_t j = 0; j < 13u; j++) {
      uint32_t ci = (k - 1u) * 13u + j;
      uint16_t ch = ci < len ? (uint8_t)lname[ci] : (ci == len ? 0x0000u : 0xFFFFu);
      put16(&e[lfn_pos[j]], ch);
    }
  }
  uint8_t *e = &dir[(*idx)++ * 32u];
  memset(e, 0, 32);
  memcpy(e, sfn, 11);
  e[11] = attr;
  e[12] = ntcase;
  put16(e + 14, FAT_TIME);
  put16(e + 16, FAT_DATE);
  put16(e + 18, FAT_DATE);
  put16(e + 22, FAT_TIME);
  put16(e + 24, FAT_DATE);
  put16(e + 26, (uint16_t)cluster);
  put32(e + 28, size);
  return true;
}

// Short name "BASIS~N.EXT" for a long name; `tag` keeps it unique.
static void make_sfn(const char *lname, uint32_t tag, char out[11]) {
  memset(out, ' ', 11);
  const char *dot = strrchr(lname, '.');
  char tail[6];
  int tl = snprintf(tail, sizeof(tail), "~%lu", (unsigned long)tag);
  uint32_t b = 0;
  for (const char *p = lname; *p && p != dot && b + (uint32_t)tl < 8u; p++) {
    char ch = *p;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-') {
      out[b++] = ch;
    }
  }
  if (b == 0) out[b++] = 'F';
  memcpy(&out[b], tail, (size_t)tl);
  if (dot) {
    uint32_t x = 0;
    for (const char *p = dot + 1; *p && x < 3u; p++) {
      char ch = *p;
      if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
      if ((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) out[8 + x++] = ch;
    }
  }
}

typedef struct {
  char name[UD_NAME_MAX];
  bool name_ok;  // plain ASCII, fits UD_NAME_MAX
  uint8_t attr;
  uint32_t cluster;
  uint32_t size;
  uint32_t lba;  // sector holding the short entry
  uint32_t off;  // its byte offset within that sector
  uint32_t idx;  // entry number of the short entry within the directory
  uint32_t run;  // ... and of the first long-name entry ahead of it
} ud_dirent_t;

typedef void (*dir_cb_t)(const ud_dirent_t *e, void *ctx);

static uint32_t dir_sectors(uint32_t cluster, uint32_t *lbas) {
  if (cluster == 0) {
    for (uint32_t i = 0; i < UD_ROOT_SECTORS; i++) lbas[i] = LBA_ROOT + i;
    return UD_ROOT_SECTORS;
  }
  uint32_t n = 0;
  while (cluster_ok(cluster) && n < UD_DIR_MAX_SECTORS) {
    lbas[n++] = cluster_lba(cluster);
    cluster = fat_get(cluster);
  }
  return n;
}

static void sfn_to_name(const uint8_t *e, ud_dirent_t *out) {
  uint32_t n = 0;
  out->name_ok = true;
  for (int i = 0; i < 11; i++) {
    if (i == 8) {
      if (e[8] == ' ') break;
      out->name[n++] = '.';
    }
    uint8_t ch = (i == 0 && e[0] == 0x05) ? 0xE5 : e[i];
    if (ch == ' ') continue;
    if (ch >= 0x80) out->name_ok = false;
    bool lower = (i < 8) ? (e[12] & NTCASE_LOWER_BASE) : (e[12] & NTCASE_LOWER_EXT);
    if (lower && ch >= 'A' && ch <= 'Z') ch = (uint8_t)(ch - 'A' + 'a');
    out->name[n++] = (char)ch;
  }
  out->name[n] = '\0';
}

// Long-name state carried from one directory sector to the next. The name
// itself lives in one shared buffer: a callback may walk another directory,
// but only after the entry it was called for has been fully read.
typedef struct {
  bool valid;
  uint32_t expect;
  uint8_t sum;
  uint32_t run;
} lfn_state_t;
static uint16_t lfn[20u * 13u];

// Calls `cb` for every live file and folder in one directory sector (entries
// first_idx...), with its long name when it has a valid one. Skips the label
// and "."/"..". False at the end of the directory.
static bool walk_sector(const uint8_t *buf, uint32_t lba, uint32_t first_idx, lfn_state_t *st,
                        dir_cb_t cb, void *ctx) {
  for (uint32_t i = 0; i < UD_SECTOR / 32u; i++) {
    const uint8_t *e = &buf[i * 32u];
    uint32_t idx = first_idx + i;
    if (e[0] == 0x00) return false;  // end of directory
    if (e[0] == 0xE5) {
      st->valid = false;
      continue;
    }
    if (e[11] == ATTR_LFN) {
      uint32_t ord = e[0] & 0x1Fu;
      if (e[0] & 0x40u) {
        st->valid = ord >= 1u && ord <= 20u;
        st->sum = e[13];
        st->run = idx;
        memset(lfn, 0xFF, sizeof(lfn));
      } else if (!st->valid || ord != st->expect || e[13] != st->sum) {
        st->valid = false;
      }
      if (st->valid) {
        for (uint32_t j = 0; j < 13u; j++) {
          lfn[(ord - 1u) * 13u + j] = (uint16_t)get16(&e[lfn_pos[j]]);
        }
        st->expect = ord - 1u;
      }
      continue;
    }
    bool use_lfn = st->valid && st->expect == 0 && sfn_checksum(e) == st->sum;
    st->valid = false;
    if ((e[11] & ATTR_LABEL) || e[0] == '.') continue;

    ud_dirent_t d;
    d.attr = e[11];
    d.cluster = get16(e + 26);
    d.size = get32(e + 28);
    d.lba = lba;
    d.off = i * 32u;
    d.idx = idx;
    d.run = use_lfn ? st->run : idx;
    if (use_lfn) {
      uint32_t k = 0;
      d.name_ok = true;
      while (k < sizeof(lfn) / 2u && lfn[k] != 0x0000u && lfn[k] != 0xFFFFu) {
        if (lfn[k] < 0x20u || lfn[k] > 0x7Eu || k + 1u >= UD_NAME_MAX) {
          d.name_ok = false;
          break;
        }
        d.name[k] = (char)lfn[k];
        k++;
      }
      d.name[d.name_ok ? k : 0] = '\0';
    } else {
      sfn_to_name(e, &d);
    }
    cb(&d, ctx);
  }
  return true;
}

// Every live file and folder in a directory (0 = root) - see walk_sector().
static void dir_walk(uint32_t dir_cluster, dir_cb_t cb, void *ctx) {
  uint32_t lbas[UD_DIR_MAX_SECTORS];
  uint32_t n = dir_sectors(dir_cluster, lbas);
  lfn_state_t st = {0};
  uint8_t buf[UD_SECTOR];
  for (uint32_t s = 0; s < n; s++) {
    read_sector(lbas[s], buf);
    if (!walk_sector(buf, lbas[s], s * (UD_SECTOR / 32u), &st, cb, ctx)) return;
  }
}

static bool name_eq_nocase(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == *b;
}

// ---------------------------------------------------------------------------
// /command/: one empty file per action, run when the PC deletes it
// ---------------------------------------------------------------------------
static void sync_pass(void);

static void cmd_update_firmware(void) {
  flush();  // keep what the PC wrote just before, and pass it on to the SPI side
  if (mirror_stale) sync_pass();
  objstore_flush();
  reset_usb_boot(0, 0);
}

typedef struct {
  const char *name;
  const char *sfn;  // 11-char short name the drive creates it with
  void (*run)(void);
  bool present;  // as of the PC's latest write to the folder
  bool armed;    // deleted: runs at `due_ms`
  uint32_t due_ms;
  bool trashed;  // already run from its trash note: its disappearance is not news
} command_t;

static command_t commands[] = {
    {"update-firmware", "UPDATE~1   ", cmd_update_firmware, false, false, 0, false},
};
#define N_COMMANDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_cb(const ud_dirent_t *e, void *ctx) {
  bool *present = (bool *)ctx;
  if ((e->attr & ATTR_DIR) || !e->name_ok) return;
  for (uint32_t i = 0; i < N_COMMANDS; i++) {
    if (name_eq_nocase(e->name, commands[i].name)) present[i] = true;
  }
}

// Called after every PC write to the root or command/ directory. Deleting
// (or renaming away) a file arms its action; the drive does not put the file
// back (the PC would not see it before remounting anyway), so to run it again
// create the file, flush, and delete it again.
static void check_commands(void) {
  bool present[N_COMMANDS] = {false};
  dir_walk(CMD_CL, cmd_cb, present);
  for (uint32_t i = 0; i < N_COMMANDS; i++) {
    command_t *c = &commands[i];
    if (c->present && !present[i]) {
      if (c->trashed) {
        c->trashed = false;  // the move to the trash, landing at last
      } else if (!c->armed) {
        c->armed = true;
        c->due_ms = now_ms() + CMD_DELAY_MS;
      }
    }
    c->present = present[i];
  }
}

// ---- Deleting from a file manager ------------------------------------------
// A desktop file manager does not delete: it moves the file into
// /.Trash-<uid>/files/ and records it in /.Trash-<uid>/info/. The PC may hold
// that move back for half a minute, but the record's (empty) placeholder,
// "<name>.trashinfo" or "<name>.<n>.trashinfo", reaches the drive at once -
// so a new one naming a command counts as that command's deletion. Once the
// record's text is there too, its "Path=" must point into command/.
#define TRASH_NOTES_MAX 32u
static uint32_t notes_prev[TRASH_NOTES_MAX], notes_now[TRASH_NOTES_MAX];
static uint32_t n_notes_prev, n_notes_now;
static bool trash_scan_due;
static bool trash_scan_quiet;  // record notes only (at boot: they are old news)

static uint32_t fnv1a(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

static bool note_known(uint32_t h) {
  for (uint32_t i = 0; i < n_notes_prev; i++) {
    if (notes_prev[i] == h) return true;
  }
  return false;
}

static void trash_note_cb(const ud_dirent_t *e, void *ctx) {
  (void)ctx;
  if ((e->attr & ATTR_DIR) || !e->name_ok) return;
  const char *ext = strstr(e->name, ".trashinfo");
  if (!ext) return;
  uint32_t h = fnv1a(e->name);
  if (n_notes_now < TRASH_NOTES_MAX) notes_now[n_notes_now++] = h;
  if (trash_scan_quiet || note_known(h)) return;

  // The trashed file's name: the note's, less ".trashinfo" and a ".<n>"
  // GLib adds when that name is in the trash already.
  char base[UD_NAME_MAX];
  size_t n = (size_t)(ext - e->name);
  if (n >= sizeof(base)) return;
  memcpy(base, e->name, n);
  base[n] = '\0';
  char *dot = strrchr(base, '.');
  if (dot && dot[1]) {
    bool digits = true;
    for (const char *p = dot + 1; *p; p++) digits &= *p >= '0' && *p <= '9';
    if (digits) *dot = '\0';
  }
  // With its text in place, the note also says where the file was.
  if (stored_cluster(e->cluster) && e->size) {
    uint8_t buf[UD_SECTOR + 1];
    read_sector(cluster_lba(e->cluster), buf);
    buf[e->size < UD_SECTOR ? e->size : UD_SECTOR] = 0;
    const char *p = strstr((const char *)buf, "Path=");
    if (p) {
      const char *dir = strstr(p, "command/");
      if (!dir || (dir != p + 5 && dir[-1] != '/')) return;  // trashed from elsewhere
    }
  }
  for (uint32_t i = 0; i < N_COMMANDS; i++) {
    command_t *c = &commands[i];
    if (name_eq_nocase(base, c->name) && c->present && !c->armed && !c->trashed) {
      c->armed = true;
      c->trashed = true;
      c->due_ms = now_ms() + CMD_DELAY_MS;
    }
  }
}

static void trash_info_dir_cb(const ud_dirent_t *e, void *ctx) {
  (void)ctx;
  if ((e->attr & ATTR_DIR) && e->name_ok && name_eq_nocase(e->name, "info")) {
    dir_walk(e->cluster, trash_note_cb, NULL);
  }
}

static void trash_root_cb(const ud_dirent_t *e, void *ctx) {
  (void)ctx;
  if ((e->attr & ATTR_DIR) && e->name_ok && strncmp(e->name, ".Trash", 6) == 0 &&
      stored_cluster(e->cluster)) {
    dir_walk(e->cluster, trash_info_dir_cb, NULL);
  }
}

// Notes that disappear (trash emptied) are forgotten, so the same name
// counts again later.
static void scan_trash(bool quiet) {
  trash_scan_quiet = quiet;
  n_notes_now = 0;
  dir_walk(0, trash_root_cb, NULL);
  memcpy(notes_prev, notes_now, n_notes_now * sizeof(notes_now[0]));
  n_notes_prev = n_notes_now;
  trash_scan_due = false;
}

static void run_due_commands(uint32_t now) {
  for (uint32_t i = 0; i < N_COMMANDS; i++) {
    command_t *c = &commands[i];
    if (c->armed && (int32_t)(now - c->due_ms) >= 0) {
      c->armed = false;
      c->run();
    }
  }
}


// ---------------------------------------------------------------------------
// Views of the SPI host's objects
// ---------------------------------------------------------------------------
static uint32_t view_first(const cview_t *w) { return vcl[w->base]; }

static bool view_chain_intact(const cview_t *w) {
  for (uint32_t k = 0; k < w->n; k++) {
    uint32_t want = k + 1u < w->n ? vcl[w->base + k + 1u] : FAT16_EOC;
    if (fat_get(vcl[w->base + k]) != want) return false;
  }
  return true;
}

static void view_chain(const cview_t *w) {
  for (uint32_t k = 0; k < w->n; k++) {
    fat_set(vcl[w->base + k], k + 1u < w->n ? vcl[w->base + k + 1u] : FAT16_EOC);
  }
  mark_unsaved();
}

// After every PC write to the FAT.
static void notice_freed_views(void) {
  for (uint32_t v = 0; v < n_views; v++) {
    cview_t *w = &views[v];
    if (w->state == VIEW_OFF) continue;
    bool gone = fat_get(view_first(w)) == 0;
    if (w->state == VIEW_PENDING) {
      if (gone || !view_chain_intact(w)) view_chain(w);  // a stale FAT written back
    } else if (gone) {
      w->state = VIEW_OFF;  // deleted or rewritten: sync_pass() tells which
      w->freed = true;
      views_missing = true;
    } else if (!view_chain_intact(w)) {
      w->written = true;  // cut short in place (a truncate): an edit too
    }
  }
  if (devinfo_view != VIEW_OFF && fat_get(DEVINFO_CL) == 0) {
    if (devinfo_view == VIEW_ON) {
      devinfo_view = VIEW_OFF;  // nothing to delete: it is generated
      views_missing = true;
    } else {
      fat_set(DEVINFO_CL, FAT16_EOC);
    }
  }
  if (readme_view != VIEW_OFF && fat_get(README_CL) == 0) {
    if (readme_view == VIEW_ON) {
      readme_view = VIEW_OFF;  // the firmware's own text: back at the next mount
      views_missing = true;
    } else {
      fat_chain(README_CL, readme_clusters());
    }
  }
}

// The PC reading a FAT sector from the device sees what is in it now.
static void notice_fat_read(uint32_t fat_sector) {
  for (uint32_t v = 0; v < n_views; v++) {
    cview_t *w = &views[v];
    if (w->state == VIEW_PENDING && view_first(w) * 2u / UD_SECTOR == fat_sector) w->state = VIEW_ON;
  }
  if (devinfo_view == VIEW_PENDING && DEVINFO_CL * 2u / UD_SECTOR == fat_sector) devinfo_view = VIEW_ON;
  if (readme_view == VIEW_PENDING && README_CL * 2u / UD_SECTOR == fat_sector) readme_view = VIEW_ON;
}

// Forgets the PC's edits kept in one view's clusters: the object has them.
static int trim_view;
static bool view_edit_keep(uint32_t key, uint32_t seq, void *ctx) {
  (void)seq;
  (void)ctx;
  if (FS_KEY_SPACE(key) != FS_KEY_SPACE(FS_USB_KEY(0))) return true;
  uint32_t lba = FS_KEY_INDEX(key);
  return lba < LBA_DATA || view_of(lba - LBA_DATA + 2u, NULL) != trim_view;
}

static void drop_view_edits(int v) {
  for (uint32_t i = 0; i < UD_OVL_SLOTS; i++) {
    if (ovl_lba[i] != SLOT_EMPTY && ovl_lba[i] >= LBA_DATA &&
        view_of(ovl_lba[i] - LBA_DATA + 2u, NULL) == v) {
      ovl_lba[i] = SLOT_EMPTY;
      ovl_dirty[i] = false;
    }
  }
  trim_view = v;
  fstore_trim(view_edit_keep, NULL);
  views[v].written = false;
}

// The folder's copy of an object, sector by sector.
typedef struct {
  uint16_t lbas[VIEW_MAX_N];
  uint32_t n;
} copy_src_t;
static copy_src_t copy_src;

static void copy_fill(void *ctx, uint32_t off, uint8_t *buf) {
  const copy_src_t *src = (const copy_src_t *)ctx;
  uint32_t k = off / UD_SECTOR;
  if (k < src->n) read_sector(src->lbas[k], buf);
}

// Makes the object `name` what the folder's copy (starting at `first`, `size`
// bytes) holds now - unless it already does, spelling of the name included.
static bool copy_to_object(const char *name, uint32_t first, uint32_t size) {
  if (size > OBJ_MAX_BYTES) {
    set_status("not passed to the SPI side: too big", name);
    return false;
  }
  if (!objstore_name_ok(name)) {
    set_status("not passed to the SPI side: unusable name", name);
    return false;
  }
  copy_src.n = (size + UD_SECTOR - 1u) / UD_SECTOR;
  uint32_t c = first;
  for (uint32_t k = 0; k < copy_src.n; k++) {
    if (!cluster_ok(c) || (!stored_cluster(c) && !own_cluster(c))) return false;  // mid-write
    copy_src.lbas[k] = (uint16_t)cluster_lba(c);
    c = fat_get(c);
  }
  uint32_t obj_size;
  char spelled[EMU_NAME_MAX + 1] = "";
  for (uint32_t i = 0; objstore_host_object(i, spelled, &obj_size); i++) {
    if (objstore_name_eq(spelled, name)) break;
    spelled[0] = '\0';
  }
  bool same = spelled[0] && strcmp(spelled, name) == 0 && obj_size == size;
  static uint8_t a[UD_SECTOR], b[UD_SECTOR];
  for (uint32_t k = 0; same && k < copy_src.n; k++) {
    copy_fill(&copy_src, k * UD_SECTOR, a);
    objstore_host_page(name, k * UD_SECTOR, b);
    uint32_t n = size - k * UD_SECTOR < UD_SECTOR ? size - k * UD_SECTOR : UD_SECTOR;
    same = memcmp(a, b, n) == 0;
  }
  if (same) return true;
  if (!objstore_host_replace(name, size, copy_fill, &copy_src)) {
    set_status("not passed to the SPI side: flash full", name);
    return false;
  }
  return true;
}

// What the folder shows for one object once the PC's writes settled.
typedef struct {
  bool found;
  uint32_t cluster, size;
  char name[EMU_NAME_MAX + 1];  // how the PC spells it
} seen_t;

static void sync_view(int v, const seen_t *s) {
  cview_t *w = &views[v];
  if (!s->found) {
    // Gone from the folder: deleted - the view itself, or the PC's own copy
    // that replaced it.
    if (w->freed || w->pc_file) objstore_host_delete(w->name);
    w->freed = w->pc_file = false;
    return;
  }
  w->freed = false;
  if (w->state != VIEW_OFF && s->cluster == view_first(w)) {
    if (w->written && copy_to_object(s->name, s->cluster, s->size)) {
      drop_view_edits(v);
      if (!view_chain_intact(w)) {
        // Room to grow again - but the PC keeps its shorter chain until it
        // remounts, so only count on it once the PC has read it back.
        view_chain(w);
        w->state = VIEW_PENDING;
      }
    }
  } else if (copy_to_object(s->name, s->cluster, s->size)) {
    // The PC's own file (a rewrite): the view takes its place at the next
    // mount, when the PC rereads the folder.
    w->pc_file = true;
    views_missing = true;
  }
}

// ---------------------------------------------------------------------------
// Publishing /spi_virtual_device/ to the SPI side
// ---------------------------------------------------------------------------
static int view_named(const char *name) {
  for (uint32_t v = 0; v < n_views; v++) {
    if (objstore_name_eq(views[v].name, name)) return (int)v;
  }
  return -1;
}

typedef struct {
  obj_mirror_t files[OBJ_MIRROR_MAX];
  uint32_t n;
  uint32_t lbas_used;
  bool unsure;  // a file was caught mid-update: publish nothing this time
  seen_t seen[VIEWS_MAX];
  seen_t new_log;  // a log.txt the PC made while the SPI host has none
} sync_ctx_t;

static uint16_t sync_lbas[MIRROR_SECTORS];

static void sync_cb(const ud_dirent_t *e, void *ctx) {
  sync_ctx_t *s = (sync_ctx_t *)ctx;
  if (e->attr & (ATTR_DIR | ATTR_HIDDEN | ATTR_SYSTEM)) return;
  if (e->name_ok) {
    int v = view_named(e->name);
    bool log = v < 0 && objstore_name_eq(e->name, OBJ_LOG_NAME) && !objstore_host_find(e->name, &(uint32_t){0});
    seen_t *seen = v >= 0 ? &s->seen[v] : log ? &s->new_log : NULL;
    if (seen) {
      // An object (or a new log.txt, which the SPI host is to append to -
      // not a read-only copy): handled in sync_pass().
      if (!seen->found) {
        seen->found = true;
        seen->cluster = e->cluster;
        seen->size = e->size;
        strncpy(seen->name, e->name, EMU_NAME_MAX);
        seen->name[EMU_NAME_MAX] = '\0';
      }
      return;
    }
  }
  if (own_cluster(e->cluster)) return;  // the drive's own files
  if (!e->name_ok || e->name[0] == '\0') {
    set_status("skipped: non-ASCII name", NULL);
    return;
  }
  if (e->name[0] == '.') return;  // OS metadata such as macOS "._x"
  if (objstore_reserved_name(e->name)) {
    set_status("skipped: reserved name", e->name);
    return;
  }
  if (!objstore_name_ok(e->name)) {
    set_status("skipped: name too long for the SPI side", e->name);
    return;
  }
  uint32_t sz;
  if (objstore_host_find(e->name, &sz)) {
    set_status("skipped: the SPI host has a file of that name", e->name);
    return;
  }
  if (s->n >= OBJ_MIRROR_MAX) {
    set_status("skipped: too many files", e->name);
    return;
  }
  uint32_t need = (e->size + UD_SECTOR - 1u) / UD_SECTOR;
  if (s->lbas_used + need > MIRROR_SECTORS) {
    set_status("skipped: too much to show the SPI host", e->name);
    return;
  }
  uint16_t *lbas = &sync_lbas[s->lbas_used];
  uint32_t c = e->cluster;
  for (uint32_t k = 0; k < need; k++) {
    if (!stored_cluster(c)) {
      s->unsure = true;  // chain shorter than the size: the PC is mid-write
      return;
    }
    lbas[k] = (uint16_t)cluster_lba(c);
    c = fat_get(c);
  }
  obj_mirror_t *m = &s->files[s->n++];
  strncpy(m->name, e->name, EMU_NAME_MAX);
  m->name[EMU_NAME_MAX] = '\0';
  m->size = e->size;
  m->usb_lbas = lbas;
  s->lbas_used += need;
}

static void mirror_sector(uint32_t lba, uint8_t *buf) { read_sector(lba, buf); }

static void sync_pass(void) {
  static sync_ctx_t s;
  memset(&s, 0, sizeof(s));
  // A PC write refused for lack of room is reported by the pass after it.
  set_status(write_failed ? "PC write failed: flash full" : "ok", NULL);
  write_failed = false;
  dir_walk(SPIDIR_CL, sync_cb, &s);
  if (s.unsure) return;  // mirror_stale stays set: try again later
  for (uint32_t v = 0; v < n_views; v++) sync_view((int)v, &s.seen[v]);
  if (s.new_log.found && copy_to_object(s.new_log.name, s.new_log.cluster, s.new_log.size)) {
    views_missing = true;  // the SPI host's log.txt: a view from the next mount on
  }
  n_mirrored = objstore_mirror_publish(s.files, s.n, mirror_sector);
  mirror_stale = false;
}

// ---------------------------------------------------------------------------
// Keeping the objects' sizes current
// ---------------------------------------------------------------------------
static void refresh_cb(const ud_dirent_t *e, void *ctx) {
  (void)ctx;
  uint32_t off;
  int v = view_of(e->cluster, &off);
  // Only a view itself - not a file of the PC's that took over its clusters -
  // and not while the PC's edit, size and all, is for the object.
  if (!view_live(v) || off != 0 || !e->name_ok) return;
  cview_t *w = &views[v];
  if (w->written || w->pc_file || !objstore_name_eq(e->name, w->name)) return;
  uint32_t size;
  if (!objstore_host_find(w->name, &size)) size = 0;
  if (size > w->n * UD_SECTOR) size = w->n * UD_SECTOR;
  if (size == e->size) return;
  uint8_t buf[UD_SECTOR];
  read_sector(e->lba, buf);
  put32(&buf[e->off + 28u], size);
  store_sector(e->lba, buf);
}

// ---------------------------------------------------------------------------
// Laying the drive out
// ---------------------------------------------------------------------------
static void build_bootsec(void) {
  memset(bootsec, 0, sizeof(bootsec));
  bootsec[0] = 0xEB;
  bootsec[1] = 0x3C;
  bootsec[2] = 0x90;
  memcpy(&bootsec[3], "MSWIN4.1", 8);
  put16(&bootsec[11], UD_SECTOR);
  bootsec[13] = 1;  // sectors per cluster
  put16(&bootsec[14], 1);  // reserved sectors
  bootsec[16] = 2;  // FATs
  put16(&bootsec[17], UD_ROOT_ENTRIES);
  put16(&bootsec[19], (uint16_t)TOTAL_SECTORS);
  bootsec[21] = 0xF8;
  put16(&bootsec[22], UD_FAT_SECTORS);
  put16(&bootsec[24], 32);  // sectors per track
  put16(&bootsec[26], 2);   // heads
  bootsec[36] = 0x80;
  bootsec[38] = 0x29;
  put32(&bootsec[39], 0x4B474456u);  // volume ID
  memcpy(&bootsec[43], "KAGIGATADSK", 11);
  memcpy(&bootsec[54], "FAT16   ", 8);
  bootsec[510] = 0x55;
  bootsec[511] = 0xAA;
}

static void dot_entries(uint8_t *dir, uint32_t *idx, uint32_t self) {
  put_entry(dir, idx, 16, NULL, ".          ", ATTR_DIR, 0, self, 0);
  put_entry(dir, idx, 16, NULL, "..         ", ATTR_DIR, 0, 0, 0);
}

// Marks the entries `drop` picks (with their long names) as deleted.
static void drop_entries(uint8_t *dir, uint32_t cap, bool (*drop)(const uint8_t *e)) {
  uint32_t run = 0;  // first entry of the current long-name run
  for (uint32_t i = 0; i < cap; i++) {
    uint8_t *e = &dir[i * 32u];
    if (e[0] == 0x00) break;
    if (e[0] == 0xE5) {
      run = i + 1u;
      continue;
    }
    if (e[11] == ATTR_LFN) continue;
    if (drop(e)) {
      for (uint32_t k = run; k <= i; k++) dir[k * 32u] = 0xE5;
    }
    run = i + 1u;
  }
}

// Where new entries go: after the last one in use.
static uint32_t dir_end(const uint8_t *dir, uint32_t cap) {
  uint32_t end = 0;
  for (uint32_t i = 0; i < cap; i++) {
    if (dir[i * 32u] == 0x00) break;
    if (dir[i * 32u] != 0xE5) end = i + 1u;
  }
  return end;
}

static bool own_root_entry(const uint8_t *e) {
  if (e[11] & ATTR_LABEL) return true;
  uint32_t cl = get16(e + 26);
  return cl == SKILL_CL || cl == CMD_CL || cl == SPIDIR_CL;
}

// The drive's own part of the FAT and root directory, put back at every boot.
// Its entries move to free slots rather than over anything the PC put there.
static void apply_fixed_layout(bool fresh) {
  fat_set(0, 0xFFF8);
  fat_set(1, FAT16_EOC);
  fat_set(CMD_CL, FAT16_EOC);
  if (fresh) fat_chain(SPIDIR_CL, SPIDIR_N);
  uint32_t skill_n = (skill_len() + UD_SECTOR - 1u) / UD_SECTOR;
  if (skill_n > SKILL_MAX) skill_n = SKILL_MAX;
  fat_chain(SKILL_CL, skill_n);
  fat_fill(SKILL_CL + skill_n, SKILL_MAX - skill_n, FAT16_BAD);
  fat_fill(README_CL + readme_clusters(), README_N - readme_clusters(), FAT16_BAD);

  drop_entries(root, UD_ROOT_ENTRIES, own_root_entry);
  uint32_t idx = dir_end(root, UD_ROOT_ENTRIES);
  char sfn[11];
  put_entry(root, &idx, UD_ROOT_ENTRIES, NULL, "KAGIGATADSK", ATTR_LABEL, 0, 0, 0);
  put_entry(root, &idx, UD_ROOT_ENTRIES, NULL, "SKILL   MD ", ATTR_ARCHIVE | ATTR_RO,
            NTCASE_LOWER_EXT, SKILL_CL, skill_len() < SKILL_MAX * UD_SECTOR ? skill_len() : SKILL_MAX * UD_SECTOR);
  put_entry(root, &idx, UD_ROOT_ENTRIES, NULL, "COMMAND    ", ATTR_DIR, NTCASE_LOWER_BASE, CMD_CL, 0);
  memcpy(sfn, "SPI_VI~1   ", 11);
  put_entry(root, &idx, UD_ROOT_ENTRIES, "spi_virtual_device", sfn, ATTR_DIR, 0, SPIDIR_CL, 0);
  for (uint32_t i = 0; i < UD_ROOT_SECTORS; i++) root_dirty[i] = true;
  mark_unsaved();
}

// Flash both sides may still fill.
static uint32_t flash_free(void) {
  uint32_t used = fstore_live() + unsaved_count() + objstore_unsaved() + FLASH_MARGIN;
  uint32_t cap = fstore_capacity();
  return used < cap ? cap - used : 0u;
}

// The PC's free space follows what flash has left: the data area's free
// clusters beyond that are held back as bad. Only while the PC is not
// holding a FAT of its own (at boot, and when it mounts).
static void rebalance_data_area(void) {
  uint32_t want = flash_free(), have = 0;
  for (uint32_t c = DATA_CL; c < END_CL; c++) have += fat_get(c) == 0;
  for (uint32_t c = END_CL - 1u; c >= DATA_CL && have > want; c--) {
    if (fat_get(c) == 0) {
      fat_set(c, FAT16_BAD);
      have--;
    }
  }
  for (uint32_t c = DATA_CL; c < END_CL && have < want; c++) {
    if (fat_get(c) == FAT16_BAD) {
      fat_set(c, 0);
      have++;
    }
  }
  mark_unsaved();
}

static uint8_t dir_buf[UD_DIR_MAX_SECTORS * UD_SECTOR];

// Entries lay_out_views() replaces: the drive's own (by their clusters), and
// the PC's copies of objects (by name - the object has their content).
static bool folder_entry_to_drop(const ud_dirent_t *e) {
  if (e->attr & ATTR_DIR) return false;
  uint32_t cl = e->cluster, sz;
  if (cl == DEVINFO_CL || in_range(cl, README_CL, README_N) || view_live(view_of(cl, NULL))) {
    return true;
  }
  return e->name_ok && (objstore_host_find(e->name, &sz) || objstore_reserved_name(e->name));
}

// Drops those entries (with their long names) from dir_buf; a PC copy's
// clusters go with it.
static void drop_cb(const ud_dirent_t *e, void *ctx) {
  uint8_t *dir = (uint8_t *)ctx;
  if (!folder_entry_to_drop(e)) return;
  uint32_t c = e->cluster;
  for (uint32_t k = 0; stored_cluster(c) && k < N_CLUSTERS; k++) {
    uint32_t next = fat_get(c);
    fat_set(c, 0);
    c = next;
  }
  for (uint32_t k = e->run; k <= e->idx; k++) dir[k * 32u] = 0xE5;
}

// A short name `name` can be shown under exactly: 8.3, one case per part.
static bool plain_83(const char *name, char sfn[11], uint8_t *ntcase) {
  const char *dot = strchr(name, '.');
  if (dot && strchr(dot + 1, '.')) return false;
  size_t nb = dot ? (size_t)(dot - name) : strlen(name), ne = dot ? strlen(dot + 1) : 0;
  if (nb == 0 || nb > 8 || ne > 3 || (dot && ne == 0)) return false;
  bool lo[2] = {false, false}, up[2] = {false, false};
  memset(sfn, ' ', 11);
  for (size_t k = 0; k < nb + (dot ? ne + 1 : 0); k++) {
    char ch = name[k];
    if (&name[k] == dot) continue;
    int part = dot && &name[k] > dot;
    if (ch >= 'a' && ch <= 'z') {
      lo[part] = true;
      ch = (char)(ch - 'a' + 'A');
    } else if (ch >= 'A' && ch <= 'Z') {
      up[part] = true;
    } else if (!((ch >= '0' && ch <= '9') || strchr("!#$%&'()-@^_`{}~", ch))) {
      return false;
    }
    sfn[part ? 8 + (size_t)(&name[k] - dot - 1) : k] = ch;
  }
  if ((lo[0] && up[0]) || (lo[1] && up[1])) return false;
  *ntcase = (uint8_t)((lo[0] ? NTCASE_LOWER_BASE : 0u) | (lo[1] ? NTCASE_LOWER_EXT : 0u));
  return true;
}

static bool sfn_taken(const uint8_t *dir, uint32_t end, const char sfn[11]) {
  for (uint32_t i = 0; i < end; i++) {
    const uint8_t *e = &dir[i * 32u];
    if (e[0] != 0xE5 && e[11] != ATTR_LFN && memcmp(e, sfn, 11) == 0) return true;
  }
  return false;
}

// An entry for `name`: a plain short name when it is one, else a long name
// with a short name no other entry in the folder uses.
static bool put_named(uint8_t *dir, uint32_t *idx, uint32_t cap, const char *name, uint8_t attr,
                      uint32_t cluster, uint32_t size) {
  char sfn[11];
  uint8_t nt = 0;
  if (plain_83(name, sfn, &nt) && !sfn_taken(dir, cap, sfn)) {
    return put_entry(dir, idx, cap, NULL, sfn, attr, nt, cluster, size);
  }
  for (uint32_t tag = 1; tag < 1000u; tag++) {
    make_sfn(name, tag, sfn);
    if (!sfn_taken(dir, cap, sfn)) return put_entry(dir, idx, cap, name, sfn, attr, 0, cluster, size);
  }
  return false;
}

// Shows every SPI-host object (and device_info.txt, README.md) in the
// folder, afresh: at boot, and whenever the PC mounts. `boot`: the PC is
// enumerating anew, so the views count at once.
static void lay_out_views(bool boot) {
  uint32_t lbas[UD_DIR_MAX_SECTORS];
  uint32_t n = dir_sectors(SPIDIR_CL, lbas);
  for (uint32_t s = 0; s < n; s++) read_sector(lbas[s], &dir_buf[s * UD_SECTOR]);
  uint32_t cap = n * UD_SECTOR / 32u;

  // Out with the old: entries, and the view clusters that are still ours.
  lfn_state_t st = {0};
  for (uint32_t s = 0; s < n; s++) {
    if (!walk_sector(&dir_buf[s * UD_SECTOR], lbas[s], s * (UD_SECTOR / 32u), &st, drop_cb, dir_buf)) break;
  }
  for (uint32_t c = DATA_CL; c < END_CL; c++) {
    uint32_t f = fat_get(c);
    if (view_live(view_of(c, NULL)) || f == FAT16_BAD) fat_set(c, 0);  // ours to hand out again
  }
  memset(vmap, 0, sizeof(vmap));
  n_views = 0;

  // In with the objects as they are now.
  uint32_t idx = dir_end(dir_buf, cap), used = 0, next = DATA_CL;
  char name[EMU_NAME_MAX + 1];
  uint32_t size;
  for (uint32_t i = 0; n_views < VIEWS_MAX && objstore_host_object(i, name, &size); i++) {
    uint32_t len = (size + UD_SECTOR - 1u) / UD_SECTOR + VIEW_SLACK;
    if (len > VIEW_MAX_N) len = VIEW_MAX_N;
    cview_t *w = &views[n_views];
    w->base = (uint16_t)used;
    w->n = 0;
    for (; next < END_CL && w->n < len; next++) {
      if (fat_get(next) != 0) continue;  // the PC's data
      vcl[used++] = (uint16_t)next;
      vmap[next - DATA_CL] = (uint16_t)(((n_views + 1u) << 12) | w->n);
      w->n++;
    }
    if (w->n * UD_SECTOR < size) {  // out of data clusters: not shown
      for (uint32_t k = 0; k < w->n; k++) vmap[vcl[w->base + k] - DATA_CL] = 0;
      break;
    }
    memcpy(w->name, name, sizeof(w->name));
    w->state = boot ? VIEW_ON : VIEW_PENDING;
    w->written = w->freed = w->pc_file = false;
    view_chain(w);
    put_named(dir_buf, &idx, cap, w->name, ATTR_ARCHIVE, view_first(w), size);
    n_views++;
  }

  view_state_t fresh = boot ? VIEW_ON : VIEW_PENDING;
  if (devinfo_view == VIEW_OFF && fat_get(DEVINFO_CL) == 0) devinfo_view = fresh;
  if (devinfo_view != VIEW_OFF) {
    fat_set(DEVINFO_CL, FAT16_EOC);
    put_named(dir_buf, &idx, cap, OBJ_DEVINFO_NAME, ATTR_ARCHIVE | ATTR_RO, DEVINFO_CL, DEVINFO_SIZE);
  }
  if (readme_view == VIEW_OFF) {
    bool free_run = true;
    for (uint32_t k = 0; k < readme_clusters(); k++) free_run &= fat_get(README_CL + k) == 0;
    if (free_run) readme_view = fresh;
  }
  if (readme_view != VIEW_OFF) {
    fat_chain(README_CL, readme_clusters());
    put_named(dir_buf, &idx, cap, OBJ_README_NAME, ATTR_ARCHIVE | ATTR_RO, README_CL, readme_len());
  }
  for (uint32_t s = 0; s < n; s++) store_sector(lbas[s], &dir_buf[s * UD_SECTOR]);
  rebalance_data_area();
  views_missing = false;
  last_layout_ms = now_ms();
}

// The PC is mounting the drive afresh (it reads LBA 0 first): pass on what it
// left unsettled, then show the objects as they are now.
static bool usb_keep(uint32_t key, uint32_t seq, void *ctx);

static void on_mount(void) {
  if (now_ms() - last_layout_ms < LAYOUT_GAP_MS) return;
  if (have_unsaved) flush();
  if (mirror_stale) sync_pass();
  // The PC has just unmounted, writing everything out: whatever the FAT has
  // free now is free for good, so flash gets it back before the free space is
  // worked out.
  fstore_trim(usb_keep, NULL);
  lay_out_views(false);
}

static void seed_folder(void) {
  uint32_t lbas[UD_DIR_MAX_SECTORS];
  uint32_t n = dir_sectors(SPIDIR_CL, lbas);
  memset(dir_buf, 0, n * UD_SECTOR);
  uint32_t idx = 0;
  dot_entries(dir_buf, &idx, SPIDIR_CL);
  for (uint32_t s = 0; s < n; s++) store_sector(lbas[s], &dir_buf[s * UD_SECTOR]);
}

static void seed_cmd_dir(void) {
  memset(cmd_dir, 0, sizeof(cmd_dir));
  uint32_t idx = 0;
  dot_entries(cmd_dir, &idx, CMD_CL);
  for (uint32_t i = 0; i < N_COMMANDS; i++) {
    put_entry(cmd_dir, &idx, 16, commands[i].name, commands[i].sfn, ATTR_ARCHIVE, 0, 0, 0);
    commands[i].present = true;
    commands[i].armed = false;
    commands[i].trashed = false;
  }
}

// Sectors the drive may keep in flash: its FAT and root, the PC's data, and
// edits of objects not yet copied back. `ctx` (runtime trim) spares
// anything rewritten since the previous pass.
static bool usb_keep(uint32_t key, uint32_t seq, void *ctx) {
  if (FS_KEY_SPACE(key) != FS_KEY_SPACE(FS_USB_KEY(0))) return true;
  uint32_t lba = FS_KEY_INDEX(key);
  if (lba < LBA_DATA) return (lba >= LBA_FAT1 && lba < LBA_FAT2) || lba >= LBA_ROOT;
  if (ctx && seq >= *(uint32_t *)ctx) return true;
  uint32_t c = lba - LBA_DATA + 2u;
  int v = view_of(c, NULL);
  if (view_live(v)) return views[v].written;
  if (!stored_cluster(c)) return false;
  uint32_t f = fat_get(c);
  if (f != 0 && f != FAT16_BAD) return true;
  int i = ovl_find(lba);
  return i >= 0 && ovl_dirty[i];
}

void usbdisk_init(void) {
  memset(fat, 0, sizeof(fat));
  memset(root, 0, sizeof(root));
  for (uint32_t i = 0; i < UD_OVL_SLOTS; i++) {
    ovl_lba[i] = SLOT_EMPTY;
    ovl_dirty[i] = false;
  }
  have_unsaved = false;

  // What the PC left last time.
  bool fresh = fstore_lookup(FS_USB_KEY(LBA_ROOT)) == NULL;
  if (!fresh) {
    for (uint32_t i = 0; i < UD_FAT_SECTORS; i++) {
      const uint8_t *p = fstore_lookup(FS_USB_KEY(LBA_FAT1 + i));
      if (p) memcpy(&fat[i * UD_SECTOR], p, UD_SECTOR);
    }
    for (uint32_t i = 0; i < UD_ROOT_SECTORS; i++) {
      const uint8_t *p = fstore_lookup(FS_USB_KEY(LBA_ROOT + i));
      if (p) memcpy(&root[i * UD_SECTOR], p, UD_SECTOR);
    }
  }
  memset(fat_dirty, 0, sizeof(fat_dirty));
  memset(root_dirty, 0, sizeof(root_dirty));
  build_bootsec();
  seed_cmd_dir();
  apply_fixed_layout(fresh);
  if (fresh) seed_folder();
  // Old views (from before this boot) count as the drive's: their clusters
  // are put back to use by lay_out_views().
  devinfo_view = VIEW_OFF;
  readme_view = VIEW_OFF;
  if (!fresh) {
    fat_set(DEVINFO_CL, 0);
    fat_fill(README_CL, readme_clusters(), 0);
  }
  fstore_trim(usb_keep, NULL);  // before the free space is worked out
  lay_out_views(true);
  flush();
  trim_mark = fstore_seq();
  mirror_stale = true;
  sync_pass();
  scan_trash(true);  // notes already on the drive are old news
  last_write_ms = now_ms();
  next_refresh_ms = now_ms() + REFRESH_MS;
  next_trim_ms = now_ms() + TRIM_PERIOD_MS;
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
void usbdisk_task(void) {
  uint32_t now = now_ms();
  if (trash_scan_due && now - last_write_ms >= TRASH_QUIET_MS) scan_trash(false);
  run_due_commands(now);
  if (have_unsaved) {
    uint32_t n = unsaved_count();
    if (n == 0) {
      have_unsaved = false;
    } else if (n >= FLUSH_SECTORS || now - unsaved_since_ms >= FLUSH_AGE_MS) {
      flush();
    }
  }
  if (mirror_stale && !have_unsaved && now - last_write_ms >= SYNC_IDLE_MS) sync_pass();
  // Only while the PC leaves the drive alone: it may be halfway through
  // writing a file's data and then its size.
  if ((int32_t)(now - next_refresh_ms) >= 0 && !mirror_stale && !have_unsaved &&
      now - last_write_ms >= REFRESH_QUIET_MS) {
    next_refresh_ms = now + REFRESH_MS;
    dir_walk(SPIDIR_CL, refresh_cb, NULL);
  }
  if ((int32_t)(now - next_trim_ms) >= 0 && !have_unsaved) {
    next_trim_ms = now + TRIM_PERIOD_MS;
    uint32_t mark = trim_mark;
    trim_mark = fstore_seq();
    fstore_trim(usb_keep, &mark);
  }
}

uint32_t usbdisk_flash_free(void) { return flash_free(); }
const char *usbdisk_status(void) { return flash_full ? "flash full" : status; }
uint32_t usbdisk_mirrored(void) { return n_mirrored; }
uint32_t usbdisk_unsaved(void) { return have_unsaved ? unsaved_count() : 0; }

// ---------------------------------------------------------------------------
// Block interface (usb_device.c)
// ---------------------------------------------------------------------------
uint32_t usbdisk_sectors(void) { return TOTAL_SECTORS; }

bool usbdisk_read(uint32_t lba, uint8_t *buf) {
  if (lba >= TOTAL_SECTORS) return false;
  if (lba == 0) on_mount();
  if (lba >= LBA_FAT1 && lba < LBA_ROOT) {
    notice_fat_read(lba < LBA_FAT2 ? lba - LBA_FAT1 : lba - LBA_FAT2);
  }
  read_sector(lba, buf);
  return true;
}

bool usbdisk_write(uint32_t lba, const uint8_t *buf) {
  if (lba >= TOTAL_SECTORS) return false;
  last_write_ms = now_ms();
  mirror_stale = true;
  trash_scan_due = true;
  if (!store_sector(lba, buf)) {
    write_failed = true;
    return false;
  }
  if ((lba >= LBA_ROOT && lba < LBA_DATA) || lba == cluster_lba(CMD_CL)) check_commands();
  return true;
}
