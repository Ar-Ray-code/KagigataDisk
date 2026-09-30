#include "led.h"

#include "board_config.h"
#include "hardware/gpio.h"
#include "pico/time.h"
#include "tusb.h"

#define LED_USB_TOGGLE_MS 100u  // 5 Hz: 100 ms on, 100 ms off
#define LED_ACTIVITY_HOLD_MS 30u

volatile bool led_tx_seen;
volatile bool led_rx_seen;

static bool usb_on;
static absolute_time_t usb_next_toggle;
static absolute_time_t tx_off_at;
static absolute_time_t rx_off_at;

static void led_setup(uint pin) {
  gpio_init(pin);
  gpio_set_dir(pin, GPIO_OUT);
  gpio_put(pin, 0);
}

void led_init(void) {
  led_setup(PIN_LED_USB);
  led_setup(PIN_LED_TX);
  led_setup(PIN_LED_RX);
  usb_on = false;
  usb_next_toggle = get_absolute_time();
  tx_off_at = rx_off_at = nil_time;
}

// Lights `pin` for LED_ACTIVITY_HOLD_MS after the latest report in `*seen`.
static void update_activity(uint pin, volatile bool *seen,
                            absolute_time_t *off_at, absolute_time_t now) {
  if (*seen) {
    *seen = false;
    *off_at = delayed_by_ms(now, LED_ACTIVITY_HOLD_MS);
  }
  gpio_put(pin, absolute_time_diff_us(now, *off_at) > 0);
}

void led_update(void) {
  absolute_time_t now = get_absolute_time();

  if (tud_mounted() && !tud_suspended()) {
    if (absolute_time_diff_us(now, usb_next_toggle) <= 0) {
      usb_on = !usb_on;
      usb_next_toggle = delayed_by_ms(now, LED_USB_TOGGLE_MS);
    }
  } else {
    usb_on = false;
  }
  gpio_put(PIN_LED_USB, usb_on);

  update_activity(PIN_LED_TX, &led_tx_seen, &tx_off_at, now);
  update_activity(PIN_LED_RX, &led_rx_seen, &rx_off_at, now);
}

void led_blink_error(void) {
  for (int on = 1; on >= 0; on--) {
    gpio_put(PIN_LED_USB, on);
    gpio_put(PIN_LED_TX, on);
    gpio_put(PIN_LED_RX, on);
    sleep_ms(150);
  }
}
