#include "cf_rpc.h"
#include <string.h>

/* Layouts: cloudflared 2026.9.1 tunnelrpc/proto/tunnelrpc.capnp.go and
 * zombiezen capnproto2 v2.18.0 std/capnp/rpc/rpc.capnp.go. See docs/protocol.md.
 * Data offsets are bytes; pointer slots follow the struct's data words. */
static uint32_t message(cf_cp_builder *b, uint16_t kind, uint16_t dw, uint16_t pc)
{
    uint32_t root = cf_cp_struct(b, 0, 1, 1);
    cf_cp_set_uint(b, root, 0, 2, kind);
    return cf_cp_struct(b, root + 1, dw, pc);
}
static bool output_ok(cf_rpc *r, uint8_t *out, size_t cap, size_t *n)
{
    if (n)
        *n = 0;
    return r && out && n && cap >= CF_RPC_TX_MAX;
}
static bool ascii(cf_bytes b, size_t max)
{
    size_t i;
    if (!b.ptr || !b.len || b.len > max)
        return false;
    for (i = 0; i < b.len; ++i)
        if (b.ptr[i] < 32 || b.ptr[i] > 126)
            return false;
    return true;
}
static cf_result finish(uint32_t id, bool release, uint8_t *out, size_t cap, size_t *n)
{
    cf_cp_builder b;
    uint32_t s;
    (void)cf_cp_build_init(&b, out, cap);
    s = message(&b, 4, 1, 0);
    cf_cp_set_uint(&b, s, 0, 4, id);
    cf_cp_set_uint(&b, s, 4, 1, release ? 0 : 1); /* Default true, XOR on wire. */
    return cf_cp_build_end(&b, n);
}
static cf_result release_cap(cf_rpc *r, uint8_t *out, size_t cap, size_t *n)
{
    cf_cp_builder b;
    uint32_t s;
    (void)cf_cp_build_init(&b, out, cap);
    s = message(&b, 6, 1, 0);
    cf_cp_set_uint(&b, s, 0, 4, r->capability);
    cf_cp_set_uint(&b, s, 4, 4, 1);
    return cf_cp_build_end(&b, n);
}
static cf_result fail(cf_rpc *r, cf_result error, bool abort_peer, uint8_t *out, size_t *n)
{
    static const uint8_t reason[] = "Unsupported or invalid registration RPC";
    cf_cp_builder b;
    uint32_t s;
    r->error = error;
    r->state = CF_RPC_FAILED;
    *n = 0;
    if (abort_peer)
    {
        (void)cf_cp_build_init(&b, out, CF_RPC_TX_MAX);
        s = message(&b, 1, 1, 1);
        cf_cp_blob(&b, s + 1, (cf_bytes){reason, sizeof(reason) - 1}, true);
        (void)cf_cp_build_end(&b, n);
    }
    return error;
}
void cf_rpc_init(cf_rpc *r)
{
    if (r)
        memset(r, 0, sizeof(*r));
}
void cf_rpc_reset(cf_rpc *r)
{
    if (r)
        cf_secure_zero(r, sizeof(*r));
}

