#include "monitor_remote.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static bool equal(cf_bytes b,const char *s) { return b.len==strlen(s) && !memcmp(b.ptr,s,b.len); }
static bool name_is(cf_bytes b,const char *s)
{
    if(b.len!=strlen(s)) return false;
    for(size_t i=0;i<b.len;++i) {
        unsigned c=b.ptr[i]; if(c>='A' && c<='Z') c+=32;
        if(c!=(unsigned char)s[i]) return false;
    }
    return true;
}
static cf_result copy(cf_bytes b,char *out,size_t cap)
{
    if(b.len>=cap || (b.len && memchr(b.ptr,0,b.len))) return CF_ERR_LIMIT;
    if(b.len) memcpy(out,b.ptr,b.len);
    out[b.len]=0; return CF_OK;
}
typedef struct { unsigned seen; char origin[272], type[64]; monitor_remote_head *out; } fields;
static cf_result field(fields *f,cf_header h)
{
    unsigned bit=0;
    if(name_is(h.name,"authorization")) bit=1;
    else if(name_is(h.name,"origin")) bit=2;
    else if(name_is(h.name,"content-type")) bit=4;
    else if(name_is(h.name,"content-length")) bit=8;
    else if(name_is(h.name,"transfer-encoding") ||
            (name_is(h.name,"content-encoding") && !equal(h.value,"identity")) ||
            (name_is(h.name,"cf-cloudflared-proxy-src") && h.value.len)) return CF_ERR_UNSUPPORTED;
    if(!bit) return CF_OK;
    if(f->seen & bit) return CF_ERR_FORMAT;
    f->seen |= bit;
    if(bit==1) return copy(h.value,f->out->authorization,sizeof(f->out->authorization));
    if(bit==2) return copy(h.value,f->origin,sizeof(f->origin));
    if(bit==4) return copy(h.value,f->type,sizeof(f->type));
    size_t n=0;
    if(!h.value.len) return CF_ERR_FORMAT;
    for(size_t i=0;i<h.value.len;++i) {
        unsigned c=h.value.ptr[i];
        if(c<'0' || c>'9') return CF_ERR_FORMAT;
        n=n*10+c-'0'; if(n>MONITOR_BODY_MAX) return CF_ERR_LIMIT;
    }
    f->out->has_length=true; f->out->content_length=n; return CF_OK;
}
cf_result monitor_remote_parse(const cf_h2_request *r,monitor_remote_head *out)
{
    if(!r || !out) return CF_ERR_ARGUMENT;
    memset(out,0,sizeof(*out));
    fields f={.out=out}; cf_result rc=CF_OK;
    cf_bytes method=cf_h2_header(r,":method"), path=cf_h2_header(r,":path"), authority=cf_h2_header(r,":authority");
    out->post=equal(method,"POST"); out->head=equal(method,"HEAD");
    if(!out->post && !out->head && !equal(method,"GET")) return CF_ERR_UNSUPPORTED;
    if(!path.len || path.ptr[0]!='/' || !authority.len || authority.len>253) return CF_ERR_FORMAT;
    const uint8_t *q=memchr(path.ptr,'?',path.len); if(q) path.len=(size_t)(q-path.ptr);
    if((rc=copy(path,out->path,sizeof(out->path)))!=CF_OK) return rc;
    cf_bytes packed={0}; bool has_packed=false;
    for(size_t i=0;i<r->count && rc==CF_OK;++i) {
        if(name_is(r->headers[i].name,"cf-cloudflared-request-headers")) {
            if(has_packed) {rc=CF_ERR_FORMAT;break;}
            packed=r->headers[i].value;has_packed=true;
        } else rc=field(&f,r->headers[i]);
    }
    if(rc==CF_OK && has_packed) {
        struct { uint8_t arena[CF_HEADER_DECODED_MAX]; cf_header items[CF_HEADER_COUNT_MAX]; } *decoded=malloc(sizeof(*decoded));
        if(!decoded) rc=CF_ERR_MEMORY;
        else {
            size_t count=0;
            rc=cf_headers_decode(packed,decoded->items,CF_HEADER_COUNT_MAX,decoded->arena,sizeof(decoded->arena),&count);
            for(size_t i=0;i<count && rc==CF_OK;++i) rc=field(&f,decoded->items[i]);
            cf_secure_zero(decoded,sizeof(*decoded));free(decoded);
        }
    }
    if(rc==CF_OK) {
        char expected[272];
        int n=snprintf(expected,sizeof(expected),"https://%.*s",(int)authority.len,(const char *)authority.ptr);
        out->origin_allowed=!(f.seen&2) || (n>0 && (size_t)n<sizeof(expected) && !strcmp(f.origin,expected));
        if(!out->origin_allowed && n>0 && (size_t)n+4<sizeof(expected)) {
            strcat(expected,":443"); out->origin_allowed=!strcmp(f.origin,expected);
        }
        out->json_content=!strncmp(f.type,"application/json",16) && (!f.type[16] || f.type[16]==';');
        if(!out->post && out->content_length) rc=CF_ERR_FORMAT;
    }
    cf_secure_zero(&f,sizeof(f));
    if(rc!=CF_OK) cf_secure_zero(out,sizeof(*out));
    return rc;
}
