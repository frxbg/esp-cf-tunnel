#ifndef TEST_ESP_TLS_H
#define TEST_ESP_TLS_H
#include "esp_err.h"
typedef enum { ESP_TLS_INIT, ESP_TLS_CONNECTING, ESP_TLS_HANDSHAKE, ESP_TLS_FAIL, ESP_TLS_DONE } esp_tls_conn_state_t;
typedef struct mock_errors { int esp, tls, flags, system; } *esp_tls_error_handle_t;
typedef struct { struct mock_errors errors; } esp_tls_t;
#define ESP_TLS_ERR_TYPE_SYSTEM 1
#define ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME 0x8001
#define ESP_ERR_ESP_TLS_FAILED_CONNECT_TO_HOST 0x8004
#define ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT 0x8006
#define ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED 0x801a
esp_err_t esp_tls_get_error_handle(esp_tls_t *, esp_tls_error_handle_t *);
esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t, int, int *);
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t, int *, int *);
#endif
