#include "monitor.h"
#include "cf_json.h"
#include "monitor_api.h"
#include "monitor_ota.h"
#include "mbedtls/base64.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_psram.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <math.h>

/* HTTPD and tunnel serialize authentication/JSON/GPIO with monitor_json_lock;
 * no secret leaves a GET response, log, URL, cookie, or persistent session. */
typedef struct { const monitor_api_request *request; monitor_api_reply *reply; const char *uri; } api_context;
SemaphoreHandle_t monitor_json_lock;
static char session[65];
static uint64_t session_until, failure_window;
static unsigned failures;
static bool json_ok;
static uint64_t milliseconds(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static esp_err_t error(api_context *r, const char *status, const char *message)
{
    r->reply->status=(unsigned)strtoul(status,NULL,10);
    int n=snprintf(r->reply->body,sizeof(r->reply->body),"{\"error\":\"%s\"}",message);
    r->reply->length=n>0 && (size_t)n<sizeof(r->reply->body) ? (size_t)n : 0;
    return ESP_FAIL;
}
static esp_err_t ok(api_context *r)
{
    r->reply->status=200;
    strcpy(r->reply->body,"{\"ok\":true}");
    r->reply->length=strlen(r->reply->body); return ESP_OK;
}
static bool local_origin(api_context *r) { return r->request->origin_allowed; }

static bool equal_secret(const char *a, const char *b, size_t max)
{
    size_t alen = strlen(a), blen = strlen(b);
    volatile unsigned diff = (unsigned)(alen ^ blen);
    for (size_t i = 0; i < max; ++i) diff |= (i < alen ? (unsigned char)a[i] : 0) ^ (i < blen ? (unsigned char)b[i] : 0);
    return diff == 0;
}

static bool authorized(api_context *r)
{
    const char *auth=r->request->authorization;
    return local_origin(r) && session[0] && milliseconds()<session_until && auth &&
        !strncmp(auth,"Bearer ",7) && equal_secret(auth+7,session,64);
}
static cf_cJSON *body(api_context *r)
{
    cf_bytes bytes=r->request->body;
    return r->request->json_content && bytes.len && bytes.len<=MONITOR_BODY_MAX ? cf_json_parse(bytes) : NULL;
}

static const char *string(const cf_cJSON *j, const char *key)
{
    const cf_cJSON *v = cf_cJSON_GetObjectItemCaseSensitive(j, key);
    return cf_cJSON_IsString(v) ? v->valuestring : NULL;
}
static void s(cf_cJSON *j, const char *k, const char *v) { if (!cf_cJSON_AddStringToObject(j,k,v)) json_ok = false; }
static void n(cf_cJSON *j, const char *k, double v) { if (!cf_cJSON_AddNumberToObject(j,k,v)) json_ok = false; }
static void b(cf_cJSON *j, const char *k, bool v) { if (!cf_cJSON_AddBoolToObject(j,k,v)) json_ok = false; }

static esp_err_t send_json(api_context *r, cf_cJSON *j)
{
    bool valid=j && json_ok && cf_cJSON_PrintPreallocated(j,r->reply->body,sizeof(r->reply->body),false);
    cf_json_delete_secret(j);
    if(!valid) return error(r,"503 Service Unavailable","Not enough memory for this response.");
    r->reply->status=200; r->reply->length=strlen(r->reply->body); return ESP_OK;
}

static void connect_status(cf_cJSON *parent, const char *key, const esp_cf_connect_diagnostic *d)
{
    cf_cJSON *j = cf_cJSON_AddObjectToObject(parent,key);
    if (!j) { json_ok = false; return; }
    b(j,"valid",d->valid); if (!d->valid) return;
    s(j,"stage",esp_cf_connect_stage_name(d->stage)); b(j,"errors_available",d->errors_available);
    n(j,"rc",d->rc); n(j,"state_before",d->state_before); n(j,"state_after",d->state_after);
    n(j,"esp_error",d->esp_error); n(j,"tls_error",d->tls_error); n(j,"verify_flags",d->verify_flags);
    n(j,"system_error",d->system_error); n(j,"errno_context",d->errno_context);
    n(j,"tls_alert",d->tls_alert); n(j,"tls_version",d->tls_version);
    n(j,"elapsed_ms",d->elapsed_ms); n(j,"at_ms",d->at_ms); n(j,"utc_s",d->utc_s);
    n(j,"attempt",d->attempt); s(j,"edge_ip",d->edge_ip); n(j,"edge_port",d->edge_port);
    n(j,"heap_free",d->heap_free); n(j,"heap_largest",d->heap_largest);
}
static cf_cJSON *status_object(bool setup_access, bool admin_access, bool remote_access)
{
    monitor_network_snapshot net; monitor_network_snapshot_get(&net);
    monitor_gpio pins[MONITOR_GPIO_COUNT]; monitor_io_snapshot(pins);
    monitor_tunnel_config *tunnel = calloc(1, sizeof(*tunnel));
    cf_cJSON *j = cf_cJSON_CreateObject(); json_ok = j != NULL;
    if (!j || !tunnel) { free(tunnel); cf_json_delete_secret(j); return NULL; }
    bool valid_temp; float temp = monitor_temperature(&valid_temp);
    uint32_t flash = 0; (void)esp_flash_get_size(NULL, &flash);
    esp_chip_info_t chip; esp_chip_info(&chip);
    (void)monitor_store_tunnel(tunnel);
    s(j,"version",MONITOR_VERSION); s(j,"chip","ESP32-S3"); n(j,"revision",chip.revision); n(j,"cores",chip.cores);
    n(j,"cpu_mhz",CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ); n(j,"uptime_ms",milliseconds());
    n(j,"heap_free",heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    n(j,"heap_min",heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    n(j,"heap_largest",heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    n(j,"psram_total",heap_caps_get_total_size(MALLOC_CAP_SPIRAM)); n(j,"psram_free",heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    n(j,"flash_bytes",flash); n(j,"tasks",uxTaskGetNumberOfTasks()); n(j,"reset_reason",esp_reset_reason());
    if (valid_temp && isfinite(temp)) n(j,"temperature_c",temp); else if (!cf_cJSON_AddNullToObject(j,"temperature_c")) json_ok = false;
    b(j,"wifi_connected",net.connected); b(j,"wifi_connecting",net.connecting); s(j,"ssid",net.ssid);
    b(j,"wifi_associated",net.associated);
    s(j,"ip",net.ip); s(j,"gateway",net.gateway); n(j,"rssi",net.rssi); n(j,"channel",net.channel);
    n(j,"retries",net.retries); n(j,"disconnect_reason",net.disconnect_reason);
    b(j,"ap_enabled",net.ap_enabled); s(j,"ap_ssid",net.ap_ssid); s(j,"ap_ip",MONITOR_AP_IP);
    n(j,"ap_clients",net.ap_clients);
    uint64_t now = milliseconds();
    n(j,"ap_remaining_s",net.ap_deadline_ms > now ? (net.ap_deadline_ms-now)/1000 : 0);
    b(j,"setup_access",setup_access);
    b(j,"admin_access",admin_access); b(j,"remote_access",remote_access);
    b(j,"clock_valid",time(NULL) > 1704067200); s(j,"internet","Not checked");
    n(j,"utc_s",(int64_t)time(NULL)); n(j,"sntp_sync_count",net.sntp_sync_count);
    n(j,"sntp_last_sync_utc",net.sntp_last_sync_utc);
    esp_cf_tunnel_snapshot connection; monitor_tunnel_snapshot(&connection);
    s(j,"tunnel_state",esp_cf_tunnel_state_name(connection.state));
    s(j,"tunnel_message",connection.message); s(j,"edge_ip",connection.edge_ip); s(j,"edge_location",connection.location);
    b(j,"tunnel_registered",connection.registered); b(j,"tunnel_configured",connection.configured);
    n(j,"config_version",connection.config_version); n(j,"tunnel_attempts",connection.attempts);
    n(j,"tunnel_retry_s",connection.retry_at_ms>now ? (connection.retry_at_ms-now+999)/1000 : 0);
    n(j,"tunnel_h2_heap",connection.http2.ngheap_current); n(j,"tunnel_h2_peak",connection.http2.ngheap_peak);
    n(j,"tunnel_streams",connection.http2.active_streams); n(j,"tunnel_stack_min",connection.task_stack_min);
    s(j,"tunnel_last_failure",connection.last_failure); n(j,"tunnel_failures",connection.failures);
    n(j,"tunnel_last_failure_ms",connection.last_failure_ms); n(j,"tunnel_rejected_streams",connection.http2.rejected_streams);
    connect_status(j,"tunnel_connect",&connection.last_connect);
    connect_status(j,"tunnel_connect_failure",&connection.last_connect_failure);
    monitor_ota_status update; monitor_ota_snapshot(&update);
    cf_cJSON *ota=cf_cJSON_AddObjectToObject(j,"ota");
    if(!ota) json_ok=false;
    else {
        b(ota,"available",update.available); b(ota,"active",update.active); b(ota,"ready",update.ready);
        n(ota,"received",update.received); n(ota,"total",update.total); n(ota,"capacity",update.capacity);
        s(ota,"running",update.running); s(ota,"target",update.target); s(ota,"version",update.version); s(ota,"message",update.message);
    }
    b(j,"token_stored",tunnel->token[0] != 0); s(j,"hostname",tunnel->hostname);
    cf_secure_zero(tunnel,sizeof(*tunnel)); free(tunnel);
    cf_cJSON *gp = cf_cJSON_AddArrayToObject(j,"gpio"); if (!gp) json_ok = false;
    for (size_t i=0; gp && i<MONITOR_GPIO_COUNT; ++i) {
        cf_cJSON *p = cf_cJSON_CreateObject();
        if (!p) { json_ok = false; break; }
        cf_cJSON_AddItemToArray(gp,p); n(p,"pin",pins[i].pin); n(p,"mode",pins[i].mode); n(p,"level",pins[i].level);
    }
    monitor_rgb rgb; monitor_led_snapshot(&rgb);
    cf_cJSON *light=cf_cJSON_AddObjectToObject(j,"rgb");
    if (!light) json_ok=false;
    else {
        b(light,"available",rgb.available); b(light,"on",rgb.on); n(light,"pin",48);
        n(light,"red",rgb.red); n(light,"green",rgb.green); n(light,"blue",rgb.blue); n(light,"brightness",rgb.brightness);
    }
    return j;
}

static esp_err_t status_get(api_context *r)
{
    return send_json(r,status_object(r->request->setup_access,local_origin(r),r->request->remote_access));
}

static esp_err_t login_post(api_context *r)
{
    if (!local_origin(r)) return error(r,"403 Forbidden","The request origin does not match this device.");
    uint64_t now = milliseconds();
    if (now - failure_window >= 60000) { failures = 0; failure_window = now; }
    if (failures >= 5) return error(r,"429 Too Many Requests","Too many attempts. Try again in one minute.");
    ++failures; /* Malformed input consumes an attempt too. */
    cf_cJSON *j = body(r); const char *keys[] = {"password"};
    const char *password = string(j,"password"); char expected[64] = {0};
    bool valid = j && cf_json_keys(j,keys,1) && password && strlen(password) <= 63 &&
        monitor_store_password(expected,sizeof(expected)) == ESP_OK && equal_secret(password,expected,63);
    cf_secure_zero(expected,sizeof(expected)); cf_json_delete_secret(j);
    if (!valid) return error(r,"401 Unauthorized","Incorrect device password or invalid request.");
    uint8_t random[32]; esp_fill_random(random,sizeof(random));
    for (size_t i=0;i<sizeof(random);++i) snprintf(session+i*2,3,"%02x",random[i]);
    cf_secure_zero(random,sizeof(random)); session_until = now + 600000;
    failures = 0; json_ok = true; j = cf_cJSON_CreateObject();
    s(j,"session",session); n(j,"expires_in",600);
    return send_json(r,j);
}

static esp_err_t wifi_post(api_context *r, cf_cJSON *j)
{
    const char *keys[] = {"ssid","password","open"};
    const char *ssid = string(j,"ssid"), *pass = string(j,"password");
    const cf_cJSON *open = cf_cJSON_GetObjectItemCaseSensitive(j,"open");
    if (!cf_json_keys(j,keys,3) || !ssid || !pass || !strlen(ssid) || strlen(ssid)>32 || strlen(pass)>63 || !cf_cJSON_IsBool(open))
        return error(r,"400 Bad Request","Use a network name of 1-32 bytes and a password of up to 63 bytes.");
    monitor_wifi_config saved, value = {0}; (void)monitor_store_wifi(&saved);
    memcpy(value.ssid,ssid,strlen(ssid)); value.open = cf_cJSON_IsTrue(open);
    if (!value.open) {
        if (!pass[0] && !strcmp(saved.ssid,ssid) && !saved.open) memcpy(value.password,saved.password,sizeof(value.password));
        else memcpy(value.password,pass,strlen(pass));
    }
    cf_secure_zero(&saved,sizeof(saved));
    if (!value.open && strlen(value.password)<8) {
        cf_secure_zero(&value,sizeof(value)); return error(r,"400 Bad Request","A secured network requires a password of 8-63 bytes.");
    }
    esp_err_t rc = monitor_save_wifi(&value); cf_secure_zero(&value,sizeof(value));
    if (rc != ESP_OK) return error(r,"500 Internal Server Error","Unable to save Wi-Fi settings.");
    r->reply->effects |= MONITOR_EFFECT_WIFI; return ok(r);
}

static bool hostname_valid(const char *name)
{
    size_t len = strlen(name), label = 0;
    if (!len) return true; /* May be supplied later. */
    if (len>253 || name[len-1]=='.') return false;
    for (size_t i=0;i<len;++i) {
        unsigned char c = name[i];
        if (c=='.') { if (!label || name[i-1]=='-') return false; label=0; }
        else {
            if (!((c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='-') ||
                (!label && c=='-') || ++label>63) return false;
        }
    }
    return label && name[len-1]!='-';
}

static esp_err_t tunnel_post(api_context *r, cf_cJSON *j)
{
    const char *keys[] = {"token","hostname","clear"};
    const char *token = string(j,"token"), *host = string(j,"hostname");
    const cf_cJSON *clear = cf_cJSON_GetObjectItemCaseSensitive(j,"clear");
    if (!cf_json_keys(j,keys,3) || !token || !host || !cf_cJSON_IsBool(clear) ||
        strlen(token)>CF_TOKEN_MAX || !hostname_valid(host) || (cf_cJSON_IsTrue(clear) && token[0]))
        return error(r,"400 Bad Request","Use a valid hostname and either a tunnel token or Clear stored token.");
    if (token[0]) {
        uint8_t scratch[CF_TOKEN_JSON_MAX+1]; cf_credentials credentials;
        cf_token_diagnostic diagnostic;
        cf_result rc = cf_credentials_parse_ex((cf_bytes){(const uint8_t *)token,strlen(token)},scratch,sizeof(scratch),&credentials,&diagnostic);
        cf_secure_zero(&credentials,sizeof(credentials));
        if (rc != CF_OK) return error(r,rc == CF_ERR_MEMORY ? "503 Service Unavailable" : "400 Bad Request",cf_token_diagnostic_message(diagnostic));
    }
    monitor_tunnel_config *value = calloc(1,sizeof(*value));
    if (!value) return error(r,"503 Service Unavailable","Not enough memory.");
    (void)monitor_store_tunnel(value);
    if (cf_cJSON_IsTrue(clear)) cf_secure_zero(value->token,sizeof(value->token));
    else if (token[0]) { cf_secure_zero(value->token,sizeof(value->token)); memcpy(value->token,token,strlen(token)); }
    memset(value->hostname,0,sizeof(value->hostname)); memcpy(value->hostname,host,strlen(host));
    esp_err_t rc = monitor_save_tunnel(value); cf_secure_zero(value,sizeof(*value)); free(value);
    if (rc == ESP_OK) r->reply->effects |= MONITOR_EFFECT_TUNNEL;
    return rc == ESP_OK ? ok(r) : error(r,"500 Internal Server Error","Unable to save tunnel settings.");
}

static esp_err_t gpio_post(api_context *r, cf_cJSON *j)
{
    const char *keys[] = {"pin","mode","value"};
    cf_cJSON *pin = cf_cJSON_GetObjectItemCaseSensitive(j,"pin"), *mode = cf_cJSON_GetObjectItemCaseSensitive(j,"mode"), *value = cf_cJSON_GetObjectItemCaseSensitive(j,"value");
    if (!cf_json_keys(j,keys,3) || !cf_cJSON_IsNumber(pin) || !cf_cJSON_IsNumber(mode) || !cf_cJSON_IsNumber(value) ||
        pin->valuedouble != pin->valueint || mode->valuedouble != mode->valueint || value->valuedouble != value->valueint ||
        monitor_io_set(pin->valueint,mode->valueint,value->valueint) != ESP_OK)
        return error(r,"400 Bad Request","Allowed pins: 4, 5, 6, 7. Mode: 0 disabled, 1 input, 2 output. Value: 0 or 1.");
    return ok(r);
}

static esp_err_t ota_post(api_context *r, cf_cJSON *j)
{
    bool success=false; char upload_id[33]={0}; bool started=false;
    if(!strcmp(r->uri,"/api/ota/start")) {
        const char *keys[]={"size","sha256"}; const cf_cJSON *size=cf_cJSON_GetObjectItemCaseSensitive(j,"size");
        if(!cf_json_keys(j,keys,2) || !cf_cJSON_IsNumber(size) || size->valuedouble!=size->valueint || size->valueint<=0)
            return error(r,"400 Bad Request","Provide the firmware size and SHA256 digest.");
        success=monitor_ota_start((size_t)size->valueint,string(j,"sha256"),upload_id); started=success;
    } else if(!strcmp(r->uri,"/api/ota/chunk")) {
        const char *keys[]={"id","offset","data"}; const cf_cJSON *offset=cf_cJSON_GetObjectItemCaseSensitive(j,"offset");
        const char *data=string(j,"data"); uint8_t bytes[MONITOR_OTA_CHUNK]; size_t size=0;
        if(!cf_json_keys(j,keys,3) || !cf_cJSON_IsNumber(offset) || offset->valuedouble!=offset->valueint || offset->valueint<0 ||
           !data || strlen(data)>((MONITOR_OTA_CHUNK+2)/3)*4 ||
           mbedtls_base64_decode(bytes,sizeof(bytes),&size,(const unsigned char *)data,strlen(data))!=0)
            return error(r,"400 Bad Request","Invalid upload chunk.");
        success=monitor_ota_write(string(j,"id"),(size_t)offset->valueint,bytes,size);
        cf_secure_zero(bytes,sizeof(bytes));
    } else {
        const char *keys[]={"id"};
        if(!cf_json_keys(j,keys,1)) return error(r,"400 Bad Request","Provide an upload session id.");
        if(!strcmp(r->uri,"/api/ota/finish")) {
            success=monitor_ota_finish(string(j,"id"));
            if(success) r->reply->effects|=MONITOR_EFFECT_REBOOT;
        } else if(!strcmp(r->uri,"/api/ota/abort")) success=monitor_ota_abort(string(j,"id"));
        else return error(r,"404 Not Found","OTA operation not found.");
    }
    monitor_ota_status state; monitor_ota_snapshot(&state);
    if(!success) return error(r,"400 Bad Request",state.message);
    cf_cJSON *reply=cf_cJSON_CreateObject(); json_ok=reply!=NULL;
    b(reply,"ok",true); n(reply,"received",state.received); n(reply,"chunk_size",MONITOR_OTA_CHUNK);
    if(started) s(reply,"id",upload_id);
    cf_secure_zero(upload_id,sizeof(upload_id)); return send_json(r,reply);
}
static esp_err_t change_post(api_context *r)
{
    if (!authorized(r)) return error(r,"401 Unauthorized","Unlock Settings with the device password. Sessions expire after 10 minutes.");
    cf_cJSON *j = body(r);
    if (!j) return error(r,"400 Bad Request","Send a JSON object of at most 2048 bytes.");
    esp_err_t rc;
    if (!strcmp(r->uri,"/api/wifi")) rc = wifi_post(r,j);
    else if (!strcmp(r->uri,"/api/tunnel")) rc = tunnel_post(r,j);
    else if (!strncmp(r->uri,"/api/ota/",9)) rc = ota_post(r,j);
    else if (!strcmp(r->uri,"/api/tunnel/restart") && cf_json_keys(j,NULL,0)) {
        r->reply->effects |= MONITOR_EFFECT_TUNNEL_RESTART; rc=ok(r);
    }
    else if (!strcmp(r->uri,"/api/gpio")) rc = gpio_post(r,j);
    else if (!strcmp(r->uri,"/api/rgb")) {
        const char *keys[]={"on","red","green","blue","brightness"};
        const cf_cJSON *on=cf_cJSON_GetObjectItemCaseSensitive(j,"on");
        int values[4]={0}; bool valid=cf_json_keys(j,keys,5) && cf_cJSON_IsBool(on);
        for (size_t i=0;i<4;++i) {
            const cf_cJSON *v=cf_cJSON_GetObjectItemCaseSensitive(j,keys[i+1]);
            if (!cf_cJSON_IsNumber(v) || v->valuedouble != v->valueint || v->valueint<0 || v->valueint>(i==3 ? 100 : 255)) valid=false;
            else values[i]=v->valueint;
        }
        if (!valid) rc=error(r,"400 Bad Request","RGB values must be integers 0-255; brightness 0-100; on must be true or false.");
        else if (monitor_led_set(cf_cJSON_IsTrue(on),values[0],values[1],values[2],values[3]) != ESP_OK)
            rc=error(r,"503 Service Unavailable","The built-in RGB LED driver is unavailable.");
        else rc=ok(r);
    }
    else if (!strcmp(r->uri,"/api/logout")) {
        cf_secure_zero(session,sizeof(session)); session_until=0; rc=ok(r);
    } else if (!strcmp(r->uri,"/api/scan") && cf_json_keys(j,NULL,0)) {
        rc = monitor_network_scan() == ESP_OK ? ok(r) : error(r,"409 Conflict","Wi-Fi is busy. Wait for the current connection or scan to finish.");
    } else rc = error(r,"400 Bad Request","Invalid operation.");
    cf_json_delete_secret(j); return rc;
}

static esp_err_t scan_get(api_context *r)
{
    if (!authorized(r)) return error(r,"401 Unauthorized","Unlock Settings with the device password.");
    monitor_ap aps[12]; bool busy;
    size_t count = monitor_network_scan_results(aps,12,&busy);
    cf_cJSON *j = cf_cJSON_CreateObject(); json_ok = j != NULL; b(j,"busy",busy);
    cf_cJSON *list = cf_cJSON_AddArrayToObject(j,"networks"); if (!list) json_ok=false;
    for (size_t i=0; list && i<count; ++i) {
        cf_cJSON *ap = cf_cJSON_CreateObject(); if (!ap) { json_ok=false; break; }
        cf_cJSON_AddItemToArray(list,ap); s(ap,"ssid",aps[i].ssid); n(ap,"rssi",aps[i].rssi); b(ap,"secure",aps[i].secure); n(ap,"channel",aps[i].channel);
    }
    return send_json(r,j);
}


void monitor_api_dispatch(const monitor_api_request *request, monitor_api_reply *reply)
{
    memset(reply,0,sizeof(*reply));
    api_context context={request,reply,request->path};
    api_context *r=&context;
    if(!request->post) {
        if(!strcmp(r->uri,"/api/status")) (void)status_get(r);
        else if(!strcmp(r->uri,"/api/scan")) (void)scan_get(r);
        else (void)error(r,"404 Not Found","API endpoint not found.");
    } else if(!strcmp(r->uri,"/api/login")) (void)login_post(r);
    else (void)change_post(r);
}

static unsigned pending_effects;
static uint64_t effects_after;
void monitor_api_schedule_effects(unsigned effects)
{
    if(effects) { pending_effects |= effects; effects_after=milliseconds()+1000; }
}
void monitor_api_tick(void)
{
    if(!monitor_json_lock || xSemaphoreTake(monitor_json_lock,0)!=pdTRUE) return;
    unsigned effects=0;
    monitor_ota_tick();
    monitor_tunnel_tick();
    if(pending_effects && milliseconds()>=effects_after) { effects=pending_effects; pending_effects=0; }
    xSemaphoreGive(monitor_json_lock);
    if(effects & MONITOR_EFFECT_WIFI) monitor_network_reload();
    if(effects & MONITOR_EFFECT_TUNNEL) monitor_tunnel_reload();
    if(effects & MONITOR_EFFECT_TUNNEL_RESTART) monitor_tunnel_restart();
    if(effects & MONITOR_EFFECT_REBOOT) esp_restart();
}
