#include "monitor.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cf_core.h"
#include <string.h>
#include <stdlib.h>

static SemaphoreHandle_t store_lock;

esp_err_t monitor_store_init(void)
{
    store_lock = xSemaphoreCreateMutex();
    if (!store_lock) return ESP_ERR_NO_MEM;
    /* Never erase an existing partition on an init error. The flash tool
     * provisions dedicated partitions after a complete backup. */
    esp_err_t rc = nvs_flash_init();
    if (rc != ESP_OK) return rc;
    return nvs_flash_init_partition("monitor_cfg");
}

static esp_err_t blob(const char *key, void *value, size_t size, bool write)
{
    nvs_handle_t handle;
    esp_err_t rc;
    if (xSemaphoreTake(store_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    rc = nvs_open_from_partition("monitor_cfg", "monitor", write ? NVS_READWRITE : NVS_READONLY, &handle);
    if (rc == ESP_OK) {
        if (write) {
            rc = nvs_set_blob(handle, key, value, size);
            if (rc == ESP_OK) rc = nvs_commit(handle);
        } else {
            size_t actual = size;
            rc = nvs_get_blob(handle, key, value, &actual);
            if (rc == ESP_OK && actual != size) rc = ESP_ERR_INVALID_SIZE;
        }
        nvs_close(handle);
    }
    xSemaphoreGive(store_lock);
    return rc;
}

esp_err_t monitor_store_password(char *out, size_t cap)
{
    nvs_handle_t h;
    esp_err_t rc;
    if (!out || cap < 17) return ESP_ERR_INVALID_ARG;
    memset(out, 0, cap);
    if (xSemaphoreTake(store_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    rc = nvs_open_from_partition("monitor_cfg", "monitor", NVS_READONLY, &h);
    if (rc == ESP_OK) {
        rc = nvs_get_str(h, "setup_pass", out, &cap);
        nvs_close(h);
    }
    xSemaphoreGive(store_lock);
    if (rc == ESP_OK && strlen(out) < 12) rc = ESP_ERR_INVALID_SIZE;
    return rc;
}

esp_err_t monitor_store_wifi(monitor_wifi_config *out)
{
    memset(out, 0, sizeof(*out));
    esp_err_t rc = blob("wifi_v1", out, sizeof(*out), false);
    if (rc != ESP_OK || !memchr(out->ssid, 0, sizeof(out->ssid)) ||
        !memchr(out->password, 0, sizeof(out->password)) || out->open > 1) {
        cf_secure_zero(out, sizeof(*out));
        return rc == ESP_OK ? ESP_ERR_INVALID_ARG : rc;
    }
    return ESP_OK;
}

esp_err_t monitor_save_wifi(const monitor_wifi_config *config)
{
    return blob("wifi_v1", (void *)config, sizeof(*config), true);
}

esp_err_t monitor_store_tunnel(monitor_tunnel_config *out)
{
    memset(out, 0, sizeof(*out));
    esp_err_t rc = blob("tunnel_v1", out, sizeof(*out), false);
    if (rc != ESP_OK || !memchr(out->token, 0, sizeof(out->token)) ||
        !memchr(out->hostname, 0, sizeof(out->hostname))) {
        cf_secure_zero(out, sizeof(*out));
        return rc == ESP_OK ? ESP_ERR_INVALID_ARG : rc;
    }
    return ESP_OK;
}

esp_err_t monitor_save_tunnel(const monitor_tunnel_config *config)
{
    return blob("tunnel_v1", (void *)config, sizeof(*config), true);
}

cf_result monitor_credential_provider(cf_credentials *out)
{
    monitor_tunnel_config *config = calloc(1, sizeof(*config));
    uint8_t scratch[CF_TOKEN_JSON_MAX + 1];
    cf_result rc = CF_ERR_MEMORY;
    if (!out) { free(config); return CF_ERR_ARGUMENT; }
    cf_secure_zero(out, sizeof(*out));
    if (config && monitor_store_tunnel(config) == ESP_OK && config->token[0]) {
        rc = cf_credentials_parse((cf_bytes){(uint8_t *)config->token, strlen(config->token)},
                                   scratch, sizeof(scratch), out);
    } else if (config) rc = CF_ERR_STATE;
    if (config) { cf_secure_zero(config, sizeof(*config)); free(config); }
    cf_secure_zero(scratch, sizeof(scratch));
    return rc;
}
