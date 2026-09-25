#include "monitor.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <string.h>
#include <stdio.h>

static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static monitor_network_snapshot state;
static monitor_ap scan_list[12];
static size_t scan_count;
static bool reload_requested, scan_requested, scan_finished, clock_requested;
static bool has_credentials;
static uint64_t next_retry, button_since, connection_deadline;
static bool button_handled;
static esp_netif_t *ap_netif, *sta_netif;

static uint64_t now_ms(void) { return (uint64_t)esp_timer_get_time() / 1000; }

static void clock_synced(struct timeval *tv)
{
    portENTER_CRITICAL(&mux);
    ++state.sntp_sync_count; state.sntp_last_sync_utc = tv->tv_sec;
    portEXIT_CRITICAL(&mux);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    uint64_t now = now_ms();
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *ip = data;
        char addr[16], gateway[16];
        snprintf(addr, sizeof(addr), IPSTR, IP2STR(&ip->ip_info.ip));
        snprintf(gateway, sizeof(gateway), IPSTR, IP2STR(&ip->ip_info.gw));
        portENTER_CRITICAL(&mux);
        state.connected = true;
        state.connecting = false;
        state.retries = 0;
        connection_deadline = 0;
        state.disconnect_reason = 0;
        memcpy(state.ip, addr, sizeof(addr));
        memcpy(state.gateway, gateway, sizeof(gateway));
        if (state.ap_enabled)
            state.ap_deadline_ms = now + MONITOR_SETUP_SECONDS * 1000ULL;
        clock_requested = true;
        portEXIT_CRITICAL(&mux);
        ESP_LOGI("monitor", "Station connected, address %s", addr);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED)
    {
        portENTER_CRITICAL(&mux);
        state.associated = true;
        state.connecting = true;
        connection_deadline = now + 20000;
        portEXIT_CRITICAL(&mux);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = data;
        uint32_t jitter = esp_random() % 1000;
        portENTER_CRITICAL(&mux);
        state.connected = state.connecting = false;
        state.associated = false;
        connection_deadline = 0;
        state.ip[0] = state.gateway[0] = 0;
        state.disconnect_reason = event->reason;
        if (state.retries < 1000000)
            ++state.retries;
        unsigned exponent = state.retries > 5 ? 5 : state.retries;
        next_retry = now + (1000u << exponent) + jitter;
        state.ap_deadline_ms = 0;
        portEXIT_CRITICAL(&mux);
        ESP_LOGW("monitor", "Station disconnected, reason %u", (unsigned)event->reason);
    }
    else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE)
    {
        portENTER_CRITICAL(&mux);
        scan_finished = true;
        portEXIT_CRITICAL(&mux);
    }
}

static esp_err_t set_station(void)
{
    monitor_wifi_config saved;
    wifi_config_t config = {0};
    bool available = monitor_store_wifi(&saved) == ESP_OK && saved.ssid[0];
    if (available)
    {
        memcpy(config.sta.ssid, saved.ssid, strlen(saved.ssid));
        memcpy(config.sta.password, saved.password, strlen(saved.password));
        config.sta.threshold.authmode = saved.open ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }
    esp_err_t rc = esp_wifi_set_config(WIFI_IF_STA, &config);
    portENTER_CRITICAL(&mux);
    has_credentials = available && rc == ESP_OK;
    memcpy(state.ssid, saved.ssid, sizeof(state.ssid));
    state.retries = 0;
    next_retry = now_ms() + 1000;
    portEXIT_CRITICAL(&mux);
    cf_secure_zero(&saved, sizeof(saved));
    cf_secure_zero(&config, sizeof(config));
    return rc;
}

static void setup_enable(void)
{
    if (esp_wifi_set_mode(WIFI_MODE_APSTA) != ESP_OK)
        return;
    portENTER_CRITICAL(&mux);
    state.ap_enabled = true;
    state.ap_deadline_ms = state.connected ? now_ms() + MONITOR_SETUP_SECONDS * 1000ULL : 0;
    portEXIT_CRITICAL(&mux);
    ESP_LOGI("monitor", "Setup access point available at " MONITOR_AP_IP);
}

