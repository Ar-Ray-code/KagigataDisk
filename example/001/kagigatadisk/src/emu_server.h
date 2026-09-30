// The Kagigata protocol server, on core0: takes the request frames core1
// collected (emu_link.c), checks and decodes them, carries them out on the
// object store (objstore.c) and hands the response frame back to core1.
//
// A request carrying the same sequence number, command and CRC as the one
// before it is a resend (the host did not get the answer): it is answered
// with the response it got the first time, not carried out twice.
#ifndef EMU_SERVER_H
#define EMU_SERVER_H

#include <stdint.h>

void emu_server_init(void);
void emu_server_task(void);  // call from core0's main loop

#endif  // EMU_SERVER_H
