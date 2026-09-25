#include "monitor.h"
#include "monitor_api.h"
#include "monitor_ota.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"

void app_main(void)
{
    ESP_LOGI("monitor", "ESP Monitor %s starting", MONITOR_VERSION);
    ESP_ERROR_CHECK(monitor_store_init());
    monitor_io_init();
    monitor_led_init();
    esp_err_t rc = monitor_network_start();
    if (rc != ESP_OK) {
        ESP_LOGE("monitor", "Network startup stopped: %s. No open access point was created.", esp_err_to_name(rc));
        ESP_ERROR_CHECK(rc); /* An unconfirmed OTA boot must fail, not idle forever. */
    }
    ESP_ERROR_CHECK(monitor_http_start());
    ESP_ERROR_CHECK(monitor_tunnel_start());
    monitor_ota_confirm_boot();
    ESP_LOGI("monitor", "HTTP monitor ready; free internal heap %u bytes",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    while (true) {
        monitor_api_tick();
        monitor_network_tick();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
