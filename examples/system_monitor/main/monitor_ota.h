#ifndef MONITOR_OTA_H
#define MONITOR_OTA_H
#include <stdbool.h>
#include <stddef.h>
#define MONITOR_OTA_CHUNK 1408u
typedef struct {
    bool available, active, ready;
    size_t received, total, capacity;
    char running[17], target[17], version[33], message[112];
} monitor_ota_status;
/* API calls/tick run under monitor_json_lock; no extra task or whole-image buffer. */
bool monitor_ota_start(size_t size, const char *sha256, char id[33]);
bool monitor_ota_write(const char *id, size_t offset, const unsigned char *bytes, size_t size);
bool monitor_ota_finish(const char *id);
bool monitor_ota_abort(const char *id);
void monitor_ota_snapshot(monitor_ota_status *out);
void monitor_ota_tick(void);
void monitor_ota_confirm_boot(void);
#endif
