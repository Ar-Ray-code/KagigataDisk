#include "touch.h"

#include "board_config.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"

#define TOUCH_REPS 16           // charge cycles summed per measurement
#define TOUCH_MAX_LOOPS 1000u   // per cycle; a pin held low by a finger to GND stops here
#define TOUCH_DISCHARGE_US 3
#define TOUCH_PERIOD_MS 10
#define TOUCH_DEBOUNCE 3        // consecutive touched measurements (~30 ms)
// Touched when the charge time exceeds the untouched level by this much:
// TOUCH_MIN_DELTA counts, or TOUCH_PCT percent of it if that is more.
#define TOUCH_MIN_DELTA 40u
#define TOUCH_PCT 30u

static uint8_t pins[TOUCH_PINS_MAX];
static uint8_t n_pins;
static uint32_t base[TOUCH_PINS_MAX];
static uint32_t last_raw[TOUCH_PINS_MAX];
static uint8_t streak;
static bool over;  // some pin is over its threshold right now
static absolute_time_t next_at;

void touch_init(void) {
  n_pins = 0;
  for (uint8_t g = 0; g < 32 && n_pins < TOUCH_PINS_MAX; g++) {
    if (!(KGFIDO_TOUCH_PIN_MASK & (1u << g))) continue;
    gpio_init(g);  // SIO input, output value 0
    gpio_pull_up(g);
    pins[n_pins++] = g;
  }
}

// One charge: release the (discharged) pin and count until it reads high.
// From RAM, with interrupts off, so the count is not skewed by a flash cache
// miss or an IRQ.
static uint32_t __no_inline_not_in_flash_func(charge)(uint32_t mask) {
  uint32_t n = 0;
  sio_hw->gpio_oe_clr = mask;
  while (!(sio_hw->gpio_in & mask) && ++n < TOUCH_MAX_LOOPS) {
  }
  return n;
}

static uint32_t measure(uint8_t gpio) {
  uint32_t mask = 1u << gpio, total = 0;
  for (int r = 0; r < TOUCH_REPS; r++) {
    sio_hw->gpio_clr = mask;
    sio_hw->gpio_oe_set = mask;
    busy_wait_us_32(TOUCH_DISCHARGE_US);
    uint32_t irq = save_and_disable_interrupts();
    total += charge(mask);
    restore_interrupts(irq);
  }
  return total;  // the pin is left as an input with its pull-up
}

static uint32_t threshold(uint32_t b) {
  uint32_t t = b * TOUCH_PCT / 100u;
  return t > TOUCH_MIN_DELTA ? t : TOUCH_MIN_DELTA;
}

void touch_begin(void) {
  for (uint8_t i = 0; i < n_pins; i++) {
    uint32_t lo = UINT32_MAX;
    for (int k = 0; k < 4; k++) {
      uint32_t v = measure(pins[i]);
      if (v < lo) lo = v;
    }
    base[i] = last_raw[i] = lo;
  }
  streak = 0;
  next_at = get_absolute_time();
}

// Measures every pin once and updates the untouched levels.
static bool step(void) {
  bool any = false;
  for (uint8_t i = 0; i < n_pins; i++) {
    uint32_t raw = measure(pins[i]);
    last_raw[i] = raw;
    if (raw <= base[i]) {
      base[i] = raw;  // finger lifted or drift down: follow at once
    } else if (raw - base[i] > threshold(base[i])) {
      any = true;
    } else {
      // Slow drift up (~0.6 s time constant), so an approaching finger still
      // shows as a step but warming or humidity is followed.
      base[i] += (raw - base[i] + 63u) / 64u;
    }
  }
  over = any;
  if (!any) {
    streak = 0;
    return false;
  }
  if (streak <= TOUCH_DEBOUNCE) streak++;
  return streak == TOUCH_DEBOUNCE;
}

bool touch_active(void) { return over; }
bool touch_held(void) { return streak >= TOUCH_DEBOUNCE; }

bool touch_poll(void) {
  if (absolute_time_diff_us(get_absolute_time(), next_at) > 0) return false;
  next_at = make_timeout_time_ms(TOUCH_PERIOD_MS);
  return step();
}

static uint16_t clamp16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }

bool touch_sample(touch_pin_t *out, uint8_t *n) {
  step();
  for (uint8_t i = 0; i < n_pins; i++) {
    out[i].gpio = pins[i];
    out[i].raw = clamp16(last_raw[i]);
    out[i].base = clamp16(base[i]);
    out[i].threshold = clamp16(threshold(base[i]));
  }
  *n = n_pins;
  return over;
}
