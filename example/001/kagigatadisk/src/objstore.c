#include "objstore.h"

#include <string.h>

#include "devinfo.h"
#include "fstore.h"
#include "pico/time.h"
#include "usbdisk.h"

// ---------------------------------------------------------------------------
// Layout in the flash store
//
//   FS_OBJ_KEY(id, page)          page `page` of host object `id` (0..31)
//   FS_OBJ_KEY(TABLE_ID, k)       the object table, TABLE_SECTORS sectors
//
// The table holds each object's name, size and the store's sequence number
// when it was created. A page is part of an object only if it lies below the
// object's size and was written after the object was created, so pages left
// behind by a deleted or truncated object never come back - not even after a
// reset, when the store has forgotten that they were dropped (fstore_trim()
// only forgets in RAM; objstore_init() trims again).
// ---------------------------------------------------------------------------
#define TABLE_ID 0xFFu
#define TABLE_SECTORS 3u
#define TABLE_MAGIC 0x3154424Fu  // "OBT1"
#define PAGES_MAX (OBJ_MAX_BYTES / OBJ_PAGE)
_Static_assert(PAGES_MAX <= 0xFFFFu, "page numbers are kept in 16 bits");

typedef struct {
  char name[EMU_NAME_MAX + 1];  // "" = free
  uint32_t size;
  uint32_t create_seq;
} obj_ent_t;

#define ENTS_PER_SECTOR ((OBJ_PAGE - 4u) / sizeof(obj_ent_t))
_Static_assert(ENTS_PER_SECTOR * TABLE_SECTORS >= OBJ_HOST_MAX, "table does not fit");

static obj_ent_t tab[OBJ_HOST_MAX];
static bool tab_dirty;

// Flash kept out of the free space, for metadata and reclaiming.
#define FLASH_MARGIN 64u

// ---------------------------------------------------------------------------
// Page cache: writes land here and reach flash in batches.
// ---------------------------------------------------------------------------
#define OC_SLOTS 64u
#define OC_EMPTY 0u  // no FS key is 0: namespaces start at 1
static uint32_t oc_key[OC_SLOTS];
static bool oc_dirty[OC_SLOTS];
static uint8_t oc_buf[OC_SLOTS][OBJ_PAGE];
static uint32_t oc_victim;
static bool have_unsaved;
static uint32_t unsaved_since_ms;
static bool flash_full;

// Mirrors (PC files) and the built-in files.
static obj_mirror_t mirrors[OBJ_MIRROR_MAX];
static uint32_t n_mirrors;
static obj_sector_fn mirror_read;
static const uint8_t *readme;
static uint32_t readme_len;

static uint32_t now_ms(void) { return to_ms_since_boot(get_absolute_time()); }
static uint32_t pages_of(uint32_t size) { return (size + OBJ_PAGE - 1u) / OBJ_PAGE; }
static uint32_t page_key(uint32_t id, uint32_t page) { return FS_OBJ_KEY((id << 16) | page); }

// ---------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

bool objstore_name_eq(const char *a, const char *b) {
  for (; *a && *b; a++, b++) {
    if (lower(*a) != lower(*b)) return false;
  }
  return *a == *b;
}

bool objstore_name_ok(const char *name) {
  size_t n = strlen(name);
  if (n == 0 || n > EMU_NAME_MAX) return false;
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) return false;
  if (name[0] == ' ' || name[n - 1] == ' ' || name[n - 1] == '.') return false;
  for (const char *p = name; *p; p++) {
    if (*p < 0x20 || *p > 0x7E || strchr("/\\:*?\"<>|", *p)) return false;
  }
  return true;
}

bool objstore_reserved_name(const char *name) {
  return objstore_name_eq(name, OBJ_DEVINFO_NAME) || objstore_name_eq(name, OBJ_README_NAME);
}

// ---------------------------------------------------------------------------
// Flash accounting
// ---------------------------------------------------------------------------
static uint32_t dirty_count(void) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < OC_SLOTS; i++) n += oc_dirty[i];
  return n;
}

