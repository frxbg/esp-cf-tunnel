#include "cf_tls_diag.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks, order;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
esp_err_t esp_tls_get_error_handle(esp_tls_t *t, esp_tls_error_handle_t *h)
{ CHECK(order == 0); order = 1; *h = &t->errors; return ESP_OK; }
esp_err_t esp_tls_get_and_clear_error_type(esp_tls_error_handle_t h, int type, int *out)
{ CHECK(order == 1 && type == ESP_TLS_ERR_TYPE_SYSTEM); order = 2; *out = h->system; h->system = 0; return ESP_OK; }
esp_err_t esp_tls_get_and_clear_last_error(esp_tls_error_handle_t h, int *code, int *flags)
{ CHECK(order == 2); order = 3; int e = h->esp; *code = h->tls; *flags = h->flags; memset(h,0,sizeof(*h)); return e; }
static esp_cf_connect_diagnostic capture(int rc, int before, int after, bool deadline, struct mock_errors errors)
{
    esp_tls_t tls = {errors}; esp_cf_connect_diagnostic result; order = 0;
    cf_tls_diag_capture(&result,&tls,rc,5,before,after,deadline);
    CHECK(order == 3 && result.valid && result.errors_available);
    CHECK(result.system_error == errors.system && result.esp_error == errors.esp);
    CHECK(result.tls_error == errors.tls && result.verify_flags == errors.flags);
    CHECK(tls.errors.esp == 0 && tls.errors.system == 0);
    CHECK(result.errno_context == 5 && result.state_before == before && result.state_after == after);
    return result;
}
int main(void)
{
    esp_cf_connect_diagnostic d;
    d=capture(-1,1,3,false,(struct mock_errors){0x8004,0,0,111}); CHECK(d.stage == ESP_CF_CONNECT_TCP);
    d=capture(-1,0,3,false,(struct mock_errors){0x8015,0x2180,0,0}); CHECK(d.stage == ESP_CF_CONNECT_TLS_SETUP);
    d=capture(-1,1,3,false,(struct mock_errors){0x801a,0x2700,0x200,0}); CHECK(d.stage == ESP_CF_CONNECT_TLS_HANDSHAKE);
    d=capture(-1,2,3,true,(struct mock_errors){0,0,0,0}); CHECK(d.stage == ESP_CF_CONNECT_TLS_HANDSHAKE);
    d=capture(-1,1,3,false,(struct mock_errors){0,0,0,0}); CHECK(d.stage == ESP_CF_CONNECT_UNKNOWN);
    d=capture(0,1,1,true,(struct mock_errors){0,0,0,0}); CHECK(d.stage == ESP_CF_CONNECT_DEADLINE);
    d=capture(0,2,2,true,(struct mock_errors){0,0,0,0}); CHECK(d.stage == ESP_CF_CONNECT_DEADLINE);
    d=capture(1,2,4,true,(struct mock_errors){0,0,0,0}); CHECK(d.stage == ESP_CF_CONNECT_VERIFIED);
    cf_tls_diag_capture(&d,NULL,-1,0,-1,-1,false); CHECK(d.valid && !d.errors_available && d.stage == ESP_CF_CONNECT_UNKNOWN);
    CHECK(!strcmp(esp_cf_connect_stage_name(ESP_CF_CONNECT_DEADLINE),"application_deadline"));
    CHECK(!strcmp(esp_cf_connect_stage_name((esp_cf_connect_stage)99),"unknown"));
    printf("PASS: %u TLS diagnostic checks\n",checks); return 0;
}
