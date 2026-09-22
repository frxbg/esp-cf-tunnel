#ifndef CF_LIFECYCLE_H
#define CF_LIFECYCLE_H
#include "cf_core.h"

typedef enum {
    CF_STOPPED, CF_WAIT_NETWORK, CF_WAIT_TIME, CF_DISCOVERY, CF_CONNECT,
    CF_TLS, CF_HTTP2, CF_REGISTERING, CF_WAIT_CONFIG, CF_ONLINE,
    CF_BACKOFF, CF_AUTH_FAILED, CF_CONFIG_FAILED, CF_STOPPING
} cf_state;
typedef enum {
    CF_EVENT_DISCOVERED, CF_EVENT_CONNECTED, CF_EVENT_TLS_READY,
    CF_EVENT_HTTP2_READY, CF_EVENT_REGISTERED, CF_EVENT_CONFIG_APPLIED,
    CF_EVENT_AUTH_REJECTED, CF_EVENT_CONFIG_REJECTED
} cf_lifecycle_event;
typedef struct {
    cf_state state;
    bool network_ready, time_ready, registered, configured;
    uint32_t failures;
    uint64_t retry_at_ms;
} cf_lifecycle;

/* Pure reducer, no task/socket/timer and no credential access. One owner.
 * now_ms is monotonic; random is supplied by the platform CSPRNG.
 * Readiness never infers registration/config success from a TLS handshake. */
void cf_lifecycle_init(cf_lifecycle *life);
cf_result cf_lifecycle_start(cf_lifecycle *life);
void cf_lifecycle_environment(cf_lifecycle *life, bool network, bool time_valid);
cf_result cf_lifecycle_advance(cf_lifecycle *life, cf_lifecycle_event event);
cf_result cf_lifecycle_retry(cf_lifecycle *life, uint64_t now_ms,
                              uint32_t server_delay_ms, uint32_t random);
void cf_lifecycle_tick(cf_lifecycle *life, uint64_t now_ms);
void cf_lifecycle_stop(cf_lifecycle *life);
#endif
