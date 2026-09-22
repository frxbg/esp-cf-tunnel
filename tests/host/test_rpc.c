#include "cf_rpc.h"
#include "rpc_vectors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)
#define BYTES(x) ((cf_bytes){(const uint8_t *)(x),sizeof(x)-1})
#define ARRAY(x) ((cf_bytes){(x),sizeof(x)})
static cf_credentials credentials;
static cf_rpc_options options;
static const uint8_t ip[] = {192,0,2,1};

static void setup(void)
{
    unsigned i;
    memcpy(credentials.account_tag,"0123456789abcdef0123456789abcdef",33); /* Synthetic fixture; gitleaks:allow */
    credentials.tunnel_secret_len=36;
    for(i=0;i<36;++i) credentials.tunnel_secret[i]=(uint8_t)i;
    for(i=0;i<16;++i) credentials.tunnel_id[i]=options.connector_id[i]=(uint8_t)i;
    options.local_ip=ARRAY(ip);
    options.version=BYTES("esp-cf-test");options.arch=BYTES("host-test");
    options.connection_index=2;options.previous_attempts=3;
    options.features=CF_RPC_REMOTE_CONFIG | CF_RPC_SERIALIZED_HEADERS;
}
static void ready(cf_rpc *r,uint8_t *out)
{
    size_t n;
    cf_rpc_init(r);
    CHECK(cf_rpc_begin(r,out,CF_RPC_TX_MAX,&n)==CF_OK && n>0);
    CHECK(cf_rpc_receive(r,ARRAY(rpc_bootstrap),out,CF_RPC_TX_MAX,&n)==CF_OK);
    CHECK(r->state==CF_RPC_READY && r->have_capability && r->capability==37);
}
static void registering(cf_rpc *r,uint8_t *out)
{
    size_t n;
    ready(r,out);
    CHECK(cf_rpc_register(r,&credentials,&options,out,CF_RPC_TX_MAX,&n)==CF_OK);
    CHECK(r->state==CF_RPC_REGISTERING && n>0 && n<CF_RPC_TX_MAX);
}
static uint64_t root_kind(cf_bytes frame)
{
    cf_cp_reader rd; cf_cp_value root; uint64_t kind;
    CHECK(cf_cp_read_init(&rd,frame)==CF_OK);root=cf_cp_root(&rd);
    kind=cf_cp_uint(&rd,root,0,2);CHECK(rd.error==CF_OK);return kind;
}
static void test_rpc(void)
{
    cf_rpc r;uint8_t out[CF_RPC_TX_MAX];size_t n,i;
    const cf_bytes success[]={ARRAY(rpc_success),ARRAY(rpc_success_multi)};
    const cf_bytes bootstrap[]={ARRAY(rpc_bootstrap),ARRAY(rpc_bootstrap_multi)};
    const cf_bytes invalid[]={ARRAY(rpc_wrong_question),ARRAY(rpc_local),ARRAY(rpc_exception),ARRAY(rpc_abort)};
    for(i=0;i<2;++i){
        cf_rpc_init(&r);
        CHECK(cf_rpc_begin(&r,out,sizeof(out),&n)==CF_OK);
        CHECK(cf_rpc_begin(&r,out,sizeof(out),&n)==CF_ERR_STATE && n==0);
        CHECK(cf_rpc_receive(&r,bootstrap[i],out,sizeof(out),&n)==CF_OK && r.state==CF_RPC_READY);
        CHECK(root_kind((cf_bytes){out,n})==4);
        CHECK(cf_rpc_register(&r,&credentials,&options,out,sizeof(out),&n)==CF_OK);
        CHECK(root_kind((cf_bytes){out,n})==2);
        CHECK(cf_rpc_receive(&r,success[i],out,sizeof(out),&n)==CF_OK);
        CHECK(r.state==CF_RPC_REGISTERED && r.remotely_managed && !strcmp(r.location,"SOF"));
        CHECK(!memcmp(r.connection_id,credentials.tunnel_id,16));
        CHECK(cf_rpc_unregister(&r,out,sizeof(out),&n)==CF_OK && r.state==CF_RPC_UNREGISTERING);
        CHECK(cf_rpc_receive(&r,ARRAY(rpc_unregister_multi),out,sizeof(out),&n)==CF_OK && r.state==CF_RPC_CLOSED);
        CHECK(!r.have_capability && n==80);
        CHECK(root_kind((cf_bytes){out,40})==4 && root_kind((cf_bytes){out+40,n-40})==6);
        cf_rpc_reset(&r);CHECK(r.state==CF_RPC_IDLE && !r.have_capability);
    }
    registering(&r,out);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_retry_multi),out,sizeof(out),&n)==CF_OK);
    CHECK(r.state==CF_RPC_REJECTED && r.should_retry && r.retry_after_ms==1501);
    registering(&r,out);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_permanent),out,sizeof(out),&n)==CF_OK);
    CHECK(r.state==CF_RPC_REJECTED && !r.should_retry);
    registering(&r,out);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_negative),out,sizeof(out),&n)==CF_OK && r.retry_after_ms==0);
    for(i=0;i<sizeof(invalid)/sizeof(invalid[0]);++i){
        registering(&r,out);
        CHECK(cf_rpc_receive(&r,invalid[i],out,sizeof(out),&n)!=CF_OK);
        CHECK(r.state==CF_RPC_FAILED);
        CHECK(cf_rpc_receive(&r,ARRAY(rpc_success),out,sizeof(out),&n)==CF_ERR_STATE && n==0);
    }
    cf_rpc_init(&r);CHECK(cf_rpc_begin(&r,out,sizeof(out),&n)==CF_OK);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_promise_multi),out,sizeof(out),&n)==CF_ERR_UNSUPPORTED);
    CHECK(r.state==CF_RPC_FAILED && root_kind((cf_bytes){out,n})==1);
    ready(&r,out);
    CHECK(cf_rpc_register(&r,&credentials,&options,out,32,&n)==CF_ERR_ARGUMENT && r.state==CF_RPC_READY);
    options.local_ip.len=5;
    CHECK(cf_rpc_register(&r,&credentials,&options,out,sizeof(out),&n)==CF_ERR_ARGUMENT && n==0);
    options.local_ip.len=4;
    for(i=0;i<sizeof(rpc_success_multi);++i){
        registering(&r,out);
        CHECK(cf_rpc_receive(&r,(cf_bytes){rpc_success_multi,i},out,sizeof(out),&n)!=CF_OK && r.state==CF_RPC_FAILED);
    }
}

