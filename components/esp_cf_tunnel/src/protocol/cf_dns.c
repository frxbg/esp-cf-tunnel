#include "cf_dns.h"
#include <string.h>

static uint16_t u16(const uint8_t *p) { return (uint16_t)((uint16_t)p[0] << 8 | p[1]); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)u16(p) << 16 | u16(p + 2); }
static void put16(uint8_t *p, uint16_t n)
{
    p[0] = (uint8_t)(n >> 8);
    p[1] = (uint8_t)n;
}
static uint8_t lower(uint8_t c) { return c >= 'A' && c <= 'Z' ? (uint8_t)(c + 32) : c; }
static bool name_char(uint8_t c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'; }
bool cf_dns_edge_target(const char *name)
{
    static const char suffix[] = ".argotunnel.com";
    size_t n;
    if (!name)
        return false;
    n = strlen(name);
    return n > sizeof(suffix) - 1 && n <= CF_DNS_NAME_MAX && !strcmp(name + n - (sizeof(suffix) - 1), suffix);
}
cf_result cf_dns_query(const char *name, cf_dns_type type, uint16_t id, uint8_t *out, size_t cap, size_t *written)
{
    size_t n, label = 0, start = 0, i, offset = 12;
    if (!written)
        return CF_ERR_ARGUMENT;
    *written = 0;
    if (!name || !out || (type != CF_DNS_A && type != CF_DNS_SRV))
        return CF_ERR_ARGUMENT;
    n = strlen(name);
    if (!n || n > CF_DNS_NAME_MAX || cap < n + 29)
        return CF_ERR_LIMIT;
    memset(out, 0, 12);
    put16(out, id);
    out[2] = 1;
    out[5] = 1;
    out[11] = 1; /* RD, QD=1, AR=1. */
    for (i = 0; i <= n; ++i)
    {
        if (i == n || name[i] == '.')
        {
            if (!label || label > 63)
                return CF_ERR_ARGUMENT;
            out[offset++] = (uint8_t)label;
            while (start < i)
                out[offset++] = lower((uint8_t)name[start++]);
            start = i + 1;
            label = 0;
        }
        else
        {
            if (!name_char(lower((uint8_t)name[i])))
                return CF_ERR_ARGUMENT;
            ++label;
        }
    }
    out[offset++] = 0;
    put16(out + offset, (uint16_t)type);
    put16(out + offset + 2, 1);
    offset += 4;
    /* EDNS0 OPT advertises a bounded UDP payload, no DNSSEC flag/options. */
    memset(out + offset, 0, 11);
    put16(out + offset + 1, 41);
    put16(out + offset + 3, CF_DNS_PACKET_MAX);
    offset += 11;
    *written = offset;
    return CF_OK;
}
static cf_result read_name(cf_bytes packet, size_t *offset, char *out)
{
    size_t at = *offset, used = 0, resume = 0;
    unsigned hops = 0, labels = 0;
    while (true)
    {
        uint8_t n;
        if (at >= packet.len)
            return CF_ERR_TRUNCATED;
        n = packet.ptr[at++];
        if ((n & 0xc0) == 0xc0)
        {
            size_t target;
            if (at >= packet.len)
                return CF_ERR_TRUNCATED;
            target = (size_t)(n & 0x3f) * 256 + packet.ptr[at++];
            if (++hops > 16 || target >= at - 2)
                return CF_ERR_FORMAT; /* Backward compression only. */
            if (!resume)
                resume = at;
            at = target;
            continue;
        }
        if (n > 63)
            return CF_ERR_FORMAT;
        if (!n)
        {
            out[used] = 0;
            *offset = resume ? resume : at;
            return CF_OK;
        }
        if (++labels > 127 || n > packet.len - at || used + n + (used ? 1u : 0u) > CF_DNS_NAME_MAX)
            return CF_ERR_LIMIT;
        if (used)
            out[used++] = '.';
        while (n--)
        {
            uint8_t c = lower(packet.ptr[at++]);
            if (!name_char(c))
                return CF_ERR_FORMAT;
            out[used++] = (char)c;
        }
    }
}
cf_result cf_dns_parse(cf_bytes packet, const char *name, cf_dns_type type, uint16_t id, cf_dns_answer *out)
{
    size_t at = 12, i, count, answers;
    char owner[CF_DNS_NAME_MAX + 1];
    cf_result rc = CF_ERR_FORMAT;
    if (!out)
        return CF_ERR_ARGUMENT;
    memset(out, 0, sizeof(*out));
    if (!packet.ptr || !name || (type != CF_DNS_A && type != CF_DNS_SRV))
        return CF_ERR_ARGUMENT;
    if (packet.len < 12)
        return CF_ERR_TRUNCATED;
    if (packet.len > CF_DNS_PACKET_MAX)
        return CF_ERR_LIMIT;
    if (u16(packet.ptr) != id || (packet.ptr[2] & 0xf8) != 0x80 || u16(packet.ptr + 4) != 1)
        return CF_ERR_FORMAT;
    if (packet.ptr[2] & 2)
        return CF_ERR_TRUNCATED;
    if (packet.ptr[3] & 15)
        return CF_ERR_STATE;
    rc = read_name(packet, &at, owner);
    if (rc != CF_OK)
        return rc;
    if (strcmp(owner, name) || at + 4 > packet.len || u16(packet.ptr + at) != type || u16(packet.ptr + at + 2) != 1)
        return CF_ERR_FORMAT;
    at += 4;
    answers = u16(packet.ptr + 6);
    count = answers + u16(packet.ptr + 8) + u16(packet.ptr + 10);
    if (count > 64)
        return CF_ERR_LIMIT;
    for (i = 0; i < count; ++i)
    {
        uint16_t record_type, record_class, len;
        uint32_t ttl;
        size_t end;
        rc = read_name(packet, &at, owner);
        if (rc != CF_OK)
            goto failed;
        if (at + 10 > packet.len)
        {
            rc = CF_ERR_TRUNCATED;
            goto failed;
        }
        record_type = u16(packet.ptr + at);
        record_class = u16(packet.ptr + at + 2);
        ttl = u32(packet.ptr + at + 4);
        len = u16(packet.ptr + at + 8);
        at += 10;
        if (len > packet.len - at)
        {
            rc = CF_ERR_TRUNCATED;
            goto failed;
        }
        end = at + len;
        if (i < answers && record_type == type && record_class == 1 && !strcmp(owner, name))
        {
            cf_dns_record *record;
            if (out->count >= CF_DNS_RECORD_MAX)
            {
                rc = CF_ERR_LIMIT;
                goto failed;
            }
            record = &out->records[out->count];
            record->ttl = ttl;
            if (type == CF_DNS_A)
            {
                if (len != 4)
                {
                    rc = CF_ERR_FORMAT;
                    goto failed;
                }
                memcpy(record->ip, packet.ptr + at, 4);
                /* A discovery address must be unicast. TLS still authenticates it. */
                if (!record->ip[0] || record->ip[0] >= 224 || record->ip[0] == 127)
                {
                    rc = CF_ERR_FORMAT;
                    goto failed;
                }
            }
            else
            {
                if (len < 7)
                {
                    rc = CF_ERR_FORMAT;
                    goto failed;
                }
                record->priority = u16(packet.ptr + at);
                record->weight = u16(packet.ptr + at + 2);
                record->port = u16(packet.ptr + at + 4);
                at += 6;
                rc = read_name(packet, &at, record->target);
                if (rc != CF_OK)
                    goto failed;
                if (at != end || record->port != CF_EDGE_PORT || !cf_dns_edge_target(record->target))
                {
                    rc = CF_ERR_UNSUPPORTED;
                    goto failed;
                }
            }
            ++out->count;
        }
        at = end;
    }
    if (at != packet.len)
    {
        rc = CF_ERR_FORMAT;
        goto failed;
    }
    return out->count ? CF_OK : CF_ERR_STATE;
failed:
    memset(out, 0, sizeof(*out));
    return rc;
}
