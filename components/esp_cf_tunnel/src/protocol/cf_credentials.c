#include "cf_credentials.h"
#include "cf_json.h"
#include <string.h>

static int hex(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool uuid(const char *s, uint8_t out[16])
{
    size_t i = 0, n = 0;
    unsigned nonzero = 0;
    if (strlen(s) != 36) return false;
    while (i < 36) {
        int a, b;
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i++] != '-') return false;
            continue;
        }
        a = hex((unsigned char)s[i++]);
        b = hex((unsigned char)s[i++]);
        if (a < 0 || b < 0 || n >= 16) return false;
        out[n] = (uint8_t)((unsigned)a * 16u + (unsigned)b);
        nonzero |= out[n++];
    }
    return n == 16 && nonzero;
}

cf_result cf_credentials_parse_ex(cf_bytes token, uint8_t *scratch, size_t cap,
                                   cf_credentials *out, cf_token_diagnostic *diagnostic)
{
    static const char *const keys[] = {"a", "s", "t", "e"};
    cf_cJSON *root = NULL, *a, *s, *t, *e;
    cf_credentials candidate = {0};
    cf_result rc = CF_ERR_FORMAT;
    cf_token_diagnostic why = CF_TOKEN_FIELDS;
    size_t n = 0, i;
    if (diagnostic) *diagnostic = CF_TOKEN_FIELDS;
    if (!out) return CF_ERR_ARGUMENT;
    cf_secure_zero(out, sizeof(*out));
    if (!scratch || cap < CF_TOKEN_JSON_MAX + 1 || (!token.ptr && token.len)) return CF_ERR_ARGUMENT;
    why = CF_TOKEN_INPUT_SIZE;
    if (!token.len || token.len > CF_TOKEN_MAX) { rc = CF_ERR_LIMIT; goto done; }
    why = CF_TOKEN_BASE64;
    rc = cf_base64_decode(token, true, scratch, CF_TOKEN_JSON_MAX, &n);
    if (rc != CF_OK) goto done;
    scratch[n] = 0;
    rc = CF_ERR_FORMAT;
    why = CF_TOKEN_JSON;
    root = cf_json_parse((cf_bytes){scratch, n});
    if (!root) { rc = cf_json_failure(); goto done; }
    why = CF_TOKEN_FIELDS;
    if (!cf_json_keys(root, keys, 4)) goto done;
    a = cf_cJSON_GetObjectItemCaseSensitive(root, "a");
    s = cf_cJSON_GetObjectItemCaseSensitive(root, "s");
    t = cf_cJSON_GetObjectItemCaseSensitive(root, "t");
    e = cf_cJSON_GetObjectItemCaseSensitive(root, "e");
    if (!cf_cJSON_IsString(a) || !cf_cJSON_IsString(s) || !cf_cJSON_IsString(t)) goto done;
    if (e && !cf_cJSON_IsString(e)) goto done;
    if (e && e->valuestring[0]) { why = CF_TOKEN_ENDPOINT; rc = CF_ERR_UNSUPPORTED; goto done; }
    why = CF_TOKEN_UUID;
    if (!uuid(t->valuestring, candidate.tunnel_id)) goto done;
    why = CF_TOKEN_ACCOUNT;
    if (strlen(a->valuestring) != 32) goto done;
    for (i = 0; i < 32; ++i) if (hex((unsigned char)a->valuestring[i]) < 0) goto done;
    memcpy(candidate.account_tag, a->valuestring, 32);
    why = CF_TOKEN_SECRET_BASE64;
    rc = cf_base64_decode((cf_bytes){(const uint8_t *)s->valuestring, strlen(s->valuestring)},
                          true, candidate.tunnel_secret, sizeof(candidate.tunnel_secret), &n);
    if (rc != CF_OK) { if (rc == CF_ERR_LIMIT) why = CF_TOKEN_SECRET_SIZE; goto done; }
    why = CF_TOKEN_SECRET_SIZE;
    if (n < CF_TUNNEL_SECRET_MIN) { rc = CF_ERR_FORMAT; goto done; }
    candidate.tunnel_secret_len = n;
    *out = candidate;
done:
    cf_json_delete_secret(root);
    cf_secure_zero(&candidate, sizeof(candidate));
    cf_secure_zero(scratch, cap);
    if (diagnostic) *diagnostic = rc == CF_OK ? CF_TOKEN_VALID : rc == CF_ERR_MEMORY ? CF_TOKEN_MEMORY : why;
    return rc;
}

cf_result cf_credentials_parse(cf_bytes token, uint8_t *scratch, size_t cap, cf_credentials *out)
{
    return cf_credentials_parse_ex(token, scratch, cap, out, NULL);
}

const char *cf_token_diagnostic_message(cf_token_diagnostic why)
{
    switch (why) {
    case CF_TOKEN_VALID: return "Tunnel token accepted.";
    case CF_TOKEN_INPUT_SIZE: return "Tunnel token must contain 1 to 1024 characters.";
    case CF_TOKEN_BASE64: return "Invalid token encoding. Paste only the tunnel token from the connector install command, without the command, quotes, or an API key.";
    case CF_TOKEN_JSON: return "This value is not a named tunnel token: its decoded content is not valid JSON.";
    case CF_TOKEN_FIELDS: return "The token must contain account (a), tunnel ID (t), and secret (s), with no duplicate or unsupported fields.";
    case CF_TOKEN_ACCOUNT: return "The token account ID must be 32 hexadecimal characters.";
    case CF_TOKEN_UUID: return "The token tunnel ID must be a nonzero UUID in the standard format.";
    case CF_TOKEN_SECRET_BASE64: return "The token contains an invalid Base64 tunnel secret.";
    case CF_TOKEN_SECRET_SIZE: return "This firmware supports tunnel secrets of 32 to 128 decoded bytes.";
    case CF_TOKEN_ENDPOINT: return "Custom tunnel endpoints are not supported by this firmware.";
    case CF_TOKEN_MEMORY: return "Not enough memory to validate the tunnel token. Please retry.";
    default: return "Unable to validate the tunnel token.";
    }
}
