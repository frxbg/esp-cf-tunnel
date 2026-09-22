#ifndef CF_JSON_H
#define CF_JSON_H
#include "cf_core.h"
#include "cf_cJSON.h"
cf_cJSON *cf_json_parse(cf_bytes input);
cf_result cf_json_failure(void);
void cf_json_delete_secret(cf_cJSON *root);
/* Allowed exact property names; duplicates and unknown properties rejected. */
bool cf_json_keys(const cf_cJSON *object, const char *const *allowed, size_t count);
#endif
