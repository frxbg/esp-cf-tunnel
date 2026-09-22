#include "monitor.h"
#include "monitor_api.h"
#include "cf_json.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static void headers(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
}

static esp_err_t error(httpd_req_t *r, const char *status, const char *message)
{
    char reply[240];
    headers(r);
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Connection", "close");
    snprintf(reply, sizeof(reply), "{\"error\":\"%s\"}", message); /* constant messages only */
    (void)httpd_resp_sendstr(r, reply);
    return ESP_FAIL; /* Close even when a rejected request has an unread body. */
}

static bool local_origin(httpd_req_t *r)
{
    char host[64], origin[96], address[16], with_port[24], expected_origin[40];
    if (!monitor_request_local_ip(httpd_req_to_sockfd(r),address)) return false;
    snprintf(with_port,sizeof(with_port),"%s:80",address);
    if (
        httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK ||
        (strcmp(host,address) && strcmp(host,with_port))) return false;
    size_t len = httpd_req_get_hdr_value_len(r, "Origin");
    if (!len) return true; /* Non-browser clients still require the secret. */
    snprintf(expected_origin,sizeof(expected_origin),"http://%s%s",address,strcmp(host,address) ? ":80" : "");
    return httpd_req_get_hdr_value_str(r,"Origin",origin,sizeof(origin)) == ESP_OK && !strcmp(origin,expected_origin);
}


static esp_err_t api_handler(httpd_req_t *r)
{
    char auth[80]={0}, type[64]={0};
    uint8_t *input=NULL;
    monitor_api_request request={.path=r->uri,.authorization=auth,.post=r->method==HTTP_POST,
        .origin_allowed=local_origin(r),.setup_access=monitor_request_on_setup(httpd_req_to_sockfd(r))};
    size_t auth_len=httpd_req_get_hdr_value_len(r,"Authorization");
    if(auth_len && httpd_req_get_hdr_value_str(r,"Authorization",auth,sizeof(auth))!=ESP_OK)
        return error(r,"400 Bad Request","Invalid authorization header.");
    if(request.post) {
        if(!r->content_len || r->content_len>MONITOR_BODY_MAX)
            return error(r,"400 Bad Request","Send a JSON object of at most 2048 bytes.");
        request.json_content=httpd_req_get_hdr_value_str(r,"Content-Type",type,sizeof(type))==ESP_OK &&
            !strncmp(type,"application/json",16) && (!type[16] || type[16]==';');
        input=malloc(r->content_len);
        if(!input) return error(r,"503 Service Unavailable","Not enough memory.");
        size_t done=0;
        while(done<r->content_len) {
            int n=httpd_req_recv(r,(char *)input+done,r->content_len-done);
            if(n<=0) break;
            done+=(size_t)n;
        }
        request.body=(cf_bytes){input,done==r->content_len ? done : 0};
    }
    monitor_api_reply *reply=malloc(sizeof(*reply));
    esp_err_t rc=ESP_FAIL;
    if(!reply || xSemaphoreTake(monitor_json_lock,pdMS_TO_TICKS(3000))!=pdTRUE) {
        rc=error(r,"503 Service Unavailable","The device is busy. Please retry.");
    } else {
        monitor_api_dispatch(&request,reply);
        monitor_api_schedule_effects(reply->effects);
        xSemaphoreGive(monitor_json_lock);
        char status[40]; snprintf(status,sizeof(status),"%u %s",reply->status,reply->status==200 ? "OK" : "Error");
        headers(r); httpd_resp_set_status(r,status); httpd_resp_set_type(r,"application/json");
        rc=httpd_resp_send(r,reply->body,reply->length);
    }
    cf_secure_zero(auth,sizeof(auth));
    if(input) {cf_secure_zero(input,r->content_len);free(input);}
    if(reply) {cf_secure_zero(reply,sizeof(*reply));free(reply);}
    return rc;
}

static esp_err_t asset_get(httpd_req_t *r)
{
    for (size_t i=0;i<monitor_asset_count;++i) {
        const monitor_asset *a = &monitor_assets[i];
        if (!strcmp(r->uri,a->uri)) {
            headers(r); httpd_resp_set_type(r,a->type);
            return httpd_resp_send(r,(const char *)a->data,a->len);
        }
    }
    return error(r,"404 Not Found","Page not found. Open http://192.168.4.1 on the setup Wi-Fi.");
}

static esp_err_t socket_open(httpd_handle_t server, int socket)
{
    (void)server;
    /* HTTPD sends header fields through several small writes. Avoid the
     * Nagle/delayed-ACK interaction for these and small telemetry replies. */
    int enabled = 1;
    return setsockopt(socket,IPPROTO_TCP,TCP_NODELAY,&enabled,sizeof(enabled)) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t monitor_http_start(void)
{
    monitor_json_lock=xSemaphoreCreateMutex();
    if (!monitor_json_lock) return ESP_ERR_NO_MEM;
    /* Install quota + wiping hooks on our private parser before any allocation. */
    cf_json_delete_secret(cf_json_parse((cf_bytes){(const uint8_t *)"{}",2}));
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size=8192; cfg.max_open_sockets=4; cfg.max_uri_handlers=12;
    cfg.lru_purge_enable=true; cfg.recv_wait_timeout=3; cfg.send_wait_timeout=3;
    cfg.uri_match_fn=httpd_uri_match_wildcard;
    cfg.open_fn=socket_open;
    httpd_handle_t server; esp_err_t rc = httpd_start(&server,&cfg); if (rc != ESP_OK) return rc;
    const httpd_uri_t routes[] = {
        {.uri="/api/*",.method=HTTP_GET,.handler=api_handler},
        {.uri="/api/*",.method=HTTP_POST,.handler=api_handler},
        {.uri="/*",.method=HTTP_GET,.handler=asset_get}
    };
    for (size_t i=0;i<sizeof(routes)/sizeof(routes[0]);++i) {
        rc=httpd_register_uri_handler(server,&routes[i]);
        if (rc != ESP_OK) { httpd_stop(server); return rc; }
    }
    return ESP_OK;
}
