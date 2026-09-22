#include "esp_cf_tunnel.h"
#include "cf_dns.h"
#include "cf_rpc.h"
#include "cf_config.h"
#include "esp_tls.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/tcp.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern const unsigned char cf_edge_ca_start[] asm("_binary_cloudflare_edge_roots_pem_start");
extern const unsigned char cf_edge_ca_end[] asm("_binary_cloudflare_edge_roots_pem_end");
#define RPC_QUEUE 4096u
#define TASK_STACK 12288u
struct esp_cf_tunnel {
    esp_cf_tunnel_config callbacks;
    portMUX_TYPE lock;
    TaskHandle_t task;
    bool stop_requested, reload_requested;
    esp_cf_tunnel_snapshot snapshot;
    cf_lifecycle life;
    cf_rpc rpc;
    cf_remote_config remote;
    cf_h2 *h2;
    esp_tls_t *tls;
    esp_tls_cfg_t tls_config;
    cf_credentials credentials;
    cf_rpc_options options;
    char hostname[254], edge_ip[16], dns_name[254], message[128];
    char last_failure[128];
    uint64_t last_failure_ms;
    uint32_t failures;
    esp_cf_environment environment;
    cf_dns_answer *dns_answer;
    uint8_t dns_packet[CF_DNS_PACKET_MAX];
    int dns_socket;
    uint16_t dns_id;
    unsigned dns_stage;
    uint32_t attempts;
    uint64_t deadline_ms, shutdown_ms, write_stalled_ms;
    bool settings_ready, attempt_started, fault, stopping, goaway_sent, closing;
    int32_t control_stream, config_stream;
    cf_capnp_framer framer;
    uint8_t rpc_frame[CF_CAPNP_MESSAGE_MAX], rpc_queue[RPC_QUEUE];
    size_t rpc_queued;
    uint8_t config_bytes[CF_CONFIG_MESSAGE_MAX];
    size_t config_used, config_reply_used, config_reply_sent;
    char config_reply[128];
    bool config_pending, config_response;
};
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }
static void message(esp_cf_tunnel *t, const char *text) { if (text != t->message) snprintf(t->message, sizeof(t->message), "%s", text); }
static void secure_random(uint8_t *out, size_t size) { esp_fill_random(out, size); }
const char *esp_cf_tunnel_state_name(cf_state s)
{
    static const char *const names[] = {"Stopped", "Waiting for network", "Waiting for clock", "Discovering edge",
        "Connecting", "TLS handshake", "HTTP/2 negotiation", "Registering", "Waiting for configuration",
        "Online", "Retry backoff", "Authentication rejected", "Configuration rejected", "Stopping"};
    return (unsigned)s < sizeof(names) / sizeof(names[0]) ? names[s] : "Unknown";
}
static void publish(esp_cf_tunnel *t)
{
    esp_cf_tunnel_snapshot next = {0};
    next.state = t->life.state; next.settings_ready = t->settings_ready;
    next.registered = t->life.registered; next.configured = t->life.configured;
    next.config_version = t->remote.valid ? t->remote.version : -1;
    next.attempts = t->attempts; next.retry_at_ms = t->life.retry_at_ms;
    snprintf(next.edge_ip, sizeof(next.edge_ip), "%s", t->edge_ip);
    snprintf(next.location, sizeof(next.location), "%s", t->rpc.location);
    snprintf(next.message, sizeof(next.message), "%s", t->message);
    snprintf(next.last_failure, sizeof(next.last_failure), "%s", t->last_failure);
    next.last_failure_ms = t->last_failure_ms; next.failures = t->failures;
    if (t->h2) cf_h2_get_stats(t->h2, &next.http2);
    next.task_stack_min = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    portENTER_CRITICAL(&t->lock);
    cf_state previous = t->snapshot.state;
    t->snapshot = next;
    portEXIT_CRITICAL(&t->lock);
    if (previous != next.state) ESP_LOGI("cf_tunnel", "%s", esp_cf_tunnel_state_name(next.state));
}
static void close_connection(esp_cf_tunnel *t)
{
    if (t->dns_socket >= 0) { close(t->dns_socket); t->dns_socket = -1; }
    free(t->dns_answer); t->dns_answer = NULL;
    t->closing = true;
    if (t->h2) { cf_h2_destroy(t->h2); t->h2 = NULL; }
    t->closing = false;
    if (t->tls) { esp_tls_conn_destroy(t->tls); t->tls = NULL; }
    cf_rpc_reset(&t->rpc); cf_config_init(&t->remote);
    cf_secure_zero(&t->credentials, sizeof(t->credentials));
    cf_secure_zero(t->rpc_frame, sizeof(t->rpc_frame));
    cf_secure_zero(t->rpc_queue, sizeof(t->rpc_queue));
    cf_secure_zero(t->config_bytes, sizeof(t->config_bytes));
    t->rpc_queued = t->config_used = t->config_reply_used = t->config_reply_sent = 0;
    t->config_pending = t->config_response = false;
    t->control_stream = t->config_stream = 0;
    t->attempt_started = t->fault = t->goaway_sent = false;
    t->write_stalled_ms = 0;
}
static void retry(esp_cf_tunnel *t, const char *why)
{
    bool permanent = t->rpc.state == CF_RPC_REJECTED && !t->rpc.should_retry;
    uint64_t hint = t->rpc.state == CF_RPC_REJECTED ? t->rpc.retry_after_ms : 0;
    message(t, why);
    cf_h2_stats stats = {0};
    if (t->h2) cf_h2_get_stats(t->h2, &stats);
    snprintf(t->last_failure, sizeof(t->last_failure), "%.60s [h2=%ld, goaway=%s:%lu, peer=%lu/%ld]",
        why, (long)stats.last_error, !stats.goaway ? "none" : stats.local_goaway ? "local" : "peer", (unsigned long)stats.goaway_error,
        (unsigned long)stats.peer_goaway_error, (long)stats.peer_goaway_last_stream);
    t->last_failure_ms = now_ms(); ++t->failures;
    ESP_LOGW("cf_tunnel", "%s; streams=%lu rejected=%lu ngheap=%u/%u", t->last_failure,
        (unsigned long)stats.active_streams, (unsigned long)stats.rejected_streams,
        (unsigned)stats.ngheap_current, (unsigned)stats.ngheap_peak);
    if (permanent) (void)cf_lifecycle_advance(&t->life, CF_EVENT_AUTH_REJECTED);
    else (void)cf_lifecycle_retry(&t->life, now_ms(), hint > UINT32_MAX ? UINT32_MAX : (uint32_t)hint, esp_random());
    close_connection(t);
}
static cf_result append_rpc(esp_cf_tunnel *t, const uint8_t *bytes, size_t size)
{
    if (size > sizeof(t->rpc_queue) - t->rpc_queued) return CF_ERR_LIMIT;
    memcpy(t->rpc_queue + t->rpc_queued, bytes, size); t->rpc_queued += size;
    if (t->control_stream) (void)cf_h2_resume(t->h2, t->control_stream);
    return CF_OK;
}
static cf_result rpc_message(void *context, cf_bytes frame)
{
    esp_cf_tunnel *t = context;
    uint8_t out[CF_RPC_TX_MAX]; size_t n;
    cf_result rc = cf_rpc_receive(&t->rpc, frame, out, sizeof(out), &n);
    if (append_rpc(t, out, n) != CF_OK) rc = CF_ERR_LIMIT;
    cf_secure_zero(out, sizeof(out));
    if (rc != CF_OK) { message(t, "Registration RPC failed or used an unsupported message."); t->fault = true; return rc; }
    if (t->rpc.state == CF_RPC_READY) {
        t->options.local_ip = (cf_bytes){t->environment.local_ip, 4};
        t->options.previous_attempts = t->attempts > 256 ? 255 : (uint8_t)(t->attempts - 1);
        rc = cf_rpc_register(&t->rpc, &t->credentials, &t->options, out, sizeof(out), &n);
        cf_secure_zero(&t->credentials, sizeof(t->credentials));
        if (rc == CF_OK) rc = append_rpc(t, out, n);
        cf_secure_zero(out, sizeof(out));
    } else if (t->rpc.state == CF_RPC_REGISTERED) {
        (void)cf_lifecycle_advance(&t->life, CF_EVENT_REGISTERED);
        message(t, t->life.configured ? "Tunnel is serving the configured hostname." : "Registered. Waiting for remote ingress configuration.");
        t->deadline_ms = now_ms() + 30000;
    } else if (t->rpc.state == CF_RPC_REJECTED) {
        message(t, t->rpc.should_retry ? "Cloudflare rejected registration; retry requested." : "Cloudflare rejected the tunnel credentials.");
        t->fault = true;
    }
    if (rc != CF_OK) t->fault = true;
    return rc;
}
static bool equal(cf_bytes b, const char *s) { return b.len == strlen(s) && !memcmp(b.ptr, s, b.len); }
static cf_result h2_request(void *context, const cf_h2_request *request)
{
    esp_cf_tunnel *t = context;
    if (t->stopping) return CF_ERR_STATE;
    if (request->kind == CF_H2_CONTROL) {
        uint8_t out[CF_RPC_TX_MAX]; size_t n;
        if (t->control_stream || request->end_stream) return CF_ERR_STATE;
        t->control_stream = request->stream;
        if (t->life.state == CF_HTTP2) (void)cf_lifecycle_advance(&t->life, CF_EVENT_HTTP2_READY);
        cf_rpc_init(&t->rpc);
        (void)cf_capnp_framer_init(&t->framer, t->rpc_frame, sizeof(t->rpc_frame));
        cf_result rc = cf_rpc_begin(&t->rpc, out, sizeof(out), &n);
        if (rc == CF_OK) rc = append_rpc(t, out, n);
        cf_secure_zero(out, sizeof(out));
        if (rc != CF_OK) return rc;
        t->deadline_ms = now_ms() + 30000;
        return cf_h2_respond(t->h2, request->stream, 200, NULL, 0, false);
    }
    if (request->kind == CF_H2_CONFIG) {
        if (t->config_stream) return CF_ERR_STATE;
        t->config_stream = request->stream;
        t->config_used = t->config_reply_used = t->config_reply_sent = 0;
        t->config_pending = t->config_response = false;
        return CF_OK;
    }
    if (t->life.state != CF_ONLINE || !equal(cf_h2_header(request, ":authority"), t->remote.hostname)) return CF_ERR_STATE;
    return t->callbacks.request(t->callbacks.context, t->h2, request);
}
static cf_result h2_data(void *context, int32_t id, cf_bytes data, bool end)
{
    esp_cf_tunnel *t = context;
    if (id == t->control_stream) {
        size_t used;
        if (end && t->rpc.state != CF_RPC_CLOSED) { t->fault = true; message(t, "The control stream ended unexpectedly."); return CF_ERR_TRUNCATED; }
        if (!data.len) return CF_OK;
        cf_result rc = cf_capnp_feed(&t->framer, data, &used, rpc_message, t);
        if (rc != CF_OK || used != data.len) { t->fault = true; return rc == CF_OK ? CF_ERR_STATE : rc; }
        return CF_OK;
    }
    if (id == t->config_stream) {
        if (t->config_pending || t->config_response || data.len > sizeof(t->config_bytes) - t->config_used) return CF_ERR_LIMIT;
        if (data.len) memcpy(t->config_bytes + t->config_used, data.ptr, data.len);
        t->config_used += data.len;
        if (end) t->config_pending = true;
        return CF_OK;
    }
    return t->callbacks.data(t->callbacks.context, id, data, end);
}
static cf_result h2_read(void *context, int32_t id, uint8_t *out, size_t cap, size_t *n, bool *eof)
{
    esp_cf_tunnel *t = context;
    if (id == t->control_stream) {
        *n = t->rpc_queued < cap ? t->rpc_queued : cap;
        if (*n) {
            memcpy(out, t->rpc_queue, *n); t->rpc_queued -= *n;
            memmove(t->rpc_queue, t->rpc_queue + *n, t->rpc_queued);
            cf_secure_zero(t->rpc_queue + t->rpc_queued, *n);
        }
        *eof = t->rpc.state == CF_RPC_CLOSED && !t->rpc_queued;
        return *n || *eof ? CF_OK : CF_ERR_STATE;
    }
    if (id == t->config_stream) {
        if (!t->config_response) return CF_ERR_STATE;
        *n = t->config_reply_used - t->config_reply_sent;
        if (*n > cap) *n = cap;
        memcpy(out, t->config_reply + t->config_reply_sent, *n); t->config_reply_sent += *n;
        *eof = t->config_reply_sent == t->config_reply_used; return CF_OK;
    }
    return t->callbacks.read_response(t->callbacks.context, id, out, cap, n, eof);
}
static void h2_closed(void *context, int32_t id, uint32_t error)
{
    esp_cf_tunnel *t = context;
    if (id == t->control_stream) {
        if (!t->closing && (!t->stopping || t->rpc.state != CF_RPC_CLOSED)) { t->fault = true; message(t, "Cloudflare closed the control stream."); }
        t->control_stream = 0;
    } else if (id == t->config_stream) {
        t->config_stream = 0; t->config_used = 0; t->config_pending = t->config_response = false;
        cf_secure_zero(t->config_bytes, sizeof(t->config_bytes));
    } else t->callbacks.closed(t->callbacks.context, id, error);
}
static void apply_config(esp_cf_tunnel *t)
{
    if (!t->config_pending || !t->callbacks.json_try_lock(t->callbacks.context)) return;
    const char *reason;
    cf_result rc = cf_config_apply_ex(&t->remote, (cf_bytes){t->config_bytes, t->config_used}, t->hostname, t->callbacks.service, &reason);
    t->callbacks.json_unlock(t->callbacks.context);
    (void)cf_config_response(&t->remote, rc, t->config_reply, sizeof(t->config_reply), &t->config_reply_used);
    cf_secure_zero(t->config_bytes, sizeof(t->config_bytes)); t->config_used = 0;
    t->config_pending = false; t->config_response = true;
    cf_header header = {(cf_bytes){(const uint8_t *)"content-type", 12}, (cf_bytes){(const uint8_t *)"application/json", 16}};
    if (cf_h2_respond(t->h2, t->config_stream, 200, &header, 1, false) != CF_OK) t->fault = true;
    (void)cf_lifecycle_advance(&t->life, rc == CF_OK ? CF_EVENT_CONFIG_APPLIED : CF_EVENT_CONFIG_REJECTED);
    message(t, reason);
}
static cf_result dns_send(esp_cf_tunnel *t, const char *name, cf_dns_type type)
{
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(53)};
    size_t n;
    if (t->dns_socket >= 0) close(t->dns_socket);
    t->dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (t->dns_socket < 0) return CF_ERR_STATE;
    memcpy(&address.sin_addr.s_addr, t->environment.dns_ip, 4);
    if (fcntl(t->dns_socket, F_SETFL, O_NONBLOCK) < 0 || connect(t->dns_socket, (struct sockaddr *)&address, sizeof(address))) return CF_ERR_STATE;
    t->dns_id = (uint16_t)esp_random();
    snprintf(t->dns_name, sizeof(t->dns_name), "%s", name);
    cf_result rc = cf_dns_query(name, type, t->dns_id, t->dns_packet, sizeof(t->dns_packet), &n);
    if (rc != CF_OK) return rc;
    if (send(t->dns_socket, t->dns_packet, n, 0) != (int)n) return CF_ERR_STATE;
    t->deadline_ms = now_ms() + 5000; return CF_OK;
}
static bool discover(esp_cf_tunnel *t)
{
    if (!t->attempt_started) {
        if (!t->callbacks.json_try_lock(t->callbacks.context)) return true;
        cf_result rc = t->callbacks.credentials(t->callbacks.context, &t->credentials, t->hostname);
        t->callbacks.json_unlock(t->callbacks.context);
        t->settings_ready = rc == CF_OK && t->hostname[0] && memchr(t->hostname, 0, sizeof(t->hostname));
        if (!t->settings_ready) { cf_secure_zero(&t->credentials, sizeof(t->credentials)); message(t, "Save a named tunnel token and public hostname in Settings."); return true; }
        t->attempt_started = true; ++t->attempts; t->dns_stage = 0;
        t->dns_answer = calloc(1, sizeof(*t->dns_answer));
        if (!t->dns_answer || dns_send(t, CF_EDGE_DISCOVERY, CF_DNS_SRV) != CF_OK) { message(t, "Unable to start edge DNS discovery."); return false; }
        message(t, "Resolving Cloudflare edge service records.");
    }
    int n = recv(t->dns_socket, t->dns_packet, sizeof(t->dns_packet), 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if (now_ms() < t->deadline_ms) return true;
        message(t, "DNS query timed out."); return false;
    }
    if (n <= 0) { message(t, "DNS query failed."); return false; }
    cf_result rc = cf_dns_parse((cf_bytes){t->dns_packet, (size_t)n}, t->dns_name,
        t->dns_stage ? CF_DNS_A : CF_DNS_SRV, t->dns_id, t->dns_answer);
    if (rc != CF_OK) { message(t, "DNS response is invalid, truncated, or unsupported."); return false; }
    if (!t->dns_stage) {
        /* Sort by priority. Failover rotates across the advertised SRV entries
         * on reconnect; within equal priority use the advertised weights. */
        size_t count = t->dns_answer->count;
        for (size_t i = 0; i < count; ++i) for (size_t j = i + 1; j < count; ++j)
            if (t->dns_answer->records[j].priority < t->dns_answer->records[i].priority) {
                cf_dns_record tmp = t->dns_answer->records[i]; t->dns_answer->records[i] = t->dns_answer->records[j]; t->dns_answer->records[j] = tmp;
            }
        size_t pick = (t->attempts - 1) % count, first = pick, last = pick;
        uint16_t priority = t->dns_answer->records[pick].priority;
        while (first && t->dns_answer->records[first - 1].priority == priority) --first;
        while (last + 1 < count && t->dns_answer->records[last + 1].priority == priority) ++last;
        uint32_t weights = 0;
        for (size_t i = first; i <= last; ++i) weights += t->dns_answer->records[i].weight;
        if (weights) {
            uint32_t value = esp_random() % weights;
            for (pick = first; pick < last; ++pick) { if (value < t->dns_answer->records[pick].weight) break; value -= t->dns_answer->records[pick].weight; }
        }
        t->dns_stage = 1;
        char target[254]; snprintf(target, sizeof(target), "%s", t->dns_answer->records[pick].target);
        if (dns_send(t, target, CF_DNS_A) != CF_OK) { message(t, "Unable to resolve the discovered edge hostname."); return false; }
        return true;
    }
    const uint8_t *ip = t->dns_answer->records[esp_random() % t->dns_answer->count].ip;
    snprintf(t->edge_ip, sizeof(t->edge_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    close(t->dns_socket); t->dns_socket = -1; free(t->dns_answer); t->dns_answer = NULL;
    t->tls = esp_tls_init();
    if (!t->tls) { message(t, "Not enough memory for TLS."); return false; }
    t->tls_config = (esp_tls_cfg_t){.non_block = true, .timeout_ms = 1, .common_name = CF_EDGE_TLS_NAME,
        .cacert_buf = cf_edge_ca_start, .cacert_bytes = (unsigned int)(cf_edge_ca_end - cf_edge_ca_start)};
    t->deadline_ms = now_ms() + 30000;
    (void)cf_lifecycle_advance(&t->life, CF_EVENT_DISCOVERED);
    message(t, "Connecting to the discovered edge on TCP 7844."); return true;
}
static bool connect_tls(esp_cf_tunnel *t)
{
    int rc = esp_tls_conn_new_async(t->edge_ip, (int)strlen(t->edge_ip), CF_EDGE_PORT, &t->tls_config, t->tls);
    esp_tls_conn_state_t state;
    (void)esp_tls_get_conn_state(t->tls, &state);
    if (t->life.state == CF_CONNECT && (state == ESP_TLS_HANDSHAKE || rc == 1)) (void)cf_lifecycle_advance(&t->life, CF_EVENT_CONNECTED);
    if (rc < 0 || now_ms() >= t->deadline_ms) { message(t, "TLS connection failed. Check clock, CA trust, and outbound TCP 7844."); return false; }
    if (rc == 0) return true;
    int fd, enabled = 1;
    if (esp_tls_get_conn_sockfd(t->tls, &fd) == ESP_OK) (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
    cf_h2_callbacks callbacks = {t, h2_request, h2_data, h2_read, h2_closed, secure_random};
    if (cf_h2_create(&t->h2, &callbacks, t->callbacks.application_streams, CF_H2_NGHEAP_MAX) != CF_OK) { message(t, "Not enough memory for HTTP/2."); return false; }
    (void)cf_lifecycle_advance(&t->life, CF_EVENT_TLS_READY);
    t->deadline_ms = now_ms() + 30000;
    message(t, "TLS verified. Waiting for the edge HTTP/2 control stream."); return true;
}
static bool communicate(esp_cf_tunnel *t)
{
    uint8_t input[CF_H2_CHUNK];
    bool write_wait = false, write_needs_read = false;
    /* Retry a pending TLS write with the same borrowed buffer before doing
     * another TLS read. nghttp2 keeps this output stable until cf_h2_sent. */
    for (unsigned i = 0; !t->write_stalled_ms && i < 8; ++i) {
        ssize_t n = esp_tls_conn_read(t->tls, input, sizeof(input));
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) { write_wait = n == ESP_TLS_ERR_SSL_WANT_WRITE; break; }
        if (n <= 0) { snprintf(t->message, sizeof(t->message), "Edge TLS read ended (code %ld).", (long)n); return false; }
        size_t used;
        if (cf_h2_receive(t->h2, (cf_bytes){input, (size_t)n}, &used, now_ms()) != CF_OK || used != (size_t)n) { message(t, "HTTP/2 protocol or memory limit reached."); return false; }
    }
    cf_h2_stats stats; cf_h2_get_stats(t->h2, &stats);
    if (stats.peer_settings && t->life.state == CF_HTTP2) (void)cf_lifecycle_advance(&t->life, CF_EVENT_HTTP2_READY);
    if (stats.goaway && !t->stopping) {
        if (stats.local_goaway && stats.local_goaway_reason[0]) snprintf(t->message, sizeof(t->message), "HTTP/2: %s", stats.local_goaway_reason);
        else message(t, stats.local_goaway ? "Local HTTP/2 ended the connection (GOAWAY)." : "The edge requested a new connection (GOAWAY).");
        return false;
    }
    if (t->fault) return false;
    apply_config(t);
    cf_h2_tick(t->h2, now_ms());
    if (t->stopping && t->rpc.state == CF_RPC_CLOSED && !t->rpc_queued && !t->goaway_sent) {
        (void)cf_h2_shutdown(t->h2); t->goaway_sent = true;
    }
    for (unsigned i = 0; i < 8; ++i) {
        cf_bytes out;
        if (cf_h2_output(t->h2, &out) != CF_OK) { message(t, "Unable to encode the HTTP/2 response."); return false; }
        if (!out.len) break;
        size_t chunk = out.len < CF_H2_CHUNK ? out.len : CF_H2_CHUNK;
        ssize_t n = esp_tls_conn_write(t->tls, out.ptr, chunk);
        if (n == ESP_TLS_ERR_SSL_WANT_READ || n == ESP_TLS_ERR_SSL_WANT_WRITE) {
            if (!t->write_stalled_ms) t->write_stalled_ms = now_ms();
            if (now_ms() - t->write_stalled_ms > 15000) { message(t, "TLS output stalled."); return false; }
            write_wait = n == ESP_TLS_ERR_SSL_WANT_WRITE;
            write_needs_read = n == ESP_TLS_ERR_SSL_WANT_READ; break;
        }
        if (n <= 0 || cf_h2_sent(t->h2, (size_t)n) != CF_OK) { snprintf(t->message, sizeof(t->message), "TLS write failed (code %ld).", (long)n); return false; }
        t->write_stalled_ms = 0;
    }
    if (!t->life.registered && now_ms() >= t->deadline_ms) { message(t, "Edge registration timed out."); return false; }
    if (t->life.state == CF_WAIT_CONFIG && now_ms() >= t->deadline_ms) { message(t, "No ingress configuration received. Check the tunnel public hostname."); return false; }
    /* Poll sockets, not tasks per stream. Pending output makes write readiness
     * relevant; otherwise wait for input to avoid spinning on a writable TCP fd. */
    int fd;
    if (esp_tls_get_conn_sockfd(t->tls, &fd) == ESP_OK) {
        cf_bytes out;
        (void)cf_h2_output(t->h2, &out);
        fd_set reads, writes; FD_ZERO(&reads); FD_ZERO(&writes); FD_SET(fd, &reads);
        if ((out.len && !write_needs_read) || write_wait) FD_SET(fd, &writes);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        (void)select(fd + 1, &reads, &writes, NULL, &timeout);
    }
    return true;
}
static void run(void *context)
{
    esp_cf_tunnel *t = context;
    (void)cf_lifecycle_start(&t->life);
    for (;;) {
        bool reload, stop;
        portENTER_CRITICAL(&t->lock);
        reload = t->reload_requested; t->reload_requested = false; stop = t->stop_requested;
        portEXIT_CRITICAL(&t->lock);
        if (reload && !stop) {
            close_connection(t); cf_lifecycle_stop(&t->life); (void)cf_lifecycle_start(&t->life);
            t->settings_ready = false; t->stopping = false;
        }
        if (stop && !t->stopping) {
            if (t->h2 && t->rpc.state == CF_RPC_REGISTERED) {
                uint8_t out[CF_RPC_TX_MAX]; size_t n;
                if (cf_rpc_unregister(&t->rpc, out, sizeof(out), &n) == CF_OK) (void)append_rpc(t, out, n);
                cf_secure_zero(out, sizeof(out)); t->stopping = true; t->shutdown_ms = now_ms() + 2000;
                t->life.state = CF_STOPPING;
            } else break;
        }
        t->callbacks.environment(t->callbacks.context, &t->environment);
        if (!t->environment.network_ready || !t->environment.time_valid) {
            if (t->stopping) break;
            if (t->tls || t->dns_socket >= 0) close_connection(t);
        }
        cf_lifecycle_environment(&t->life, t->environment.network_ready, t->environment.time_valid);
        cf_lifecycle_tick(&t->life, now_ms());
        if (t->life.state == CF_DISCOVERY) {
            if (!discover(t)) retry(t, t->message);
        } else if (t->life.state == CF_CONNECT || t->life.state == CF_TLS) {
            if (!connect_tls(t)) retry(t, t->message);
        } else if (t->h2) {
            if (!communicate(t)) { if (t->stopping) break; retry(t, t->message); }
        }
        if (t->stopping && (now_ms() >= t->shutdown_ms || (t->goaway_sent && !t->control_stream))) break;
        publish(t);
        if (!t->h2) vTaskDelay(pdMS_TO_TICKS(t->attempt_started ? 10 : 250));
    }
    close_connection(t); cf_lifecycle_stop(&t->life); message(t, "Tunnel stopped."); publish(t);
    portENTER_CRITICAL(&t->lock); t->task = NULL; portEXIT_CRITICAL(&t->lock);
    vTaskDelete(NULL);
}
esp_err_t esp_cf_tunnel_init(const esp_cf_tunnel_config *config, esp_cf_tunnel **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!config || !config->environment || !config->credentials || !config->json_try_lock || !config->json_unlock ||
        !config->request || !config->data || !config->read_response || !config->closed || !config->version || !config->arch || !config->service ||
        !*config->version || strlen(config->version) > 63 || !*config->arch || strlen(config->arch) > 63 ||
        !*config->service || strlen(config->service) > CF_LOCAL_SERVICE_MAX ||
        (config->application_streams != 2 && config->application_streams != 4)) return ESP_ERR_INVALID_ARG;
    esp_cf_tunnel *t = calloc(1, sizeof(*t));
    if (!t) return ESP_ERR_NO_MEM;
    t->callbacks = *config; t->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED; t->dns_socket = -1;
    cf_lifecycle_init(&t->life); cf_config_init(&t->remote);
    esp_fill_random(t->options.connector_id, sizeof(t->options.connector_id));
    t->options.connector_id[6] = (uint8_t)((t->options.connector_id[6] & 15) | 64);
    t->options.connector_id[8] = (uint8_t)((t->options.connector_id[8] & 63) | 128);
    t->options.version = (cf_bytes){(const uint8_t *)config->version, strlen(config->version)};
    t->options.arch = (cf_bytes){(const uint8_t *)config->arch, strlen(config->arch)};
    t->options.features = CF_RPC_REMOTE_CONFIG | CF_RPC_SERIALIZED_HEADERS;
    t->snapshot.config_version = -1; *out = t; return ESP_OK;
}
esp_err_t esp_cf_tunnel_start(esp_cf_tunnel *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    if (t->task) return ESP_ERR_INVALID_STATE;
    t->stop_requested = t->reload_requested = t->stopping = false;
    return xTaskCreate(run, "cf_tunnel", TASK_STACK, t, 4, &t->task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
void esp_cf_tunnel_reload(esp_cf_tunnel *t)
{
    if (!t) return;
    portENTER_CRITICAL(&t->lock); t->reload_requested = true; portEXIT_CRITICAL(&t->lock);
}
void esp_cf_tunnel_stop(esp_cf_tunnel *t)
{
    if (!t) return;
    portENTER_CRITICAL(&t->lock); t->stop_requested = true; portEXIT_CRITICAL(&t->lock);
}
esp_err_t esp_cf_tunnel_deinit(esp_cf_tunnel *t)
{
    if (!t) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&t->lock); bool running = t->task != NULL; portEXIT_CRITICAL(&t->lock);
    if (running) return ESP_ERR_INVALID_STATE;
    close_connection(t); cf_secure_zero(t, sizeof(*t)); free(t); return ESP_OK;
}
void esp_cf_tunnel_get_snapshot(esp_cf_tunnel *t, esp_cf_tunnel_snapshot *out)
{
    if (!t || !out) return;
    portENTER_CRITICAL(&t->lock); *out = t->snapshot; portEXIT_CRITICAL(&t->lock);
}
