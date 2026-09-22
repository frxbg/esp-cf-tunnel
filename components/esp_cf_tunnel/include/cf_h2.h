#ifndef CF_H2_H
#define CF_H2_H
#include "cf_headers.h"

#define CF_H2_APPLICATION_MAX 4u
#define CF_H2_HEADER_MAX 48u
#define CF_H2_HEADER_BYTES 8192u
#define CF_H2_CHUNK 2048u
#define CF_H2_NGHEAP_MAX 65536u
typedef struct cf_h2 cf_h2;
typedef enum { CF_H2_HTTP, CF_H2_CONTROL, CF_H2_CONFIG } cf_h2_kind;
typedef struct {
    int32_t stream;
    cf_h2_kind kind;
    const cf_header *headers;
    size_t count;
    bool end_stream;
} cf_h2_request;
typedef struct {
    void *context;
    /* Headers borrow one session scratch area, valid only during this call.
     * DATA must be consumed/copied completely before returning. The initial
     * backend subset rejects a stream whose consumer cannot accept a chunk;
     * it never blocks the owner or allocates an unbounded body queue. */
    cf_result (*request)(void *, const cf_h2_request *);
    cf_result (*data)(void *, int32_t, cf_bytes, bool end_stream);
    /* Return CF_ERR_STATE to defer output; later call cf_h2_resume. */
    cf_result (*read_response)(void *, int32_t, uint8_t *, size_t, size_t *, bool *eof);
    void (*closed)(void *, int32_t, uint32_t error);
    void (*random)(uint8_t *, size_t);
} cf_h2_callbacks;
typedef struct {
    size_t engine_bytes, ngheap_current, ngheap_peak, ngheap_limit;
    uint32_t active_streams, rejected_streams;
    int32_t last_error;
    uint32_t goaway_error;
    uint32_t peer_goaway_error;
    int32_t peer_goaway_last_stream;
    char local_goaway_reason[64];
    bool local_goaway;
    bool peer_settings, goaway;
} cf_h2_stats;

/* Single owner. Uses the HTTP/2 SERVER role and requires the client preface.
 * A bounded custom allocator accounts for every nghttp2 session allocation,
 * including allocation metadata and the transient peak of realloc. */
cf_result cf_h2_create(cf_h2 **out, const cf_h2_callbacks *callbacks, unsigned application_streams, size_t ngheap_limit);
void cf_h2_destroy(cf_h2 *h);
cf_result cf_h2_receive(cf_h2 *h, cf_bytes input, size_t *consumed, uint64_t now_ms);
/* Output borrows nghttp2 storage; may be partially written. Call sent for
 * exactly the transmitted bytes before requesting more output. Never retain
 * the pointer after sent exhausts it. Input may progress while output waits. */
cf_result cf_h2_output(cf_h2 *h, cf_bytes *bytes);
cf_result cf_h2_sent(cf_h2 *h, size_t bytes);
cf_result cf_h2_respond(cf_h2 *h, int32_t stream, unsigned status,
                       const cf_header *headers, size_t count, bool origin);
cf_result cf_h2_resume(cf_h2 *h, int32_t stream);
cf_result cf_h2_reset(cf_h2 *h, int32_t stream, uint32_t error);
cf_result cf_h2_shutdown(cf_h2 *h);
void cf_h2_tick(cf_h2 *h, uint64_t now_ms);
void cf_h2_get_stats(const cf_h2 *h, cf_h2_stats *out);
cf_bytes cf_h2_header(const cf_h2_request *request, const char *lowercase_name);
#endif
