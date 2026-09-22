#include "cf_h2.h"
#include <nghttp2/nghttp2.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef union { long double alignment; void *pointer; size_t bytes; } allocation;
typedef struct {
    int32_t id;
    cf_h2_kind kind;
    bool headers_done, response, reset, deferred;
    uint64_t progress_ms;
} stream;
struct cf_h2 {
    nghttp2_session *session;
    cf_h2_callbacks callbacks;
    cf_h2_stats stats;
    unsigned app_limit;
    uint64_t now_ms;
    stream streams[CF_H2_APPLICATION_MAX + 2];
    uint8_t header_bytes[CF_H2_HEADER_BYTES];
    cf_header headers[CF_H2_HEADER_MAX];
    size_t header_count, header_used;
    int32_t header_stream;
    char serialized[CF_HEADER_WIRE_MAX];
    cf_bytes pending;
    cf_result error;
};
static void *allocate(size_t size, void *context)
{
    cf_h2 *h = context;
    allocation *a;
    size_t total;
    if (size > h->stats.ngheap_limit - sizeof(*a)) return NULL;
    total = size + sizeof(*a);
    if (total > h->stats.ngheap_limit - h->stats.ngheap_current) return NULL;
    a = malloc(total);
    if (!a) return NULL;
    a->bytes = total;
    h->stats.ngheap_current += total;
    if (h->stats.ngheap_peak < h->stats.ngheap_current) h->stats.ngheap_peak = h->stats.ngheap_current;
    return a + 1;
}
static void deallocate(void *p, void *context)
{
    cf_h2 *h = context;
    allocation *a;
    size_t bytes;
    if (!p) return;
    a = (allocation *)p - 1; bytes = a->bytes;
    h->stats.ngheap_current -= bytes;
    cf_secure_zero(a, bytes); free(a);
}
static void *resize(void *p, size_t size, void *context)
{
    void *next;
    size_t old;
    if (!p) return allocate(size, context);
    if (!size) { deallocate(p, context); return NULL; }
    old = ((allocation *)p - 1)->bytes - sizeof(allocation);
    next = allocate(size, context); /* Account old+new simultaneously. */
    if (!next) return NULL;
    memcpy(next, p, old < size ? old : size);
    deallocate(p, context); return next;
}
static void *zero_allocate(size_t n, size_t size, void *context)
{
    void *p;
    if (n && size > SIZE_MAX / n) return NULL;
    p = allocate(n * size, context);
    if (p) memset(p, 0, n * size);
    return p;
}
static stream *find_stream(cf_h2 *h, int32_t id)
{
    unsigned i;
    for (i = 0; i < CF_H2_APPLICATION_MAX + 2; ++i) if (h->streams[i].id == id) return &h->streams[i];
    return NULL;
}
static bool equals(cf_bytes b, const char *s) { return b.len == strlen(s) && !memcmp(b.ptr, s, b.len); }
cf_bytes cf_h2_header(const cf_h2_request *r, const char *name)
{
    size_t i;
    for (i = 0; i < r->count; ++i) if (equals(r->headers[i].name, name)) return r->headers[i].value;
    return (cf_bytes){0};
}
cf_result cf_h2_reset(cf_h2 *h, int32_t id, uint32_t error)
{
    stream *s;
    if (!h) return CF_ERR_ARGUMENT;
    s = find_stream(h, id);
    if (s && s->reset) return CF_OK;
    if (nghttp2_submit_rst_stream(h->session, NGHTTP2_FLAG_NONE, id, error)) return h->error = CF_ERR_MEMORY;
    if (s) s->reset = true;
    ++h->stats.rejected_streams;
    return CF_OK;
}
static int begin_headers(nghttp2_session *session, const nghttp2_frame *frame, void *context)
{
    cf_h2 *h = context;
    unsigned i;
    (void)session;
    h->header_stream = 0;
    /* Trailers are outside the initial native-backend subset. */
    if (frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
        return cf_h2_reset(h, frame->hd.stream_id, NGHTTP2_PROTOCOL_ERROR) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    for (i = 0; i < CF_H2_APPLICATION_MAX + 2; ++i) if (!h->streams[i].id) break;
    if (i == CF_H2_APPLICATION_MAX + 2) {
        return cf_h2_reset(h, frame->hd.stream_id, NGHTTP2_REFUSED_STREAM) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    h->streams[i] = (stream){.id = frame->hd.stream_id, .progress_ms = h->now_ms};
    ++h->stats.active_streams;
    h->header_stream = frame->hd.stream_id;
    h->header_count = h->header_used = 0;
    return 0;
}
static int on_header(nghttp2_session *session, const nghttp2_frame *frame,
                     const uint8_t *name, size_t namelen, const uint8_t *value, size_t valuelen,
                     uint8_t flags, void *context)
{
    cf_h2 *h = context;
    cf_header *header;
    size_t i;
    (void)session; (void)flags;
    if (h->header_stream != frame->hd.stream_id) return 0;
    if (h->header_count == CF_H2_HEADER_MAX || namelen > CF_H2_HEADER_BYTES - h->header_used ||
        valuelen > CF_H2_HEADER_BYTES - h->header_used - namelen) goto reject;
    /* Duplicate control/pseudo fields must not yield an ambiguous routing decision. */
    if ((namelen && name[0] == ':') || (namelen >= 15 && !memcmp(name, "cf-cloudflared-", 15))) {
        for (i = 0; i < h->header_count; ++i)
            if (h->headers[i].name.len == namelen && !memcmp(h->headers[i].name.ptr, name, namelen)) goto reject;
    }
    header = &h->headers[h->header_count++];
    header->name = (cf_bytes){h->header_bytes + h->header_used, namelen};
    memcpy(h->header_bytes + h->header_used, name, namelen); h->header_used += namelen;
    header->value = (cf_bytes){h->header_bytes + h->header_used, valuelen};
    memcpy(h->header_bytes + h->header_used, value, valuelen); h->header_used += valuelen;
    return 0;
reject:
    h->header_stream = 0;
    return cf_h2_reset(h, frame->hd.stream_id, NGHTTP2_ENHANCE_YOUR_CALM) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
}
static int received_frame(nghttp2_session *session, const nghttp2_frame *frame, void *context)
{
    cf_h2 *h = context;
    stream *s = find_stream(h, frame->hd.stream_id);
    (void)session;
    if (frame->hd.type == NGHTTP2_SETTINGS && !(frame->hd.flags & NGHTTP2_FLAG_ACK)) h->stats.peer_settings = true;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        h->stats.goaway = true; h->stats.goaway_error = frame->goaway.error_code;
        h->stats.peer_goaway_error = frame->goaway.error_code;
        h->stats.peer_goaway_last_stream = frame->goaway.last_stream_id;
    }
    if (!s || s->reset) return 0;
    if (frame->hd.type == NGHTTP2_HEADERS && h->header_stream == frame->hd.stream_id) {
        cf_h2_request request = {s->id, CF_H2_HTTP, h->headers, h->header_count, (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0};
        cf_bytes upgrade = cf_h2_header(&request, "cf-cloudflared-proxy-connection-upgrade");
        unsigned count = 0, i, limit;
        if (equals(upgrade, "control-stream")) request.kind = CF_H2_CONTROL;
        else if (equals(upgrade, "update-configuration")) request.kind = CF_H2_CONFIG;
        else if (upgrade.len) return cf_h2_reset(h, s->id, NGHTTP2_REFUSED_STREAM) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
        s->kind = request.kind;
        for (i = 0; i < CF_H2_APPLICATION_MAX + 2; ++i)
            if (h->streams[i].id && h->streams[i].headers_done && !h->streams[i].reset && h->streams[i].kind == s->kind) ++count;
        limit = s->kind == CF_H2_HTTP ? h->app_limit : 1;
        if (count >= limit) return cf_h2_reset(h, s->id, NGHTTP2_REFUSED_STREAM) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
        s->headers_done = true;
        if (h->callbacks.request(h->callbacks.context, &request) != CF_OK)
            return cf_h2_reset(h, s->id, NGHTTP2_REFUSED_STREAM) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
        cf_secure_zero(h->header_bytes, h->header_used);
        h->header_stream = 0;
    }
    if ((frame->hd.type == NGHTTP2_HEADERS || frame->hd.type == NGHTTP2_DATA) && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        if (h->callbacks.data(h->callbacks.context, s->id, (cf_bytes){0}, true) != CF_OK)
            return cf_h2_reset(h, s->id, NGHTTP2_CANCEL) == CF_OK ? 0 : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}
static int received_data(nghttp2_session *session, uint8_t flags, int32_t id,
                         const uint8_t *data, size_t len, void *context)
{
    cf_h2 *h = context;
    stream *s = find_stream(h, id);
    size_t offset = 0;
    (void)flags;
    if (!s || s->reset) {
        return nghttp2_session_consume_connection(session, len) ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
    }
    while (offset < len) {
        size_t n = len - offset;
        if (n > CF_H2_CHUNK) n = CF_H2_CHUNK;
        if (h->callbacks.data(h->callbacks.context, id, (cf_bytes){data + offset, n}, false) != CF_OK) {
            if (cf_h2_reset(h, id, NGHTTP2_CANCEL) != CF_OK) return NGHTTP2_ERR_CALLBACK_FAILURE;
            break;
        }
        offset += n;
    }
    s->progress_ms = h->now_ms;
    /* The callback has copied/consumed all accepted bytes. Rejected bytes are
     * discarded with a reset; return connection credit so control can progress. */
    return nghttp2_session_consume(session, id, len) ? NGHTTP2_ERR_CALLBACK_FAILURE : 0;
}
static int closed_stream(nghttp2_session *session, int32_t id, uint32_t error, void *context)
{
    cf_h2 *h = context;
    stream *s = find_stream(h, id);
    (void)session;
    if (s) {
        h->callbacks.closed(h->callbacks.context, id, error);
        memset(s, 0, sizeof(*s)); --h->stats.active_streams;
    }
    return 0;
}
static int sent_frame(nghttp2_session *session, const nghttp2_frame *frame, void *context)
{
    cf_h2 *h = context; (void)session;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        h->stats.goaway = h->stats.local_goaway = true;
        h->stats.goaway_error = frame->goaway.error_code;
        /* This is our outgoing frame: the pinned nghttp2 implementation uses
         * fixed protocol diagnostics here. Incoming GOAWAY data is never kept. */
        size_t n = frame->goaway.opaque_data_len;
        if (n >= sizeof(h->stats.local_goaway_reason)) n = sizeof(h->stats.local_goaway_reason) - 1;
        if (n) memcpy(h->stats.local_goaway_reason, frame->goaway.opaque_data, n);
        h->stats.local_goaway_reason[n] = 0;
    }
    return 0;
}
static int protocol_error(nghttp2_session *session, int code, const char *text, size_t len, void *context)
{
    /* Upstream text can contain peer data. Retain only the numeric error. */
    cf_h2 *h = context; (void)session; (void)text; (void)len;
    h->stats.last_error = code; return 0;
}
static int invalid_frame(nghttp2_session *session, const nghttp2_frame *frame, int code, void *context)
{
    cf_h2 *h = context; (void)session;
    if (frame->hd.type == NGHTTP2_GOAWAY) {
        h->stats.peer_goaway_error = frame->goaway.error_code;
        h->stats.peer_goaway_last_stream = frame->goaway.last_stream_id;
    }
    h->stats.last_error = code; return 0;
}
static nghttp2_ssize read_response(nghttp2_session *session, int32_t id, uint8_t *buf,
                                   size_t capacity, uint32_t *flags, nghttp2_data_source *source, void *context)
{
    cf_h2 *h = context;
    stream *s = find_stream(h, id);
    size_t n = 0;
    bool eof = false;
    cf_result rc;
    (void)session; (void)source;
    if (!s || s->reset) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    s->deferred = false;
    if (capacity > CF_H2_CHUNK) capacity = CF_H2_CHUNK;
    rc = h->callbacks.read_response(h->callbacks.context, id, buf, capacity, &n, &eof);
    if (rc == CF_ERR_STATE || (rc == CF_OK && !n && !eof)) { s->deferred = true; return NGHTTP2_ERR_DEFERRED; }
    if (rc != CF_OK || n > capacity) return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    if (eof) *flags |= NGHTTP2_DATA_FLAG_EOF;
    if (n || eof) s->progress_ms = h->now_ms;
    return (nghttp2_ssize)n;
}
cf_result cf_h2_create(cf_h2 **out, const cf_h2_callbacks *cb, unsigned apps, size_t limit)
{
    cf_h2 *h;
    nghttp2_option *option = NULL;
    nghttp2_session_callbacks *callbacks = NULL;
    int rc;
    nghttp2_mem memory;
    nghttp2_settings_entry settings[] = {
        /* Match cloudflared's wire setting. The reverse edge connection pool
         * retires a capacity-exhausted connection with GOAWAY(NO_ERROR).
         * Enforce local admission below, independently of the wire setting:
         * fixed app/control/config slots and the hard nghttp2 heap quota. */
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, UINT32_MAX},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, CF_H2_CHUNK},
        {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE, 1024},
        {NGHTTP2_SETTINGS_MAX_HEADER_LIST_SIZE, CF_H2_HEADER_BYTES}
    };
    if (!out) return CF_ERR_ARGUMENT;
    *out = NULL;
    if (!cb || !cb->request || !cb->data || !cb->read_response || !cb->closed || !cb->random ||
        (apps != 2 && apps != 4) || limit < 8192 || limit > CF_H2_NGHEAP_MAX) return CF_ERR_ARGUMENT;
    h = calloc(1, sizeof(*h));
    if (!h) return CF_ERR_MEMORY;
    h->callbacks = *cb; h->app_limit = apps;
    h->stats.engine_bytes = sizeof(*h); h->stats.ngheap_limit = limit;
    memory = (nghttp2_mem){h, allocate, deallocate, zero_allocate, resize};
    /* These two small temporary upstream objects use the default allocator;
     * they are released before create returns, never used in the I/O loop. */
    if (nghttp2_option_new(&option) || nghttp2_session_callbacks_new(&callbacks)) goto failed;
    nghttp2_option_set_no_auto_window_update(option, 1);
    nghttp2_option_set_no_closed_streams(option, 1);
    nghttp2_option_set_max_reserved_remote_streams(option, 0);
    nghttp2_option_set_max_deflate_dynamic_table_size(option, 1024);
    nghttp2_option_set_max_send_header_block_length(option, CF_H2_HEADER_BYTES);
    nghttp2_option_set_max_outbound_ack(option, 16);
    nghttp2_option_set_max_settings(option, 16);
    nghttp2_option_set_max_continuations(option, 8);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, begin_headers);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, received_frame);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks, received_data);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, closed_stream);
    nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, sent_frame);
    nghttp2_session_callbacks_set_error_callback2(callbacks, protocol_error);
    nghttp2_session_callbacks_set_on_invalid_frame_recv_callback(callbacks, invalid_frame);
    nghttp2_session_callbacks_set_rand_callback(callbacks, cb->random);
    rc = nghttp2_session_server_new3(&h->session, callbacks, h, option, &memory);
    if (rc) goto failed;
    if (nghttp2_submit_settings(h->session, NGHTTP2_FLAG_NONE, settings, sizeof(settings) / sizeof(settings[0]))) goto failed;
    nghttp2_option_del(option); nghttp2_session_callbacks_del(callbacks);
    *out = h; return CF_OK;