static void put32(uint8_t *p,uint32_t n){unsigned i;for(i=0;i<4;++i)p[i]=(uint8_t)(n>>(8*i));}
static void put64(uint8_t *p,uint64_t n){unsigned i;for(i=0;i<8;++i)p[i]=(uint8_t)(n>>(8*i));}
static void test_pointers(void)
{
    uint8_t frame[512];cf_cp_reader rd;cf_cp_value root,v;cf_bytes data;
    size_t n;cf_cp_builder b;uint32_t s;unsigned i;
    CHECK(cf_cp_build_init(&b,frame,sizeof(frame))==CF_OK);
    s=cf_cp_struct(&b,0,1,1);cf_cp_set_uint(&b,s,0,8,UINT64_C(0x0123456789abcdef));
    cf_cp_blob(&b,s+1,BYTES("hello"),true);CHECK(cf_cp_build_end(&b,&n)==CF_OK);
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,n})==CF_OK);root=cf_cp_root(&rd);
    CHECK(cf_cp_uint(&rd,root,0,8)==UINT64_C(0x0123456789abcdef));
    data=cf_cp_data(&rd,cf_cp_field(&rd,root,0),true);
    CHECK(data.len==5 && !memcmp(data.ptr,"hello",5));
    CHECK(cf_cp_uint(&rd,root,8,8)==0 && cf_cp_field(&rd,root,10).kind==CF_CP_NULL && rd.error==CF_OK);
    /* Single far pointer: segment 0 root -> segment 1 landing + struct. */
    memset(frame,0,sizeof(frame));put32(frame,1);put32(frame+4,1);put32(frame+8,2);
    put64(frame+16,UINT64_C(1)<<32|2);put64(frame+24,UINT64_C(1)<<32);put64(frame+32,42);
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,40})==CF_OK);root=cf_cp_root(&rd);
    CHECK(cf_cp_uint(&rd,root,0,8)==42 && rd.error==CF_OK);
    /* Double far: landing pad in segment 1, object in segment 2. */
    memset(frame,0,sizeof(frame));put32(frame,2);put32(frame+4,1);put32(frame+8,2);put32(frame+12,1);
    put64(frame+16,UINT64_C(1)<<32|6);put64(frame+24,UINT64_C(2)<<32|2);
    put64(frame+32,UINT64_C(1)<<32);put64(frame+40,123);
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,48})==CF_OK);root=cf_cp_root(&rd);
    CHECK(cf_cp_uint(&rd,root,0,8)==123 && rd.error==CF_OK);
    put64(frame+32,UINT64_C(1)<<32|4); /* Invalid nonzero tag offset. */
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,48})==CF_OK);(void)cf_cp_root(&rd);CHECK(rd.error==CF_ERR_FORMAT);
    put64(frame+32,UINT64_C(1)<<32);put64(frame+24,UINT64_C(9)<<32|2);
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,48})==CF_OK);(void)cf_cp_root(&rd);CHECK(rd.error==CF_ERR_FORMAT);
    /* Backward pointer to itself: each step consumes depth/work, no recursion. */
    memset(frame,0,sizeof(frame));put32(frame+4,1);put64(frame+8,UINT64_C(1)<<48|UINT64_C(0xfffffffc));
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,16})==CF_OK);v=cf_cp_root(&rd);
    for(i=0;i<CF_CAPNP_DEPTH_MAX;++i)v=cf_cp_field(&rd,v,0);
    CHECK(rd.error==CF_ERR_LIMIT);
    /* Huge byte list and negative target cannot wrap into the input. */
    put64(frame+8,UINT64_MAX-2);
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,16})==CF_OK);(void)cf_cp_root(&rd);CHECK(rd.error!=CF_OK);
    put64(frame+8,UINT64_C(0xfffffff8));
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,16})==CF_OK);(void)cf_cp_root(&rd);CHECK(rd.error==CF_ERR_FORMAT);
    /* Zero-sized composite elements still cost traversal budget when read. */
    memset(frame,0,sizeof(frame));put32(frame+4,2);put64(frame+8,UINT64_C(7)<<32|1);put64(frame+16,UINT64_C(0xfffffffc));
    CHECK(cf_cp_read_init(&rd,(cf_bytes){frame,24})==CF_OK);root=cf_cp_root(&rd);
    for(i=0;i<CF_CAPNP_TRAVERSAL_WORDS;++i)(void)cf_cp_at(&rd,root,0);
    CHECK(rd.error==CF_ERR_LIMIT);
    CHECK(cf_cp_build_init(&b,frame,16)==CF_OK);(void)cf_cp_struct(&b,0,1,1);
    CHECK(cf_cp_build_end(&b,&n)==CF_ERR_LIMIT && n==0);
}

