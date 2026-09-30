// PIO byte transport for the board's side of the SPI link (spi_slave_transport.pio).
//
// Contract with its user (emu_link.c):
//   * One PIO FIFO word carries one byte.
//   * The caller must keep the TX FIFO non-empty while selected. If `out`
//     ever stalls, the following `in` is delayed too and the bit stream loses
//     alignment, so underrun is a hard failure rather than a glitch.
//   * Because the FIFO is 4 deep plus the OSR, a byte queued now appears on
//     MISO roughly 4 byte times later. The protocol is poll-driven (the host
//     keeps clocking until it sees a response frame's magic), so this latency
//     is harmless.
#ifndef SPI_SLAVE_TRANSPORT_H
#define SPI_SLAVE_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "board_config.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"

// Number of bytes kept queued in the PIO TX FIFO. 3 leaves headroom against
// the 4-entry FIFO while still bounding response latency.
#define SPIS_TX_PRIME 3

void spis_init(void);

// Getting ready for the next selection, while deselected: drops everything
// in flight, restarts byte framing (so a transfer always begins on a byte
// boundary, whatever the bus carried for other devices meanwhile) and queues
// the first SPIS_TX_PRIME bytes to send, from `prime`. Done ahead of time
// because a host may start clocking within a microsecond of selecting.
void spis_arm(uint8_t (*prime)(void));

// The host selected the board: drive MISO and start the armed state machine.
void spis_go(void);

// The host deselected the board: stops the state machine (so traffic for
// other devices on the shared SCK/MOSI - the Core2's LCD - is neither shifted
// in nor parsed) and releases MISO.
void spis_stop(void);

static inline bool spis_rx_ready(void) { return !pio_sm_is_rx_fifo_empty(PIO_LINK, SM_LINK); }

static inline uint8_t spis_rx_get(void) { return (uint8_t)pio_sm_get(PIO_LINK, SM_LINK); }

static inline uint spis_tx_level(void) { return pio_sm_get_tx_fifo_level(PIO_LINK, SM_LINK); }

static inline void spis_tx_put(uint8_t b) { pio_sm_put(PIO_LINK, SM_LINK, (uint32_t)b << 24); }

// True while the host has the board selected (CS is active low).
static inline bool spis_selected(void) { return !gpio_get(PIN_SPI_CS); }

#endif  // SPI_SLAVE_TRANSPORT_H
