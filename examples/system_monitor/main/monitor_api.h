#ifndef MONITOR_API_H
#define MONITOR_API_H
#include "cf_core.h"
#define MONITOR_BODY_MAX 2048u
#define MONITOR_REPLY_MAX 6144u
#define MONITOR_EFFECT_WIFI 1u
#define MONITOR_EFFECT_TUNNEL 2u
#define MONITOR_EFFECT_REBOOT 4u
#define MONITOR_EFFECT_TUNNEL_RESTART 8u
typedef struct {
    const char *path, *authorization;
    cf_bytes body;
    bool post, json_content, origin_allowed, setup_access, remote_access;
} monitor_api_request;
typedef struct {
    unsigned status, effects;
    size_t length;
    char body[MONITOR_REPLY_MAX];
} monitor_api_reply;
/* Caller owns monitor_json_lock. No socket operations; reply never borrows JSON. */
void monitor_api_dispatch(const monitor_api_request *request, monitor_api_reply *reply);
/* Called under the same lock after queuing the reply. Network changes run later
 * on the main task, allowing the success response to leave the connection. */
void monitor_api_schedule_effects(unsigned effects);
void monitor_api_tick(void);
#endif
