#ifndef CF_HEADERS_H
#define CF_HEADERS_H
#include "cf_core.h"
#define CF_HEADER_COUNT_MAX 32u
#define CF_HEADER_DECODED_MAX 4096u
#define CF_HEADER_WIRE_MAX 6144u
#define CF_REQUEST_HEADERS "cf-cloudflared-request-headers"
#define CF_RESPONSE_HEADERS "cf-cloudflared-response-headers"
#define CF_RESPONSE_META "cf-cloudflared-response-meta"
#define CF_RESPONSE_META_ORIGIN "{\"src\":\"origin\"}"
#define CF_RESPONSE_META_CONNECTOR "{\"src\":\"cloudflared\"}"
#define CF_RESPONSE_META_OVERLOAD "{\"src\":\"cloudflared\",\"flow_rate_limited\":true}"

typedef struct { cf_bytes name, value; } cf_header;
/* Entries borrow caller-owned arena; repeated names and empty values survive.
 * On failure count=0, arena/entries are unspecified and must not be consumed.
 * Input, arena and entries must not overlap. Empty semicolon pairs are ignored
 * like cloudflared; invalid HTTP field names/CRLF/NUL are rejected. */
cf_result cf_headers_decode(cf_bytes wire, cf_header *entries, size_t slots,
                            uint8_t *arena, size_t arena_size, size_t *count);
cf_result cf_headers_encode(const cf_header *entries, size_t count,
                            char *wire, size_t capacity, size_t *written);
bool cf_header_is_control(cf_bytes name);
#endif
