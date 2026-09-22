#ifndef MONITOR_REMOTE_H
#define MONITOR_REMOTE_H
#include "cf_h2.h"
#include "monitor_api.h"
typedef struct {
    char path[128], authorization[80];
    bool post, head, json_content, origin_allowed, has_length;
    size_t content_length;
} monitor_remote_head;
/* The transport has already checked the exact configured :authority. */
cf_result monitor_remote_parse(const cf_h2_request *request, monitor_remote_head *out);
#endif
