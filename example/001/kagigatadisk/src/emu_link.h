// The frame link on core1: moves bytes between the PIO transport
// (spi_slave_transport.c) and two frame buffers, and nothing else - no
// decoding, no storage, no flash. core0 (emu_server.c) does the work.
//
// Per chip-select window:
//   * The first byte the host clocks in decides what the window is. The
//     frame magic starts a request: bytes are collected until the length in
//     its header is complete (or the window ends) and then handed to core0.
//     Anything else makes the window a poll: its bytes are ignored.
//   * Whatever the host clocks in, the board clocks out the current response
//     from its first byte - once core0 has one ready - and EMU_FILL around it.
//
// Handover: a request is posted only while core0 is not busy with the
// previous one (otherwise it is dropped: the host times out and sends it
// again). Posting withdraws the old response; core0 then answers
// with emu_link_reply().
#ifndef EMU_LINK_H
#define EMU_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "emu_protocol_defs.h"

void emu_link_init(void);                  // core0, before core1 starts
void __attribute__((noreturn)) emu_link_run(void);  // core1's entry point

// core0: the request waiting for an answer, or NULL. `truncated`: the window
// ended before the frame was complete.
const uint8_t *emu_link_request(uint32_t *len, bool *truncated);
// core0: where the response goes, and handing it over (`len` bytes, a whole
// frame). emu_link_reply(0) keeps the previous response, e.g. to answer a
// repeated request again.
uint8_t *emu_link_response_buf(void);
void emu_link_reply(uint32_t len);


#endif  // EMU_LINK_H
