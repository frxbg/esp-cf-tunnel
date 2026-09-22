#include "cf_core.h"

void cf_secure_zero(void *ptr, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)ptr;
    while (len--)
        *p++ = 0;
}

static int digit(uint8_t c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}

cf_result cf_base64_decode(cf_bytes in, bool padded, uint8_t *out,
                           size_t cap, size_t *written)
{
    size_t n = in.len, count, i, pos = 0;
    uint32_t bits = 0;
    unsigned pending = 0;
    if (!written)
        return CF_ERR_ARGUMENT;
    *written = 0;
    if ((!in.ptr && n) || (!out && cap))
        return CF_ERR_ARGUMENT;
    if (padded)
    {
        if (n % 4)
            return CF_ERR_FORMAT;
        if (n && in.ptr[n - 1] == '=')
            --n;
        if (n && in.ptr[n - 1] == '=')
            --n;
    }
    if (n % 4 == 1)
        return CF_ERR_FORMAT;
    count = (n / 4) * 3 + (n % 4 ? n % 4 - 1 : 0);
    if (count > cap)
        return CF_ERR_LIMIT;
    for (i = 0; i < n; ++i)
    {
        int d = digit(in.ptr[i]);
        if (d < 0)
            return CF_ERR_FORMAT;
        bits = (bits << 6) | (uint32_t)d;
        pending += 6;
        if (pending >= 8)
        {
            pending -= 8;
            out[pos++] = (uint8_t)(bits >> pending);
        }
    }
    if (pending && (bits & ((1u << pending) - 1u)))
        return CF_ERR_FORMAT;
    *written = pos;
    return CF_OK;
}

cf_result cf_base64_encode(cf_bytes in, bool padded, char *out,
                           size_t cap, size_t *written)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, pos = 0, need, rem = in.len % 3;
    if (!written)
        return CF_ERR_ARGUMENT;
    *written = 0;
    if ((!in.ptr && in.len) || (!out && cap))
        return CF_ERR_ARGUMENT;
    if (in.len / 3 > (SIZE_MAX - 4) / 4)
        return CF_ERR_LIMIT;
    need = (in.len / 3) * 4 + (rem ? (padded ? 4 : rem + 1) : 0);
    if (need > cap)
        return CF_ERR_LIMIT;
    for (i = 0; i < in.len;)
    {
        size_t left = in.len - i;
        uint32_t word = (uint32_t)in.ptr[i++] << 16;
        if (left > 1)
            word |= (uint32_t)in.ptr[i++] << 8;
        if (left > 2)
            word |= in.ptr[i++];
        out[pos++] = alphabet[(word >> 18) & 63];
        out[pos++] = alphabet[(word >> 12) & 63];
        if (left > 1)
            out[pos++] = alphabet[(word >> 6) & 63];
        else if (padded)
            out[pos++] = '=';
        if (left > 2)
            out[pos++] = alphabet[word & 63];
        else if (padded)
            out[pos++] = '=';
    }
    *written = pos;
    return CF_OK;
}
