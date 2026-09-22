#include "cf_headers.h"
#include <string.h>

static bool valid_name(cf_bytes s)
{
    size_t i;
    if (!s.ptr || !s.len)
        return false;
    for (i = 0; i < s.len; ++i)
    {
        uint8_t c = s.ptr[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9'))
            continue;
        if (c && c < 128 && strchr("!#$%&'*+-.^_`|~", (int)c))
            continue;
        return false;
    }
    return true;
}

static bool valid_value(cf_bytes s)
{
    size_t i;
    if (!s.ptr && s.len)
        return false;
    for (i = 0; i < s.len; ++i)
        if ((s.ptr[i] < 32 && s.ptr[i] != '\t') || s.ptr[i] == 127)
            return false;
    return true;
}

static bool prefix(cf_bytes s, const char *p)
{
    size_t i, n = strlen(p);
    if (n > s.len || !s.ptr)
        return false;
    for (i = 0; i < n; ++i)
    {
        unsigned c = s.ptr[i];
        if (c >= 'A' && c <= 'Z')
            c += 'a' - 'A';
        if (c != (unsigned char)p[i])
            return false;
    }
    return true;
}

bool cf_header_is_control(cf_bytes name)
{
    return prefix(name, ":") || prefix(name, "cf-int-") ||
           prefix(name, "cf-cloudflared-") || prefix(name, "cf-proxy-");
}

cf_result cf_headers_decode(cf_bytes wire, cf_header *entries, size_t slots,
                            uint8_t *arena, size_t cap, size_t *count)
{
    size_t start = 0, used = 0, num = 0;
    if (!count)
        return CF_ERR_ARGUMENT;
    *count = 0;
    if ((!wire.ptr && wire.len) || !entries || !arena)
        return CF_ERR_ARGUMENT;
    if (wire.len > CF_HEADER_WIRE_MAX)
        return CF_ERR_LIMIT;
    if (cap > CF_HEADER_DECODED_MAX)
        cap = CF_HEADER_DECODED_MAX;
    if (slots > CF_HEADER_COUNT_MAX)
        slots = CF_HEADER_COUNT_MAX;
    while (start < wire.len)
    {
        size_t end = start, colon, n;
        cf_result rc;
        cf_header h;
        while (end < wire.len && wire.ptr[end] != ';')
            ++end;
        if (end == start)
        {
            start = end + 1;
            continue;
        }
        if (num == slots)
            return CF_ERR_LIMIT;
        colon = start;
        while (colon < end && wire.ptr[colon] != ':')
            ++colon;
        if (colon == end)
            return CF_ERR_FORMAT;
        rc = cf_base64_decode((cf_bytes){wire.ptr + start, colon - start},
                              false, arena + used, cap - used, &n);
        if (rc != CF_OK)
            return rc;
        h.name = (cf_bytes){arena + used, n};
        used += n;
        rc = cf_base64_decode((cf_bytes){wire.ptr + colon + 1, end - colon - 1},
                              false, arena + used, cap - used, &n);
        if (rc != CF_OK)
            return rc;
        h.value = (cf_bytes){arena + used, n};
        used += n;
        if (!valid_name(h.name) || !valid_value(h.value))
            return CF_ERR_FORMAT;
        entries[num++] = h;
        start = end + 1;
    }
    *count = num;
    return CF_OK;
}

cf_result cf_headers_encode(const cf_header *entries, size_t count,
                            char *wire, size_t cap, size_t *written)
{
    size_t i, used = 0, decoded = 0, need = 0;
    if (!written)
        return CF_ERR_ARGUMENT;
    *written = 0;
    if ((!entries && count) || (!wire && cap))
        return CF_ERR_ARGUMENT;
    if (count > CF_HEADER_COUNT_MAX)
        return CF_ERR_LIMIT;
    for (i = 0; i < count; ++i)
    {
        size_t a = entries[i].name.len, b = entries[i].value.len;
        if (a > CF_HEADER_DECODED_MAX - decoded)
            return CF_ERR_LIMIT;
        decoded += a;
        if (b > CF_HEADER_DECODED_MAX - decoded)
            return CF_ERR_LIMIT;
        decoded += b;
        if (!valid_name(entries[i].name) || !valid_value(entries[i].value))
            return CF_ERR_FORMAT;
        need += (a / 3) * 4 + (a % 3 ? a % 3 + 1 : 0);
        need += (b / 3) * 4 + (b % 3 ? b % 3 + 1 : 0) + 1 + (i ? 1 : 0);
    }
    if (need > cap || need > CF_HEADER_WIRE_MAX)
        return CF_ERR_LIMIT;
    for (i = 0; i < count; ++i)
    {
        size_t n;
        if (i)
            wire[used++] = ';';
        (void)cf_base64_encode(entries[i].name, false, wire + used, cap - used, &n);
        used += n;
        wire[used++] = ':';
        (void)cf_base64_encode(entries[i].value, false, wire + used, cap - used, &n);
        used += n;
    }
    *written = used;
    return CF_OK;
}
