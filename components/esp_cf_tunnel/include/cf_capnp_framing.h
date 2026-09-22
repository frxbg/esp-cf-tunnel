#ifndef CF_CAPNP_FRAMING_H
#define CF_CAPNP_FRAMING_H
#include "cf_core.h"
#define CF_CAPNP_MAX_SEGMENTS 8u
#define CF_CAPNP_MESSAGE_MAX 16384u

/* Unpacked stream framing only; NOT an RPC or pointer/list decoder.
 * Fixed caller-owned buffer includes segment table. No allocation.
 * One owner; callback must not reenter feed/reset and must not retain views.
 * Any malformed frame or callback failure poisons the framer until reset. */
typedef struct {
    uint8_t *buffer;
    size_t capacity, used, target;
    uint32_t segments;
    unsigned phase;
    cf_result error;
} cf_capnp_framer;
typedef cf_result (*cf_capnp_message_fn)(void *context, cf_bytes frame);
cf_result cf_capnp_framer_init(cf_capnp_framer *f, uint8_t *buffer, size_t capacity);
void cf_capnp_framer_reset(cf_capnp_framer *f);
cf_result cf_capnp_feed(cf_capnp_framer *f, cf_bytes input, size_t *consumed,
                        cf_capnp_message_fn on_message, void *context);
cf_result cf_capnp_eof(const cf_capnp_framer *f);
/* Validate exact frame size and return a segment view. Does not follow pointers. */
cf_result cf_capnp_segment(cf_bytes frame, uint32_t index, cf_bytes *segment);
#endif
