#include "monitor_ota.h"
#include "cf_core.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_log.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

static struct {
    monitor_ota_status status;
    const esp_partition_t *partition;
    esp_ota_handle_t handle;
    bool open;
    char id[33];
    unsigned char expected[32];
    psa_hash_operation_t hash;
    uint64_t activity_ms;
} upload;
static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time()/1000; }
static bool fail(const char *reason)
{ snprintf(upload.status.message,sizeof(upload.status.message),"%s",reason); return false; }
static void release(void)
{
    if (upload.open) (void)esp_ota_abort(upload.handle);
    upload.open=false; upload.status.active=false;
    (void)psa_hash_abort(&upload.hash);
    cf_secure_zero(upload.id,sizeof(upload.id));
    cf_secure_zero(upload.expected,sizeof(upload.expected));
}
static bool matches(const char *id)
{ return upload.status.active && id && strlen(id)==32 && !memcmp(id,upload.id,32); }
static int hex(unsigned char c)
{ return c>='0' && c<='9' ? c-'0' : c>='a' && c<='f' ? c-'a'+10 : c>='A' && c<='F' ? c-'A'+10 : -1; }
void monitor_ota_snapshot(monitor_ota_status *out)
{
    *out=upload.status;
    const esp_partition_t *running=esp_ota_get_running_partition(), *next=esp_ota_get_next_update_partition(NULL);
    out->available=next && running && next->address!=running->address;
    out->capacity=next ? next->size : 0;
    snprintf(out->running,sizeof(out->running),"%s",running ? running->label : "unknown");
    if (!out->message[0]) snprintf(out->message,sizeof(out->message),"%s",out->available ? "Ready for a firmware image." : "Install the OTA partition layout over USB first.");
}
bool monitor_ota_start(size_t size, const char *sha256, char id[33])
{
    if (upload.status.active || upload.status.ready) return fail("An update is already in progress or awaiting restart.");
    monitor_ota_status status; monitor_ota_snapshot(&status);
    if (!status.available) return fail("OTA partitions unavailable. Install the OTA layout over USB first.");
    if (size<sizeof(esp_image_header_t)+sizeof(esp_image_segment_header_t)+sizeof(esp_app_desc_t) || size>status.capacity)
        return fail("The firmware size is outside the inactive OTA slot capacity.");
    if (!sha256 || strlen(sha256)!=64) return fail("A 64-character SHA256 digest is required.");
    unsigned char expected[32];
    for(size_t i=0;i<32;++i) {
        int a=hex(sha256[2*i]), b=hex(sha256[2*i+1]);
        if(a<0 || b<0) return fail("Invalid SHA256 digest.");
        expected[i]=(unsigned char)((a<<4)|b);
    }
    memset(&upload,0,sizeof(upload)); upload.status.active=true; upload.status.total=size;
    upload.partition=esp_ota_get_next_update_partition(NULL);
    memcpy(upload.expected,expected,sizeof(expected)); cf_secure_zero(expected,sizeof(expected));
    uint8_t random[16]; esp_fill_random(random,sizeof(random));
    for(size_t i=0;i<sizeof(random);++i) snprintf(upload.id+2*i,3,"%02x",random[i]);
    memcpy(id,upload.id,sizeof(upload.id)); cf_secure_zero(random,sizeof(random));
    snprintf(upload.status.target,sizeof(upload.status.target),"%s",upload.partition->label);
    upload.activity_ms=now_ms(); upload.hash=psa_hash_operation_init();
    if(psa_crypto_init()!=PSA_SUCCESS || psa_hash_setup(&upload.hash,PSA_ALG_SHA_256)!=PSA_SUCCESS) {release();return fail("SHA256 initialization failed.");}
    snprintf(upload.status.message,sizeof(upload.status.message),"Upload started."); return true;
}
bool monitor_ota_write(const char *id, size_t offset, const unsigned char *bytes, size_t size)
{
    if(!matches(id)) return fail("The upload session is not active.");
    if(offset!=upload.status.received || !bytes || !size || size>MONITOR_OTA_CHUNK || size>upload.status.total-upload.status.received)
        return fail("Invalid chunk size or offset; upload was not advanced.");
    if(!upload.open) {
        esp_image_header_t header; esp_app_desc_t app;
        size_t prefix=sizeof(header)+sizeof(esp_image_segment_header_t);
        if(size<prefix+sizeof(app)) return fail("The first chunk must include the firmware descriptor.");
        memcpy(&header,bytes,sizeof(header)); memcpy(&app,bytes+prefix,sizeof(app));
        const esp_app_desc_t *current=esp_app_get_description();
        if(header.magic!=ESP_IMAGE_HEADER_MAGIC || header.chip_id!=CONFIG_IDF_FIRMWARE_CHIP_ID || !header.hash_appended ||
           app.magic_word!=ESP_APP_DESC_MAGIC_WORD || memcmp(app.project_name,current->project_name,sizeof(app.project_name))) {
            release();return fail("Use an ESP32-S3 ESP Monitor application .bin, not a merged flash image.");
        }
        snprintf(upload.status.version,sizeof(upload.status.version),"%.*s",(int)sizeof(app.version),app.version);
        /* Incremental erase keeps each bounded request short. The running slot,
         * partition table, bootloader, Wi-Fi and tunnel NVS are never written. */
        if(esp_ota_begin(upload.partition,OTA_WITH_SEQUENTIAL_WRITES,&upload.handle)!=ESP_OK) {
            release();return fail("Unable to open the inactive OTA slot.");
        }
        upload.open=true;
    }
    if(esp_ota_write(upload.handle,bytes,size)!=ESP_OK || psa_hash_update(&upload.hash,bytes,size)!=PSA_SUCCESS) {
        release();return fail("Firmware write failed; the running image is unchanged.");
    }
    upload.status.received+=size; upload.activity_ms=now_ms(); return true;
}
bool monitor_ota_finish(const char *id)
{
    if(!matches(id) || !upload.open || upload.status.received!=upload.status.total) return fail("The firmware upload is incomplete.");
    unsigned char actual[32]; size_t hash_size=0;
    if(psa_hash_finish(&upload.hash,actual,sizeof(actual),&hash_size)!=PSA_SUCCESS || hash_size!=sizeof(actual) || memcmp(actual,upload.expected,sizeof(actual))) {
        cf_secure_zero(actual,sizeof(actual)); release();return fail("SHA256 mismatch; the running image is unchanged.");
    }
    cf_secure_zero(actual,sizeof(actual));
    esp_err_t result=esp_ota_end(upload.handle); upload.open=false;
    if(result!=ESP_OK) {release();return fail("ESP image validation failed; the running image is unchanged.");}
    result=esp_ota_set_boot_partition(upload.partition);
    release();
    if(result!=ESP_OK) return fail("Unable to select the verified image for the next boot.");
    upload.status.ready=true;
    snprintf(upload.status.message,sizeof(upload.status.message),"Firmware verified. Restarting into the updated image.");
    return true;
}
bool monitor_ota_abort(const char *id)
{
    if(!matches(id)) return fail("The upload session is not active.");
    release();snprintf(upload.status.message,sizeof(upload.status.message),"Upload cancelled; the running image is unchanged.");return true;
}
void monitor_ota_tick(void)
{
    if(upload.status.active && now_ms()-upload.activity_ms>60000) {
        release();(void)fail("Upload expired after 60 seconds without a chunk.");
    }
}
void monitor_ota_confirm_boot(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *running=esp_ota_get_running_partition();
    if(running && esp_ota_get_state_partition(running,&state)==ESP_OK && state==ESP_OTA_IMG_PENDING_VERIFY)
        ESP_ERROR_CHECK(esp_ota_mark_app_valid_cancel_rollback());
    ESP_LOGI("monitor_ota","Boot confirmed after storage, network, HTTP and tunnel task initialization.");
}