failed:
    nghttp2_option_del(option); nghttp2_session_callbacks_del(callbacks);
    cf_h2_destroy(h); return CF_ERR_MEMORY;
}
void cf_h2_destroy(cf_h2 *h)
{
    unsigned i;
    if (!h) return;
    for (i = 0; i < CF_H2_APPLICATION_MAX + 2; ++i) {
        if (h->streams[i].id) h->callbacks.closed(h->callbacks.context, h->streams[i].id, NGHTTP2_CANCEL);
    }
    nghttp2_session_del(h->session);
    cf_secure_zero(h, sizeof(*h)); free(h);
}
cf_result cf_h2_receive(cf_h2 *h, cf_bytes in, size_t *consumed, uint64_t now)
{
    nghttp2_ssize n;
    if (!h || !consumed || (!in.ptr && in.len)) return CF_ERR_ARGUMENT;
    *consumed = 0; h->now_ms = now;
    if (h->error != CF_OK) return h->error;
    n = nghttp2_session_mem_recv2(h->session, in.ptr, in.len);
    if (n < 0) { h->stats.last_error = (int32_t)n; return h->error = n == NGHTTP2_ERR_NOMEM ? CF_ERR_MEMORY : CF_ERR_FORMAT; }
    *consumed = (size_t)n; return h->error;
}
cf_result cf_h2_output(cf_h2 *h, cf_bytes *out)
{
    nghttp2_ssize n;
    const uint8_t *p;
    if (!h || !out) return CF_ERR_ARGUMENT;
    *out = (cf_bytes){0};
    if (h->error != CF_OK) return h->error;
    if (!h->pending.len) {
        n = nghttp2_session_mem_send2(h->session, &p);
        if (n < 0) { h->stats.last_error = (int32_t)n; return h->error = n == NGHTTP2_ERR_NOMEM ? CF_ERR_MEMORY : CF_ERR_STATE; }
        h->pending = (cf_bytes){p, (size_t)n};
    }
    *out = h->pending; return CF_OK;
}
cf_result cf_h2_sent(cf_h2 *h, size_t n)
{
    if (!h || n > h->pending.len) return CF_ERR_ARGUMENT;
    if (n) h->pending.ptr += n;
    h->pending.len -= n; return CF_OK;
}
static nghttp2_nv nv(const char *name, const char *value, size_t len)
{
    return (nghttp2_nv){(uint8_t *)name, (uint8_t *)value, strlen(name), len, NGHTTP2_NV_FLAG_NONE};
}
cf_result cf_h2_respond(cf_h2 *h, int32_t id, unsigned status, const cf_header *headers, size_t count, bool origin)
{
    stream *s;
    char code[4]; size_t n, i, fields = 3;
    nghttp2_nv values[4];
    nghttp2_data_provider2 provider = {0};
    const char *meta = origin ? CF_RESPONSE_META_ORIGIN : CF_RESPONSE_META_CONNECTOR;
    cf_result rc;
    if (!h || (count && !headers) || status < 200 || status > 599) return CF_ERR_ARGUMENT;
    provider.read_callback = read_response;
    s = find_stream(h, id);
    if (!s || !s->headers_done || s->response || s->reset) return CF_ERR_STATE;
    rc = cf_headers_encode(headers, count, h->serialized, sizeof(h->serialized), &n);
    if (rc != CF_OK) return rc;
    (void)snprintf(code, sizeof(code), "%u", status);
    values[0] = nv(":status", code, 3);
    values[1] = nv(CF_RESPONSE_HEADERS, h->serialized, n);
    values[2] = nv(CF_RESPONSE_META, meta, strlen(meta));
    for (i = 0; i < count; ++i) if (equals(headers[i].name, "content-length") || equals(headers[i].name, "Content-Length")) {
        if (fields == 4) return CF_ERR_FORMAT;
        values[fields++] = nv("content-length", (const char *)headers[i].value.ptr, headers[i].value.len);
    }
    if (nghttp2_submit_response2(h->session, id, values, fields, &provider)) return CF_ERR_MEMORY;
    s->response = true; return CF_OK;
}
cf_result cf_h2_resume(cf_h2 *h, int32_t id)
{
    stream *s;
    if (!h) return CF_ERR_ARGUMENT;
    s = find_stream(h, id);
    if (!s || !s->response || s->reset) return CF_ERR_STATE;
    if (!s->deferred) return CF_OK;
    if (nghttp2_session_resume_data(h->session, id)) return CF_ERR_STATE;
    s->deferred = false;
    return CF_OK;
}
cf_result cf_h2_shutdown(cf_h2 *h)
{
    if (!h) return CF_ERR_ARGUMENT;
    h->stats.goaway = true;
    return nghttp2_submit_goaway(h->session, NGHTTP2_FLAG_NONE, nghttp2_session_get_last_proc_stream_id(h->session),
        NGHTTP2_NO_ERROR, NULL, 0) ? CF_ERR_MEMORY : CF_OK;
}
void cf_h2_tick(cf_h2 *h, uint64_t now)
{
    unsigned i;
    if (!h) return;
    h->now_ms = now;
    for (i = 0; i < CF_H2_APPLICATION_MAX + 2; ++i) {
        stream *s = &h->streams[i];
        if (s->id && !s->reset && (!s->headers_done || s->kind != CF_H2_CONTROL) &&
            now >= s->progress_ms && now - s->progress_ms > 15000)
            (void)cf_h2_reset(h, s->id, NGHTTP2_CANCEL);
    }
}
void cf_h2_get_stats(const cf_h2 *h, cf_h2_stats *out) { if (h && out) *out = h->stats; }
