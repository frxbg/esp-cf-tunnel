#include "cf_core.h"
#include "cf_headers.h"
#include "cf_credentials.h"
#include "cf_dns.h"
#include "cf_capnp_framing.h"
#include "cf_config.h"
#include "cf_lifecycle.h"
#include "reference_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define BYTES(literal) ((cf_bytes){(const uint8_t *)(literal), sizeof(literal) - 1})
#define ARRAY_BYTES(array) ((cf_bytes){array, sizeof(array)})

static bool zeroed(const void *ptr, size_t len)
{
    const uint8_t *p = ptr;
    size_t i;
    for (i = 0; i < len; ++i) if (p[i]) return false;
    return true;
}

static void test_base64(void)
{
    static const char *plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    static const char *padded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    static const char *bad[] = {"A", "A===", "====", "Zg=", "Zh==", "Zm9=", "Zg==x", "AA-_", "Zg\n=", "Zg==\r\n"};
    char encoded[64]; uint8_t decoded[64]; size_t n, m, i;
    for (i = 0; i < 7; ++i) {
        cf_bytes in = {(const uint8_t *)plain[i], strlen(plain[i])};
        CHECK(cf_base64_encode(in, true, encoded, sizeof(encoded), &n) == CF_OK);
        CHECK(n == strlen(padded[i]) && memcmp(encoded, padded[i], n) == 0);
        CHECK(cf_base64_decode((cf_bytes){(uint8_t *)encoded, n}, true, decoded, sizeof(decoded), &m) == CF_OK);
        CHECK(m == in.len && memcmp(decoded, in.ptr, m) == 0);
        CHECK(cf_base64_encode(in, false, encoded, sizeof(encoded), &n) == CF_OK);
        CHECK(cf_base64_decode((cf_bytes){(uint8_t *)encoded, n}, false, decoded, sizeof(decoded), &m) == CF_OK);
        CHECK(m == in.len && memcmp(decoded, in.ptr, m) == 0);
    }
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        CHECK(cf_base64_decode((cf_bytes){(const uint8_t *)bad[i], strlen(bad[i])}, true,
                               decoded, sizeof(decoded), &n) != CF_OK);
        CHECK(n == 0);
    }
    CHECK(cf_base64_decode(BYTES("Zg=="), false, decoded, sizeof(decoded), &n) == CF_ERR_FORMAT);
    CHECK(cf_base64_decode(BYTES("Zm9v"), true, decoded, 2, &n) == CF_ERR_LIMIT);
    CHECK(cf_base64_encode(BYTES("foo"), true, encoded, 3, &n) == CF_ERR_LIMIT);
}

static void test_headers(void)
{
    cf_header entries[CF_HEADER_COUNT_MAX];
    uint8_t arena[CF_HEADER_DECODED_MAX];
    char wire[CF_HEADER_WIRE_MAX];
    size_t count, n, i;
    const char *bad[] = {"abc", "YQ:Yg:Yw", "YQ==:Yg", ":Yg", "YQ:Cg", "YQ:AA", "Og:YQ", "YTpi:Yw"};
    CHECK(cf_headers_decode(ARRAY_BYTES(gold_headers), entries, CF_HEADER_COUNT_MAX, arena, sizeof(arena), &count) == CF_OK);
    CHECK(count == 5);
    CHECK(entries[0].name.len == 10 && memcmp(entries[0].name.ptr, "Set-Cookie", 10) == 0);
    CHECK(entries[1].name.len == 10 && entries[2].value.len == 0);
    CHECK(cf_headers_encode(entries, count, wire, sizeof(wire), &n) == CF_OK);
    CHECK(n == sizeof(gold_headers) && memcmp(wire, gold_headers, n) == 0);
    CHECK(cf_headers_encode(entries, count, wire, n - 1, &n) == CF_ERR_LIMIT);
    CHECK(cf_headers_decode(ARRAY_BYTES(gold_headers), entries, 4, arena, sizeof(arena), &count) == CF_ERR_LIMIT && count == 0);
    CHECK(cf_headers_decode(ARRAY_BYTES(gold_headers), entries, 32, arena, 2, &count) == CF_ERR_LIMIT && count == 0);
    CHECK(cf_headers_decode(BYTES(";;YQ:;;"), entries, 32, arena, sizeof(arena), &count) == CF_OK && count == 1);
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i)
        CHECK(cf_headers_decode((cf_bytes){(const uint8_t *)bad[i], strlen(bad[i])}, entries, 32,
                                 arena, sizeof(arena), &count) == CF_ERR_FORMAT && count == 0);
    CHECK(cf_header_is_control(BYTES("CF-Cloudflared-Response-Meta")));
    CHECK(cf_header_is_control(BYTES(":status")));
    CHECK(!cf_header_is_control(BYTES("Set-Cookie")));
    memset(wire, ';', sizeof(wire));
    CHECK(cf_headers_decode((cf_bytes){(uint8_t *)wire, sizeof(wire) + 1}, entries, 32,
                             arena, sizeof(arena), &count) == CF_ERR_LIMIT);
}