// Pages either side may still fill (the USB drive's unsaved sectors count
// against it too: the flash is shared).
static uint32_t flash_free_pages(void) {
  uint32_t used = fstore_live() + dirty_count() + (tab_dirty ? TABLE_SECTORS : 0u) +
                  usbdisk_unsaved() + FLASH_MARGIN;
  uint32_t cap = fstore_capacity();
  return used < cap ? cap - used : 0u;
}

uint32_t objstore_available(void) { return flash_free_pages() * OBJ_PAGE; }

// ---------------------------------------------------------------------------
// Saving
// ---------------------------------------------------------------------------
static void save_table(void) {
  static uint8_t sec[OBJ_PAGE];
  bool ok = true;
  for (uint32_t k = 0; k < TABLE_SECTORS; k++) {
    memset(sec, 0, sizeof(sec));
    uint32_t magic = TABLE_MAGIC;
    memcpy(sec, &magic, 4);
    for (uint32_t j = 0; j < ENTS_PER_SECTOR; j++) {
      uint32_t i = k * ENTS_PER_SECTOR + j;
      if (i < OBJ_HOST_MAX) memcpy(sec + 4 + j * sizeof(obj_ent_t), &tab[i], sizeof(obj_ent_t));
    }
    ok &= fstore_write(page_key(TABLE_ID, k), sec);
  }
  if (ok) tab_dirty = false;
  flash_full |= !ok;
}

void objstore_flush(void) {
  flash_full = false;
  // The table first: a size that got ahead of its data reads as zeros after
  // a power cut, never as another object's leftovers.
  if (tab_dirty) save_table();
  for (uint32_t i = 0; i < OC_SLOTS; i++) {
    if (!oc_dirty[i]) continue;
    if (fstore_write(oc_key[i], oc_buf[i])) {
      oc_dirty[i] = false;
    } else {
      flash_full = true;
    }
  }
  have_unsaved = false;
}

void objstore_task(void) {
  if (!have_unsaved) return;
  uint32_t n = dirty_count() + (tab_dirty ? 1u : 0u);
  if (n == 0) {
    have_unsaved = false;
  } else if (n >= OBJ_FLUSH_PAGES || now_ms() - unsaved_since_ms >= OBJ_FLUSH_AGE_MS) {
    objstore_flush();
  }
}

uint32_t objstore_unsaved(void) { return have_unsaved ? dirty_count() + (tab_dirty ? TABLE_SECTORS : 0u) : 0u; }
bool objstore_flash_full(void) { return flash_full; }

static void mark_unsaved(void) {
  if (!have_unsaved) {
    have_unsaved = true;
    unsaved_since_ms = now_ms();
  }
}

static void table_changed(void) {
  tab_dirty = true;
  mark_unsaved();
}

// ---------------------------------------------------------------------------
// Pages
// ---------------------------------------------------------------------------
static int oc_find(uint32_t key) {
  for (uint32_t i = 0; i < OC_SLOTS; i++) {
    if (oc_key[i] == key) return (int)i;
  }
  return -1;
}

static void read_page(uint32_t id, uint32_t page, uint8_t *buf) {
  uint32_t key = page_key(id, page);
  int i = oc_find(key);
  const uint8_t *p = i >= 0 ? oc_buf[i] : fstore_lookup(key);
  if (p) {
    memcpy(buf, p, OBJ_PAGE);
  } else {
    memset(buf, 0, OBJ_PAGE);
  }
}

// A cache slot holding the page, to modify. When every slot is waiting for
// flash, saves them all first. NULL if even that does not free one.
static uint8_t *page_for_write(uint32_t id, uint32_t page) {
  uint32_t key = page_key(id, page);
  int i = oc_find(key);
  if (i < 0) {
    for (int pass = 0; pass < 2 && i < 0; pass++) {
      for (uint32_t n = 0; n < OC_SLOTS; n++) {
        uint32_t k = (oc_victim + n) % OC_SLOTS;
        if (oc_key[k] == OC_EMPTY || !oc_dirty[k]) {
          i = (int)k;
          break;
        }
      }
      if (i < 0) objstore_flush();
    }
    if (i < 0) return NULL;
    oc_victim = (uint32_t)i + 1u;
    const uint8_t *p = fstore_lookup(key);
    if (p) {
      memcpy(oc_buf[i], p, OBJ_PAGE);
    } else {
      memset(oc_buf[i], 0, OBJ_PAGE);
    }
    oc_key[i] = key;
  }
  oc_dirty[i] = true;
  mark_unsaved();
  return oc_buf[i];
}

