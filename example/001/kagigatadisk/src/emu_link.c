#include "emu_link.h"

#include "hardware/sync.h"
#include "led.h"
#include "pico.h"
#include "spi_slave_transport.h"

// How many consecutive polls a CS deassert has to hold before it is believed.
// Deliberately one-sided: a noisy CS line has been seen to blip high
// mid-transfer, and a spurious deassert would cut a request short. The
// assert side must react with zero added latency instead - delaying it lets
// the host clock a bit or two before the resync, which shifts every byte.
#define CS_DEASSERT_CONFIRM_POLLS 8u

enum { L_IDLE, L_PENDING, L_READY };  // link_state
enum { RX_FIRST, RX_FRAME, RX_IGNORE };

// Two receive buffers: core1 fills one while core0 may still be reading the
// request posted from the other.
static uint8_t rxb[2][EMU_FRAME_MAX];
static uint32_t rx_cur;
static uint32_t rx_n, rx_need;
static uint32_t rx_mode;

static uint8_t resp[EMU_FRAME_MAX];
static volatile uint32_t resp_len;

// L_PENDING: core0 owns the posted request and the response buffer.
// L_READY: core1 streams the response. Only core1 leaves L_READY (by
// posting), only core0 leaves L_PENDING (by replying).
static volatile uint32_t link_state;
static const uint8_t *volatile req_ptr;
static volatile uint32_t req_len;
static volatile bool req_truncated;

static bool tx_on;  // streaming the response in this window
static uint32_t tx_pos;

void emu_link_init(void) {
  link_state = L_IDLE;
  resp_len = 0;
  rx_cur = 0;
  rx_mode = RX_IGNORE;
}

// ---- core1 ----------------------------------------------------------------
static uint8_t __not_in_flash_func(next_tx)(void) {
  if (link_state == L_READY) {
    __dmb();  // resp[] and resp_len as core0 left them
    if (!tx_on) {
      tx_on = true;
      tx_pos = 0;
      led_note_tx();
    }
    if (tx_pos < resp_len) return resp[tx_pos++];
  } else {
    tx_on = false;  // a new request withdrew it; start over once it is ready
  }
  return EMU_FILL;
}

static void __not_in_flash_func(post)(bool truncated) {
  rx_mode = RX_IGNORE;
  if (link_state == L_PENDING) return;  // core0 still busy: dropped
  req_ptr = rxb[rx_cur];
  req_len = rx_n;
  req_truncated = truncated;
  rx_cur ^= 1u;
  led_note_rx();
  __dmb();
  link_state = L_PENDING;
}

static void __not_in_flash_func(rx_byte)(uint8_t b) {
  switch (rx_mode) {
    case RX_FIRST:
      if (b != EMU_MAGIC0) {
        rx_mode = RX_IGNORE;  // a poll
        return;
      }
      rx_mode = RX_FRAME;
      rx_n = 0;
      rx_need = 0;
      // fall through
    case RX_FRAME:
      if (rx_n < EMU_FRAME_MAX) rxb[rx_cur][rx_n] = b;
      rx_n++;
      if (rx_n == EMU_HEADER_SIZE) {
        uint32_t len = (uint32_t)rxb[rx_cur][6] | ((uint32_t)rxb[rx_cur][7] << 8);
        // Too long to be a frame: keep counting until the window ends,
        // then hand over what fitted, as truncated.
        rx_need = len <= EMU_MAX_PAYLOAD ? EMU_HEADER_SIZE + len + EMU_CRC_SIZE : 0u;
      }
      if (rx_need && rx_n == rx_need) post(false);
      return;
    default:
      return;
  }
}

// Streams from the response's first byte in the next window, if one is ready
// by then (or becomes ready during it).
static void __not_in_flash_func(arm)(void) {
  tx_on = false;
  spis_arm(next_tx);
}

static void __not_in_flash_func(window_start)(void) {
  spis_go();
  rx_mode = RX_FIRST;
}

static void __not_in_flash_func(window_end)(void) {
  spis_stop();
  if (rx_mode == RX_FRAME) {
    if (rx_n > EMU_FRAME_MAX) rx_n = EMU_FRAME_MAX;
    post(true);
  }
  rx_mode = RX_IGNORE;
  arm();
}

// Runs from RAM only and never touches flash, so it keeps going while core0
// programs or erases it.
void __not_in_flash_func(emu_link_run)(void) {
  bool sel = false;
  uint32_t deassert_confirm = 0;
  arm();

  for (;;) {
    if (sel) {
      // Never let the transmit FIFO run dry; consume everything clocked in.
      while (spis_tx_level() < SPIS_TX_PRIME) spis_tx_put(next_tx());
      while (spis_rx_ready()) {
        rx_byte(spis_rx_get());
        while (spis_tx_level() < SPIS_TX_PRIME) spis_tx_put(next_tx());
      }
    }

    bool cs = spis_selected();
    if (cs) {
      deassert_confirm = 0;
      if (!sel) {
        sel = true;
        window_start();
      }
    } else if (sel && ++deassert_confirm >= CS_DEASSERT_CONFIRM_POLLS) {
      sel = false;
      // Drain what arrived just before the deassert, then close the window.
      while (spis_rx_ready()) rx_byte(spis_rx_get());
      window_end();
    }
  }
}

// ---- core0 ----------------------------------------------------------------
const uint8_t *emu_link_request(uint32_t *len, bool *truncated) {
  if (link_state != L_PENDING) return NULL;
  __dmb();
  *len = req_len;
  *truncated = req_truncated;
  return req_ptr;
}

uint8_t *emu_link_response_buf(void) { return resp; }

void emu_link_reply(uint32_t len) {
  if (len) resp_len = len;
  __dmb();
  link_state = L_READY;
}

