#include "monitor.h"
#include "led_strip.h"
#include "esp_log.h"

/* YD-ESP32-23 has one addressable WS2812 on GPIO48. The RGB solder jumper
 * must be connected. This pin is deliberately separate from generic GPIO. */
static led_strip_handle_t led;
static monitor_rgb state = {.red=0,.green=128,.blue=255,.brightness=20};

void monitor_led_init(void)
{
    led_strip_config_t config = {
        .strip_gpio_num=48, .max_leds=1,
        .led_model=LED_MODEL_WS2812,
        .color_component_format=LED_STRIP_COLOR_COMPONENT_FMT_GRB
    };
    led_strip_rmt_config_t rmt = {.resolution_hz=10000000,.flags.with_dma=false};
    esp_err_t rc = led_strip_new_rmt_device(&config,&rmt,&led);
    if (rc == ESP_OK) rc = led_strip_clear(led);
    if (rc != ESP_OK) {
        if (led) { led_strip_del(led); led=NULL; }
        ESP_LOGW("monitor","Built-in RGB LED unavailable: %s",esp_err_to_name(rc));
        return;
    }
    state.available=true;
    ESP_LOGI("monitor","Built-in RGB LED ready on GPIO48; starts off");
}

void monitor_led_snapshot(monitor_rgb *out) { *out=state; }

esp_err_t monitor_led_set(bool on, unsigned red, unsigned green, unsigned blue, unsigned brightness)
{
    if (!led) return ESP_ERR_INVALID_STATE;
    if (red>255 || green>255 || blue>255 || brightness>100) return ESP_ERR_INVALID_ARG;
    esp_err_t rc = led_strip_set_pixel(led,0,on ? red*brightness/100 : 0,
        on ? green*brightness/100 : 0,on ? blue*brightness/100 : 0);
    if (rc == ESP_OK) rc=led_strip_refresh(led);
    if (rc == ESP_OK) state=(monitor_rgb){.available=true,.on=on,.red=red,.green=green,.blue=blue,.brightness=brightness};
    return rc;
}
