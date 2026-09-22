#ifndef CF_CREDENTIALS_H
#define CF_CREDENTIALS_H
#include "cf_core.h"
#define CF_TOKEN_MAX 1024u
#define CF_TOKEN_JSON_MAX 768u
#define CF_TUNNEL_SECRET_MIN 32u
#define CF_TUNNEL_SECRET_MAX 128u
typedef struct {
    char account_tag[33];
    uint8_t tunnel_id[16];
    uint8_t tunnel_secret[CF_TUNNEL_SECRET_MAX];
    size_t tunnel_secret_len;
} cf_credentials;

typedef enum {
    CF_TOKEN_VALID, CF_TOKEN_INPUT_SIZE, CF_TOKEN_BASE64, CF_TOKEN_JSON,
    CF_TOKEN_FIELDS, CF_TOKEN_ACCOUNT, CF_TOKEN_UUID, CF_TOKEN_SECRET_BASE64,
    CF_TOKEN_SECRET_SIZE, CF_TOKEN_ENDPOINT, CF_TOKEN_MEMORY
} cf_token_diagnostic;

/* Named tunnel subset: 32 hex account tag, canonical UUID, 32..128-byte secret.
 * Nonempty custom endpoint e is rejected, never ignored.
 * Caller supplies scratch >= CF_TOKEN_JSON_MAX+1. Scratch is erased on every
 * return after argument validation. Output is erased on every failure.
 * Token/scratch/output must not overlap. Token remains owned by provider.
 * JSON parser calls (credentials/config) must be serialized by one owner. */
cf_result cf_credentials_parse(cf_bytes token, uint8_t *scratch, size_t capacity,
                                cf_credentials *out);
cf_result cf_credentials_parse_ex(cf_bytes token, uint8_t *scratch, size_t capacity,
                                   cf_credentials *out, cf_token_diagnostic *diagnostic);
/* Fixed English text only: never includes input or decoded credential fields. */
const char *cf_token_diagnostic_message(cf_token_diagnostic diagnostic);
#endif