// Copies `len` bytes (NULL data: zeros) into the object at `off`.
static bool put_bytes(uint32_t id, uint32_t off, const uint8_t *data, uint32_t len) {
  while (len) {
    uint32_t page = off / OBJ_PAGE, in = off % OBJ_PAGE;
    uint32_t n = OBJ_PAGE - in < len ? OBJ_PAGE - in : len;
    uint8_t *p = page_for_write(id, page);
    if (!p) return false;
    if (data) {
      memcpy(p + in, data, n);
      data += n;
    } else {
      memset(p + in, 0, n);
    }
    off += n;
    len -= n;
  }
  return true;
}

// Forgets the object's pages from `from_page` on, cached and stored.
static uint32_t trim_id, trim_from;
static bool trim_keep(uint32_t key, uint32_t seq, void *ctx) {
  (void)seq;
  (void)ctx;
  if (FS_KEY_SPACE(key) != FS_KEY_SPACE(FS_OBJ_KEY(0))) return true;
  uint32_t v = FS_KEY_INDEX(key);
  return (v >> 16) != trim_id || (v & 0xFFFFu) < trim_from;
}

static void drop_pages(uint32_t id, uint32_t from_page) {
  for (uint32_t i = 0; i < OC_SLOTS; i++) {
    if (oc_key[i] == OC_EMPTY) continue;
    uint32_t v = FS_KEY_INDEX(oc_key[i]);
    if ((v >> 16) == id && (v & 0xFFFFu) >= from_page) {
      oc_key[i] = OC_EMPTY;
      oc_dirty[i] = false;
    }
  }
  trim_id = id;
  trim_from = from_page;
  fstore_trim(trim_keep, NULL);
}

// Sets the object's size, zeroing whatever the new size uncovers (bytes past
// the old end are not necessarily zero in flash) or the tail of a page it
// cuts through.
static bool set_size(uint32_t id, uint32_t size) {
  obj_ent_t *e = &tab[id];
  if (size > e->size) {
    if (!put_bytes(id, e->size, NULL, size - e->size)) return false;
  } else if (size < e->size) {
    drop_pages(id, pages_of(size));
    if (size % OBJ_PAGE) {
      uint32_t end = pages_of(size) * OBJ_PAGE;
      if (!put_bytes(id, size, NULL, end - size)) return false;
    }
  }
  e->size = size;
  table_changed();
  return true;
}

// ---------------------------------------------------------------------------
// Objects
// ---------------------------------------------------------------------------
static int host_find(const char *name) {
  for (uint32_t i = 0; i < OBJ_HOST_MAX; i++) {
    if (tab[i].name[0] && objstore_name_eq(tab[i].name, name)) return (int)i;
  }
  return -1;
}

static int mirror_find(const char *name) {
  for (uint32_t i = 0; i < n_mirrors; i++) {
    if (objstore_name_eq(mirrors[i].name, name)) return (int)i;
  }
  return -1;
}

static int host_create(const char *name) {
  for (uint32_t i = 0; i < OBJ_HOST_MAX; i++) {
    if (tab[i].name[0]) continue;
    // Anything still stored under this id is a deleted object's.
    drop_pages(i, 0);
    strncpy(tab[i].name, name, EMU_NAME_MAX);
    tab[i].name[EMU_NAME_MAX] = '\0';
    tab[i].size = 0;
    tab[i].create_seq = fstore_seq();
    table_changed();
    return (int)i;
  }
  return -1;
}

static void host_remove(uint32_t id) {
  drop_pages(id, 0);
  memset(&tab[id], 0, sizeof(tab[id]));
  table_changed();
}

