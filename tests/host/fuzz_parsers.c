#include "cf_headers.h"
#include "cf_credentials.h"
#include "cf_capnp_framing.h"
#include "cf_config.h"
#include "cf_rpc.h"
#include "cf_dns.h"
#include <stdint.h>

static cf_result frame(void *context, cf_bytes data)
{
    cf_bytes segment;
    uint32_t i;
    cf_cp_reader rd;
    cf_cp_value root, child;
    cf_rpc rpc;
    uint8_t tx[CF_RPC_TX_MAX];
    size_t written;
    (void)context;
    for (i = 0; i < CF_CAPNP_MAX_SEGMENTS; ++i) (void)cf_capnp_segment(data, i, &segment);
    if (cf_cp_read_init(&rd, data) == CF_OK) {
        root = cf_cp_root(&rd);
        for (i = 0; i < CF_CAPNP_DEPTH_MAX && rd.error == CF_OK; ++i) {
            if (root.kind == CF_CP_STRUCT) {
                (void)cf_cp_uint(&rd, root, 0, 8);
                child = cf_cp_field(&rd, root, 0);
            } else if (root.kind == CF_CP_LIST && root.count && (root.element_size == 6 || root.element_size == 7)) {
                child = cf_cp_at(&rd, root, root.count - 1);
            } else { (void)cf_cp_data(&rd, root, true); break; }
            root = child;
        }
    }
    cf_rpc_init(&rpc);
    rpc.state = CF_RPC_BOOTSTRAPPING;
    (void)cf_rpc_receive(&rpc, data, tx, sizeof(tx), &written);
    cf_rpc_init(&rpc);
    rpc.state = CF_RPC_REGISTERING;
    (void)cf_rpc_receive(&rpc, data, tx, sizeof(tx), &written);
    return CF_OK;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uint8_t arena[CF_HEADER_DECODED_MAX], scratch[CF_TOKEN_JSON_MAX + 1], rpc[CF_CAPNP_MESSAGE_MAX];
    cf_header headers[CF_HEADER_COUNT_MAX];
    cf_credentials credentials;
    cf_remote_config config;
    cf_dns_answer dns;
    cf_capnp_framer f;
    size_t n, used = 0;
    cf_bytes input = {data, size};
    (void)cf_dns_parse(input, CF_EDGE_DISCOVERY, CF_DNS_SRV, 0x1234, &dns);
    (void)cf_dns_parse(input, "region1.v2.argotunnel.com", CF_DNS_A, 0x1234, &dns);
    (void)cf_headers_decode(input, headers, CF_HEADER_COUNT_MAX, arena, sizeof(arena), &n);
    (void)cf_credentials_parse(input, scratch, sizeof(scratch), &credentials);
    cf_config_init(&config);
    (void)cf_config_apply(&config, input, "device.example.com", "http://localhost:80");
    (void)cf_capnp_framer_init(&f, rpc, sizeof(rpc));
    while (used < size) {
        size_t chunk = 1u + data[used] % 64u;
        if (chunk > size - used) chunk = size - used;
        if (cf_capnp_feed(&f, (cf_bytes){data + used, chunk}, &n, frame, NULL) != CF_OK) break;
        used += n;
    }
    (void)cf_capnp_eof(&f);
    return 0;
}
