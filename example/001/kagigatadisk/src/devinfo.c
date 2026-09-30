#include "devinfo.h"

#include <stdio.h>
#include <string.h>

#include "fstore.h"
#include "hardware/clocks.h"
#include "hardware/structs/qmi.h"
#include "objstore.h"
#include "pico.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "usbdisk.h"

#define DEVINFO_REFRESH_MS 1000u

static uint8_t text[DEVINFO_SIZE];
static char id_hex[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
static absolute_time_t next_refresh;

static void render(void) {
  char buf[DEVINFO_SIZE + 1];
  int n = snprintf(
      buf, sizeof(buf),
      "kagigatadisk device info (updated every second)\n"
      "unique_id=%s\nuptime_s=%lu\nclk_sys_hz=%lu\nclk_peri_hz=%lu\nflash_clkdiv=%lu\n"
      "flash_sectors=%lu/%lu\nflash_erases=%lu\nspi_unsaved=%lu\nusb_unsaved=%lu\n"
      "mirrored_files=%lu\nusb_sync=%s%s\n",
      id_hex, (unsigned long)(to_ms_since_boot(get_absolute_time()) / 1000u),
      (unsigned long)clock_get_hz(clk_sys), (unsigned long)clock_get_hz(clk_peri),
      (unsigned long)((qmi_hw->m[0].timing & QMI_M0_TIMING_CLKDIV_BITS) >>
                      QMI_M0_TIMING_CLKDIV_LSB),
      (unsigned long)fstore_live(), (unsigned long)fstore_capacity(),
      (unsigned long)fstore_erases(), (unsigned long)objstore_unsaved(),
      (unsigned long)usbdisk_unsaved(), (unsigned long)usbdisk_mirrored(), usbdisk_status(),
      objstore_flash_full() ? " (spi: flash full)" : "");
  if (n < 0) n = 0;
  if (n > (int)DEVINFO_SIZE) n = DEVINFO_SIZE;
  memcpy(text, buf, (size_t)n);
  memset(text + n, ' ', DEVINFO_SIZE - (size_t)n);
}

const uint8_t *devinfo_text(void) { return text; }

void devinfo_init(void) {
  pico_get_unique_board_id_string(id_hex, sizeof(id_hex));
  render();
  next_refresh = make_timeout_time_ms(DEVINFO_REFRESH_MS);
}

void devinfo_update(void) {
  if (absolute_time_diff_us(get_absolute_time(), next_refresh) > 0) return;
  next_refresh = delayed_by_ms(next_refresh, DEVINFO_REFRESH_MS);
  render();
}