static cf_result parse_json_token(const char *json, cf_credentials *out, uint8_t *scratch)
{
    char token[CF_TOKEN_MAX]; size_t n;
    CHECK(cf_base64_encode((cf_bytes){(const uint8_t *)json, strlen(json)}, true, token, sizeof(token), &n) == CF_OK);
    return cf_credentials_parse((cf_bytes){(uint8_t *)token, n}, scratch, CF_TOKEN_JSON_MAX + 1, out);
}

static void test_credentials(void)
{
    uint8_t scratch[CF_TOKEN_JSON_MAX + 1];
    cf_credentials out; size_t i;
    static const char good[] = "{\"a\":\"0123456789abcdef0123456789abcdef\",\"s\":\"AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8=\",\"t\":\"00112233-4455-4677-8899-aabbccddeeff\"}";
    char modified[512];
    memset(scratch, 0xa5, sizeof(scratch));
    CHECK(cf_credentials_parse(ARRAY_BYTES(gold_token), scratch, sizeof(scratch), &out) == CF_OK);
    CHECK(zeroed(scratch, sizeof(scratch)));
    CHECK(strcmp(out.account_tag, "0123456789abcdef0123456789abcdef") == 0);
    for (i = 0; i < 32; ++i) CHECK(out.tunnel_secret[i] == i);
    CHECK(out.tunnel_id[0] == 0 && out.tunnel_id[15] == 255);
    CHECK(out.tunnel_secret_len == 32);
    {
        const cf_bytes tokens[] = {ARRAY_BYTES(gold_token_secret_36), ARRAY_BYTES(gold_token_secret_64), ARRAY_BYTES(gold_token_secret_128)};
        const size_t sizes[] = {36, 64, 128};
        cf_token_diagnostic diag;
        size_t j;
        for (j = 0; j < 3; ++j) {
            CHECK(cf_credentials_parse_ex(tokens[j], scratch, sizeof(scratch), &out, &diag) == CF_OK);
            CHECK(out.tunnel_secret_len == sizes[j] && diag == CF_TOKEN_VALID);
            for (i = 0; i < sizes[j]; ++i) CHECK(out.tunnel_secret[i] == i);
            CHECK(zeroed(scratch, sizeof(scratch)));
        }
        CHECK(cf_credentials_parse_ex(ARRAY_BYTES(gold_token_secret_31), scratch, sizeof(scratch), &out, &diag) == CF_ERR_FORMAT);
        CHECK(diag == CF_TOKEN_SECRET_SIZE && zeroed(&out, sizeof(out)));
        CHECK(cf_credentials_parse_ex(ARRAY_BYTES(gold_token_secret_129), scratch, sizeof(scratch), &out, &diag) == CF_ERR_LIMIT);
        CHECK(diag == CF_TOKEN_SECRET_SIZE && zeroed(&out, sizeof(out)));
        CHECK(cf_credentials_parse_ex(BYTES("cloudflared tunnel run --token fake"), scratch, sizeof(scratch), &out, &diag) != CF_OK);
        CHECK(diag == CF_TOKEN_BASE64 && zeroed(&out, sizeof(out)));
    }
    CHECK(parse_json_token("{}", &out, scratch) != CF_OK && zeroed(&out, sizeof(out)));
    CHECK(parse_json_token("{\"a\":null}", &out, scratch) != CF_OK);
    CHECK(parse_json_token("{\"a\":\"\\u0000\"}", &out, scratch) != CF_OK);
    snprintf(modified, sizeof(modified), "%.*s,\"e\":\"evil.example\"}", (int)strlen(good)-1, good);
    CHECK(parse_json_token(modified, &out, scratch) == CF_ERR_UNSUPPORTED);
    snprintf(modified, sizeof(modified), "%.*s,\"a\":\"duplicate\"}", (int)strlen(good)-1, good);
    CHECK(parse_json_token(modified, &out, scratch) != CF_OK);
    snprintf(modified, sizeof(modified), "%s trailing", good);
    CHECK(parse_json_token(modified, &out, scratch) != CF_OK);
    CHECK(parse_json_token(good, &out, scratch) == CF_OK);
    memset(scratch, 0xa5, sizeof(scratch));
    CHECK(cf_credentials_parse(BYTES("Bearer secret"), scratch, sizeof(scratch), &out) != CF_OK);
    CHECK(zeroed(&out, sizeof(out)) && zeroed(scratch, sizeof(scratch)));
}

