#ifndef DEVINFO_H
#define DEVINFO_H

#include <stdint.h>

// /device_info.txt: a small, always-present, read-only text file reporting
// uptime, clocks, the RP2354A's unique board ID and the state of the flash store and the USB drive. The SPI host reads it as an
// object (objstore.c); the USB drive shows it as
// /spi_virtual_device/device_info.txt.
//
// Nothing stores it: it is re-rendered once a second (devinfo_update(), from
// core0's main loop).

// Fixed size, padded with trailing spaces, so the USB drive's directory entry
// never has to change.
#define DEVINFO_SIZE 512u

void devinfo_init(void);
void devinfo_update(void);
const uint8_t *devinfo_text(void);  // DEVINFO_SIZE bytes

#endif  // DEVINFO_H
