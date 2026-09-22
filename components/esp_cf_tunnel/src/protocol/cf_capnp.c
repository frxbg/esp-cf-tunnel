#include "cf_capnp.h"
#include <string.h>

static uint64_t le(const uint8_t *p, unsigned n)
{
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < n; ++i)
        v |= (uint64_t)p[i] << (8u * i);
    return v;
}
static void put(uint8_t *p, unsigned n, uint64_t v)
{
    unsigned i;
    for (i = 0; i < n; ++i)
        p[i] = (uint8_t)(v >> (8u * i));
}
static bool range(cf_cp_reader *r, uint32_t seg, uint32_t word, uint64_t words)
{
    if (r->error != CF_OK)
        return false;
    if (seg >= r->segment_count || word > r->segments[seg].len / 8 ||
        words > r->segments[seg].len / 8 - word)
    {
        r->error = CF_ERR_FORMAT;
        return false;
    }
    return true;
}
static bool charge(cf_cp_reader *r, uint64_t words)
{
    if (!words)
        words = 1; /* Empty structures still cost work. */
    if (words > r->budget)
    {
        r->error = CF_ERR_LIMIT;
        return false;
    }
    r->budget -= (uint32_t)words;
    return true;
}
static uint64_t word_at(cf_cp_reader *r, uint32_t seg, uint32_t word)
{
    if (!range(r, seg, word, 1))
        return 0;
    return le(r->segments[seg].ptr + (size_t)word * 8, 8);
}

cf_result cf_cp_read_init(cf_cp_reader *r, cf_bytes frame)
{
    uint32_t i;
    if (!r)
        return CF_ERR_ARGUMENT;
    memset(r, 0, sizeof(*r));
    r->budget = CF_CAPNP_TRAVERSAL_WORDS;
    r->error = cf_capnp_segment(frame, 0, &r->segments[0]);
    if (r->error != CF_OK)
        return r->error;
    r->segment_count = (uint32_t)le(frame.ptr, 4) + 1;
    for (i = 1; i < r->segment_count; ++i)
    {
        r->error = cf_capnp_segment(frame, i, &r->segments[i]);
        if (r->error != CF_OK)
            return r->error;
    }
    return CF_OK;
}

static cf_cp_value follow(cf_cp_reader *r, uint32_t seg, uint32_t slot, uint8_t depth)
{
    cf_cp_value v = {0};
    uint64_t p, words;
    int64_t target, offset;
    bool direct_target = false;
    if (r->error != CF_OK)
        return v;
    if (depth > CF_CAPNP_DEPTH_MAX)
    {
        r->error = CF_ERR_LIMIT;
        return v;
    }
    p = word_at(r, seg, slot);
    if (r->error != CF_OK || !charge(r, 1) || p == 0)
        return v;
    /* Far pointers resolve exactly one landing pad, not an arbitrary chain. */
    if ((p & 3u) == 2)
    {
        bool two = (p & 4u) != 0;
        slot = (uint32_t)p >> 3;
        seg = (uint32_t)(p >> 32);
        if (!range(r, seg, slot, two ? 2 : 1) || !charge(r, two ? 2 : 1))
            return v;
        p = word_at(r, seg, slot);
        if (two)
        {
            uint64_t tag = word_at(r, seg, slot + 1);
            if ((p & 7u) != 2 || ((uint32_t)tag >> 2) != 0 || (tag & 3u) > 1)
            {
                r->error = CF_ERR_FORMAT;
                return v;
            }
            seg = (uint32_t)(p >> 32);
            slot = (uint32_t)p >> 3;
            p = tag;
            direct_target = true;
        }
        else if ((p & 3u) > 1 || p == 0)
        {
            r->error = CF_ERR_FORMAT;
            return v;
        }
    }
    if ((p & 3u) == 3)
    {
        if ((uint32_t)p != 3)
        {
            r->error = CF_ERR_UNSUPPORTED;
            return v;
        }
        v.kind = CF_CP_CAP;
        v.cap = (uint32_t)(p >> 32);
        v.depth = depth;
        return v;
    }
    offset = (int64_t)((uint32_t)p >> 2);
    if (offset & INT64_C(0x20000000))
        offset -= INT64_C(0x40000000);
    target = direct_target ? (int64_t)slot : (int64_t)slot + 1 + offset;
    if (target < 0 || target > UINT32_MAX)
    {
        r->error = CF_ERR_FORMAT;
        return v;
    }
    v.segment = seg;
    v.word = (uint32_t)target;
    v.depth = depth;
    if ((p & 3u) == 0)
    {
        v.kind = CF_CP_STRUCT;
        v.data_words = (uint16_t)(p >> 32);
        v.pointers = (uint16_t)(p >> 48);
        words = (uint32_t)v.data_words + v.pointers;
    }
    else if ((p & 3u) == 1)
    {
        static const uint8_t bits[] = {0, 1, 8, 16, 32, 64, 64};
        v.kind = CF_CP_LIST;
        v.element_size = (uint8_t)((p >> 32) & 7);
        v.count = (uint32_t)(p >> 35);
        if (v.element_size == 7)
        {
            uint64_t tag;
            words = (uint64_t)v.count + 1;
            if (!range(r, seg, v.word, words))
                return (cf_cp_value){0};
            tag = word_at(r, seg, v.word++);
            if ((tag & 3u) != 0)
            {
                r->error = CF_ERR_FORMAT;
                return (cf_cp_value){0};
            }
            v.count = (uint32_t)tag >> 2;
            v.data_words = (uint16_t)(tag >> 32);
            v.pointers = (uint16_t)(tag >> 48);
            if ((uint64_t)v.count * ((uint32_t)v.data_words + v.pointers) > words - 1)
            {
                r->error = CF_ERR_FORMAT;
                return (cf_cp_value){0};
            }
            if (!charge(r, words))
                return (cf_cp_value){0};
            return v;
        }
        words = ((uint64_t)v.count * bits[v.element_size] + 63) / 64;
    }
    else
    {
        r->error = CF_ERR_FORMAT;
        return (cf_cp_value){0};
    }
    if (!range(r, seg, v.word, words) || !charge(r, words))
        return (cf_cp_value){0};
    return v;
}