// Pages the object would add by growing to `size`: are they there to take?
static bool room_for(uint32_t id, uint32_t size) {
  uint32_t have = pages_of(tab[id].size), want = pages_of(size);
  return want <= have || want - have <= flash_free_pages();
}

// ---------------------------------------------------------------------------
// The protocol's view
// ---------------------------------------------------------------------------
uint32_t objstore_count(void) {
  uint32_t n = 2u + n_mirrors;  // README.md, device_info.txt
  for (uint32_t i = 0; i < OBJ_HOST_MAX; i++) n += tab[i].name[0] != '\0';
  return n;
}

static void info_of(const char *name, uint32_t size, uint8_t attr, obj_info_t *out) {
  strncpy(out->name, name, EMU_NAME_MAX);
  out->name[EMU_NAME_MAX] = '\0';
  out->size = size;
  out->attr = attr;
}

bool objstore_entry(uint32_t index, obj_info_t *out) {
  for (uint32_t i = 0; i < OBJ_HOST_MAX; i++) {
    if (!tab[i].name[0]) continue;
    if (index-- == 0) {
      info_of(tab[i].name, tab[i].size, 0, out);
      return true;
    }
  }
  if (index-- == 0) {
    info_of(OBJ_README_NAME, readme_len, EMU_ATTR_READ_ONLY, out);
    return true;
  }
  if (index-- == 0) {
    info_of(OBJ_DEVINFO_NAME, DEVINFO_SIZE, EMU_ATTR_READ_ONLY, out);
    return true;
  }
  if (index < n_mirrors) {
    info_of(mirrors[index].name, mirrors[index].size, EMU_ATTR_READ_ONLY, out);
    return true;
  }
  return false;
}

emu_status_t objstore_stat(const char *name, obj_info_t *out) {
  int i = host_find(name);
  if (i >= 0) {
    info_of(tab[i].name, tab[i].size, 0, out);
  } else if (objstore_name_eq(name, OBJ_README_NAME)) {
    info_of(OBJ_README_NAME, readme_len, EMU_ATTR_READ_ONLY, out);
  } else if (objstore_name_eq(name, OBJ_DEVINFO_NAME)) {
    info_of(OBJ_DEVINFO_NAME, DEVINFO_SIZE, EMU_ATTR_READ_ONLY, out);
  } else if ((i = mirror_find(name)) >= 0) {
    info_of(mirrors[i].name, mirrors[i].size, EMU_ATTR_READ_ONLY, out);
  } else {
    return EMU_ST_NOT_FOUND;
  }
  return EMU_ST_OK;
}

emu_status_t objstore_read(const char *name, uint32_t off, uint8_t *buf, uint32_t len,
                           uint32_t *got) {
  static uint8_t page[OBJ_PAGE];
  *got = 0;
  int id = host_find(name), m = -1;
  uint32_t size;
  const uint8_t *blob = NULL;
  if (id >= 0) {
    size = tab[id].size;
  } else if (objstore_name_eq(name, OBJ_README_NAME)) {
    blob = readme;
    size = readme_len;
  } else if (objstore_name_eq(name, OBJ_DEVINFO_NAME)) {
    blob = devinfo_text();
    size = DEVINFO_SIZE;
  } else if ((m = mirror_find(name)) >= 0) {
    size = mirrors[m].size;
  } else {
    return EMU_ST_NOT_FOUND;
  }
  if (off >= size) return EMU_ST_OK;  // at or past the end: nothing
  if (len > size - off) len = size - off;
  if (blob) {
    memcpy(buf, blob + off, len);
    *got = len;
    return EMU_ST_OK;
  }
  while (*got < len) {
    uint32_t p = (off + *got) / OBJ_PAGE, in = (off + *got) % OBJ_PAGE;
    uint32_t n = OBJ_PAGE - in < len - *got ? OBJ_PAGE - in : len - *got;
    if (id >= 0) {
      read_page((uint32_t)id, p, page);
    } else {
      mirror_read(mirrors[m].usb_lbas[p], page);
    }
    memcpy(buf + *got, page + in, n);
    *got += n;
  }
  return EMU_ST_OK;
}

