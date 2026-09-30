// The USB device: Raspberry Pi's VID with the Pico SDK's PID and a single
// mass-storage interface, named "RP2350-KagigataDisk", serving the volume
// usbdisk.c builds.
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "usbdisk.h"

#define SECTOR 512u

#define USB_VID 0x2E8A  // Raspberry Pi
#define USB_PID 0x0009  // Raspberry Pi Pico SDK

enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL };

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,  // per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USB_VID,
    .idProduct = USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

#define EP_MSC_OUT 0x01
#define EP_MSC_IN 0x81
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_MSC_DESCRIPTOR(0, 0, EP_MSC_OUT, EP_MSC_IN, 64),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_config;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  static uint16_t desc[1 + 32];
  static char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
  const char *s;
  switch (index) {
    case STR_LANG:
      desc[1] = 0x0409;  // English (US)
      desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | 4);
      return desc;
    case STR_MANUFACTURER:
      s = "Raspberry Pi";
      break;
    case STR_PRODUCT:
      s = "RP2350-KagigataDisk";
      break;
    case STR_SERIAL:
      pico_get_unique_board_id_string(serial, sizeof(serial));
      s = serial;
      break;
    default:
      return NULL;
  }
  size_t n = strlen(s);
  if (n > 32) n = 32;
  for (size_t i = 0; i < n; i++) desc[1 + i] = (uint8_t)s[i];
  desc[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return desc;
}

// ---------------------------------------------------------------------------
// Mass-storage class callbacks
// ---------------------------------------------------------------------------
static bool ejected;  // by the PC, until it loads the medium again or replug

static void pad_copy(uint8_t *dst, const char *src, size_t n) {
  size_t l = strlen(src);
  memset(dst, ' ', n);
  memcpy(dst, src, l < n ? l : n);
}

// Linux names the disk after these: /dev/disk/by-id/usb-RP2350_KagigataDisk_*.
void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
  (void)lun;
  pad_copy(vendor_id, "RP2350", 8);
  pad_copy(product_id, "KagigataDisk", 16);
  pad_copy(product_rev, "1.0", 4);
}

bool tud_msc_test_unit_ready_cb(uint8_t lun) {
  if (ejected) {
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  // medium not present
    return false;
  }
  return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
  (void)lun;
  *block_count = usbdisk_sectors();
  *block_size = SECTOR;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject) {
  (void)lun;
  (void)power_condition;
  if (load_eject) ejected = !start;
  return true;
}

bool tud_msc_is_writable_cb(uint8_t lun) {
  (void)lun;
  return true;
}

// CFG_TUD_MSC_EP_BUFSIZE is one sector, so every call is exactly one sector.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize) {
  (void)lun;
  if (offset != 0 || bufsize < SECTOR || !usbdisk_read(lba, (uint8_t *)buffer)) return -1;
  return SECTOR;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize) {
  (void)lun;
  if (offset != 0 || bufsize < SECTOR || !usbdisk_write(lba, buffer)) return -1;
  return SECTOR;
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
  (void)buffer;
  (void)bufsize;
  switch (scsi_cmd[0]) {
    case 0x35:  // SYNCHRONIZE CACHE (10): nothing is cached here
      return 0;
    default:
      tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
      return -1;
  }
}
