#include "cf_lifecycle.h"
#include <string.h>

static cf_state waiting(const cf_lifecycle *l)
{
    return !l->network_ready ? CF_WAIT_NETWORK : !l->time_ready ? CF_WAIT_TIME
                                                                : CF_DISCOVERY;
}

static bool terminal(cf_state state)
{
    return state == CF_STOPPED || state == CF_STOPPING || state == CF_AUTH_FAILED;
}

void cf_lifecycle_init(cf_lifecycle *l)
{
    if (!l)
        return;
    memset(l, 0, sizeof(*l));
    l->state = CF_STOPPED;
}

cf_result cf_lifecycle_start(cf_lifecycle *l)
{
    if (!l)
        return CF_ERR_ARGUMENT;
    if (l->state != CF_STOPPED)
        return CF_ERR_STATE;
    l->registered = l->configured = false;
    l->failures = 0;
    l->retry_at_ms = 0;
    l->state = waiting(l);
    return CF_OK;
}

void cf_lifecycle_environment(cf_lifecycle *l, bool network, bool time_valid)
{
    if (!l)
        return;
    l->network_ready = network;
    l->time_ready = time_valid;
    if (terminal(l->state))
        return;
    if (!network || !time_valid)
    {
        l->registered = l->configured = false;
        /* Keep an outstanding server retry deadline through network flaps. */
        if (l->state != CF_BACKOFF)
            l->state = waiting(l);
    }
    else if (l->state == CF_WAIT_NETWORK || l->state == CF_WAIT_TIME)
    {
        l->state = CF_DISCOVERY;
    }
}

cf_result cf_lifecycle_advance(cf_lifecycle *l, cf_lifecycle_event event)
{
    if (!l)
        return CF_ERR_ARGUMENT;
    if (terminal(l->state) || !l->network_ready || !l->time_ready)
        return CF_ERR_STATE;
    switch (event)
    {
    case CF_EVENT_DISCOVERED:
        if (l->state != CF_DISCOVERY)
            return CF_ERR_STATE;
        l->state = CF_CONNECT;
        break;
    case CF_EVENT_CONNECTED:
        if (l->state != CF_CONNECT)
            return CF_ERR_STATE;
        l->state = CF_TLS;
        break;
    case CF_EVENT_TLS_READY:
        if (l->state != CF_TLS)
            return CF_ERR_STATE;
        l->state = CF_HTTP2;
        break;
    case CF_EVENT_HTTP2_READY:
        if (l->state != CF_HTTP2)
            return CF_ERR_STATE;
        l->state = CF_REGISTERING;
        break;
    case CF_EVENT_REGISTERED:
        if (l->state != CF_REGISTERING && l->state != CF_CONFIG_FAILED)
            return CF_ERR_STATE;
        l->registered = true;
        l->state = l->configured ? CF_ONLINE : l->state == CF_CONFIG_FAILED ? CF_CONFIG_FAILED
                                                                            : CF_WAIT_CONFIG;
        break;
    case CF_EVENT_CONFIG_APPLIED:
        if (l->state != CF_REGISTERING && l->state != CF_WAIT_CONFIG && l->state != CF_ONLINE && l->state != CF_CONFIG_FAILED)
            return CF_ERR_STATE;
        l->configured = true;
        l->state = l->registered ? CF_ONLINE : CF_REGISTERING;
        break;
    case CF_EVENT_AUTH_REJECTED:
        if (l->state != CF_REGISTERING && l->state != CF_WAIT_CONFIG && l->state != CF_ONLINE && l->state != CF_CONFIG_FAILED)
            return CF_ERR_STATE;
        l->registered = l->configured = false;
        l->state = CF_AUTH_FAILED;
        break;
    case CF_EVENT_CONFIG_REJECTED:
        if (l->state != CF_REGISTERING && l->state != CF_WAIT_CONFIG && l->state != CF_ONLINE && l->state != CF_CONFIG_FAILED)
            return CF_ERR_STATE;
        /* A failed update does not revoke a previously valid config. */
        if (!l->configured)
            l->state = CF_CONFIG_FAILED;
        break;
    default:
        return CF_ERR_ARGUMENT;
    }
    return CF_OK;
}

cf_result cf_lifecycle_retry(cf_lifecycle *l, uint64_t now, uint32_t hint, uint32_t random)
{
    uint32_t ceiling, delay, shift;
    if (!l)
        return CF_ERR_ARGUMENT;
    if (terminal(l->state) || l->state == CF_BACKOFF ||
        l->state == CF_WAIT_NETWORK || l->state == CF_WAIT_TIME)
        return CF_ERR_STATE;
    shift = l->failures < 6 ? l->failures : 6;
    ceiling = 1000u << shift;
    if (ceiling > 60000)
        ceiling = 60000;
    /* Equal jitter: never an immediate reconnect storm. No modulo overflow. */
    delay = ceiling / 2 + random % (ceiling / 2 + 1);
    if (delay < hint)
        delay = hint;
    l->retry_at_ms = now > UINT64_MAX - delay ? UINT64_MAX : now + delay;
    if (l->failures < UINT32_MAX)
        ++l->failures;
    l->registered = l->configured = false;
    l->state = CF_BACKOFF;
    return CF_OK;
}

void cf_lifecycle_tick(cf_lifecycle *l, uint64_t now)
{
    if (l && l->state == CF_BACKOFF && now >= l->retry_at_ms)
        l->state = waiting(l);
}

void cf_lifecycle_stop(cf_lifecycle *l)
{
    if (!l)
        return;
    l->registered = l->configured = false;
    l->state = CF_STOPPED;
    l->retry_at_ms = 0;
}
