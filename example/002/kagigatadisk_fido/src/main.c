// RP2350 "kagigatadisk_fido": KagigataDisk as a FIDO2 (CTAP 2.0) USB security
// key. USB is the only interface: one FIDO HID interface (usb_desc.c) carrying
// CTAPHID (ctaphid.c) and the CTAP2 authenticator (ctap2.c). User presence is
// a touch on the card-edge contacts (touch.c). Credentials are derived from a
// master secret kept with the PIN and the resident credentials in the last
// 64 KiB of flash (store.c, fstore.c).
//
// The firmware is replaced over USB alone: the KG_BOOTSEL vendor command
// (tools/kgfido.py bootsel) reboots into the RP2350 UF2 bootloader, so no one
// ever has to reach the BOOTSEL pad.
#include "board_config.h"
#include "crypto.h"
#include "ctap2.h"
#include "ctaphid.h"
#include "led.h"
#include "pico/stdlib.h"
#include "store.h"
#include "touch.h"
#include "tusb.h"

int main(void) {
  led_init();
  crypto_init();
  touch_init();
  // The store sits right above the end of the firmware's flash. A firmware
  // grown into it must not touch it - but still runs USB, so it can always
  // be replaced (CTAP commands then fail).
  extern char __flash_binary_end;
  if ((uintptr_t)&__flash_binary_end - XIP_BASE <= STORE_FLASH_OFFSET) store_init();
  ctap2_init();
  ctaphid_init();
  tusb_init();

  for (;;) {
    tud_task();
    ctaphid_task();
    led_update();
  }
}
