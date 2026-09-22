#include "cf_headers.h"
#include "cf_lifecycle.h"
#include "cf_config.h"
#include "cf_credentials.h"
#include "cf_capnp_framing.h"
#include "esp_cf_tunnel.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/* Compile/smoke example only. No network setup, credentials, or live tunnel.
 * Buffers are static so this example does not require a larger app_main stack. */
static uint8_t arena[128], scratch[CF_TOKEN_JSON_MAX + 1], frame_buffer[128];
static cf_header headers[4];
static cf_remote_config config;
static cf_credentials credentials;
static cf_capnp_framer framer;

static void environment(void *ctx, esp_cf_environment *out)
{ (void)ctx; memset(out, 0, sizeof(*out)); }
static cf_result no_credentials(void *ctx, cf_credentials *out, char hostname[254])
{ (void)ctx; (void)out; (void)hostname; return CF_ERR_STATE; }
static bool json_lock(void *ctx) { (void)ctx; return true; }
static void json_unlock(void *ctx) { (void)ctx; }
static cf_result request(void *ctx, cf_h2 *h, const cf_h2_request *r)
{ (void)ctx; (void)h; (void)r; return CF_ERR_UNSUPPORTED; }
static cf_result data(void *ctx, int32_t id, cf_bytes bytes, bool end)
{ (void)ctx; (void)id; (void)bytes; (void)end; return CF_ERR_UNSUPPORTED; }
static cf_result read_response(void *ctx, int32_t id, uint8_t *out, size_t cap, size_t *n, bool *eof)
{ (void)ctx; (void)id; (void)out; (void)cap; *n = 0; *eof = true; return CF_OK; }
static void closed(void *ctx, int32_t id, uint32_t error)
{ (void)ctx; (void)id; (void)error; }

static void transport_smoke(void)
{
    const esp_cf_tunnel_config options = {
        .environment = environment, .credentials = no_credentials,
        .json_try_lock = json_lock, .json_unlock = json_unlock,
        .request = request, .data = data, .read_response = read_response, .closed = closed,
        .version = "compile-smoke", .arch = CONFIG_IDF_TARGET "-espidf6.1",
        .service = "http://localhost:80", .application_streams = CONFIG_CF_TUNNEL_APPLICATION_STREAMS
    };
    esp_cf_tunnel *t;
    ESP_ERROR_CHECK(esp_cf_tunnel_init(&options, &t));
    ESP_ERROR_CHECK(esp_cf_tunnel_start(t));
    vTaskDelay(pdMS_TO_TICKS(20));
    esp_cf_tunnel_stop(t);
    esp_err_t rc = ESP_ERR_INVALID_STATE;
    for (unsigned i = 0; i < 100 && rc == ESP_ERR_INVALID_STATE; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
        rc = esp_cf_tunnel_deinit(t);
    }
    ESP_ERROR_CHECK(rc);
    ESP_LOGI("cf_smoke", "transport init/start/stop/deinit complete; network intentionally unavailable");
}

void app_main(void)
{
    static const char wire[] = "WC1FbXB0eQ:";
    static const char invalid_token[] = "e30="; /* {}: intentionally invalid synthetic token */
    cf_lifecycle life;
    size_t count;
    cf_result rc = cf_headers_decode((cf_bytes){(const uint8_t *)wire, sizeof(wire) - 1},
                                     headers, 4, arena, sizeof(arena), &count);
    cf_config_init(&config);
    cf_lifecycle_init(&life);
    (void)cf_lifecycle_start(&life);
    (void)cf_capnp_framer_init(&framer, frame_buffer, sizeof(frame_buffer));
    cf_result token_rc = cf_credentials_parse((cf_bytes){(const uint8_t *)invalid_token, sizeof(invalid_token) - 1},
                                              scratch, sizeof(scratch), &credentials);
    ESP_LOGI("cf_smoke", "codec=%d fields=%u invalid-token-rejected=%d state=%d",
             (int)rc, (unsigned)count, token_rc != CF_OK, (int)life.state);
    transport_smoke();
    ESP_LOGI("cf_smoke", "internal free=%u minimum=%u largest=%u (not tunnel overhead)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}
