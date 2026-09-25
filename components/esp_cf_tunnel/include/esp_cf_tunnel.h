#ifndef ESP_CF_TUNNEL_H
#define ESP_CF_TUNNEL_H
#include "cf_h2.h"
#include "cf_lifecycle.h"
#include "cf_credentials.h"
#include "esp_err.h"

typedef struct esp_cf_tunnel esp_cf_tunnel;
typedef enum {
    ESP_CF_CONNECT_UNKNOWN, ESP_CF_CONNECT_TCP, ESP_CF_CONNECT_TLS_SETUP,
    ESP_CF_CONNECT_TLS_HANDSHAKE, ESP_CF_CONNECT_DEADLINE, ESP_CF_CONNECT_VERIFIED
} esp_cf_connect_stage;
typedef struct {
    bool valid, errors_available;
    esp_cf_connect_stage stage;
    int rc, state_before, state_after;
    int esp_error, tls_error, verify_flags, system_error;
    int errno_context; /* Supplemental only: errno can be stale. */
    int tls_alert, tls_version; /* Public mbedTLS getters; -1 if unavailable. */
    uint64_t elapsed_ms, at_ms;
    int64_t utc_s; /* Observed system clock, not proof of clock accuracy. */
    uint32_t attempt, heap_free, heap_largest;
    char edge_ip[16];
    uint16_t edge_port;
} esp_cf_connect_diagnostic;
typedef struct {
    bool network_ready, time_valid;
    uint8_t local_ip[4], dns_ip[4];
} esp_cf_environment;
typedef struct {
    void *context;
    void (*environment)(void *, esp_cf_environment *);
    /* Called with the application's JSON lock held. Never print the token.
     * Fill credentials and the exact public hostname, or return CF_ERR_STATE
     * when not configured. The library immediately owns/wipes these copies. */
    cf_result (*credentials)(void *, cf_credentials *, char hostname[254]);
    bool (*json_try_lock)(void *);
    void (*json_unlock)(void *);
    cf_result (*request)(void *, cf_h2 *, const cf_h2_request *);
    cf_result (*data)(void *, int32_t, cf_bytes, bool);
    cf_result (*read_response)(void *, int32_t, uint8_t *, size_t, size_t *, bool *);
    void (*closed)(void *, int32_t, uint32_t);
    const char *version, *arch;
    const char *service; /* Exact permitted remote config label, e.g. http://localhost:80. */
    unsigned application_streams; /* 2 or 4. */
} esp_cf_tunnel_config;
typedef struct {
    cf_state state;
    bool settings_ready, registered, configured;
    int32_t config_version;
    uint32_t attempts;
    uint64_t retry_at_ms;
    char edge_ip[16], location[32], message[128];
    char last_failure[128]; /* Fixed local diagnostic, retained across reconnects. */
    uint64_t last_failure_ms;
    uint32_t failures;
    cf_h2_stats http2;
    uint32_t task_stack_min;
    esp_cf_connect_diagnostic last_connect, last_connect_failure;
} esp_cf_tunnel_snapshot;

/* Application owns networking, clock, credential persistence and callbacks.
 * One additional task owns DNS/TLS/HTTP2/RPC; no task per request. Config
 * strings/callback context must outlive deinit. Application callbacks must be
 * nonblocking. This first port is IPv4, one edge connection, native backend. */
esp_err_t esp_cf_tunnel_init(const esp_cf_tunnel_config *config, esp_cf_tunnel **out);
esp_err_t esp_cf_tunnel_start(esp_cf_tunnel *tunnel);
void esp_cf_tunnel_reload(esp_cf_tunnel *tunnel);
void esp_cf_tunnel_stop(esp_cf_tunnel *tunnel);
esp_err_t esp_cf_tunnel_deinit(esp_cf_tunnel *tunnel); /* INVALID_STATE until stopped. */
void esp_cf_tunnel_get_snapshot(esp_cf_tunnel *tunnel, esp_cf_tunnel_snapshot *out);
const char *esp_cf_tunnel_state_name(cf_state state);
const char *esp_cf_connect_stage_name(esp_cf_connect_stage stage);
#endif
