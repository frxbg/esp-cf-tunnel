#ifndef CF_CORE_H
#define CF_CORE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    CF_OK = 0, CF_ERR_ARGUMENT = -1, CF_ERR_FORMAT = -2,
    CF_ERR_LIMIT = -3, CF_ERR_UNSUPPORTED = -4, CF_ERR_STATE = -5,
    CF_ERR_MEMORY = -6, CF_ERR_TRUNCATED = -7
} cf_result;

/* Non-owning bytes. No implicit NUL terminator. */
typedef struct { const uint8_t *ptr; size_t len; } cf_bytes;
void cf_secure_zero(void *ptr, size_t len);

/* Strict standard alphabet. padded=false is RawStdEncoding, not base64url.
 * Output length is zero on failure; output bytes are unspecified on failure.
 * Input/output must not overlap. No allocation, no NUL termination. */
cf_result cf_base64_decode(cf_bytes input, bool padded, uint8_t *out,
                           size_t capacity, size_t *written);
cf_result cf_base64_encode(cf_bytes input, bool padded, char *out,
                           size_t capacity, size_t *written);
#endif
