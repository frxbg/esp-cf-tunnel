#include "cf_h2.h"
#include <nghttp2/nghttp2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRANSFER (10u * 1024u * 1024u)
#define B(s) ((cf_bytes){(const uint8_t *)(s),sizeof(s)-1})
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) {fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);} } while (0)
typedef struct {int32_t id;size_t sent,received;bool hold,sink;cf_h2_kind kind;} app_stream;
typedef struct {
    cf_h2 *h;nghttp2_session *peer;app_stream streams[6];
    unsigned requests,closed,resets,cookies,origin_meta,connector_meta;
    size_t upload,download;int32_t large_id;bool reject_body;
} fixture;
static bool eq(cf_bytes b,const char *s){return b.len==strlen(s) && !memcmp(b.ptr,s,b.len);}
static app_stream *slot(fixture *f,int32_t id){unsigned i;for(i=0;i<6;++i)if(f->streams[i].id==id)return &f->streams[i];return NULL;}
static void random_test(uint8_t *out,size_t size){/* Synthetic tests only, never a firmware RNG. */memset(out,0x73,size);}
static cf_result request(void *ctx,const cf_h2_request *r)
{
    fixture *f=ctx;unsigned i;app_stream *s;cf_header headers[4];size_t count=3;
    const cf_bytes path=cf_h2_header(r,":path");
    for(i=0;i<6;++i)if(!f->streams[i].id)break;
    CHECK(i<6);s=&f->streams[i];s->id=r->stream;s->kind=r->kind;
    s->hold=eq(path,"/hold");s->sink=eq(path,"/sink");++f->requests;
    headers[0]=(cf_header){B("content-type"),B("application/octet-stream")};
    headers[1]=(cf_header){B("Set-Cookie"),B("a=1; Path=/")};
    headers[2]=(cf_header){B("Set-Cookie"),B("b=2")};
    if(eq(path,"/large")){headers[3]=(cf_header){B("content-length"),B("10485760")};count=4;f->large_id=r->stream;}
    return cf_h2_respond(f->h,r->stream,200,headers,count,r->kind==CF_H2_HTTP);
}
static cf_result body(void *ctx,int32_t id,cf_bytes data,bool end)
{
    fixture *f=ctx;app_stream *s=slot(f,id);size_t i;
    CHECK(s!=NULL);CHECK(data.len<=CF_H2_CHUNK);
    if(f->reject_body && data.len)return CF_ERR_LIMIT;
    if(s->sink){for(i=0;i<data.len;++i)CHECK(data.ptr[i]==(uint8_t)((s->received+i)%251));s->received+=data.len;}
    if(end && s->sink){CHECK(s->received==TRANSFER);CHECK(cf_h2_resume(f->h,id)==CF_OK);}
    return CF_OK;
}
static cf_result response(void *ctx,int32_t id,uint8_t *buffer,size_t cap,size_t *written,bool *eof)
{
    fixture *f=ctx;app_stream *s=slot(f,id);size_t total,i;
    CHECK(s!=NULL);CHECK(cap<=CF_H2_CHUNK);
    if(s->hold || (s->sink && s->received<TRANSFER))return CF_ERR_STATE;
    total=id==f->large_id?TRANSFER:2;
    *written=total-s->sent;if(*written>cap)*written=cap;
    for(i=0;i<*written;++i)buffer[i]=id==f->large_id?(uint8_t)((s->sent+i)%251):(uint8_t)"OK"[s->sent+i];
    s->sent+=*written;*eof=s->sent==total;return CF_OK;
}
static void closed(void *ctx,int32_t id,uint32_t error)
{
    fixture *f=ctx;app_stream *s=slot(f,id);
    if(s)memset(s,0,sizeof(*s));
    ++f->closed;if(error)++f->resets;
}
static int peer_header(nghttp2_session *session,const nghttp2_frame *frame,const uint8_t *name,size_t nn,
                       const uint8_t *value,size_t vn,uint8_t flags,void *ctx)
{
    fixture *f=ctx;cf_bytes n={name,nn},v={value,vn};(void)session;(void)frame;(void)flags;
    if(eq(n,CF_RESPONSE_HEADERS)){
        cf_header decoded[CF_HEADER_COUNT_MAX];uint8_t arena[CF_HEADER_DECODED_MAX];size_t count,i;
        CHECK(cf_headers_decode(v,decoded,CF_HEADER_COUNT_MAX,arena,sizeof(arena),&count)==CF_OK);
        for(i=0;i<count;++i)if(eq(decoded[i].name,"Set-Cookie"))++f->cookies;
    }
    if(eq(n,CF_RESPONSE_META)){if(eq(v,CF_RESPONSE_META_ORIGIN))++f->origin_meta;else if(eq(v,CF_RESPONSE_META_CONNECTOR))++f->connector_meta;else CHECK(false);}
    return 0;
}
static int peer_data(nghttp2_session *session,uint8_t flags,int32_t id,const uint8_t *data,size_t len,void *ctx)
{
    fixture *f=ctx;size_t i;(void)session;(void)flags;
    if(id==f->large_id){for(i=0;i<len;++i)CHECK(data[i]==(uint8_t)((f->download+i)%251));f->download+=len;}
    return 0;
}
static void init(fixture *f,unsigned apps)
{
    nghttp2_session_callbacks *callbacks;
    cf_h2_callbacks cb={f,request,body,response,closed,random_test};
    memset(f,0,sizeof(*f));CHECK(cf_h2_create(&f->h,&cb,apps,CF_H2_NGHEAP_MAX)==CF_OK);
    CHECK(nghttp2_session_callbacks_new(&callbacks)==0);
    nghttp2_session_callbacks_set_on_header_callback(callbacks,peer_header);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,peer_data);
    CHECK(nghttp2_session_client_new(&f->peer,callbacks,f)==0);nghttp2_session_callbacks_del(callbacks);
    CHECK(nghttp2_submit_settings(f->peer,NGHTTP2_FLAG_NONE,NULL,0)==0);
}
static void destroy(fixture *f){nghttp2_session_del(f->peer);cf_h2_destroy(f->h);}
static void exchange(fixture *f,size_t fragment)
{
    unsigned rounds;
    for(rounds=0;rounds<50000;++rounds){
        const uint8_t *p;nghttp2_ssize n=nghttp2_session_mem_send2(f->peer,&p);size_t offset=0;
        cf_bytes out;bool progress=n>0;
        CHECK(n>=0);
        while(offset<(size_t)n){size_t chunk=(size_t)n-offset,used;if(chunk>fragment)chunk=fragment;
            CHECK(cf_h2_receive(f->h,(cf_bytes){p+offset,chunk},&used,1000)==CF_OK && used==chunk);offset+=used;}
        CHECK(cf_h2_output(f->h,&out)==CF_OK);
        if(out.len){
            size_t chunk=out.len;if(chunk>fragment)chunk=fragment;
            CHECK(!(out.len>=24 && !memcmp(out.ptr,NGHTTP2_CLIENT_MAGIC,24)));
            CHECK(nghttp2_session_mem_recv2(f->peer,out.ptr,chunk)==(nghttp2_ssize)chunk);
            CHECK(cf_h2_sent(f->h,chunk)==CF_OK);progress=true;
        }
        if(!progress)return;
    }
    CHECK(!"exchange did not quiesce");
}
static nghttp2_nv nv(const char *name,const char *value){return (nghttp2_nv){(uint8_t *)name,(uint8_t *)value,strlen(name),strlen(value),NGHTTP2_NV_FLAG_NONE};}
static nghttp2_ssize upload(nghttp2_session *session,int32_t id,uint8_t *out,size_t cap,uint32_t *flags,nghttp2_data_source *src,void *ctx)
{
    fixture *f=ctx;size_t n=TRANSFER-f->upload,i;(void)session;(void)id;(void)src;
    if(n>cap)n=cap;
    for(i=0;i<n;++i)out[i]=(uint8_t)((f->upload+i)%251);
    f->upload+=n;if(f->upload==TRANSFER)*flags|=NGHTTP2_DATA_FLAG_EOF;return (nghttp2_ssize)n;
}
static int32_t submit(fixture *f,const char *path,const char *upgrade,bool with_body)
{
    nghttp2_nv headers[]={nv(":method",with_body?"POST":"GET"),nv(":scheme","https"),nv(":authority","device.example.com"),nv(":path",path),nv("cf-cloudflared-proxy-connection-upgrade",upgrade?upgrade:"")};
    nghttp2_data_provider2 provider={0};int32_t id;provider.read_callback=upload;
    id=nghttp2_submit_request2(f->peer,NULL,headers,upgrade?5:4,with_body?&provider:NULL,NULL);CHECK(id>0);return id;
}
static void test_streams(void)
{
    fixture f;cf_h2_stats stats;int32_t h1,h2,control,config;unsigned requests;
    init(&f,2);exchange(&f,1);
    cf_h2_get_stats(f.h,&stats);CHECK(stats.peer_settings && stats.ngheap_current<=stats.ngheap_limit);
    (void)submit(&f,"/hello",NULL,false);exchange(&f,1);
    CHECK(f.requests==1 && f.cookies==2 && f.origin_meta==1 && f.closed==1);
    h1=submit(&f,"/hold",NULL,false);h2=submit(&f,"/hold",NULL,false);exchange(&f,17);
    (void)submit(&f,"/hold",NULL,false);exchange(&f,17);CHECK(f.requests==3);
    control=submit(&f,"/hold","control-stream",false);exchange(&f,17);
    (void)submit(&f,"/hold","control-stream",false);exchange(&f,17);CHECK(f.requests==4);
    config=submit(&f,"/hold","update-configuration",false);exchange(&f,17);
    CHECK(f.requests==5 && f.connector_meta==2);
    cf_h2_get_stats(f.h,&stats);CHECK(stats.active_streams==4 && stats.rejected_streams>=2);
    CHECK(nghttp2_submit_rst_stream(f.peer,NGHTTP2_FLAG_NONE,h1,NGHTTP2_CANCEL)==0);
    CHECK(nghttp2_submit_rst_stream(f.peer,NGHTTP2_FLAG_NONE,h2,NGHTTP2_CANCEL)==0);
    CHECK(nghttp2_submit_rst_stream(f.peer,NGHTTP2_FLAG_NONE,config,NGHTTP2_CANCEL)==0);exchange(&f,19);
    cf_h2_tick(f.h,20000);exchange(&f,19);CHECK(slot(&f,control)!=NULL); /* Idle control is preserved. */
    requests=f.requests;(void)submit(&f,"/hold","websocket",false);exchange(&f,19);CHECK(f.requests==requests);
    CHECK(nghttp2_submit_rst_stream(f.peer,NGHTTP2_FLAG_NONE,control,NGHTTP2_CANCEL)==0);exchange(&f,19);
    (void)submit(&f,"/hold",NULL,false);exchange(&f,19);cf_h2_tick(f.h,20000);exchange(&f,19);
    cf_h2_get_stats(f.h,&stats);CHECK(stats.active_streams==0);
    CHECK(cf_h2_shutdown(f.h)==CF_OK);exchange(&f,19);cf_h2_get_stats(f.h,&stats);CHECK(stats.goaway);
    printf("H2 engine=%zu bytes, nghttp2 peak=%zu/%zu bytes\n",stats.engine_bytes,stats.ngheap_peak,stats.ngheap_limit);
    destroy(&f);
}
static void test_large(void)
{
    fixture f;cf_h2_stats stats;init(&f,4);exchange(&f,4096);
    (void)submit(&f,"/large",NULL,false);exchange(&f,4096);CHECK(f.download==TRANSFER);
    (void)submit(&f,"/sink",NULL,true);exchange(&f,4096);CHECK(f.upload==TRANSFER);
    CHECK(f.closed==2);cf_h2_get_stats(f.h,&stats);CHECK(stats.ngheap_peak<=CF_H2_NGHEAP_MAX);
    destroy(&f);
}
static void test_limits(void)
{
    fixture f;cf_h2_stats stats;nghttp2_nv headers[5];char big[9000];cf_h2 *h;cf_h2_callbacks cb={&f,request,body,response,closed,random_test};unsigned i;
    init(&f,2);exchange(&f,4096);memset(big,'x',sizeof(big)-1);big[sizeof(big)-1]=0;
    headers[0]=nv(":method","GET");headers[1]=nv(":scheme","https");headers[2]=nv(":authority","device.example.com");headers[3]=nv(":path","/hello");headers[4]=nv("x-large",big);
    CHECK(nghttp2_submit_request2(f.peer,NULL,headers,5,NULL,NULL)>0);exchange(&f,29);
    CHECK(f.requests==0);cf_h2_get_stats(f.h,&stats);CHECK(stats.rejected_streams==1 && stats.ngheap_peak<=CF_H2_NGHEAP_MAX);
    (void)submit(&f,"/hello",NULL,false);exchange(&f,29);CHECK(f.requests==1);
    destroy(&f);
    CHECK(cf_h2_create(&h,&cb,2,8192)==CF_ERR_MEMORY && h==NULL);
    for(i=0;i<100;++i){init(&f,2);exchange(&f,37);destroy(&f);}
}
static void test_refresh_burst(void)
{
    fixture f; cf_h2_stats stats; unsigned round, i;
    init(&f,4); exchange(&f,4096);
    CHECK(nghttp2_session_get_remote_settings(f.peer,NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)==UINT32_MAX);
    int32_t control=submit(&f,"/hold","control-stream",false); exchange(&f,4096);
    for(i=0;i<4;++i)(void)submit(&f,"/hello",NULL,false);
    /* Browser assets reach the connector together. Let the peer use every
     * advertised slot before the connector starts draining its responses. */
    for(round=0;round<500;++round){
        const uint8_t *p; nghttp2_ssize n; cf_bytes out; bool progress=false;
        while((n=nghttp2_session_mem_send2(f.peer,&p))>0){
            size_t used; CHECK(cf_h2_receive(f.h,(cf_bytes){p,(size_t)n},&used,1000)==CF_OK && used==(size_t)n); progress=true;
        }
        CHECK(n==0); CHECK(cf_h2_output(f.h,&out)==CF_OK);
        if(out.len){CHECK(nghttp2_session_mem_recv2(f.peer,out.ptr,out.len)==(nghttp2_ssize)out.len);CHECK(cf_h2_sent(f.h,out.len)==CF_OK);progress=true;}
        if(!progress)break;
    }
    cf_h2_get_stats(f.h,&stats);
    CHECK(f.requests==5); CHECK(f.closed==4 && f.resets==0);
    CHECK(stats.rejected_streams==0 && !stats.goaway && slot(&f,control)!=NULL);
    destroy(&f);
}
static void test_goaway_diagnostic(void)
{
    fixture f; cf_h2_stats stats; size_t used;
    const uint8_t bad_ping[17]={0,0,8,NGHTTP2_PING,0,0,0,0,1}; /* PING must use stream 0. */
    init(&f,2);exchange(&f,4096);
    CHECK(cf_h2_receive(f.h,(cf_bytes){bad_ping,sizeof(bad_ping)},&used,1000)==CF_OK);
    exchange(&f,4096); cf_h2_get_stats(f.h,&stats);
    CHECK(stats.goaway && stats.local_goaway && stats.goaway_error==NGHTTP2_PROTOCOL_ERROR);
    CHECK(stats.last_error==NGHTTP2_ERR_PROTO && !strcmp(stats.local_goaway_reason,"PING: stream_id != 0"));
    destroy(&f);
    /* Actual edge failure captured during the low wire-limit experiment:
     * Go sends a graceful GOAWAY with an odd client stream ID. Preserve the
     * peer's original NO_ERROR/id even when nghttp2 rejects its parity. */
    const uint8_t odd_goaway[17]={0,0,8,NGHTTP2_GOAWAY,0,0,0,0,0,0,0,0,17,0,0,0,0};
    init(&f,2);exchange(&f,4096);
    CHECK(cf_h2_receive(f.h,(cf_bytes){odd_goaway,sizeof(odd_goaway)},&used,1000)==CF_OK);
    exchange(&f,4096);cf_h2_get_stats(f.h,&stats);
    CHECK(stats.local_goaway && stats.goaway_error==NGHTTP2_PROTOCOL_ERROR);
    CHECK(stats.peer_goaway_error==0 && stats.peer_goaway_last_stream==17);
    CHECK(!strcmp(stats.local_goaway_reason,"GOAWAY: invalid last_stream_id"));
    destroy(&f);
}
static void test_bounded_overload(void)
{
    fixture f; cf_h2_stats stats; int32_t app[4]; unsigned i; const uint8_t *p; nghttp2_ssize n;
    init(&f,4);exchange(&f,4096);
    int32_t control=submit(&f,"/hold","control-stream",false);exchange(&f,4096);
    for(i=0;i<4;++i)app[i]=submit(&f,"/hold",NULL,false);
    for(i=0;i<12;++i)(void)submit(&f,"/hold",NULL,false);
    /* Advertising cloudflared's setting must never remove local limits. */
    while((n=nghttp2_session_mem_send2(f.peer,&p))>0){
        size_t used;CHECK(cf_h2_receive(f.h,(cf_bytes){p,(size_t)n},&used,1000)==CF_OK && used==(size_t)n);
    }
    CHECK(n==0);exchange(&f,4096);cf_h2_get_stats(f.h,&stats);
    CHECK(f.requests==5 && stats.rejected_streams==12 && stats.active_streams==5);
    CHECK(!stats.goaway && stats.ngheap_peak<=stats.ngheap_limit);
    (void)submit(&f,"/hello","update-configuration",false);exchange(&f,4096);
    CHECK(f.requests==6 && slot(&f,control)!=NULL); /* Configuration still has its own slot. */
    for(i=0;i<4;++i)CHECK(nghttp2_submit_rst_stream(f.peer,NGHTTP2_FLAG_NONE,app[i],NGHTTP2_CANCEL)==0);
    exchange(&f,4096);(void)submit(&f,"/hello",NULL,false);exchange(&f,4096);
    CHECK(f.requests==7);cf_h2_get_stats(f.h,&stats);CHECK(stats.active_streams==1 && !stats.goaway);
    destroy(&f);
}
int main(void){test_refresh_burst();test_goaway_diagnostic();test_bounded_overload();test_streams();test_large();test_limits();printf("PASS: HTTP/2 server (%u checks, 10 MiB each direction, 100 session cycles)\n",checks);return 0;}