cf_cp_value cf_cp_root(cf_cp_reader *r) { return follow(r, 0, 0, 1); }
cf_cp_value cf_cp_field(cf_cp_reader *r, cf_cp_value s, uint16_t index)
{
    if (r->error != CF_OK || s.kind == CF_CP_NULL)
        return (cf_cp_value){0};
    if (s.kind != CF_CP_STRUCT)
    {
        r->error = CF_ERR_FORMAT;
        return (cf_cp_value){0};
    }
    if (index >= s.pointers)
        return (cf_cp_value){0};
    return follow(r, s.segment, s.word + s.data_words + index, (uint8_t)(s.depth + 1));
}
cf_cp_value cf_cp_at(cf_cp_reader *r, cf_cp_value list, uint32_t index)
{
    cf_cp_value s = {0};
    if (r->error != CF_OK)
        return s;
    if (list.kind != CF_CP_LIST || index >= list.count)
    {
        r->error = CF_ERR_FORMAT;
        return s;
    }
    if (list.element_size == 6)
        return follow(r, list.segment, list.word + index, (uint8_t)(list.depth + 1));
    if (list.element_size != 7)
    {
        r->error = CF_ERR_UNSUPPORTED;
        return s;
    }
    if (list.depth >= CF_CAPNP_DEPTH_MAX || !charge(r, 1))
    {
        r->error = CF_ERR_LIMIT;
        return s;
    }
    s = list;
    s.kind = CF_CP_STRUCT;
    s.word += index * ((uint32_t)s.data_words + s.pointers);
    s.depth++;
    return s;
}
uint64_t cf_cp_uint(cf_cp_reader *r, cf_cp_value s, uint16_t byte, uint8_t width)
{
    if (r->error != CF_OK || s.kind == CF_CP_NULL)
        return 0;
    if (s.kind != CF_CP_STRUCT || (width != 1 && width != 2 && width != 4 && width != 8))
    {
        r->error = CF_ERR_FORMAT;
        return 0;
    }
    if ((uint32_t)byte + width > (uint32_t)s.data_words * 8)
        return 0;
    if (!range(r, s.segment, s.word, s.data_words) || !charge(r, 1))
        return 0;
    return le(r->segments[s.segment].ptr + (size_t)s.word * 8 + byte, width);
}
cf_bytes cf_cp_data(cf_cp_reader *r, cf_cp_value v, bool text)
{
    cf_bytes b = {0};
    if (r->error != CF_OK || v.kind == CF_CP_NULL)
        return b;
    if (v.kind != CF_CP_LIST || v.element_size != 2)
    {
        r->error = CF_ERR_FORMAT;
        return b;
    }
    if (!range(r, v.segment, v.word, ((uint64_t)v.count + 7) / 8))
        return b;
    b.ptr = r->segments[v.segment].ptr + (size_t)v.word * 8;
    b.len = v.count;
    if (text)
    {
        if (!b.len || b.ptr[b.len - 1] != 0)
        {
            r->error = CF_ERR_FORMAT;
            return (cf_bytes){0};
        }
        --b.len;
    }
    return b;
}

