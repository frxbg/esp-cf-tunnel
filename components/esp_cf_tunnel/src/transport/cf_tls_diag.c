#include "cf_tls_diag.h"
#include <string.h>
#if CONFIG_ESP_TLS_USING_MBEDTLS
#include "mbedtls/ssl.h"
#endif

const char *esp_cf_connect_stage_name(esp_cf_connect_stage stage)
{
    static const char *const names[] = {"unknown", "tcp_connect", "tls_setup",
        "tls_handshake", "application_deadline", "verified"};
    return (unsigned)stage < sizeof(names) / sizeof(names[0]) ? names[stage] : "unknown";
}

void cf_tls_diag_capture(esp_cf_connect_diagnostic *out, esp_tls_t *tls, int rc,
                         int errno_context, int before, int after, bool deadline)
{
    memset(out, 0, sizeof(*out));
    out->valid = true; out->rc = rc; out->errno_context = errno_context;
    out->state_before = before; out->state_after = after;
    out->tls_alert = out->tls_version = -1;
#if CONFIG_ESP_TLS_USING_MBEDTLS
    mbedtls_ssl_context *ssl = tls ? esp_tls_get_ssl_context(tls) : NULL;
    if (ssl) {
        if (rc < 0) {
            int alert = mbedtls_ssl_get_fatal_alert(ssl);
            out->tls_alert = alert >= 0 ? alert : -1;
        }
        if (rc == 1) out->tls_version = mbedtls_ssl_get_version_number(ssl);
    }
#endif
    esp_tls_error_handle_t errors = NULL;
    if (tls && esp_tls_get_error_handle(tls, &errors) == ESP_OK && errors) {
        /* Capture SYSTEM before the clearing ESP/TLS/verification accessor. */
        out->errors_available = esp_tls_get_and_clear_error_type(errors,
            ESP_TLS_ERR_TYPE_SYSTEM, &out->system_error) == ESP_OK;
        out->esp_error = esp_tls_get_and_clear_last_error(errors,
            &out->tls_error, &out->verify_flags);
    }
    if (rc == 1) out->stage = ESP_CF_CONNECT_VERIFIED;
    else if (rc == 0 && deadline) out->stage = ESP_CF_CONNECT_DEADLINE;
    else if (rc < 0) {
        if (before == ESP_TLS_HANDSHAKE || after == ESP_TLS_HANDSHAKE ||
            out->esp_error == ESP_ERR_MBEDTLS_SSL_HANDSHAKE_FAILED)
            out->stage = ESP_CF_CONNECT_TLS_HANDSHAKE;
        else if (out->esp_error >= ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME &&
                 out->esp_error <= ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT)
            out->stage = ESP_CF_CONNECT_TCP;
        else if (out->esp_error || out->tls_error || out->verify_flags)
            out->stage = ESP_CF_CONNECT_TLS_SETUP;
        /* With no recorded error, INIT/CONNECTING -> FAIL is ambiguous:
         * TCP, TLS setup and handshake may all execute in one SDK call. */
    }
}