// A name the SPI host may write to: its own object, or a new one.
static emu_status_t writable(const char *name, bool create, int *id_out) {
  int id = host_find(name);
  if (id < 0) {
    if (objstore_reserved_name(name) || mirror_find(name) >= 0) return EMU_ST_READ_ONLY;
    if (!create) return EMU_ST_NOT_FOUND;
    if (!objstore_name_ok(name)) return EMU_ST_INVALID_ARGUMENT;
    if (flash_free_pages() < TABLE_SECTORS) return EMU_ST_NO_SPACE;
    id = host_create(name);
    if (id < 0) return EMU_ST_NO_SPACE;
  }
  *id_out = id;
  return EMU_ST_OK;
}

emu_status_t objstore_write(const char *name, uint32_t off, const uint8_t *data, uint32_t len,
                            bool create, uint32_t *size_out) {
  int id;
  emu_status_t st = writable(name, create, &id);
  if (st != EMU_ST_OK) return st;
  obj_ent_t *e = &tab[id];
  if (off > OBJ_MAX_BYTES || len > OBJ_MAX_BYTES - off) return EMU_ST_NO_SPACE;
  uint32_t end = off + len;
  if (!room_for((uint32_t)id, end > e->size ? end : e->size)) return EMU_ST_NO_SPACE;
  if (off > e->size && !set_size((uint32_t)id, off)) return EMU_ST_NO_SPACE;
  if (!put_bytes((uint32_t)id, off, data, len)) return EMU_ST_NO_SPACE;
  if (end > e->size) {
    e->size = end;
    table_changed();
  }
  *size_out = e->size;
  return EMU_ST_OK;
}

emu_status_t objstore_append(const char *name, const uint8_t *data, uint32_t len,
                             uint32_t *size_out) {
  int id;
  emu_status_t st = writable(name, true, &id);
  if (st != EMU_ST_OK) return st;
  return objstore_write(tab[id].name, tab[id].size, data, len, false, size_out);
}

emu_status_t objstore_truncate(const char *name, uint32_t size) {
  int id;
  emu_status_t st = writable(name, false, &id);
  if (st != EMU_ST_OK) return st;
  if (size > OBJ_MAX_BYTES || !room_for((uint32_t)id, size)) return EMU_ST_NO_SPACE;
  return set_size((uint32_t)id, size) ? EMU_ST_OK : EMU_ST_NO_SPACE;
}

emu_status_t objstore_remove(const char *name) {
  int id;
  emu_status_t st = writable(name, false, &id);
  if (st != EMU_ST_OK) return st;
  host_remove((uint32_t)id);
  return EMU_ST_OK;
}

// ---------------------------------------------------------------------------
// The USB drive's view
// ---------------------------------------------------------------------------
bool objstore_host_object(uint32_t index, char name_out[EMU_NAME_MAX + 1], uint32_t *size_out) {
  for (uint32_t i = 0; i < OBJ_HOST_MAX; i++) {
    if (!tab[i].name[0]) continue;
    if (index-- == 0) {
      memcpy(name_out, tab[i].name, EMU_NAME_MAX + 1);
      *size_out = tab[i].size;
      return true;
    }
  }
  return false;
}

bool objstore_host_find(const char *name, uint32_t *size_out) {
  int id = host_find(name);
  if (id < 0) return false;
  *size_out = tab[id].size;
  return true;
}

void objstore_host_page(const char *name, uint32_t off, uint8_t *buf) {
  int id = host_find(name);
  if (id < 0 || off >= tab[id].size) {
    memset(buf, 0, OBJ_PAGE);
    return;
  }
  read_page((uint32_t)id, off / OBJ_PAGE, buf);
  uint32_t left = tab[id].size - off;
  if (left < OBJ_PAGE) memset(buf + left, 0, OBJ_PAGE - left);
}

bool objstore_host_delete(const char *name) {
  int id = host_find(name);
  if (id < 0) return false;
  host_remove((uint32_t)id);
  return true;
}

