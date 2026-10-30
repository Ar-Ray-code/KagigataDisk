// TinyUSB configuration: a single FIDO HID interface (usb_desc.c, ctaphid.c).
#ifndef TUSB_CONFIG_H
#define TUSB_CONFIG_H

#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE
#define CFG_TUD_ENABLED 1
#define CFG_TUD_ENDPOINT0_SIZE 64

#define CFG_TUD_HID 1
#define CFG_TUD_MSC 0
#define CFG_TUD_CDC 0
#define CFG_TUD_MIDI 0
#define CFG_TUD_VENDOR 0

// CTAPHID packets are 64-byte reports with no report ID.
#define CFG_TUD_HID_EP_BUFSIZE 64

#endif  // TUSB_CONFIG_H
