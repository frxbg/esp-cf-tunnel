#include "monitor.h"
#include "cf_h2.h"
#include "monitor_api.h"
#include "monitor_remote.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

/* Native backend: flash assets and authenticated API shared with HTTPD.
 * One bounded response record per application stream; no loopback proxy. */
typedef struct {
    int32_t id;
    const uint8_t *data;
    size_t length, sent;
    monitor_api_reply *owned;
    uint8_t *input;
    size_t input_used;
    monitor_remote_head request;
    cf_h2 *h;
    bool head, api, complete;
} response;
static esp_cf_tunnel *tunnel;
static response responses[CF_H2_APPLICATION_MAX];

static response *find(int32_t id)
{
    for(size_t i=0;i<CF_H2_APPLICATION_MAX;++i) if(responses[i].id==id) return &responses[i];
    return NULL;
}
static void environment(void *ctx,esp_cf_environment *out)
{
    (void)ctx;
    monitor_network_snapshot network; monitor_network_snapshot_get(&network);
    memset(out,0,sizeof(*out)); out->network_ready=network.connected; out->time_valid=time(NULL)>1704067200;
    struct in_addr ip;
    if(inet_pton(AF_INET,network.ip,&ip)==1) memcpy(out->local_ip,&ip.s_addr,4);
    esp_netif_t *sta=esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_dns_info_t dns;
    if(sta && esp_netif_get_dns_info(sta,ESP_NETIF_DNS_MAIN,&dns)==ESP_OK && dns.ip.type==ESP_IPADDR_TYPE_V4)
        memcpy(out->dns_ip,&dns.ip.u_addr.ip4.addr,4);
}
static bool json_lock(void *ctx) { (void)ctx; return xSemaphoreTake(monitor_json_lock,0)==pdTRUE; }
static void json_unlock(void *ctx) { (void)ctx; xSemaphoreGive(monitor_json_lock); }
static cf_result credentials(void *ctx,cf_credentials *out,char hostname[254])
{
    (void)ctx;
    monitor_tunnel_config *config=calloc(1,sizeof(*config));
    uint8_t scratch[CF_TOKEN_JSON_MAX+1]; cf_result rc=CF_ERR_MEMORY;
    hostname[0]=0; cf_secure_zero(out,sizeof(*out));
    if(config && monitor_store_tunnel(config)==ESP_OK && config->hostname[0] && config->token[0]) {
        rc=cf_credentials_parse((cf_bytes){(uint8_t *)config->token,strlen(config->token)},scratch,sizeof(scratch),out);
        if(rc==CF_OK) memcpy(hostname,config->hostname,strlen(config->hostname)+1);
    } else if(config) rc=CF_ERR_STATE;
    if(config) {cf_secure_zero(config,sizeof(*config));free(config);}
    cf_secure_zero(scratch,sizeof(scratch)); return rc;
}
static cf_result respond(response *s,unsigned status,const char *type)
{
    char length[24]; snprintf(length,sizeof(length),"%u",(unsigned)s->length);
#define HEADER(name,value) {(cf_bytes){(const uint8_t *)(name),strlen(name)},(cf_bytes){(const uint8_t *)(value),strlen(value)}}
    cf_header headers[]={HEADER("content-type",type),HEADER("content-length",length),HEADER("Cache-Control","no-store"),
        HEADER("X-Content-Type-Options","nosniff"),HEADER("Referrer-Policy","no-referrer"),
        HEADER("Content-Security-Policy","default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'")};
#undef HEADER
    s->complete=true;
    return cf_h2_respond(s->h,s->id,status,headers,sizeof(headers)/sizeof(headers[0]),true);
}
static cf_result fail_response(response *s,unsigned status,const char *json)
{
    s->data=(const uint8_t *)json;s->length=strlen(json);return respond(s,status,"application/json");
}
static cf_result request(void *ctx,cf_h2 *h,const cf_h2_request *r)
{
    (void)ctx;
    response *s=NULL;
    for(size_t i=0;i<CF_H2_APPLICATION_MAX;++i) if(!responses[i].id){s=&responses[i];break;}
    if(!s) return CF_ERR_LIMIT;
    s->id=r->stream;s->h=h;
    cf_result rc=monitor_remote_parse(r,&s->request);
    if(rc!=CF_OK) return fail_response(s,rc==CF_ERR_UNSUPPORTED ? 405 : 400,
        "{\"error\":\"Unsupported method, body size or ambiguous request headers.\"}");
    s->head=s->request.head;
    s->api=!strncmp(s->request.path,"/api/",5);
    if(s->api) {
        if(s->request.post) {
            s->input=malloc(MONITOR_BODY_MAX);
            if(!s->input) return fail_response(s,503,"{\"error\":\"Not enough memory for this request.\"}");
        }
        /* Execute once, after END_STREAM and exact Content-Length validation. */
        return CF_OK;
    }
    if(s->request.post) return fail_response(s,405,"{\"error\":\"This page supports GET and HEAD.\"}");
    for(size_t i=0;i<monitor_asset_count;++i) if(!strcmp(s->request.path,monitor_assets[i].uri)) {
        s->data=monitor_assets[i].data;s->length=monitor_assets[i].len;
        return respond(s,200,monitor_assets[i].type);
    }
    return fail_response(s,404,"{\"error\":\"Page not found.\"}");
}
static cf_result data(void *ctx,int32_t id,cf_bytes bytes,bool end)
{
    (void)ctx;response *s=find(id);
    if(!s) return CF_ERR_STATE;
    if(s->complete) return CF_OK; /* rejected request body is discarded, never executed */
    if(bytes.len) {
        if(!s->input || bytes.len>MONITOR_BODY_MAX-s->input_used)
            return fail_response(s,400,"{\"error\":\"Send a JSON object of at most 2048 bytes.\"}");
        memcpy(s->input+s->input_used,bytes.ptr,bytes.len);s->input_used+=bytes.len;
    }
    if(!end) return CF_OK;
    if(s->request.has_length && s->request.content_length!=s->input_used)
        return fail_response(s,400,"{\"error\":\"Request body length mismatch.\"}");
    s->owned=malloc(sizeof(*s->owned));
    if(!s->owned) return fail_response(s,503,"{\"error\":\"Not enough memory for this response.\"}");
    if(!json_lock(NULL)) return fail_response(s,503,"{\"error\":\"The device is busy. Please retry.\"}");
    monitor_api_request req={.path=s->request.path,.authorization=s->request.authorization,
        .post=s->request.post,.json_content=s->request.json_content,.origin_allowed=s->request.origin_allowed,
        .remote_access=true,.body={s->input,s->input_used}};
    monitor_api_dispatch(&req,s->owned);
    monitor_api_schedule_effects(s->owned->effects);
    json_unlock(NULL);
    if(s->input) {cf_secure_zero(s->input,MONITOR_BODY_MAX);free(s->input);s->input=NULL;}
    cf_secure_zero(s->request.authorization,sizeof(s->request.authorization));
    s->data=(const uint8_t *)s->owned->body;s->length=s->owned->length;
    return respond(s,s->owned->status,"application/json");
}
static cf_result read_response(void *ctx,int32_t id,uint8_t *out,size_t capacity,size_t *written,bool *eof)
{
    (void)ctx;response *s=find(id);
    if(!s) return CF_ERR_STATE;
    *written=s->head ? 0 : s->length-s->sent;
    if(*written>capacity) *written=capacity;
    if(*written) memcpy(out,s->data+s->sent,*written);
    s->sent+=*written;*eof=s->head || s->sent==s->length;return CF_OK;
}
static void closed(void *ctx,int32_t id,uint32_t error)
{
    (void)ctx;(void)error;response *s=find(id);
    if(s) {
        if(s->owned){cf_secure_zero(s->owned,sizeof(*s->owned));free(s->owned);}
        if(s->input){cf_secure_zero(s->input,MONITOR_BODY_MAX);free(s->input);}
        cf_secure_zero(s,sizeof(*s));
    }
}
esp_err_t monitor_tunnel_start(void)
{
    const esp_cf_tunnel_config config={.environment=environment,.credentials=credentials,.json_try_lock=json_lock,.json_unlock=json_unlock,
        .request=request,.data=data,.read_response=read_response,.closed=closed,.version="esp-monitor/" MONITOR_VERSION,
        .arch="esp32s3-espidf6.1",.service="http://localhost:80",.application_streams=4};
    esp_err_t rc=esp_cf_tunnel_init(&config,&tunnel);
    return rc==ESP_OK ? esp_cf_tunnel_start(tunnel) : rc;
}
void monitor_tunnel_reload(void) { if(tunnel) esp_cf_tunnel_reload(tunnel); }
void monitor_tunnel_snapshot(esp_cf_tunnel_snapshot *out)
{
    memset(out,0,sizeof(*out));out->config_version=-1;
    if(tunnel) esp_cf_tunnel_get_snapshot(tunnel,out);
}