esp_err_t monitor_network_start(void)
{
    esp_err_t rc;
    uint8_t mac[6];
    char password[64] = {0};
    if ((rc = monitor_store_password(password, sizeof(password))) != ESP_OK)
    {
        ESP_LOGE("monitor", "No setup password provisioned. Run tools/provision_monitor.py before flashing.");
        return rc;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ap_netif = esp_netif_create_default_wifi_ap();
    sta_netif = esp_netif_create_default_wifi_sta();
    if (!ap_netif || !sta_netif)
    {
        cf_secure_zero(password, sizeof(password));
        return ESP_ERR_NO_MEM;
    }
    esp_netif_set_hostname(sta_netif, "esp-monitor");
    esp_netif_set_hostname(ap_netif, "esp-monitor-setup");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    /* This USB-powered monitor needs predictable LAN latency. The default
     * WIFI_PS_MIN_MODEM delays reception until DTIM; disable modem sleep. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    /* Prefer a single 20 MHz channel for the modest traffic of this monitor.
     * Avoid HT40 negotiation/coexistence problems on crowded 2.4 GHz networks. */
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW20));
    ESP_ERROR_CHECK(esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20));
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(state.ap_ssid, sizeof(state.ap_ssid), "ESP-Monitor-%02X%02X%02X", mac[3], mac[4], mac[5]);
    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, state.ap_ssid, strlen(state.ap_ssid));
    ap.ap.ssid_len = strlen(state.ap_ssid);
    memcpy(ap.ap.password, password, strlen(password));
    ap.ap.channel = 6;
    ap.ap.max_connection = 3;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.pmf_cfg.required = false;
    rc = esp_wifi_set_config(WIFI_IF_AP, &ap);
    cf_secure_zero(password, sizeof(password));
    cf_secure_zero(&ap, sizeof(ap));
    if (rc != ESP_OK)
        return rc;
    if ((rc = set_station()) != ESP_OK)
        return rc;
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    sntp.start = false;
    sntp.sync_cb = clock_synced;
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp));
    gpio_config_t button = {.pin_bit_mask = 1ULL << 0, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE, .intr_type = GPIO_INTR_DISABLE};
    ESP_ERROR_CHECK(gpio_config(&button));
    ESP_ERROR_CHECK(esp_wifi_start());
    state.ap_enabled = true;
    ESP_LOGI("monitor", "Setup SSID %s, address " MONITOR_AP_IP "; secrets are never logged", state.ap_ssid);
    return ESP_OK;
}

void monitor_network_reload(void)
{
    portENTER_CRITICAL(&mux);
    reload_requested = true;
    portEXIT_CRITICAL(&mux);
}

esp_err_t monitor_network_scan(void)
{
    esp_err_t rc = ESP_OK;
    portENTER_CRITICAL(&mux);
    if (state.scanning || state.connecting)
        rc = ESP_ERR_INVALID_STATE;
    else
    {
        scan_requested = true;
        state.scanning = true;
        state.scan_ready = false;
    }
    portEXIT_CRITICAL(&mux);
    return rc;
}

size_t monitor_network_scan_results(monitor_ap *out, size_t cap, bool *busy)
{
    portENTER_CRITICAL(&mux);
    size_t n = cap < scan_count ? cap : scan_count;
    memcpy(out, scan_list, n * sizeof(*out));
    *busy = state.scanning;
    portEXIT_CRITICAL(&mux);
    return n;
}

void monitor_network_snapshot_get(monitor_network_snapshot *out)
{
    portENTER_CRITICAL(&mux);
    *out = state;
    portEXIT_CRITICAL(&mux);
    wifi_ap_record_t record;
    if (out->associated && esp_wifi_sta_get_ap_info(&record) == ESP_OK)
    {
        out->rssi = record.rssi;
        out->channel = record.primary;
    }
    wifi_sta_list_t clients;
    if (out->ap_enabled && esp_wifi_ap_get_sta_list(&clients) == ESP_OK)
        out->ap_clients = clients.num;
}

