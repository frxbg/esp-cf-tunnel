#include "cf_config.h"
#include "cf_json.h"
#include <stdio.h>
#include <string.h>

void cf_config_init(cf_remote_config *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->version = -1;
}

static bool empty_object(const cf_cJSON *item)
{
    return !item || (cf_cJSON_IsObject(item) && !item->child);
}

static bool same_string(const cf_cJSON *s, const char *expected)
{
    return cf_cJSON_IsString(s) && strcmp(s->valuestring, expected) == 0;
}

static bool hostname_valid(const char *s)
{
    size_t i, label = 0, n = strlen(s);
    if (!n || n > CF_HOSTNAME_MAX || s[0] == '-' || s[n - 1] == '-')
        return false;
    for (i = 0; i < n; ++i)
    {
        unsigned char c = (unsigned char)s[i];
        if (c == '.')
        {
            if (!label || s[i - 1] == '-' || s[i + 1] == '-')
                return false;
            label = 0;
        }
        else
        {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
                return false;
            if (++label > 63)
                return false;
        }
    }
    return label != 0;
}

cf_result cf_config_apply_ex(cf_remote_config *state, cf_bytes update,
                             const char *hostname, const char *service, const char **reason)
{
    static const char *const top[] = {"version", "config"};
    static const char *const config_keys[] = {"ingress", "originRequest", "warp-routing"};
    static const char *const ingress_keys[] = {"hostname", "service", "originRequest"};
    static const char *const fallback_keys[] = {"service"};
    static const char *const warp_keys[] = {"enabled"};
    cf_cJSON *root, *version, *config, *rules, *first, *last, *warp;
    cf_result rc = CF_ERR_UNSUPPORTED;
#define REASON(text)          \
    do                        \
    {                         \
        if (reason)           \
            *reason = (text); \
    } while (0)
    REASON("Invalid local ingress policy.");
    if (!state || !hostname || !service || !hostname_valid(hostname) ||
        !*service || strlen(service) > CF_LOCAL_SERVICE_MAX || (!update.ptr && update.len))
        return CF_ERR_ARGUMENT;
    REASON("Ingress update exceeds the message limit.");
    if (!update.len || update.len > CF_CONFIG_MESSAGE_MAX)
        return CF_ERR_LIMIT;
    REASON("Invalid ingress JSON or JSON memory limit reached.");
    root = cf_json_parse(update);
    if (!root)
        return cf_json_failure();
    REASON("Unknown or duplicate fields in the configuration update envelope.");
    if (!cf_json_keys(root, top, 2))
        goto done;
    version = cf_cJSON_GetObjectItemCaseSensitive(root, "version");
    config = cf_cJSON_GetObjectItemCaseSensitive(root, "config");
    REASON("Invalid configuration version.");
    if (!cf_cJSON_IsNumber(version) || version->valuedouble < 0 ||
        version->valuedouble > INT32_MAX ||
        (double)(int32_t)version->valuedouble != version->valuedouble)
    {
        rc = CF_ERR_FORMAT;
        goto done;
    }
    REASON("Unknown or duplicate fields in the tunnel configuration.");
    if (!cf_json_keys(config, config_keys, 3))
        goto done;
    REASON("Global originRequest options are unsupported.");
    if (!empty_object(cf_cJSON_GetObjectItemCaseSensitive(config, "originRequest")))
        goto done;
    REASON("WARP routing options are unsupported; only enabled=false is accepted.");
    warp = cf_cJSON_GetObjectItemCaseSensitive(config, "warp-routing");
    if (warp && (!cf_json_keys(warp, warp_keys, 1) ||
                 !cf_cJSON_IsFalse(cf_cJSON_GetObjectItemCaseSensitive(warp, "enabled"))))
        goto done;
    REASON("Expected one hostname rule followed by one 404 fallback rule.");
    rules = cf_cJSON_GetObjectItemCaseSensitive(config, "ingress");
    if (!cf_cJSON_IsArray(rules) || cf_cJSON_GetArraySize(rules) != 2)
        goto done;
    first = cf_cJSON_GetArrayItem(rules, 0);
    last = cf_cJSON_GetArrayItem(rules, 1);
    REASON("Unknown or duplicate fields in the hostname ingress rule.");
    if (!cf_json_keys(first, ingress_keys, 3))
        goto done;
    REASON("Unknown or duplicate fields in the fallback ingress rule.");
    if (!cf_json_keys(last, fallback_keys, 1))
        goto done;
    REASON("Ingress hostname differs from the hostname saved on this device.");
    if (!same_string(cf_cJSON_GetObjectItemCaseSensitive(first, "hostname"), hostname))
        goto done;
    REASON("Ingress service differs from the permitted local service (http://localhost:80 for ESP Monitor).");
    if (!same_string(cf_cJSON_GetObjectItemCaseSensitive(first, "service"), service))
        goto done;
    REASON("The final ingress service must be http_status:404.");
    if (!same_string(cf_cJSON_GetObjectItemCaseSensitive(last, "service"), "http_status:404"))
        goto done;
    REASON("Hostname originRequest options are unsupported.");
    if (!empty_object(cf_cJSON_GetObjectItemCaseSensitive(first, "originRequest")))
        goto done;
    /* No fallible operation follows this point. Publish only a fully validated candidate. */
    if (!state->valid || (int32_t)version->valuedouble > state->version)
    {
        memcpy(state->hostname, hostname, strlen(hostname) + 1);
        memcpy(state->service, service, strlen(service) + 1);
        state->version = (int32_t)version->valuedouble;
        state->valid = true;
    }
    rc = CF_OK;
    REASON("Remote ingress configuration applied.");
done:
    cf_cJSON_Delete(root);
    return rc;
#undef REASON
}

cf_result cf_config_apply(cf_remote_config *state, cf_bytes update,
                          const char *hostname, const char *service)
{
    return cf_config_apply_ex(state, update, hostname, service, NULL);
}

cf_result cf_config_response(const cf_remote_config *config, cf_result result,
                             char *out, size_t cap, size_t *written)
{
    int n;
    if (!written)
        return CF_ERR_ARGUMENT;
    *written = 0;
    if (!config || !out || !cap)
        return CF_ERR_ARGUMENT;
    n = snprintf(out, cap, "{\"lastAppliedVersion\":%ld%s}",
                 (long)(config->valid ? config->version : -1),
                 result == CF_OK ? ",\"err\":null" : ",\"err\":{}");
    if (n < 0 || (size_t)n >= cap)
    {
        out[0] = 0;
        return CF_ERR_LIMIT;
    }
    *written = (size_t)n;
    return CF_OK;
}