static void test_mutations(void)
{
    const cf_bytes seeds[]={ARRAY(rpc_bootstrap),ARRAY(rpc_bootstrap_multi),ARRAY(rpc_success),ARRAY(rpc_success_multi),
        ARRAY(rpc_retry_multi),ARRAY(rpc_permanent),ARRAY(rpc_exception),ARRAY(rpc_promise_multi),ARRAY(rpc_unregister_multi)};
    size_t seed,byte;unsigned bit;
    uint8_t frame[512],out[CF_RPC_TX_MAX];
    for(seed=0;seed<sizeof(seeds)/sizeof(seeds[0]);++seed){
        CHECK(seeds[seed].len<=sizeof(frame));
        for(byte=0;byte<seeds[seed].len;++byte)for(bit=0;bit<8;++bit){
            cf_rpc r;size_t n;cf_result rc;
            memcpy(frame,seeds[seed].ptr,seeds[seed].len);frame[byte]^=(uint8_t)(1u<<bit);
            cf_rpc_init(&r);r.state=seed<2 || seed==7 ? CF_RPC_BOOTSTRAPPING : seed==8 ? CF_RPC_UNREGISTERING : CF_RPC_REGISTERING;
            r.have_capability=seed>=2;r.capability=37;
            rc=cf_rpc_receive(&r,(cf_bytes){frame,seeds[seed].len},out,sizeof(out),&n);
            CHECK(n<=sizeof(out));
            CHECK(rc==CF_OK || r.state==CF_RPC_FAILED);
        }
    }
}

