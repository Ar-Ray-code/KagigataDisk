// RP2350 "kagigatadisk": a programmable SPI peripheral for an M5Stack host.
// The host talks to it with the Kagigata protocol (protocol/emu_protocol_defs.h)
// through a TF-card-shaped connector used purely as a four-wire SPI port, and
// reads and writes named objects: /log.txt and anything else it creates,
// /device_info.txt, /README.md, and the files a PC saved through the USB
// drive.
//
// core1 owns the SPI link: it does nothing but move bytes between the PIO
// transport and the frame buffers (emu_link.c), so the transmit FIFO is never
// starved and core1 never touches flash. core0 answers the requests
// (emu_server.c, objstore.c) and runs the USB mass-storage drive (usbdisk.c),
// the status LEDs and the flash store.
#include "devinfo.h"
#include "emu_link.h"
#include "emu_server.h"
#include "fstore.h"
#include "led.h"
#include "objstore.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "spi_slave_transport.h"
#include "tusb.h"
#include "usbdisk.h"

// The folder's README.md, embedded by skill_md.S; the SPI host sees it too.
extern const uint8_t readme_md_start[];
extern const uint8_t readme_md_end[];

int main(void) {
  led_init();
  // The flash store starts right above the firmware.
  extern char __flash_binary_end;
  if ((uintptr_t)&__flash_binary_end - XIP_BASE > FS_FLASH_OFFSET) {
    for (;;) led_blink_error();
  }
  fstore_init();  // scans flash (~0.1 s); nothing is erased up front
  devinfo_init();
  objstore_init();
  objstore_set_readme(readme_md_start, (uint32_t)(readme_md_end - readme_md_start));
  emu_server_init();
  emu_link_init();
  spis_init();
  usbdisk_init();

  // core1 owns the real-time SPI link from here on.
  multicore_launch_core1(emu_link_run);
  tusb_init();

  for (;;) {
    tud_task();
    emu_server_task();
    usbdisk_task();
    objstore_task();
    emu_server_task();
    fstore_idle();
    led_update();
    devinfo_update();
  }
}
