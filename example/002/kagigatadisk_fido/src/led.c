#include "led.h"

#include "board_config.h"
#include "hardware/gpio.h"
#include "pico/time.h"

#define LED_WAIT_TOGGLE_MS 125u  // 4 Hz
#define LED_TOUCH_HOLD_MS 100u
#define LED_ACTIVITY_HOLD_MS 150u
#define LED_WINK_TOGGLE_MS 100u
#define LED_WINK_MS 2000u

static bool waiting;
static bool bootsel;
static absolute_time_t touch_off_at;
static absolute_time_t tx_off_at;
static absolute_time_t rx_off_at;
static absolute_time_t wink_until;

static void led_setup(uint pin) {
  gpio_init(pin);
  gpio_set_dir(pin, GPIO_OUT);
  gpio_put(pin, 0);
}

void led_init(void) {
  led_setup(PIN_LED_TOUCH);
  led_setup(PIN_LED_TX);
  led_setup(PIN_LED_RX);
  touch_off_at = tx_off_at = rx_off_at = wink_until = nil_time;
}

void led_set_waiting(bool on) { waiting = on; }
void led_set_bootsel(bool on) { bootsel = on; }
void led_note_touch(void) { touch_off_at = make_timeout_time_ms(LED_TOUCH_HOLD_MS); }
void led_note_tx(void) { tx_off_at = make_timeout_time_ms(LED_ACTIVITY_HOLD_MS); }
void led_note_rx(void) { rx_off_at = make_timeout_time_ms(LED_ACTIVITY_HOLD_MS); }
void led_wink(void) { wink_until = make_timeout_time_ms(LED_WINK_MS); }

static bool before(absolute_time_t now, absolute_time_t t) {
  return absolute_time_diff_us(now, t) > 0;
}

void led_update(void) {
  absolute_time_t now = get_absolute_time();
  uint32_t ms = to_ms_since_boot(now);

  if (bootsel || before(now, wink_until)) {
    bool on = bootsel || ((ms / LED_WINK_TOGGLE_MS) & 1u);
    gpio_put(PIN_LED_TOUCH, on);
    gpio_put(PIN_LED_TX, on);
    gpio_put(PIN_LED_RX, on);
    return;
  }
  bool touch = before(now, touch_off_at);
  gpio_put(PIN_LED_TOUCH, touch || (waiting && ((ms / LED_WAIT_TOGGLE_MS) & 1u)));
  gpio_put(PIN_LED_TX, before(now, tx_off_at));
  gpio_put(PIN_LED_RX, before(now, rx_off_at));
}
