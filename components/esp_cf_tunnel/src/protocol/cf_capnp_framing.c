#include "cf_capnp_framing.h"
#include <string.h>

static uint32_t u32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static size_t table_size(uint32_t n) { return ((size_t)n + 2u) / 2u * 8u; }

static cf_result total_size(const uint8_t *p, uint32_t n, size_t cap, size_t *out)
{
    size_t total = table_size(n);
    uint32_t i;
    if (total > cap)
        return CF_ERR_LIMIT;
    /* Segment zero must contain the root pointer. Empty later segments are legal. */
    if (u32(p + 4) == 0)
        return CF_ERR_FORMAT;
    for (i = 0; i < n; ++i)
    {
        uint32_t words = u32(p + 4 + (size_t)i * 4);
        if (words > (cap - total) / 8)
            return CF_ERR_LIMIT;
        total += (size_t)words * 8;
    }
    *out = total;
    return CF_OK;
}

cf_result cf_capnp_framer_init(cf_capnp_framer *f, uint8_t *buffer, size_t cap)
{
    if (!f || !buffer || cap < 16 || cap > CF_CAPNP_MESSAGE_MAX)
        return CF_ERR_ARGUMENT;
    memset(f, 0, sizeof(*f));
    f->buffer = buffer;
    f->capacity = cap;
    cf_capnp_framer_reset(f);
    return CF_OK;
}

void cf_capnp_framer_reset(cf_capnp_framer *f)
{
    if (!f)
        return;
    f->used = 0;
    f->target = 4;
    f->segments = 0;
    f->phase = 0;
    f->error = CF_OK;
}

cf_result cf_capnp_feed(cf_capnp_framer *f, cf_bytes in, size_t *consumed,
                        cf_capnp_message_fn callback, void *ctx)
{
    if (!consumed)
        return CF_ERR_ARGUMENT;
    *consumed = 0;
    if (!f || !f->buffer || !callback || (!in.ptr && in.len))
        return CF_ERR_ARGUMENT;
    if (f->error != CF_OK)
        return f->error;
    while (*consumed < in.len)
    {
        size_t n = f->target - f->used;
        if (n > in.len - *consumed)
            n = in.len - *consumed;
        memcpy(f->buffer + f->used, in.ptr + *consumed, n);
        f->used += n;
        *consumed += n;
        if (f->used != f->target)
            continue;
        if (f->phase == 0)
        {
            uint32_t minus_one = u32(f->buffer);
            if (minus_one >= CF_CAPNP_MAX_SEGMENTS)
                return f->error = CF_ERR_LIMIT;
            f->segments = minus_one + 1;
            f->target = table_size(f->segments);
            if (f->target > f->capacity)
                return f->error = CF_ERR_LIMIT;
            f->phase = 1;
        }
        else if (f->phase == 1)
        {
            cf_result rc = total_size(f->buffer, f->segments, f->capacity, &f->target);
            if (rc != CF_OK)
                return f->error = rc;
            f->phase = 2;
        }
        else
        {
            cf_result rc = callback(ctx, (cf_bytes){f->buffer, f->used});
            if (rc != CF_OK)
                return f->error = rc;
            cf_capnp_framer_reset(f);
        }
    }
    return CF_OK;
}

cf_result cf_capnp_eof(const cf_capnp_framer *f)
{
    if (!f)
        return CF_ERR_ARGUMENT;
    if (f->error != CF_OK)
        return f->error;
    return f->used ? CF_ERR_TRUNCATED : CF_OK;
}

cf_result cf_capnp_segment(cf_bytes frame, uint32_t index, cf_bytes *out)
{
    uint32_t n, i;
    size_t total, offset;
    cf_result rc;
    if (!out || (!frame.ptr && frame.len))
        return CF_ERR_ARGUMENT;
    *out = (cf_bytes){NULL, 0};
    if (frame.len < 8)
        return CF_ERR_TRUNCATED;
    n = u32(frame.ptr);
    if (n >= CF_CAPNP_MAX_SEGMENTS)
        return CF_ERR_LIMIT;
    ++n;
    if (index >= n)
        return CF_ERR_ARGUMENT;
    offset = table_size(n);
    if (offset > frame.len)
        return CF_ERR_TRUNCATED;
    rc = total_size(frame.ptr, n, CF_CAPNP_MESSAGE_MAX, &total);
    if (rc != CF_OK)
        return rc;
    if (total != frame.len)
        return total > frame.len ? CF_ERR_TRUNCATED : CF_ERR_FORMAT;
    for (i = 0; i < index; ++i)
        offset += (size_t)u32(frame.ptr + 4 + (size_t)i * 4) * 8;
    *out = (cf_bytes){frame.ptr + offset, (size_t)u32(frame.ptr + 4 + (size_t)index * 4) * 8};
    return CF_OK;
}
