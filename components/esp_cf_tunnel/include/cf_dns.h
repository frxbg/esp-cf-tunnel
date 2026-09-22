#ifndef CF_DNS_H
#define CF_DNS_H
#include "cf_core.h"
#define CF_EDGE_DISCOVERY "_v2-origintunneld._tcp.argotunnel.com"
#define CF_EDGE_TLS_NAME "h2.cftunnel.com"
#define CF_EDGE_PORT 7844
#define CF_DNS_PACKET_MAX 1232u
#define CF_DNS_RECORD_MAX 16u
#define CF_DNS_NAME_MAX 253u
typedef enum { CF_DNS_A = 1, CF_DNS_SRV = 33 } cf_dns_type;
typedef struct {
    char target[CF_DNS_NAME_MAX + 1];
    uint8_t ip[4];
    uint16_t priority, weight, port;
    uint32_t ttl;
} cf_dns_record;
typedef struct { cf_dns_record records[CF_DNS_RECORD_MAX]; size_t count; } cf_dns_answer;
/* Small stub codec. Caller uses its configured recursive resolver and verifies
 * UDP source via a connected socket. Exact question/id/class matching; bounded
 * compression traversal. Truncated responses are rejected, never used. No
 * DNSSEC, cache, CNAME chasing, TCP/DoT fallback, or IPv6 in this first port.
 * TLS certificate and hostname validation remain mandatory after discovery. */
cf_result cf_dns_query(const char *name, cf_dns_type type, uint16_t id,
                       uint8_t *out, size_t capacity, size_t *written);
cf_result cf_dns_parse(cf_bytes packet, const char *name, cf_dns_type type,
                       uint16_t id, cf_dns_answer *out);
bool cf_dns_edge_target(const char *name);
#endif