bool objstore_host_replace(const char *name, uint32_t size, obj_fill_fn fill, void *ctx) {
  if (size > OBJ_MAX_BYTES || !objstore_name_ok(name)) return false;
  int id = host_find(name);
  if (id < 0) {
    if (flash_free_pages() < pages_of(size) + TABLE_SECTORS) return false;
    id = host_create(name);
    if (id < 0) return false;
  } else if (!room_for((uint32_t)id, size)) {
    return false;
  }
  obj_ent_t *e = &tab[id];
  static uint8_t page[OBJ_PAGE];
  for (uint32_t k = 0; k < pages_of(size); k++) {
    memset(page, 0, sizeof(page));
    fill(ctx, k * OBJ_PAGE, page);
    uint32_t left = size - k * OBJ_PAGE;
    if (left < OBJ_PAGE) memset(page + left, 0, OBJ_PAGE - left);
    uint8_t *dst = page_for_write((uint32_t)id, k);
    if (!dst) return false;
    memcpy(dst, page, OBJ_PAGE);
  }
  if (size < e->size) drop_pages((uint32_t)id, pages_of(size));
  e->size = size;
  strncpy(e->name, name, EMU_NAME_MAX);  // the PC's spelling
  e->name[EMU_NAME_MAX] = '\0';
  table_changed();
  return true;
}

uint32_t objstore_mirror_publish(const obj_mirror_t *files, uint32_t n, obj_sector_fn read) {
  if (n > OBJ_MIRROR_MAX) n = OBJ_MIRROR_MAX;
  memcpy(mirrors, files, n * sizeof(files[0]));
  n_mirrors = n;
  mirror_read = read;
  return n;
}

void objstore_set_readme(const uint8_t *text, uint32_t len) {
  readme = text;
  readme_len = len;
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------
static bool boot_keep(uint32_t key, uint32_t seq, void *ctx) {
  (void)ctx;
  if (FS_KEY_SPACE(key) != FS_KEY_SPACE(FS_OBJ_KEY(0))) return true;
  uint32_t v = FS_KEY_INDEX(key), id = v >> 16, page = v & 0xFFFFu;
  if (id == TABLE_ID) return page < TABLE_SECTORS;
  if (id >= OBJ_HOST_MAX || !tab[id].name[0]) return false;
  return page < pages_of(tab[id].size) && seq >= tab[id].create_seq;
}

void objstore_init(void) {
  memset(tab, 0, sizeof(tab));
  for (uint32_t k = 0; k < TABLE_SECTORS; k++) {
    const uint8_t *sec = fstore_lookup(page_key(TABLE_ID, k));
    uint32_t magic = 0;
    if (sec) memcpy(&magic, sec, 4);
    if (magic != TABLE_MAGIC) continue;
    for (uint32_t j = 0; j < ENTS_PER_SECTOR; j++) {
      uint32_t i = k * ENTS_PER_SECTOR + j;
      if (i >= OBJ_HOST_MAX) break;
      memcpy(&tab[i], sec + 4 + j * sizeof(obj_ent_t), sizeof(obj_ent_t));
      tab[i].name[EMU_NAME_MAX] = '\0';
      if (tab[i].name[0] && (!objstore_name_ok(tab[i].name) || tab[i].size > OBJ_MAX_BYTES)) {
        memset(&tab[i], 0, sizeof(tab[i]));
      }
    }
  }
  for (uint32_t i = 0; i < OC_SLOTS; i++) {
    oc_key[i] = OC_EMPTY;
    oc_dirty[i] = false;
  }
  have_unsaved = false;
  tab_dirty = false;
  flash_full = false;
  n_mirrors = 0;
  fstore_trim(boot_keep, NULL);
  // A fresh board starts with an empty log.txt for the host to append to.
  bool any = false;
  for (uint32_t i = 0; i < OBJ_HOST_MAX; i++) any |= tab[i].name[0] != '\0';
  if (!any && fstore_lookup(page_key(TABLE_ID, 0)) == NULL) {
    host_create(OBJ_LOG_NAME);
    objstore_flush();
  }
}