static unsigned messages;
static cf_result on_frame(void *ctx, cf_bytes frame)
{
    cf_bytes segment;
    (void)ctx;
    CHECK(cf_capnp_segment(frame, 0, &segment) == CF_OK && segment.len == 8);
    if (frame.ptr[0] == 1) {
        CHECK(cf_capnp_segment(frame, 1, &segment) == CF_OK && segment.len == 8);
        CHECK(segment.ptr[0] == 8 && segment.ptr[7] == 1);
    }
    ++messages;
    return CF_OK;
}

static cf_result reject_frame(void *ctx, cf_bytes frame)
{
    (void)ctx; (void)frame;
    return CF_ERR_UNSUPPORTED;
}

static void test_framing(void)
{
    uint8_t buffer[128], joined[sizeof(gold_single_segment) + sizeof(gold_multi_segment)];
    cf_capnp_framer f; size_t i, split, n; cf_bytes seg;
    memcpy(joined, gold_single_segment, sizeof(gold_single_segment));
    memcpy(joined + sizeof(gold_single_segment), gold_multi_segment, sizeof(gold_multi_segment));
    for (split = 0; split <= sizeof(joined); ++split) {
        messages = 0;
        CHECK(cf_capnp_framer_init(&f, buffer, sizeof(buffer)) == CF_OK);
        CHECK(cf_capnp_feed(&f, (cf_bytes){joined, split}, &n, on_frame, NULL) == CF_OK && n == split);
        CHECK(cf_capnp_feed(&f, (cf_bytes){joined + split, sizeof(joined) - split}, &n, on_frame, NULL) == CF_OK);
        CHECK(messages == 2 && cf_capnp_eof(&f) == CF_OK);
    }
    messages = 0;
    for (i = 0; i < sizeof(joined); ++i)
        CHECK(cf_capnp_feed(&f, (cf_bytes){joined + i, 1}, &n, on_frame, NULL) == CF_OK && n == 1);
    CHECK(messages == 2);
    for (i = 1; i < sizeof(gold_multi_segment); ++i) {
        cf_capnp_framer_reset(&f);
        CHECK(cf_capnp_feed(&f, (cf_bytes){gold_multi_segment, i}, &n, on_frame, NULL) == CF_OK);
        CHECK(cf_capnp_eof(&f) == CF_ERR_TRUNCATED);
    }
    cf_capnp_framer_reset(&f);
    CHECK(cf_capnp_feed(&f, BYTES("\xff\xff\xff\xff"), &n, on_frame, NULL) == CF_ERR_LIMIT);
    CHECK(cf_capnp_feed(&f, ARRAY_BYTES(gold_single_segment), &n, on_frame, NULL) == CF_ERR_LIMIT && n == 0);
    cf_capnp_framer_reset(&f);
    CHECK(cf_capnp_feed(&f, BYTES("\0\0\0\0\xff\xff\xff\xff"), &n, on_frame, NULL) == CF_ERR_LIMIT);
    cf_capnp_framer_reset(&f);
    CHECK(cf_capnp_feed(&f, BYTES("\0\0\0\0\0\0\0\0"), &n, on_frame, NULL) == CF_ERR_FORMAT);
    cf_capnp_framer_reset(&f);
    CHECK(cf_capnp_feed(&f, ARRAY_BYTES(gold_single_segment), &n, reject_frame, NULL) == CF_ERR_UNSUPPORTED);
    CHECK(cf_capnp_eof(&f) == CF_ERR_UNSUPPORTED);
    CHECK(cf_capnp_segment(ARRAY_BYTES(gold_single_segment), 1, &seg) == CF_ERR_ARGUMENT);
    CHECK(cf_capnp_segment((cf_bytes){joined, sizeof(joined)}, 0, &seg) == CF_ERR_FORMAT);
}

