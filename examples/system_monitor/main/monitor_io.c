#include "monitor.h"
#include "driver/gpio.h"
#include "driver/temperature_sensor.h"

/* Only explicitly selected, exposed general-purpose pins. USB 19/20,
 * flash/PSRAM 26..37, UART 43/44, boot/straps and RGB 48 are excluded.
 * Modes are volatile: every boot starts with all four outputs disabled. */
static monitor_gpio pins[MONITOR_GPIO_COUNT] = {{4,0,-1},{5,0,-1},{6,0,-1},{7,0,-1}};
static temperature_sensor_handle_t sensor;

void monitor_io_init(void)
{
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&cfg, &sensor) != ESP_OK) { sensor = NULL; return; }
    if (temperature_sensor_enable(sensor) != ESP_OK) {
        temperature_sensor_uninstall(sensor);
        sensor = NULL;
    }
    /* No GPIO direction/pull changes until an authenticated user chooses one. */
}

void monitor_io_snapshot(monitor_gpio out[MONITOR_GPIO_COUNT])
{
    for (unsigned i = 0; i < MONITOR_GPIO_COUNT; ++i) {
        out[i] = pins[i];
        if (pins[i].mode) out[i].level = gpio_get_level(pins[i].pin);
    }
}

esp_err_t monitor_io_set(int pin, int mode, int value)
{
    unsigned i;
    if (mode < 0 || mode > 2 || (value != 0 && value != 1)) return ESP_ERR_INVALID_ARG;
    for (i = 0; i < MONITOR_GPIO_COUNT && pins[i].pin != pin; ++i) {}
    if (i == MONITOR_GPIO_COUNT) return ESP_ERR_INVALID_ARG;
    if (mode == 2) {
        esp_err_t rc = gpio_set_level(pin, value);
        if (rc != ESP_OK) return rc;
    }
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << pin,
        .mode = mode == 2 ? GPIO_MODE_INPUT_OUTPUT : mode == 1 ? GPIO_MODE_INPUT : GPIO_MODE_DISABLE,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE};
    esp_err_t rc = gpio_config(&cfg);
    if (rc == ESP_OK) { pins[i].mode = mode; pins[i].level = mode ? gpio_get_level(pin) : -1; }
    return rc;
}

float monitor_temperature(bool *valid)
{
    float value = 0;
    *valid = sensor && temperature_sensor_get_celsius(sensor, &value) == ESP_OK;
    return value;
}
