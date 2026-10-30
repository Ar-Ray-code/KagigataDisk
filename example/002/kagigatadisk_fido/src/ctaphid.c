#include "ctaphid.h"

#include <stdio.h>
#include <string.h>

#include "board_config.h"
#include "crypto.h"
#include "ctap2.h"
#include "led.h"
#include "pico/bootrom.h"
#include "pico/time.h"
#include "store.h"
#include "touch.h"
#include "tusb.h"

#define RPT 64
#define INIT_DATA (RPT - 7)  // cid(4) cmd(1) bcnt(2)
#define CONT_DATA (RPT - 5)  // cid(4) seq(1)
#define MSG_MAX 2048         // largest request accepted (and getInfo's maxMsgSize)
#define CID_BROADCAST 0xFFFFFFFFu
#define MSG_TIMEOUT_MS 500   // between the packets of one message
#define KEEPALIVE_MS 100
#define UP_TIMEOUT_MS 30000
#define BOOTSEL_DELAY_MS 100
#define BOOTSEL_HOLD_MS 3000  // finger kept on the contacts to allow a bootloader request
#define RXQ_LEN 16

_Static_assert(MSG_MAX <= INIT_DATA + 128 * CONT_DATA, "fits 128 continuation packets");
_Static_assert(MSG_MAX == CTAP2_MSG_MAX, "getInfo reports this size");

// Commands, without bit 7 (set in init packets).
#define CMD_PING 0x01
#define CMD_INIT 0x06
#define CMD_WINK 0x08
#define CMD_CBOR 0x10
#define CMD_CANCEL 0x11
#define CMD_KEEPALIVE 0x3B
#define CMD_ERROR 0x3F

#define ERR_INVALID_CMD 0x01
#define ERR_INVALID_PAR 0x02
#define ERR_INVALID_LEN 0x03
#define ERR_INVALID_SEQ 0x04
#define ERR_MSG_TIMEOUT 0x05
#define ERR_CHANNEL_BUSY 0x06
#define ERR_INVALID_CHANNEL 0x0B

#define CAP_WINK 0x01
#define CAP_CBOR 0x04
#define CAP_NMSG 0x08  // no CTAP1/U2F MSG

#define KEEPALIVE_UPNEEDED 2

// ---------------------------------------------------------------------------
// Receive queue: filled from TinyUSB's callback (inside tud_task), drained by
// ctaphid_task() or, while a command runs, by pump().
// ---------------------------------------------------------------------------
static uint8_t rxq[RXQ_LEN][RPT];
static uint8_t rxq_head, rxq_tail;

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize) {
  (void)instance;
  (void)report_id;
  (void)report_type;
  uint8_t next = (uint8_t)((rxq_head + 1u) % RXQ_LEN);
  if (next == rxq_tail) return;  // full: dropped; the platform times out and retries
  memset(rxq[rxq_head], 0, RPT);
  memcpy(rxq[rxq_head], buffer, bufsize < RPT ? bufsize : RPT);
  rxq_head = next;
}

