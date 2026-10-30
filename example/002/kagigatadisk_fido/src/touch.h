// User presence: a capacitive touch sensor on the card-edge GPIO contacts
// (KGFIDO_TOUCH_PINS, default GPIO0/1/2/7 - the four gold fingers 001 uses
// for SPI). No extra parts: each pin is driven low, released to its internal
// pull-up, and the time it takes to read high is counted. A finger on the
// contacts adds capacitance (and, bridging to the GND contact, a leak), which
// makes that time grow.
//
// The pins are only driven while a request waits for a touch (and for the
// KG_TOUCH diagnostic), so a board sitting in an SPI host's slot is left alone
// otherwise.
#ifndef TOUCH_H
#define TOUCH_H

#include <stdbool.h>
#include <stdint.h>

#define TOUCH_PINS_MAX 8

void touch_init(void);

// Starts a wait for a touch: measures the untouched level afresh. A finger
// already on the contacts must be lifted and put back to count.
void touch_begin(void);

// Call often while waiting; measures every TOUCH_PERIOD_MS. True once per
// touch (after the debounce).
bool touch_poll(void);

// Whether some pin was over its threshold at the latest measurement.
bool touch_active(void);
// Whether a touch is in progress (past the debounce, not yet released).
bool touch_held(void);

// For the KG_TOUCH diagnostic: measures now (no time gate) and reports.
typedef struct {
  uint8_t gpio;
  uint16_t raw;   // charge time, loop counts summed over TOUCH_REPS
  uint16_t base;  // untouched level
  uint16_t threshold;  // raw - base above this counts as touched
} touch_pin_t;
// Returns whether any pin is over its threshold right now (no debounce).
bool touch_sample(touch_pin_t *pins, uint8_t *n);

#endif  // TOUCH_H
