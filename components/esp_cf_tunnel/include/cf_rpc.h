#ifndef CF_RPC_H
#define CF_RPC_H
#include "cf_credentials.h"
#include "cf_capnp.h"

#define CF_RPC_TX_MAX 1024u
#define CF_REGISTRATION_INTERFACE UINT64_C(0xf71695ec7fe85497)
#define CF_RPC_REMOTE_CONFIG 1u
#define CF_RPC_SERIALIZED_HEADERS 2u
typedef enum {
    CF_RPC_IDLE, CF_RPC_BOOTSTRAPPING, CF_RPC_READY, CF_RPC_REGISTERING,
    CF_RPC_REGISTERED, CF_RPC_UNREGISTERING, CF_RPC_CLOSED,
    CF_RPC_REJECTED, CF_RPC_FAILED
} cf_rpc_state;
typedef struct {
    uint8_t connector_id[16]; /* Application-generated UUID, stable for this run. */
    cf_bytes local_ip;        /* 4 or 16 network-order bytes. */
    cf_bytes version;         /* Printable ASCII, 1..63 bytes. */
    cf_bytes arch;            /* Printable ASCII, 1..63 bytes. */
    uint8_t connection_index, previous_attempts;
    bool replace_existing;
    unsigned features;       /* Only the two supported CF_RPC_* flags. */
} cf_rpc_options;
typedef struct {
    cf_rpc_state state;
    uint32_t capability;
    bool have_capability, remotely_managed, should_retry;
    uint64_t retry_after_ms;
    uint8_t connection_id[16];
    char location[32];
    cf_result error;
} cf_rpc;

/* Single owner, one outstanding question, one imported senderHosted capability.
 * No RPC exports, promises, third parties, pipelining, or local-config method.
 * Registration success is NOT configuration readiness or tunnel ONLINE.
 * Every output buffer must have >= CF_RPC_TX_MAX capacity. Input/output must
 * not overlap. Queue the complete returned output before the next operation;
 * the transport owns partial writes, deadlines, fragmentation, and EOF.
 * Register output contains the secret and must be wiped after transmission.
 * Protocol failures produce Abort when possible, then require transport close. */
void cf_rpc_init(cf_rpc *rpc);
cf_result cf_rpc_begin(cf_rpc *rpc, uint8_t *out, size_t capacity, size_t *written);
cf_result cf_rpc_register(cf_rpc *rpc, const cf_credentials *credentials,
                          const cf_rpc_options *options,
                          uint8_t *out, size_t capacity, size_t *written);
cf_result cf_rpc_receive(cf_rpc *rpc, cf_bytes frame,
                         uint8_t *out, size_t capacity, size_t *written);
cf_result cf_rpc_unregister(cf_rpc *rpc, uint8_t *out, size_t capacity, size_t *written);
void cf_rpc_reset(cf_rpc *rpc); /* Use after transport close; forget all imports. */
#endif
