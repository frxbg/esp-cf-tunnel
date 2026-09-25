#ifndef CF_TLS_DIAG_H
#define CF_TLS_DIAG_H
#include "esp_cf_tunnel.h"
#include "esp_tls.h"
/* Called once at a terminal connect outcome, before destroying the handle.
 * The caller adds peer, monotonic/UTC timestamps and heap measurements. */
void cf_tls_diag_capture(esp_cf_connect_diagnostic *out, esp_tls_t *tls, int rc,
                         int errno_context, int before, int after, bool deadline);
#endif
