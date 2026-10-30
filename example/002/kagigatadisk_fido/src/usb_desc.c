// The USB device: one HID interface with the FIDO usage page (0xF1D0) and a
// 64-byte input and output report, as CTAPHID requires. Platforms (browsers,
// libfido2, Windows) find the key by that usage page; Linux's fido_id udev
// helper tags the hidraw node as a security token, which gives the desktop
// user access to it. Identifiers and strings come from the KGFIDO_* build
// variables.
//
// The default USB IDs are quoted from TinyUSB 0.18.0's device example
// examples/device/hid_generic_inout (VID 0xCafe, PID derived from the enabled
// classes: 0x4000 | HID bit = 0x4004), as example/001 quotes msc_dual_lun.
// They are not USB identifiers assigned to KagigataDisk products; a product
// built from this code needs its own (KGFIDO_USB_VID / KGFIDO_USB_PID).
#include <string.h>

#include "kgfido_config.h"
#include "pico/unique_id.h"
#include "tusb.h"

// TinyUSB's example PID scheme, to tell when the defaults are in use.
#define _PID_MAP(itf, n) ((CFG_TUD_##itf) << (n))
#define TINYUSB_EXAMPLE_PID                                                            \
  (0x4000 | _PID_MAP(CDC, 0) | _PID_MAP(MSC, 1) | _PID_MAP(HID, 2) | _PID_MAP(MIDI, 3) | \
   _PID_MAP(VENDOR, 4))
_Static_assert(KGFIDO_USB_VID != 0xCAFE || KGFIDO_USB_PID == TINYUSB_EXAMPLE_PID,
               "with TinyUSB's example VID, keep its PID scheme (HID only: 0x4004)");

enum { STR_LANG, STR_MANUFACTURER, STR_PRODUCT, STR_SERIAL };

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,  // per interface
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = KGFIDO_USB_VID,
    .idProduct = KGFIDO_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUFACTURER,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

static const uint8_t desc_report[] = {TUD_HID_REPORT_DESC_FIDO_U2F(CFG_TUD_HID_EP_BUFSIZE)};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance) {
  (void)instance;
  return desc_report;
}

#define EP_HID_OUT 0x01
#define EP_HID_IN 0x81
#define HID_POLL_MS 5
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

static const uint8_t desc_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, CONFIG_TOTAL_LEN, 0x00, 100),
    TUD_HID_INOUT_DESCRIPTOR(0, 0, HID_ITF_PROTOCOL_NONE, sizeof(desc_report), EP_HID_OUT,
                             EP_HID_IN, CFG_TUD_HID_EP_BUFSIZE, HID_POLL_MS),
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
      s = KGFIDO_MANUFACTURER;
      break;
    case STR_PRODUCT:
      s = KGFIDO_PRODUCT;
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

// No feature/input reports are served over the control pipe.
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0;
}