static uint32_t alloc_words(cf_cp_builder *b, uint32_t n)
{
    uint32_t at = b->words;
    if (b->error != CF_OK)
        return 0;
    if (n > (b->capacity - 8) / 8 - b->words)
    {
        b->error = CF_ERR_LIMIT;
        return 0;
    }
    memset(b->buffer + 8 + (size_t)at * 8, 0, (size_t)n * 8);
    b->words += n;
    return at;
}
static void set_word(cf_cp_builder *b, uint32_t slot, uint64_t value)
{
    if (b->error != CF_OK)
        return;
    if (slot >= b->words)
    {
        b->error = CF_ERR_ARGUMENT;
        return;
    }
    put(b->buffer + 8 + (size_t)slot * 8, 8, value);
}
cf_result cf_cp_build_init(cf_cp_builder *b, uint8_t *out, size_t capacity)
{
    if (!b)
        return CF_ERR_ARGUMENT;
    memset(b, 0, sizeof(*b));
    if (!out || capacity < 16 || capacity > CF_CAPNP_MESSAGE_MAX)
        return b->error = CF_ERR_ARGUMENT;
    b->buffer = out;
    b->capacity = capacity;
    memset(out, 0, 8);
    (void)alloc_words(b, 1);
    return CF_OK;
}
uint32_t cf_cp_struct(cf_cp_builder *b, uint32_t slot, uint16_t data_words, uint16_t pointers)
{
    uint32_t at = alloc_words(b, (uint32_t)data_words + pointers);
    int64_t delta = (int64_t)at - slot - 1;
    /* Zero-sized structs must be non-null even when allocated next to slot. */
    if (!data_words && !pointers)
        delta = -1;
    set_word(b, slot, ((uint64_t)(delta & INT64_C(0x3fffffff)) << 2) | (uint64_t)data_words << 32 | (uint64_t)pointers << 48);
    return at;
}
void cf_cp_set_uint(cf_cp_builder *b, uint32_t object, uint16_t byte, uint8_t width, uint64_t value)
{
    size_t offset = (size_t)object * 8 + byte;
    if (b->error != CF_OK)
        return;
    if ((width != 1 && width != 2 && width != 4 && width != 8) || offset > (size_t)b->words * 8 ||
        width > (size_t)b->words * 8 - offset)
    {
        b->error = CF_ERR_ARGUMENT;
        return;
    }
    put(b->buffer + 8 + offset, width, value);
}
uint32_t cf_cp_pointer_list(cf_cp_builder *b, uint32_t slot, uint32_t count)
{
    uint32_t at = alloc_words(b, count);
    set_word(b, slot, (uint64_t)(at - slot - 1) << 2 | 1u | UINT64_C(6) << 32 | (uint64_t)count << 35);
    return at;
}
void cf_cp_blob(cf_cp_builder *b, uint32_t slot, cf_bytes value, bool text)
{
    uint32_t at, n;
    if (b->error != CF_OK)
        return;
    if ((!value.ptr && value.len) || value.len > CF_CAPNP_MESSAGE_MAX - 1)
    {
        b->error = CF_ERR_ARGUMENT;
        return;
    }
    n = (uint32_t)value.len + (text ? 1u : 0u);
    at = alloc_words(b, (n + 7) / 8);
    set_word(b, slot, (uint64_t)(at - slot - 1) << 2 | 1u | UINT64_C(2) << 32 | (uint64_t)n << 35);
    if (b->error == CF_OK && value.len)
        memcpy(b->buffer + 8 + (size_t)at * 8, value.ptr, value.len);
}
cf_result cf_cp_build_end(cf_cp_builder *b, size_t *written)
{
    if (!written || !b)
        return CF_ERR_ARGUMENT;
    *written = 0;
    if (b->error != CF_OK)
        return b->error;
    put(b->buffer + 4, 4, b->words);
    *written = 8 + (size_t)b->words * 8;
    return CF_OK;
}
