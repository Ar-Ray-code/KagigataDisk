// CTAPHID (CTAP 2.0 section 8.1): messages over 64-byte HID reports, split
// into an init packet and continuation packets, on channels the platform
// allocates with INIT. Supported: INIT, PING, WINK, CBOR (-> ctap2.c), CANCEL,
// KEEPALIVE and ERROR; no U2F MSG (the NMSG capability says so) and no LOCK.
//
// Vendor commands (0x40-0x7F), used by tools/kgfido.py:
//   KG_BOOTSEL (0x70)  payload ignored: erase every credential, the PIN and the
//                      master secret, then reboot into the UF2 bootloader so
//                      the firmware can be replaced over USB alone. Needs a
//                      finger held on the contacts for 3 s (all LEDs lit
//                      meanwhile).
//                      Credentials never survive a firmware change.
//                      Response [status]; the reboot follows ~100 ms later.
//   KG_TOUCH (0x71)    touch sensor readings, for checking/tuning the sensor:
//                      [touched, n, n x (gpio, raw u16le, base u16le, threshold u16le)]
//   KG_INFO (0x72)     a line of text: firmware version and settings
#ifndef CTAPHID_H
#define CTAPHID_H

#include <stdint.h>

#define CTAPHID_VENDOR_BOOTSEL 0x70
#define CTAPHID_VENDOR_TOUCH 0x71
#define CTAPHID_VENDOR_INFO 0x72

void ctaphid_init(void);

// Call from the main loop after tud_task().
void ctaphid_task(void);

// Waits up to 30 s for a touch while a command runs, sending KEEPALIVE
// (UPNEEDED) every 100 ms and answering other channels CHANNEL_BUSY.
// Returns a CTAP2 status: CTAP2_OK, CTAP2_ERR_KEEPALIVE_CANCEL (CANCEL or
// INIT on the command's channel) or CTAP2_ERR_USER_ACTION_TIMEOUT.
uint8_t ctaphid_wait_up(void);

#endif  // CTAPHID_H