static const char valid_config[] = "{\"version\":1,\"config\":{\"ingress\":[{\"hostname\":\"device.example.com\",\"service\":\"http://localhost:80\"},{\"service\":\"http_status:404\"}],\"warp-routing\":{\"enabled\":false}}}";
static cf_result apply(cf_remote_config *c, const char *json)
{
    return cf_config_apply(c, (cf_bytes){(const uint8_t *)json, strlen(json)}, "device.example.com", "http://localhost:80");
}

static void test_config(void)
{
    cf_remote_config c, before;
    char changed[512], response[128]; size_t n;
    const char *bad[] = {"{}", "[]", "{\"version\":1}",
        "{\"version\":1,\"version\":2,\"config\":{}}",
        "{\"version\":2147483648,\"config\":{}}", "{\"version\":1.5,\"config\":{}}"};
    size_t i;
    cf_config_init(&c);
    CHECK(!c.valid && c.version == -1);
    CHECK(cf_config_response(&c, CF_ERR_FORMAT, response, sizeof(response), &n) == CF_OK);
    CHECK(strcmp(response, "{\"lastAppliedVersion\":-1,\"err\":{}}") == 0);
    CHECK(apply(&c, valid_config) == CF_OK && c.valid && c.version == 1);
    before = c;
    CHECK(apply(&c, valid_config) == CF_OK && memcmp(&c, &before, sizeof(c)) == 0);
    for (i = 0; i < sizeof(bad)/sizeof(bad[0]); ++i) {
        CHECK(apply(&c, bad[i]) != CF_OK);
        CHECK(memcmp(&c, &before, sizeof(c)) == 0);
    }
    {
        const char *numbers[] = {"01", "1.", "1e+", "-01", "+1", "NaN", "1e999"};
        for (i = 0; i < sizeof(numbers)/sizeof(numbers[0]); ++i) {
            snprintf(changed, sizeof(changed), "{\"version\":%s%s", numbers[i], valid_config + 12);
            CHECK(apply(&c, changed) != CF_OK && memcmp(&c, &before, sizeof(c)) == 0);
        }
    }
    {
        char expensive[8000];
        size_t pos = 0;
        expensive[pos++] = '[';
        for (i = 0; i < 1500; ++i) {
            if (i) expensive[pos++] = ',';
            expensive[pos++] = '0';
        }
        expensive[pos++] = ']'; expensive[pos] = 0;
        CHECK(apply(&c, expensive) == CF_ERR_MEMORY);
        CHECK(memcmp(&c, &before, sizeof(c)) == 0);
        /* Repeated budget exhaustion must release every private allocation. */
        for (i = 0; i < 100; ++i) CHECK(apply(&c, valid_config) == CF_OK);
    }
    memcpy(changed, valid_config, sizeof(valid_config));
    *strstr(changed, "false") = 't';
    CHECK(apply(&c, changed) != CF_OK && memcmp(&c, &before, sizeof(c)) == 0);
    snprintf(changed, sizeof(changed), "%.*s,\"originRequest\":{\"access\":{\"required\":true}}}}", (int)strlen(valid_config)-2, valid_config);
    CHECK(apply(&c, changed) != CF_OK && memcmp(&c, &before, sizeof(c)) == 0);
    CHECK(cf_config_apply(&c, BYTES(valid_config), "other.example.com", "http://localhost:80") != CF_OK);
    CHECK(cf_config_apply(&c, BYTES(valid_config), "device.example.com", "http://localhost:81") != CF_OK);
    CHECK(cf_config_response(&c, CF_OK, response, sizeof(response), &n) == CF_OK);
    CHECK(strcmp(response, "{\"lastAppliedVersion\":1,\"err\":null}") == 0);
    CHECK(cf_config_response(&c, CF_OK, response, 2, &n) == CF_ERR_LIMIT && n == 0);
}

