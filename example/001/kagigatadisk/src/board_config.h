// Board / pin configuration for the KagigataDisk PCB (RP2354A).
//
// The board plugs into a TF-card-shaped slot, which is used purely as a
// four-wire SPI connector:
//
//   connector pin | SPI signal            | RP2350 GPIO
//   --------------+-----------------------+------------
//   2             | CS   (active low)     | GPIO0
//   3             | MOSI (host -> board)  | GPIO1
//   5             | SCK                   | GPIO2
//   7             | MISO (board -> host)  | GPIO7
//
// The SPI side is driven by PIO, so the four pins need not be adjacent or in
// any particular order - see spi_slave_transport.pio.
#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define PIN_SPI_CS 0    // chip select, active low
#define PIN_SPI_MOSI 1  // host -> board
#define PIN_SPI_SCK 2   // clock, SPI mode 0 (idles low)
#define PIN_SPI_MISO 7  // board -> host

// Status LEDs - see led.h for what each one shows. TX/RX are from the board's
// point of view.
#define PIN_LED_USB 19
#define PIN_LED_TX 14
#define PIN_LED_RX 12

// PIO allocation. Only the SPI transport needs PIO.
#define PIO_LINK pio0
#define SM_LINK 0u

// The four SPI lines must be distinct, and `wait gpio` can only reach GPIO 0-31.
_Static_assert(PIN_SPI_MOSI != PIN_SPI_CS && PIN_SPI_MOSI != PIN_SPI_SCK &&
                   PIN_SPI_MOSI != PIN_SPI_MISO && PIN_SPI_CS != PIN_SPI_SCK &&
                   PIN_SPI_CS != PIN_SPI_MISO && PIN_SPI_SCK != PIN_SPI_MISO,
               "the four SPI pins must be distinct");
_Static_assert(PIN_SPI_SCK < 32, "wait gpio cannot reach this SCK pin");
_Static_assert(PIN_LED_USB != PIN_LED_TX && PIN_LED_USB != PIN_LED_RX &&
                   PIN_LED_TX != PIN_LED_RX,
               "the three LEDs must be distinct pins");

#endif  // BOARD_CONFIG_H
