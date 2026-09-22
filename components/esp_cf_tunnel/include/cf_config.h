#ifndef CF_CONFIG_H
#define CF_CONFIG_H
#include "cf_core.h"
#define CF_CONFIG_MESSAGE_MAX 8192u
#define CF_HOSTNAME_MAX 253u
#define CF_LOCAL_SERVICE_MAX 63u

typedef struct {
    bool valid;
    int32_t version;
    char hostname[CF_HOSTNAME_MAX + 1];
    char service[CF_LOCAL_SERVICE_MAX + 1];
} cf_remote_config;

/* Narrow subset: exactly one configured hostname -> exact allowed service,
 * followed by http_status:404. No path regex, JWT, WARP, arbitrary targets,
 * nonempty originRequest, or unknown fields. JSON calls require one owner.
 * Application supplies trusted policy (hostname + explicitly allowed local
 * service); neither string may be sourced from the remote update itself.
 * Atomic: every error preserves the previous config. Older/equal valid
 * versions return CF_OK without replacing it. Initialize via cf_config_init.
 * Stored service is a label; this module never opens a socket. */
void cf_config_init(cf_remote_config *config);
cf_result cf_config_apply(cf_remote_config *config, cf_bytes update,
                          const char *allowed_hostname, const char *allowed_service);
/* Optional reason is a fixed English string, never bytes from the update. */
cf_result cf_config_apply_ex(cf_remote_config *config, cf_bytes update,
                            const char *allowed_hostname, const char *allowed_service,
                            const char **reason);
cf_result cf_config_response(const cf_remote_config *config, cf_result result,
                             char *out, size_t capacity, size_t *written);
#endif
