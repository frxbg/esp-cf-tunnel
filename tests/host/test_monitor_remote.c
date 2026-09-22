#include "monitor_remote.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define B(s) (cf_bytes){(const uint8_t *)(s),strlen(s)}
#define H(n,v) (cf_header){B(n),B(v)}
#define CHECK(v) do { ++checks; if(!(v)) {fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#v);exit(1);} } while(0)
static unsigned checks;
int main(void)
{
    cf_header headers[16]={H(":method","POST"),H(":path","/api/login"),H(":authority","device.example.com"),
        H("origin","https://device.example.com"),H("content-type","application/json"),H("content-length","24")};
    cf_h2_request r={.headers=headers,.count=6};monitor_remote_head out;
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && out.post && out.origin_allowed && out.json_content && out.content_length==24);
    CHECK(!strcmp(out.path,"/api/login"));
    headers[3]=H("origin","https://attacker.example");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && !out.origin_allowed);
    headers[3]=H("origin","https://device.example.com.attacker.example");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && !out.origin_allowed);
    headers[3]=H("origin","http://device.example.com");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && !out.origin_allowed);
    headers[3]=H("origin","https://device.example.com:443");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && out.origin_allowed);
    headers[3]=H("origin","null");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && !out.origin_allowed);
    headers[3]=H("authorization","Bearer synthetic-session");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && out.origin_allowed && !strcmp(out.authorization,"Bearer synthetic-session"));
    headers[6]=H("Authorization","Bearer second-session");r.count=7;
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_FORMAT && !out.authorization[0]);
    r.count=6;
    const char *bad_lengths[]={"-1","2x","","999999999999999999999999","2049","2,2"};
    for(size_t i=0;i<sizeof(bad_lengths)/sizeof(*bad_lengths);++i) {
        headers[5]=H("content-length",bad_lengths[i]);
        CHECK(monitor_remote_parse(&r,&out)!=CF_OK && !out.authorization[0]);
    }
    headers[5]=H("content-length","2048");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && out.content_length==2048);
    headers[0]=H(":method","GET");
    CHECK(monitor_remote_parse(&r,&out)!=CF_OK);
    headers[5]=H("content-length","0");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && !out.post);
    headers[0]=H(":method","HEAD");
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && out.head);
    headers[0]=H(":method","CONNECT");
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_UNSUPPORTED);
    headers[0]=H(":method","POST");
    headers[6]=H("transfer-encoding","chunked");r.count=7;
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_UNSUPPORTED);
    headers[6]=H("cf-cloudflared-proxy-src","tcp");
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_UNSUPPORTED);
    headers[6]=H("content-encoding","gzip");
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_UNSUPPORTED);
    r.count=3; /* Content-Length absent: bounded HTTP/2 body accepted until END_STREAM. */
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && !out.has_length);
    cf_header user[]={H("Authorization","Bearer synthetic-packed"),H("Origin","https://device.example.com"),H("Content-Type","application/json; charset=utf-8")};
    char wire[1024];size_t n;
    CHECK(cf_headers_encode(user,3,wire,sizeof(wire),&n)==CF_OK);
    headers[3]=(cf_header){B(CF_REQUEST_HEADERS),(cf_bytes){(const uint8_t *)wire,n}};r.count=4;
    CHECK(monitor_remote_parse(&r,&out)==CF_OK && out.origin_allowed && out.json_content && !strcmp(out.authorization,"Bearer synthetic-packed"));
    headers[4]=H("authorization","Bearer duplicate");r.count=5;
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_FORMAT && !out.authorization[0]);
    r.count=4;headers[3]=H(CF_REQUEST_HEADERS,"invalid-base64");
    CHECK(monitor_remote_parse(&r,&out)!=CF_OK);
    r.count=3;char long_path[160];memset(long_path,'a',sizeof(long_path));long_path[0]='/';long_path[159]=0;headers[1]=H(":path",long_path);
    CHECK(monitor_remote_parse(&r,&out)==CF_ERR_LIMIT);
    puts("PASS: remote request origin, duplicate/serialized auth headers, methods and body bounds");
    printf("%u checks\n",checks);return 0;
}
