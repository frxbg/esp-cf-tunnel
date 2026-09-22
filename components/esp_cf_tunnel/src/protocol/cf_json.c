#include "cf_json.h"
#include <string.h>
#include <stdlib.h>

/* These hooks belong only to our prefixed private cJSON copy. They neither
 * replace malloc globally nor affect an application's cJSON instance.
 * One JSON owner, no reentry. Charge metadata too; erase parse-error paths. */
#define JSON_HEAP_LIMIT 32768u
/* cJSON allocates char arrays and structs containing pointers/double. This
 * union supplies their alignment on MSVC and both ESP targets (MSVC's C11
 * headers do not expose max_align_t). */
typedef union
{
    long double alignment;
    void *pointer_alignment;
    size_t size;
} json_allocation;
static size_t json_live_bytes;
static bool json_allocation_failed;

static void *json_alloc(size_t size)
{
    json_allocation *p;
    size_t charge;
    if (size > JSON_HEAP_LIMIT - sizeof(*p))
    {
        json_allocation_failed = true;
        return NULL;
    }
    charge = size + sizeof(*p);
    if (charge > JSON_HEAP_LIMIT - json_live_bytes)
    {
        json_allocation_failed = true;
        return NULL;
    }
    p = malloc(charge);
    if (!p)
    {
        json_allocation_failed = true;
        return NULL;
    }
    p->size = charge;
    json_live_bytes += charge;
    return p + 1;
}

static void json_free(void *ptr)
{
    json_allocation *p;
    size_t charge;
    if (!ptr)
        return;
    p = (json_allocation *)ptr - 1;
    charge = p->size;
    json_live_bytes -= charge;
    cf_secure_zero(p, charge);
    free(p);
}

cf_result cf_json_failure(void)
{
    return json_allocation_failed ? CF_ERR_MEMORY : CF_ERR_FORMAT;
}

static bool number_digit(uint8_t c) { return c >= '0' && c <= '9'; }

/* cJSON deliberately accepts some non-JSON number/control spellings. Restrict
 * those lexemes before the upstream parser handles structure and escapes. */
static bool valid_lexemes(cf_bytes in)
{
    bool string = false, escaped = false;
    size_t i;
    for (i = 0; i < in.len; ++i)
    {
        uint8_t c = in.ptr[i];
        if (string)
        {
            if (c < 32)
                return false;
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (c == '\\')
                escaped = true;
            else if (c == '"')
                string = false;
            continue;
        }
        if (c == '"')
        {
            string = true;
            continue;
        }
        if (c < 32 && c != '\t' && c != '\r' && c != '\n')
            return false;
        if (c == '-' || number_digit(c))
        {
            size_t p = i;
            if (in.ptr[p] == '-' && ++p == in.len)
                return false;
            if (!number_digit(in.ptr[p]))
                return false;
            if (in.ptr[p] == '0')
                ++p;
            else
                while (p < in.len && number_digit(in.ptr[p]))
                    ++p;
            if (p < in.len && in.ptr[p] == '.')
            {
                if (++p == in.len || !number_digit(in.ptr[p]))
                    return false;
                while (p < in.len && number_digit(in.ptr[p]))
                    ++p;
            }
            if (p < in.len && (in.ptr[p] == 'e' || in.ptr[p] == 'E'))
            {
                if (++p == in.len)
                    return false;
                if (in.ptr[p] == '+' || in.ptr[p] == '-')
                    ++p;
                if (p == in.len || !number_digit(in.ptr[p]))
                    return false;
                while (p < in.len && number_digit(in.ptr[p]))
                    ++p;
            }
            if (p < in.len && in.ptr[p] != ',' && in.ptr[p] != '}' && in.ptr[p] != ']' &&
                in.ptr[p] != ' ' && in.ptr[p] != '\t' && in.ptr[p] != '\r' && in.ptr[p] != '\n')
                return false;
            i = p - 1;
        }
    }
    return !string;
}

cf_cJSON *cf_json_parse(cf_bytes in)
{
    const char *end = NULL;
    cf_cJSON *root;
    size_t i;
    cf_cJSON_Hooks hooks = {json_alloc, json_free};
    json_allocation_failed = false;
    cf_cJSON_InitHooks(&hooks);
    if (!in.ptr || !in.len || in.len > 8192 || !valid_lexemes(in))
        return NULL;
    /* Embedded NULs cannot be represented by cJSON's C-string API. Also reject
     * escaped NULs before parsing so field comparisons cannot truncate. */
    for (i = 0; i < in.len; ++i)
    {
        if (!in.ptr[i])
            return NULL;
        if (i + 6 <= in.len && memcmp(in.ptr + i, "\\u0000", 6) == 0)
            return NULL;
    }
    root = cf_cJSON_ParseWithLengthOpts((const char *)in.ptr, in.len, &end, 0);
    if (!root)
        return NULL;
    while (end < (const char *)in.ptr + in.len &&
           (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        ++end;
    if (end != (const char *)in.ptr + in.len)
    {
        cf_json_delete_secret(root);
        return NULL;
    }
    return root;
}

static void wipe_tree(cf_cJSON *item)
{
    for (; item; item = item->next)
    {
        if (item->valuestring)
            cf_secure_zero(item->valuestring, strlen(item->valuestring));
        if (item->child)
            wipe_tree(item->child);
    }
}

void cf_json_delete_secret(cf_cJSON *root)
{
    wipe_tree(root);
    cf_cJSON_Delete(root);
}

bool cf_json_keys(const cf_cJSON *object, const char *const *allowed, size_t count)
{
    const cf_cJSON *a, *b;
    if (!cf_cJSON_IsObject(object))
        return false;
    for (a = object->child; a; a = a->next)
    {
        size_t i;
        if (!a->string)
            return false;
        for (i = 0; i < count; ++i)
            if (strcmp(a->string, allowed[i]) == 0)
                break;
        if (i == count)
            return false;
        for (b = a->next; b; b = b->next)
            if (b->string && strcmp(a->string, b->string) == 0)
                return false;
    }
    return true;
}