static void write_file(const char *dir,const char *name,const uint8_t *bytes,size_t size)
{
    char path[1024];FILE *f;
    CHECK(snprintf(path,sizeof(path),"%s/%s.bin",dir,name)>0);
    f=fopen(path,"wb");CHECK(f!=NULL);CHECK(fwrite(bytes,1,size,f)==size);CHECK(fclose(f)==0);
}
static void emit(const char *dir)
{
    cf_rpc r;uint8_t out[CF_RPC_TX_MAX];size_t n;
    cf_rpc_init(&r);CHECK(cf_rpc_begin(&r,out,sizeof(out),&n)==CF_OK);write_file(dir,"bootstrap",out,n);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_bootstrap),out,sizeof(out),&n)==CF_OK);write_file(dir,"finish_bootstrap",out,n);
    CHECK(cf_rpc_register(&r,&credentials,&options,out,sizeof(out),&n)==CF_OK);write_file(dir,"register",out,n);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_success),out,sizeof(out),&n)==CF_OK);write_file(dir,"finish_register",out,n);
    CHECK(cf_rpc_unregister(&r,out,sizeof(out),&n)==CF_OK);write_file(dir,"unregister",out,n);
    CHECK(cf_rpc_receive(&r,ARRAY(rpc_unregister),out,sizeof(out),&n)==CF_OK);
    write_file(dir,"finish_unregister",out,40);write_file(dir,"release",out+40,n-40);
}
static void send_bytes(uint8_t *out,size_t n)
{
    CHECK(fwrite(out,1,n,stdout)==n);CHECK(fflush(stdout)==0);cf_secure_zero(out,CF_RPC_TX_MAX);
}
static cf_result receive_peer(void *ctx,cf_bytes frame)
{
    cf_rpc *r=ctx;uint8_t out[CF_RPC_TX_MAX];size_t n;
    cf_result rc=cf_rpc_receive(r,frame,out,sizeof(out),&n);
    send_bytes(out,n);if(rc!=CF_OK)return rc;
    if(r->state==CF_RPC_READY){rc=cf_rpc_register(r,&credentials,&options,out,sizeof(out),&n);send_bytes(out,n);}
    else if(r->state==CF_RPC_REGISTERED){rc=cf_rpc_unregister(r,out,sizeof(out),&n);send_bytes(out,n);}
    return rc;
}
static void peer(void)
{
    cf_rpc r;cf_capnp_framer framer;uint8_t in[CF_CAPNP_MESSAGE_MAX],out[CF_RPC_TX_MAX];size_t n;int c;
#ifdef _WIN32
    CHECK(_setmode(_fileno(stdin),_O_BINARY)!=-1);CHECK(_setmode(_fileno(stdout),_O_BINARY)!=-1);
#endif
    cf_rpc_init(&r);CHECK(cf_capnp_framer_init(&framer,in,sizeof(in))==CF_OK);
    CHECK(cf_rpc_begin(&r,out,sizeof(out),&n)==CF_OK);send_bytes(out,n);
    while(r.state!=CF_RPC_CLOSED && (c=getchar())!=EOF){
        uint8_t byte=(uint8_t)c;
        CHECK(cf_capnp_feed(&framer,(cf_bytes){&byte,1},&n,receive_peer,&r)==CF_OK && n==1);
    }
    CHECK(r.state==CF_RPC_CLOSED && cf_capnp_eof(&framer)==CF_OK);
    fprintf(stderr,"PASS: C RPC peer registered and gracefully unregistered through official Go RPC runtime\n");
}
int main(int argc,char **argv)
{
    setup();
    if(argc>1 && !strcmp(argv[1],"peer")){peer();return 0;}
    if(argc>2 && !strcmp(argv[1],"emit")){emit(argv[2]);return 0;}
    test_pointers();test_rpc();test_mutations();printf("PASS: Cap'n Proto pointers and RPC (%u checks)\n",checks);return 0;
}
