// Custom board header for a bare RP2354A target (this project's actual
// hardware), since pico-sdk 2.3.0 ships no board file for it.
//
// RP2354A is package/pin compatible with RP2350A ("A" = QFN60, 30 GPIO) and
// differs only in having 2 MiB of flash integrated on-die instead of 4 MiB
// on an external QSPI chip - see
// https://forums.raspberrypi.com/viewtopic.php?t=398455. The on-die flash is
// still reached through the same QMI/XIP interface an external chip would
// use and is command-compatible with it, so everything pico2.h sets up
// (UART/I2C/SPI defaults, PICO_RP2350A, the W25Q080-style boot_stage2, the
// A2-stepping flag) applies unchanged; only the flash size differs.
//
// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
#ifndef _BOARDS_PICO2_RP2354A_H
#define _BOARDS_PICO2_RP2354A_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// Must be defined before pulling in pico2.h, whose own #ifndef guard (same
// pattern, 4 MiB) then leaves this value alone.
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#include "boards/pico2.h"

#endif