cf_result cf_rpc_begin(cf_rpc *r, uint8_t *out, size_t cap, size_t *n)
{
    cf_cp_builder b;
    cf_result rc;
    if (!output_ok(r, out, cap, n))
        return CF_ERR_ARGUMENT;
    if (r->state != CF_RPC_IDLE)
        return CF_ERR_STATE;
    (void)cf_cp_build_init(&b, out, CF_RPC_TX_MAX);
    (void)message(&b, 8, 1, 1); /* Bootstrap question 0; no deprecated object ID. */
    rc = cf_cp_build_end(&b, n);
    if (rc == CF_OK)
        r->state = CF_RPC_BOOTSTRAPPING;
    return rc;
}
static uint32_t call(cf_cp_builder *b, uint32_t cap, uint32_t question, uint16_t method)
{
    uint32_t c = message(b, 2, 3, 3);
    uint32_t target;
    cf_cp_set_uint(b, c, 0, 4, question);
    cf_cp_set_uint(b, c, 4, 2, method);
    cf_cp_set_uint(b, c, 8, 8, CF_REGISTRATION_INTERFACE);
    target = cf_cp_struct(b, c + 3, 1, 1);
    cf_cp_set_uint(b, target, 0, 4, cap); /* importedCap; sendResultsTo.caller. */
    return cf_cp_struct(b, c + 4, 0, 2);  /* Payload, empty capability table. */
}
cf_result cf_rpc_register(cf_rpc *r, const cf_credentials *cred, const cf_rpc_options *opt,
                          uint8_t *out, size_t cap, size_t *n)
{
    cf_cp_builder b;
    uint32_t payload, params, auth, options, client;
    cf_result rc;
    size_t i;
    if (!output_ok(r, out, cap, n) || !cred || !opt)
        return CF_ERR_ARGUMENT;
    if (r->state != CF_RPC_READY || !r->have_capability)
        return CF_ERR_STATE;
    if (!ascii(opt->version, 63) || !ascii(opt->arch, 63) || !opt->local_ip.ptr ||
        (opt->local_ip.len != 4 && opt->local_ip.len != 16) || cred->account_tag[32] != '\0' ||
        cred->tunnel_secret_len < CF_TUNNEL_SECRET_MIN || cred->tunnel_secret_len > CF_TUNNEL_SECRET_MAX ||
        (opt->features & ~(CF_RPC_REMOTE_CONFIG | CF_RPC_SERIALIZED_HEADERS)))
        return CF_ERR_ARGUMENT;
    for (i = 0; i < 32; ++i)
    {
        char c = cred->account_tag[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return CF_ERR_ARGUMENT;
    }
    (void)cf_cp_build_init(&b, out, CF_RPC_TX_MAX);
    payload = call(&b, r->capability, 1, 0);
    params = cf_cp_struct(&b, payload, 1, 3);
    cf_cp_set_uint(&b, params, 0, 1, opt->connection_index);
    auth = cf_cp_struct(&b, params + 1, 0, 2);
    cf_cp_blob(&b, auth, (cf_bytes){(const uint8_t *)cred->account_tag, 32}, true);
    cf_cp_blob(&b, auth + 1, (cf_bytes){cred->tunnel_secret, cred->tunnel_secret_len}, false);
    cf_cp_blob(&b, params + 2, (cf_bytes){cred->tunnel_id, 16}, false);
    options = cf_cp_struct(&b, params + 3, 1, 2);
    cf_cp_set_uint(&b, options, 0, 1, opt->replace_existing ? 1 : 0);
    /* CompressionQuality is zero; advertise only explicitly implemented features. */
    cf_cp_set_uint(&b, options, 2, 1, opt->previous_attempts);
    client = cf_cp_struct(&b, options + 1, 0, 4);
    cf_cp_blob(&b, client, (cf_bytes){opt->connector_id, 16}, false);
    if (opt->features)
    {
        static const uint8_t remote[] = "allow_remote_config", serialized[] = "serialized_headers";
        uint32_t count = ((opt->features & CF_RPC_REMOTE_CONFIG) != 0) + ((opt->features & CF_RPC_SERIALIZED_HEADERS) != 0);
        uint32_t list = cf_cp_pointer_list(&b, client + 1, count);
        if (opt->features & CF_RPC_REMOTE_CONFIG)
            cf_cp_blob(&b, list++, (cf_bytes){remote, sizeof(remote) - 1}, true);
        if (opt->features & CF_RPC_SERIALIZED_HEADERS)
            cf_cp_blob(&b, list, (cf_bytes){serialized, sizeof(serialized) - 1}, true);
    }
    cf_cp_blob(&b, client + 2, opt->version, true);
    cf_cp_blob(&b, client + 3, opt->arch, true);
    cf_cp_blob(&b, options + 2, opt->local_ip, false);
    rc = cf_cp_build_end(&b, n);
    if (rc == CF_OK)
        r->state = CF_RPC_REGISTERING;
    else
        cf_secure_zero(out, CF_RPC_TX_MAX);
    return rc;
}
cf_result cf_rpc_unregister(cf_rpc *r, uint8_t *out, size_t cap, size_t *n)
{
    cf_cp_builder b;
    cf_result rc;
    uint32_t payload;
    if (!output_ok(r, out, cap, n))
        return CF_ERR_ARGUMENT;
    if (r->state != CF_RPC_REGISTERED || !r->have_capability)
        return CF_ERR_STATE;
    (void)cf_cp_build_init(&b, out, CF_RPC_TX_MAX);
    payload = call(&b, r->capability, 2, 1);
    (void)cf_cp_struct(&b, payload, 0, 0);
    rc = cf_cp_build_end(&b, n);
    if (rc == CF_OK)
        r->state = CF_RPC_UNREGISTERING;
    return rc;
}

static cf_cp_value required_struct(cf_cp_reader *rd, cf_cp_value v)
{
    if (v.kind != CF_CP_STRUCT && rd->error == CF_OK)
        rd->error = CF_ERR_FORMAT;
    return v;
}
static cf_result bootstrap_result(cf_rpc *r, cf_cp_reader *rd, cf_cp_value payload)
{
    cf_cp_value content = cf_cp_field(rd, payload, 0);
    cf_cp_value caps = cf_cp_field(rd, payload, 1), desc;
    uint64_t kind;
    if (content.kind != CF_CP_CAP || caps.kind != CF_CP_LIST || caps.element_size != 7 ||
        caps.count != 1 || content.cap != 0)
        return CF_ERR_UNSUPPORTED;
    desc = cf_cp_at(rd, caps, 0);
    kind = cf_cp_uint(rd, desc, 0, 2);
    if (kind != 1)
        return CF_ERR_UNSUPPORTED; /* senderHosted only. */
    r->capability = (uint32_t)cf_cp_uint(rd, desc, 4, 4);
    if (rd->error != CF_OK)
        return rd->error;
    r->have_capability = true;
    r->state = CF_RPC_READY;
    return CF_OK;
}
static cf_result registration_result(cf_rpc *r, cf_cp_reader *rd, cf_cp_value payload)
{
    cf_cp_value results, response, details;
    cf_bytes id, location;
    uint64_t kind;
    results = required_struct(rd, cf_cp_field(rd, payload, 0));
    response = required_struct(rd, cf_cp_field(rd, results, 0));
    kind = cf_cp_uint(rd, response, 0, 2);
    details = required_struct(rd, cf_cp_field(rd, response, 0));
    if (rd->error != CF_OK)
        return rd->error;
    if (kind == 0)
    {
        uint64_t ns = cf_cp_uint(rd, details, 0, 8);
        /* Validate cause text, but never retain/log a server-supplied secret. */
        (void)cf_cp_data(rd, cf_cp_field(rd, details, 0), true);
        r->should_retry = (cf_cp_uint(rd, details, 8, 1) & 1) != 0;
        /* Go time.Duration is signed nanoseconds. Negative means no delay. */
        r->retry_after_ms = ns >> 63 ? 0 : ns / 1000000 + (ns % 1000000 != 0);
        if (rd->error != CF_OK)
            return rd->error;
        r->state = CF_RPC_REJECTED;
        return CF_OK;
    }
    if (kind != 1)
        return CF_ERR_UNSUPPORTED;
    id = cf_cp_data(rd, cf_cp_field(rd, details, 0), false);
    location = cf_cp_data(rd, cf_cp_field(rd, details, 1), true);
    r->remotely_managed = (cf_cp_uint(rd, details, 0, 1) & 1) != 0;
    if (rd->error != CF_OK)
        return rd->error;
    if (id.len != 16 || !ascii(location, sizeof(r->location) - 1))
        return CF_ERR_FORMAT;
    if (!r->remotely_managed)
        return CF_ERR_UNSUPPORTED;
    memcpy(r->connection_id, id.ptr, 16);
    memcpy(r->location, location.ptr, location.len);
    r->location[location.len] = '\0';
    r->state = CF_RPC_REGISTERED;
    return CF_OK;
}
cf_result cf_rpc_receive(cf_rpc *r, cf_bytes frame, uint8_t *out, size_t cap, size_t *n)
{
    cf_cp_reader rd;
    cf_cp_value root, ret, payload, caps;
    uint64_t kind, result_kind;
    uint32_t expected, answer;
    cf_result rc;
    bool boot;
    if (!output_ok(r, out, cap, n))
        return CF_ERR_ARGUMENT;
    if (r->state == CF_RPC_IDLE || r->state == CF_RPC_FAILED || r->state == CF_RPC_CLOSED ||
        r->state == CF_RPC_REJECTED)
        return CF_ERR_STATE;
    rc = cf_cp_read_init(&rd, frame);
    if (rc != CF_OK)
        return fail(r, rc, true, out, n);
    root = required_struct(&rd, cf_cp_root(&rd));
    kind = cf_cp_uint(&rd, root, 0, 2);
    ret = required_struct(&rd, cf_cp_field(&rd, root, 0));
    if (rd.error != CF_OK)
        return fail(r, rd.error, true, out, n);
    if (kind == 1)
    {
        (void)cf_cp_data(&rd, cf_cp_field(&rd, ret, 0), true);
        return fail(r, rd.error == CF_OK ? CF_ERR_STATE : rd.error, false, out, n);
    }
    if (kind != 3)
        return fail(r, CF_ERR_UNSUPPORTED, true, out, n);
    boot = r->state == CF_RPC_BOOTSTRAPPING;
    if (!boot && r->state != CF_RPC_REGISTERING && r->state != CF_RPC_UNREGISTERING)
        return fail(r, CF_ERR_STATE, true, out, n);
    expected = boot ? 0 : r->state == CF_RPC_REGISTERING ? 1
                                                         : 2;
    answer = (uint32_t)cf_cp_uint(&rd, ret, 0, 4);
    result_kind = cf_cp_uint(&rd, ret, 6, 2);
    if (rd.error != CF_OK)
        return fail(r, rd.error, true, out, n);
    if (answer != expected)
        return fail(r, CF_ERR_STATE, true, out, n);
    if (result_kind == 1)
    { /* RPC exception; don't misclassify as auth rejection. */
        cf_cp_value ex = required_struct(&rd, cf_cp_field(&rd, ret, 0));
        (void)cf_cp_data(&rd, cf_cp_field(&rd, ex, 0), true);
        (void)finish(answer, true, out, CF_RPC_TX_MAX, n);
        r->state = CF_RPC_FAILED;
        return r->error = rd.error == CF_OK ? CF_ERR_STATE : rd.error;
    }
    if (result_kind != 0)
        return fail(r, CF_ERR_UNSUPPORTED, true, out, n);
    payload = required_struct(&rd, cf_cp_field(&rd, ret, 0));
    if (boot)
        rc = bootstrap_result(r, &rd, payload);
    else
    {
        caps = cf_cp_field(&rd, payload, 1);
        if (caps.kind != CF_CP_NULL && (caps.kind != CF_CP_LIST || caps.element_size != 7 || caps.count != 0))
            rc = CF_ERR_UNSUPPORTED;
        else if (expected == 1)
            rc = registration_result(r, &rd, payload);
        else
        {
            cf_cp_value content = cf_cp_field(&rd, payload, 0);
            /* Empty result may be represented by a null or empty struct. */
            rc = content.kind == CF_CP_NULL || content.kind == CF_CP_STRUCT ? CF_OK : CF_ERR_FORMAT;
            r->state = CF_RPC_CLOSED;
        }
    }
    if (rd.error != CF_OK)
        rc = rd.error;
    if (rc != CF_OK)
        return fail(r, rc, true, out, n);
    rc = finish(answer, !boot, out, CF_RPC_TX_MAX, n);
    if (rc == CF_OK && r->state == CF_RPC_CLOSED)
    {
        size_t extra = 0;
        rc = release_cap(r, out + *n, CF_RPC_TX_MAX - *n, &extra);
        *n += extra;
        r->have_capability = false;
    }
    return rc;
}
