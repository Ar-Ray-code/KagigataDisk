#include "spi_slave_transport.h"

#include <assert.h>

#include "hardware/structs/io_bank0.h"
#include "spi_slave_transport.pio.h"

static uint pio_offset;

// The assembled program carries a placeholder index in its two `wait`
// instructions; fill in where SCK sits relative to the IN base here, so the pin
// map is not baked into the .pio file and MOSI/SCK/CS need not be adjacent.
// The index wraps modulo 32, so SCK may sit either side of MOSI.
static uint16_t patched_instructions[16];
static pio_program_t patched_prog;

static const pio_program_t *patched_program(void) {
  const uint16_t *src = spi_slave_transport_program.instructions;
  uint len = spi_slave_transport_program.length;
  assert(len <= count_of(patched_instructions));

  for (uint i = 0; i < len; i++) {
    uint16_t op = src[i];
    if ((op & 0xE000u) == 0x2000u) {  // WAIT: 001 dddd d P ss iiiii
      bool polarity = (op >> 7) & 1u;
      op = (uint16_t)pio_encode_wait_pin(polarity, (PIN_SPI_SCK - PIN_SPI_MOSI) & 31u);
    }
    patched_instructions[i] = op;
  }
  patched_prog = spi_slave_transport_program;
  patched_prog.instructions = patched_instructions;
  return &patched_prog;
}

void spis_init(void) {
  // CS is read by the CPU (core1 polls it to detect transaction boundaries),
  // so it stays a plain SIO input.
  gpio_init(PIN_SPI_CS);
  gpio_set_dir(PIN_SPI_CS, GPIO_IN);
  gpio_pull_up(PIN_SPI_CS);

  // Weak pulls keep the bus defined while the host is held in reset.
  gpio_pull_up(PIN_SPI_MOSI);
  gpio_pull_down(PIN_SPI_SCK);

  // Hysteresis (should already be the pad reset default, set explicitly
  // rather than trusting that) rejects a slow/noisy transition sitting near
  // the switching threshold.
  gpio_set_input_hysteresis_enabled(PIN_SPI_MOSI, true);
  gpio_set_input_hysteresis_enabled(PIN_SPI_SCK, true);
  gpio_set_input_hysteresis_enabled(PIN_SPI_CS, true);
  // MISO has to rise within the half clock between the PIO updating it
  // (after a falling edge) and the host sampling it. At the pad default of
  // 4 mA its rising edges came in late through the connector at high clock
  // rates (only 1 -> 0 bit errors); 12 mA with a fast slew fixes that.
  gpio_set_slew_rate(PIN_SPI_MISO, GPIO_SLEW_RATE_FAST);
  gpio_set_drive_strength(PIN_SPI_MISO, GPIO_DRIVE_STRENGTH_12MA);

  // SCK skips the PIO's two-stage input synchroniser: that saves two clk_sys
  // cycles between a falling SCK edge and the next MISO bit, which is what
  // limits the clock rate (the host samples MISO half a period later). The
  // glitch re-check in the program still reads SCK twice before trusting an
  // edge.
  hw_set_bits(&PIO_LINK->input_sync_bypass, 1u << PIN_SPI_SCK);

  pio_offset = pio_add_program(PIO_LINK, patched_program());
  spi_slave_transport_program_init(PIO_LINK, SM_LINK, pio_offset, PIN_SPI_MOSI, PIN_SPI_SCK,
                                   PIN_SPI_MISO);
  spis_stop();  // emu_link arms it
}

// MISO's output enable, forced off while the board is deselected so it leaves
// the line to the other devices on the bus (the Core2's LCD shares it).
// Written directly: the SDK's gpio_set_oeover() lives in flash.
static inline void miso_output(bool on) {
  hw_write_masked(&io_bank0_hw->io[PIN_SPI_MISO].ctrl,
                  (on ? GPIO_OVERRIDE_NORMAL : GPIO_OVERRIDE_LOW) << IO_BANK0_GPIO0_CTRL_OEOVER_LSB,
                  IO_BANK0_GPIO0_CTRL_OEOVER_BITS);
}

void __not_in_flash_func(spis_stop)(void) {
  pio_sm_set_enabled(PIO_LINK, SM_LINK, false);
  pio_sm_clear_fifos(PIO_LINK, SM_LINK);
  miso_output(false);
}

void __not_in_flash_func(spis_arm)(uint8_t (*prime)(void)) {
  pio_sm_set_enabled(PIO_LINK, SM_LINK, false);
  pio_sm_clear_fifos(PIO_LINK, SM_LINK);
  pio_sm_restart(PIO_LINK, SM_LINK);  // clears ISR/OSR and shift counters
  pio_sm_clkdiv_restart(PIO_LINK, SM_LINK);
  pio_sm_exec(PIO_LINK, SM_LINK, pio_encode_jmp(pio_offset));
  for (int i = 0; i < SPIS_TX_PRIME; i++) spis_tx_put(prime());
}

void __not_in_flash_func(spis_go)(void) {
  miso_output(true);
  pio_sm_set_enabled(PIO_LINK, SM_LINK, true);
}
