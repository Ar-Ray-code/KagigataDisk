// TinyUSB configuration: a single mass-storage interface (usbdisk.c).
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_MSC 1
#define CFG_TUD_CDC 0
#define CFG_TUD_HID 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// One sector per callback, so usbdisk.c never sees a partial sector.
#define CFG_TUD_MSC_EP_BUFSIZE 512

#endif  // TUSB_CONFIG_H
