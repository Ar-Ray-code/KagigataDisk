// Status LEDs:
//   PIN_LED_USB - blinks at 5 Hz while a USB host has the device configured
//   PIN_LED_TX  - lit while the board sends a response on the SPI link
//   PIN_LED_RX  - lit while the SPI host sends a request
// SPI activity is far too brief to see, so each report keeps its LED lit for
// LED_ACTIVITY_HOLD_MS.
#ifndef LED_H
#define LED_H

#include <stdbool.h>

#include "pico.h"

void led_init(void);

// Call frequently from core0's main loop; it is internally time-gated.
void led_update(void);

// All three LEDs flashing together, for a fault found at boot. One cycle.
void led_blink_error(void);

// Activity reports from core1's link loop. Inline so they stay in RAM with
// the caller: core1 must never execute from flash (see emu_link.c). A report
// lost to the core0 read-and-clear race only shortens one flash, so no lock.
extern volatile bool led_tx_seen;
extern volatile bool led_rx_seen;

static __force_inline void led_note_tx(void) { led_tx_seen = true; }
static __force_inline void led_note_rx(void) { led_rx_seen = true; }

#endif  // LED_H
