// Status LEDs:
//   PIN_LED_TOUCH (LED1) - blinks at 4 Hz while a request waits for a touch;
//                          lit while a finger is on the contacts
//   PIN_LED_RX    (LED3) - a credential/PIN request arrived (lit while it is
//                          worked on, at least LED_ACTIVITY_HOLD_MS)
//   PIN_LED_TX    (LED2) - its response went out
// Only makeCredential, getAssertion & co. light them (ctap2_is_user_action());
// the getInfo / INIT / keepalive traffic of an attached key does not.
// All three stay lit while a bootloader request waits for its 3 s touch, and
// flash together for a WINK.
#ifndef LED_H
#define LED_H

#include <stdbool.h>

void led_init(void);

// Call frequently from the main loop (and while waiting); internally time-gated.
void led_update(void);

void led_set_waiting(bool on);
void led_set_bootsel(bool on);
void led_note_touch(void);  // a finger is on the contacts now
void led_note_tx(void);
void led_note_rx(void);
void led_wink(void);  // ~2 s

#endif  // LED_H
