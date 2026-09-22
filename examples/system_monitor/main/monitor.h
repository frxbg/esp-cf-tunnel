#ifndef MONITOR_H
#define MONITOR_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_netif.h"
#include "cf_credentials.h"
#include "esp_cf_tunnel.h"

#define MONITOR_VERSION "0.3.2"
#define MONITOR_AP_IP "192.168.4.1"
#define MONITOR_SETUP_SECONDS 600
#define MONITOR_GPIO_COUNT 4

typedef struct { char ssid[33]; char password[64]; uint8_t open; } monitor_wifi_config;
typedef struct { char token[CF_TOKEN_MAX + 1]; char hostname[254]; } monitor_tunnel_config;
typedef struct {
    bool connected, connecting, associated, ap_enabled, scanning, scan_ready;
    unsigned retries, disconnect_reason, ap_clients;
    uint64_t ap_deadline_ms;
    char ip[16], gateway[16], ssid[33], ap_ssid[33];
    int rssi;
    unsigned channel;
} monitor_network_snapshot;
typedef struct { char ssid[33]; int rssi; bool secure; unsigned channel; } monitor_ap;
typedef struct { int pin, mode, level; } monitor_gpio;
typedef struct { bool available, on; unsigned red, green, blue, brightness; } monitor_rgb;

esp_err_t monitor_store_init(void);
esp_err_t monitor_store_password(char *out, size_t cap);
esp_err_t monitor_store_wifi(monitor_wifi_config *out);
esp_err_t monitor_save_wifi(const monitor_wifi_config *config);
esp_err_t monitor_store_tunnel(monitor_tunnel_config *out);
esp_err_t monitor_save_tunnel(const monitor_tunnel_config *config);
/* Provider reads the stored token only on demand, never through a GET API. */
cf_result monitor_credential_provider(cf_credentials *out);

esp_err_t monitor_network_start(void);
void monitor_network_tick(void);
void monitor_network_snapshot_get(monitor_network_snapshot *out);
void monitor_network_reload(void);
esp_err_t monitor_network_scan(void);
size_t monitor_network_scan_results(monitor_ap *out, size_t cap, bool *busy);
bool monitor_request_on_setup(int socket);
bool monitor_request_local_ip(int socket, char out[16]);
void monitor_io_init(void);
void monitor_io_snapshot(monitor_gpio out[MONITOR_GPIO_COUNT]);
esp_err_t monitor_io_set(int pin, int mode, int value);
float monitor_temperature(bool *valid);
void monitor_led_init(void);
void monitor_led_snapshot(monitor_rgb *out);
esp_err_t monitor_led_set(bool on, unsigned red, unsigned green, unsigned blue, unsigned brightness);
esp_err_t monitor_http_start(void);
extern SemaphoreHandle_t monitor_json_lock;
esp_err_t monitor_tunnel_start(void);
void monitor_tunnel_reload(void);
void monitor_tunnel_snapshot(esp_cf_tunnel_snapshot *out);

typedef struct { const char *uri, *type; const uint8_t *data; size_t len; } monitor_asset;
extern const monitor_asset monitor_assets[];
extern const size_t monitor_asset_count;
#endif