static bool rx_pop(uint8_t *pkt) {
  if (rxq_tail == rxq_head) return false;
  memcpy(pkt, rxq[rxq_tail], RPT);
  rxq_tail = (uint8_t)((rxq_tail + 1u) % RXQ_LEN);
  return true;
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------
static uint32_t get_cid(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_cid(uint8_t *p, uint32_t cid) {
  p[0] = (uint8_t)(cid >> 24);
  p[1] = (uint8_t)(cid >> 16);
  p[2] = (uint8_t)(cid >> 8);
  p[3] = (uint8_t)cid;
}

static void write_report(const uint8_t *pkt) {
  absolute_time_t until = make_timeout_time_ms(1000);
  while (!tud_hid_ready()) {
    tud_task();
    if (!tud_mounted() || time_reached(until)) return;
  }
  tud_hid_report(0, pkt, RPT);
}

static void send(uint32_t cid, uint8_t cmd, const uint8_t *data, size_t len) {
  uint8_t pkt[RPT];
  memset(pkt, 0, RPT);
  put_cid(pkt, cid);
  pkt[4] = 0x80u | cmd;
  pkt[5] = (uint8_t)(len >> 8);
  pkt[6] = (uint8_t)len;
  size_t n = len < INIT_DATA ? len : INIT_DATA;
  if (n) memcpy(pkt + 7, data, n);
  write_report(pkt);
  for (uint8_t seq = 0; n < len; seq++) {
    size_t k = len - n < CONT_DATA ? len - n : CONT_DATA;
    memset(pkt, 0, RPT);
    put_cid(pkt, cid);
    pkt[4] = seq;
    memcpy(pkt + 5, data + n, k);
    write_report(pkt);
    n += k;
  }
}

static void send_error(uint32_t cid, uint8_t err) { send(cid, CMD_ERROR, &err, 1); }

// ---------------------------------------------------------------------------
// Channels and INIT
// ---------------------------------------------------------------------------
static uint32_t next_cid = 1;

static bool cid_valid(uint32_t cid) { return cid != 0 && cid != CID_BROADCAST && cid < next_cid; }

static void do_init(uint32_t cid, const uint8_t *nonce, uint16_t len) {
  if (len != 8) {
    send_error(cid, ERR_INVALID_LEN);
    return;
  }
  uint32_t ncid;
  if (cid == CID_BROADCAST) {
    ncid = next_cid++;
  } else if (cid_valid(cid)) {
    ncid = cid;  // resync of an existing channel
  } else {
    send_error(cid, ERR_INVALID_CHANNEL);
    return;
  }
  uint8_t r[17];
  memcpy(r, nonce, 8);
  put_cid(r + 8, ncid);
  r[12] = 2;  // CTAPHID protocol version
  r[13] = 0;  // device version major, minor, build
  r[14] = 1;
  r[15] = 0;
  r[16] = CAP_WINK | CAP_CBOR | CAP_NMSG;
  send(cid, CMD_INIT, r, sizeof(r));
}

// ---------------------------------------------------------------------------
// A command in progress (CBOR, or KG_BOOTSEL waiting for a touch)
// ---------------------------------------------------------------------------
static uint32_t op_cid;
static bool op_cancel;
static absolute_time_t keepalive_at;

static void begin_op(uint32_t cid) {
  op_cid = cid;
  op_cancel = false;
  keepalive_at = make_timeout_time_ms(KEEPALIVE_MS);
}

// Keeps USB going while a command runs, sends keepalives and handles what
// arrives meanwhile. False once the command should stop.
static bool pump(uint8_t status) {
  tud_task();
  led_update();
  uint8_t pkt[RPT];
  while (rx_pop(pkt)) {
    if (!(pkt[4] & 0x80u)) continue;  // a continuation: nothing is being assembled
    uint32_t cid = get_cid(pkt);
    uint8_t cmd = pkt[4] & 0x7Fu;
    if (cmd == CMD_INIT) {
      if (cid == op_cid) op_cancel = true;  // the platform resynced this channel
      do_init(cid, pkt + 7, (uint16_t)(pkt[5] << 8 | pkt[6]));
    } else if (cid == op_cid) {
      if (cmd == CMD_CANCEL) op_cancel = true;
    } else {
      send_error(cid, ERR_CHANNEL_BUSY);
    }
  }
  if (!op_cancel && time_reached(keepalive_at)) {
    send(op_cid, CMD_KEEPALIVE, &status, 1);
    keepalive_at = make_timeout_time_ms(KEEPALIVE_MS);
  }
  return !op_cancel;
}

// Waits for a touch: a new press (hold_ms 0), or a finger kept on the
// contacts for hold_ms. LED1 blinks for a credential request; all LEDs stay
// lit for a bootloader request, so the two cannot be mistaken for each other.
static uint8_t wait_touch(uint32_t hold_ms) {
  touch_begin();
  if (hold_ms) {
    led_set_bootsel(true);
  } else {
    led_set_waiting(true);
  }
  absolute_time_t deadline = make_timeout_time_ms(UP_TIMEOUT_MS);
  absolute_time_t held_since = nil_time;
  bool holding = false;
  uint8_t st;
  for (;;) {
    if (!pump(KEEPALIVE_UPNEEDED)) {
      st = CTAP2_ERR_KEEPALIVE_CANCEL;
      break;
    }
    bool pressed = touch_poll();
    if (touch_active()) led_note_touch();
    if (!hold_ms && pressed) {
      st = CTAP2_OK;
      break;
    }
    if (hold_ms && touch_held()) {
      if (!holding) {
        holding = true;
        held_since = get_absolute_time();
      } else if (absolute_time_diff_us(held_since, get_absolute_time()) >= (int64_t)hold_ms * 1000) {
        st = CTAP2_OK;
        break;
      }
    } else {
      holding = false;  // released: start over
    }
    if (time_reached(deadline)) {
      st = CTAP2_ERR_USER_ACTION_TIMEOUT;
      break;
    }
  }
  led_set_waiting(false);
  led_set_bootsel(false);
  return st;
}

uint8_t ctaphid_wait_up(void) { return wait_touch(0); }

// ---------------------------------------------------------------------------
// Vendor commands
// ---------------------------------------------------------------------------
static bool bootsel_pending;
static absolute_time_t bootsel_at;

static void do_bootsel(uint32_t cid, uint16_t len) {
  if (len > 1) {  // one byte (the mode of earlier versions) is accepted and ignored
    send_error(cid, ERR_INVALID_LEN);
    return;
  }
  begin_op(cid);
  uint8_t st = wait_touch(BOOTSEL_HOLD_MS);
  if (st == CTAP2_OK) store_wipe();  // secrets are gone even if recreating fails
  send(cid, CTAPHID_VENDOR_BOOTSEL, &st, 1);
  if (st == CTAP2_OK) {
    bootsel_pending = true;
    bootsel_at = make_timeout_time_ms(BOOTSEL_DELAY_MS);
  }
}

static void do_touch(uint32_t cid) {
  // The first reading after a pause starts from a fresh untouched level.
  static absolute_time_t last;
  if (absolute_time_diff_us(last, get_absolute_time()) > 1000000) touch_begin();
  last = get_absolute_time();
  touch_pin_t p[TOUCH_PINS_MAX];
  uint8_t n;
  uint8_t r[2 + TOUCH_PINS_MAX * 7];
  r[0] = touch_sample(p, &n);
  if (r[0]) led_note_touch();
  r[1] = n;
  for (uint8_t i = 0; i < n; i++) {
    uint8_t *q = r + 2 + i * 7;
    q[0] = p[i].gpio;
    q[1] = (uint8_t)p[i].raw;
    q[2] = (uint8_t)(p[i].raw >> 8);
    q[3] = (uint8_t)p[i].base;
    q[4] = (uint8_t)(p[i].base >> 8);
    q[5] = (uint8_t)p[i].threshold;
    q[6] = (uint8_t)(p[i].threshold >> 8);
  }
  send(cid, CTAPHID_VENDOR_TOUCH, r, 2u + n * 7u);
}

static void do_info(uint32_t cid) {
  char s[200];
  int n = snprintf(s, sizeof(s),
                   "kagigatadisk_fido %s touch_pins=0x%08x "
                   "attestation=%s store=%s rng=%s rk=%d/%d counter=%lu pin=%s retries=%u",
                   KGFIDO_FW_VERSION,
                   (unsigned)KGFIDO_TOUCH_PIN_MASK,
                   KGFIDO_ATTEST_BATCH ? "batch" : "self", store_ready() ? "ok" : "FAILED",
                   crypto_ok() ? "ok" : "FAILED",
                   store_rk_count(), STORE_RK_MAX, (unsigned long)store_counter(),
                   store_pin_is_set() ? "set" : "unset", store_pin_retries());
  if (n < 0) n = 0;
  if ((size_t)n >= sizeof(s)) n = sizeof(s) - 1;
  send(cid, CTAPHID_VENDOR_INFO, (const uint8_t *)s, (size_t)n);
}

// ---------------------------------------------------------------------------
// Message assembly and dispatch
// ---------------------------------------------------------------------------
static struct {
  bool busy;
  uint32_t cid;
  uint8_t cmd;
  uint8_t seq;
  uint16_t bcnt, got;
  absolute_time_t deadline;
} rx;
static uint8_t msg[MSG_MAX];
static uint8_t resp[MSG_MAX];

static void dispatch(uint32_t cid, uint8_t cmd, const uint8_t *data, uint16_t len) {
  switch (cmd) {
    case CMD_PING:
      send(cid, CMD_PING, data, len);
      break;
    case CMD_WINK:
      led_wink();
      send(cid, CMD_WINK, NULL, 0);
      break;
    case CMD_CBOR: {
      if (len < 1) {
        send_error(cid, ERR_INVALID_LEN);
        break;
      }
      // TX/RX show credential operations only, not the getInfo, INIT or
      // keepalive traffic a platform exchanges with any attached key.
      bool shown = ctap2_is_user_action(data[0]);
      if (shown) {
        led_note_rx();
        led_update();  // the command may block (signing) before the next update
      }
      begin_op(cid);
      size_t n = ctap2_handle(data, len, resp, sizeof(resp));
      send(cid, CMD_CBOR, resp, n);
      if (shown) led_note_tx();
      break;
    }
    case CMD_CANCEL:
      break;  // nothing is running: no response
    case CTAPHID_VENDOR_BOOTSEL:
      do_bootsel(cid, len);
      break;
    case CTAPHID_VENDOR_TOUCH:
      do_touch(cid);
      break;
    case CTAPHID_VENDOR_INFO:
      do_info(cid);
      break;
    default:  // MSG (U2F) and LOCK included
      send_error(cid, ERR_INVALID_CMD);
      break;
  }
}

static void handle_packet(const uint8_t *p) {
  uint32_t cid = get_cid(p);
  if (p[4] & 0x80u) {
    uint8_t cmd = p[4] & 0x7Fu;
    uint16_t bcnt = (uint16_t)(p[5] << 8 | p[6]);
    if (cmd == CMD_INIT) {
      if (rx.busy && rx.cid == cid) rx.busy = false;  // resync abandons the message
      do_init(cid, p + 7, bcnt);
      return;
    }
    if (!cid_valid(cid)) {
      send_error(cid, ERR_INVALID_CHANNEL);
      return;
    }
    if (rx.busy) {
      if (rx.cid != cid) {
        send_error(cid, ERR_CHANNEL_BUSY);
        return;
      }
      rx.busy = false;  // a new message before the last one was complete
      send_error(cid, ERR_INVALID_SEQ);
      return;
    }
    if (bcnt > MSG_MAX) {
      send_error(cid, ERR_INVALID_LEN);
      return;
    }
    rx.cid = cid;
    rx.cmd = cmd;
    rx.bcnt = bcnt;
    rx.got = bcnt < INIT_DATA ? bcnt : INIT_DATA;
    rx.seq = 0;
    memcpy(msg, p + 7, rx.got);
  } else {
    if (!rx.busy || cid != rx.cid) return;  // spurious continuation: ignored
    if (p[4] != rx.seq) {
      rx.busy = false;
      send_error(cid, ERR_INVALID_SEQ);
      return;
    }
    rx.seq++;
    uint16_t k = (uint16_t)(rx.bcnt - rx.got < CONT_DATA ? rx.bcnt - rx.got : CONT_DATA);
    memcpy(msg + rx.got, p + 5, k);
    rx.got = (uint16_t)(rx.got + k);
  }
  if (rx.got < rx.bcnt) {
    rx.busy = true;
    rx.deadline = make_timeout_time_ms(MSG_TIMEOUT_MS);
    return;
  }
  rx.busy = false;
  dispatch(rx.cid, rx.cmd, msg, rx.bcnt);
}

void ctaphid_init(void) {
  rxq_head = rxq_tail = 0;
  rx.busy = false;
}

void ctaphid_task(void) {
  uint8_t pkt[RPT];
  while (rx_pop(pkt)) handle_packet(pkt);
  if (rx.busy && time_reached(rx.deadline)) {
    rx.busy = false;
    send_error(rx.cid, ERR_MSG_TIMEOUT);
  }
  if (bootsel_pending && time_reached(bootsel_at) && tud_hid_ready()) {
    reset_usb_boot(0, 0);  // the RP2350 UF2 drive appears; does not return
  }
}