bool monitor_request_local_ip(int socket, char out[16])
{
    /* IDF HTTPD uses a dual-stack listener when IPv6 is enabled. IPv4 peers
     * then have an IPv4-mapped IPv6 local address, not sockaddr_in. */
    struct sockaddr_storage local = {0};
    socklen_t length = sizeof(local);
    if (getsockname(socket, (struct sockaddr *)&local, &length) != 0)
        return false;
    struct in_addr address;
    if (local.ss_family == AF_INET)
    {
        address = ((struct sockaddr_in *)&local)->sin_addr;
        return inet_ntop(AF_INET, &address, out, 16) != NULL;
    }
#if CONFIG_LWIP_IPV6
    if (local.ss_family == AF_INET6)
    {
        const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&local;
        if (memcmp(v6->sin6_addr.s6_addr, prefix, 12))
            return false;
        memcpy(&address, v6->sin6_addr.s6_addr + 12, 4);
        return inet_ntop(AF_INET, &address, out, 16) != NULL;
    }
#endif
    return false;
}

bool monitor_request_on_setup(int socket)
{
    char address[16];
    return monitor_request_local_ip(socket, address) && !strcmp(address, MONITOR_AP_IP);
}

void monitor_network_tick(void)
{
    uint64_t now = now_ms();
    bool reload, scan, finished, clock, connect, ap_needed, ap_expired, timed_out;
    portENTER_CRITICAL(&mux);
    reload = reload_requested;
    reload_requested = false;
    scan = scan_requested;
    scan_requested = false;
    finished = scan_finished;
    scan_finished = false;
    clock = clock_requested;
    clock_requested = false;
    connect = has_credentials && !state.connected && !state.connecting && !state.scanning && now >= next_retry;
    ap_needed = !state.connected && !state.ap_enabled;
    ap_expired = state.ap_enabled && state.connected && state.ap_deadline_ms && now >= state.ap_deadline_ms;
    timed_out = state.connecting && connection_deadline && now >= connection_deadline;
    if (timed_out)
    {
        state.connecting = false;
        connection_deadline = 0;
        next_retry = now + 5000;
    }
    portEXIT_CRITICAL(&mux);
    if (reload)
    {
        (void)esp_wifi_disconnect();
        esp_err_t rc = set_station();
        if (rc != ESP_OK)
            ESP_LOGE("monitor", "Unable to apply Wi-Fi settings: %s", esp_err_to_name(rc));
        setup_enable();
        connect = false;
    }
    if (clock)
        (void)esp_netif_sntp_start();
    if (timed_out)
    {
        ESP_LOGW("monitor", "Wi-Fi connection or DHCP timed out; reconnecting");
        (void)esp_wifi_disconnect();
        connect = false;
    }
    if (ap_needed)
        setup_enable();
    if (ap_expired && esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK)
    {
        portENTER_CRITICAL(&mux);
        state.ap_enabled = false;
        portEXIT_CRITICAL(&mux);
        ESP_LOGI("monitor", "Setup window closed. Hold BOOT for 3 seconds to reopen it.");
    }
    if (scan)
    {
        wifi_scan_config_t cfg = {.show_hidden = false};
        if (esp_wifi_scan_start(&cfg, false) != ESP_OK)
        {
            portENTER_CRITICAL(&mux);
            state.scanning = false;
            portEXIT_CRITICAL(&mux);
        }
    }
    if (finished)
    {
        wifi_ap_record_t records[12];
        uint16_t n = 12;
        esp_err_t rc = esp_wifi_scan_get_ap_records(&n, records);
        (void)esp_wifi_clear_ap_list();
        portENTER_CRITICAL(&mux);
        scan_count = rc == ESP_OK ? n : 0;
        for (size_t i = 0; i < scan_count; ++i)
        {
            memcpy(scan_list[i].ssid, records[i].ssid, 32);
            scan_list[i].ssid[32] = 0;
            scan_list[i].rssi = records[i].rssi;
            scan_list[i].secure = records[i].authmode != WIFI_AUTH_OPEN;
            scan_list[i].channel = records[i].primary;
        }
        state.scanning = false;
        state.scan_ready = rc == ESP_OK;
        portEXIT_CRITICAL(&mux);
    }
    if (connect)
    {
        esp_err_t rc = esp_wifi_connect();
        portENTER_CRITICAL(&mux);
        state.connecting = rc == ESP_OK;
        connection_deadline = rc == ESP_OK ? now + 30000 : 0;
        next_retry = now + 5000;
        portEXIT_CRITICAL(&mux);
    }
    if (gpio_get_level(0) == 0)
    {
        if (!button_since)
            button_since = now;
        if (!button_handled && now - button_since >= 3000)
        {
            setup_enable();
            button_handled = true;
        }
    }
    else
    {
        button_since = 0;
        button_handled = false;
    }
}