static void to_registration(cf_lifecycle *l)
{
    CHECK(cf_lifecycle_advance(l, CF_EVENT_DISCOVERED) == CF_OK);
    CHECK(cf_lifecycle_advance(l, CF_EVENT_CONNECTED) == CF_OK);
    CHECK(cf_lifecycle_advance(l, CF_EVENT_TLS_READY) == CF_OK);
    CHECK(l->state == CF_HTTP2 && !l->registered);
    CHECK(cf_lifecycle_advance(l, CF_EVENT_HTTP2_READY) == CF_OK);
}

static void test_lifecycle(void)
{
    cf_lifecycle l; unsigned i;
    cf_lifecycle_init(&l);
    CHECK(cf_lifecycle_start(&l) == CF_OK && l.state == CF_WAIT_NETWORK);
    CHECK(cf_lifecycle_start(&l) == CF_ERR_STATE);
    cf_lifecycle_environment(&l, true, false);
    CHECK(l.state == CF_WAIT_TIME);
    cf_lifecycle_environment(&l, true, true);
    CHECK(l.state == CF_DISCOVERY);
    CHECK(cf_lifecycle_advance(&l, CF_EVENT_REGISTERED) == CF_ERR_STATE);
    to_registration(&l);
    CHECK(cf_lifecycle_advance(&l, CF_EVENT_REGISTERED) == CF_OK && l.state == CF_WAIT_CONFIG);
    CHECK(cf_lifecycle_advance(&l, CF_EVENT_CONFIG_APPLIED) == CF_OK && l.state == CF_ONLINE);
    CHECK(cf_lifecycle_advance(&l, CF_EVENT_CONFIG_REJECTED) == CF_OK && l.state == CF_ONLINE);
    CHECK(cf_lifecycle_retry(&l, 1000, 20000, 123) == CF_OK && l.retry_at_ms >= 21000);
    cf_lifecycle_environment(&l, false, false);
    cf_lifecycle_environment(&l, true, true);
    cf_lifecycle_tick(&l, 20999);
    CHECK(l.state == CF_BACKOFF);
    cf_lifecycle_tick(&l, 21000);
    CHECK(l.state == CF_DISCOVERY);
    to_registration(&l);
    CHECK(cf_lifecycle_advance(&l, CF_EVENT_AUTH_REJECTED) == CF_OK);
    CHECK(cf_lifecycle_retry(&l, 0, 0, 0) == CF_ERR_STATE);
    cf_lifecycle_environment(&l, false, false);
    cf_lifecycle_environment(&l, true, true);
    cf_lifecycle_tick(&l, UINT64_MAX);
    CHECK(l.state == CF_AUTH_FAILED);
    cf_lifecycle_stop(&l);CHECK(cf_lifecycle_start(&l)==CF_OK);to_registration(&l);
    CHECK(cf_lifecycle_advance(&l,CF_EVENT_CONFIG_REJECTED)==CF_OK && l.state==CF_CONFIG_FAILED);
    CHECK(cf_lifecycle_advance(&l,CF_EVENT_REGISTERED)==CF_OK && l.registered && l.state==CF_CONFIG_FAILED);
    CHECK(cf_lifecycle_advance(&l,CF_EVENT_CONFIG_APPLIED)==CF_OK && l.state==CF_ONLINE);
    CHECK(cf_lifecycle_advance(&l,CF_EVENT_CONFIG_REJECTED)==CF_OK && l.state==CF_ONLINE);
    for (i = 0; i < 100; ++i) {
        cf_lifecycle_stop(&l);
        CHECK(cf_lifecycle_start(&l) == CF_OK);
        to_registration(&l);
        CHECK(cf_lifecycle_advance(&l, CF_EVENT_CONFIG_APPLIED) == CF_OK && l.state == CF_REGISTERING);
        CHECK(cf_lifecycle_advance(&l, CF_EVENT_REGISTERED) == CF_OK && l.state == CF_ONLINE);
        CHECK(cf_lifecycle_retry(&l, UINT64_MAX - 1, UINT32_MAX, UINT32_MAX) == CF_OK);
        CHECK(l.retry_at_ms == UINT64_MAX);
    }
}

