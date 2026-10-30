// Board / pin configuration for the KagigataDisk PCB (RP2354A), FIDO2 key.
//
// Only USB is used as an interface. The TF-card-shaped edge is not connected
// to anything while the key sits in a PC's USB port, so its four GPIO
// contacts serve as a capacitive touch sensor for user presence
// (touch.c; KGFIDO_TOUCH_PINS picks which of them):
//
//   connector pin | 001's SPI signal | RP2350 GPIO
//   --------------+------------------+------------
//   2             | CS               | GPIO0
//   3             | MOSI             | GPIO1
//   5             | SCK              | GPIO2
//   7             | MISO             | GPIO7
//   4 / 6         | 3.3 V / GND      | -
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include "kgfido_config.h"
#include "pico.h"

// Status LEDs - see led.h for what each one shows. TX/RX are from the key's
// point of view, on the same LEDs as example/001's SPI TX/RX.
#define PIN_LED_TOUCH 19  // LED1
#define PIN_LED_TX 14     // LED2
#define PIN_LED_RX 12     // LED3

// The credential store: the last 64 KiB of the 2 MiB flash. The firmware must
// end below it (main.c checks).
#define STORE_FLASH_SIZE (64u * 1024u)
#define STORE_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - STORE_FLASH_SIZE)

_Static_assert((KGFIDO_TOUCH_PIN_MASK & ((1u << PIN_LED_TOUCH) | (1u << PIN_LED_TX) |
                                         (1u << PIN_LED_RX))) == 0,
               "a touch pin cannot also be an LED");

#endif  // BOARD_CONFIG_H