static void test_dns(void)
{
    uint8_t bytes[CF_DNS_PACKET_MAX];cf_dns_answer answer;size_t n,i,j;unsigned bit;
    CHECK(cf_dns_query(CF_EDGE_DISCOVERY,CF_DNS_SRV,0x1234,bytes,sizeof(bytes),&n)==CF_OK);
    CHECK(n==sizeof(gold_dns_query) && !memcmp(bytes,gold_dns_query,n));
    CHECK(cf_dns_parse(ARRAY_BYTES(gold_dns_srv),CF_EDGE_DISCOVERY,CF_DNS_SRV,0x1234,&answer)==CF_OK);
    CHECK(answer.count==2 && answer.records[0].port==7844 && answer.records[0].priority==1);
    CHECK(!strcmp(answer.records[1].target,"region2.v2.argotunnel.com"));
    CHECK(cf_dns_parse(ARRAY_BYTES(gold_dns_a),"region1.v2.argotunnel.com",CF_DNS_A,0x1234,&answer)==CF_OK);
    CHECK(answer.count==2 && answer.records[1].ip[0]==198 && answer.records[0].ttl==300);
    CHECK(!cf_dns_edge_target("argotunnel.com.evil.example") && !cf_dns_edge_target("argotunnel.com"));
    CHECK(cf_dns_parse(ARRAY_BYTES(gold_dns_srv),CF_EDGE_DISCOVERY,CF_DNS_SRV,1,&answer)!=CF_OK && answer.count==0);
    CHECK(cf_dns_parse(ARRAY_BYTES(gold_dns_srv),"evil.example",CF_DNS_SRV,0x1234,&answer)!=CF_OK && answer.count==0);
    for(i=0;i<sizeof(gold_dns_srv);++i)CHECK(cf_dns_parse((cf_bytes){gold_dns_srv,i},CF_EDGE_DISCOVERY,CF_DNS_SRV,0x1234,&answer)!=CF_OK && answer.count==0);
    memcpy(bytes,gold_dns_srv,sizeof(gold_dns_srv));bytes[2]|=2;
    CHECK(cf_dns_parse((cf_bytes){bytes,sizeof(gold_dns_srv)},CF_EDGE_DISCOVERY,CF_DNS_SRV,0x1234,&answer)==CF_ERR_TRUNCATED && answer.count==0);
    memcpy(bytes,gold_dns_srv,sizeof(gold_dns_srv));bytes[12]=0xc0;bytes[13]=12;
    CHECK(cf_dns_parse((cf_bytes){bytes,sizeof(gold_dns_srv)},CF_EDGE_DISCOVERY,CF_DNS_SRV,0x1234,&answer)!=CF_OK);
    for(j=0;j<2;++j){
        cf_bytes seed=j?ARRAY_BYTES(gold_dns_a):ARRAY_BYTES(gold_dns_srv);
        for(i=0;i<seed.len;++i)for(bit=0;bit<8;++bit){
            memcpy(bytes,seed.ptr,seed.len);bytes[i]^=(uint8_t)(1u<<bit);
            cf_result rc=cf_dns_parse((cf_bytes){bytes,seed.len},j?"region1.v2.argotunnel.com":CF_EDGE_DISCOVERY,j?CF_DNS_A:CF_DNS_SRV,0x1234,&answer);
            CHECK(answer.count<=CF_DNS_RECORD_MAX && (rc==CF_OK || answer.count==0));
        }
    }
}

int main(int argc, char **argv)
{
    const char *suite = argc > 1 ? argv[1] : "all";
    unsigned run = 0;
#define RUN(name) do { if (strcmp(suite, #name) == 0 || strcmp(suite, "all") == 0) { test_##name(); ++run; } } while (0)
    RUN(base64); RUN(headers); RUN(credentials); RUN(framing); RUN(config); RUN(lifecycle); RUN(dns);
    CHECK(run != 0);
    printf("PASS: %s (%u checks)\n", suite, checks);
    return 0;
}
