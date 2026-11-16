#ifndef FIPSD_GATEWAY_CONTROL_H
#define FIPSD_GATEWAY_CONTROL_H
/* The gateway control socket, the port of upstream src/gateway/control.rs
 * (nostrc-iq1n9.3): a Unix stream socket at /run/fips/gateway.sock
 * (root:fips, 0770) with two commands, show_gateway and show_mappings,
 * answered from the latest snapshot the pool tick published. The protocol is
 * the daemon's: one JSON line in, one JSON line out, then close, with the
 * same size limit, I/O deadline and response encoding. */
#include "gateway/pool.h"
#include <jansson.h>
#include <nsync.h>
#include <stdatomic.h>

#define FIPSD_GW_CONTROL_SOCKET "/run/fips/gateway.sock"
#define FIPSD_GW_CONTROL_MAX_REQUEST 4096u
#define FIPSD_GW_CONTROL_IO_TIMEOUT_MS 5000

/* What the tick publishes; immutable once published, shared by reference. */
typedef struct fipsd_gw_snapshot {
    atomic_uint refs;
    fipsd_gw_pool_status pool;
    fipsd_gw_mapping_info *mappings; size_t nmappings;
    size_t nat_mappings;
    char dns_listen[128];
    uint64_t uptime_secs;
    char pool_cidr[64], lan_interface[64], dns_upstream[128];
    uint32_t dns_ttl;
    uint64_t pool_grace_period;
} fipsd_gw_snapshot;
/* The static half of a snapshot, from the configuration. */
typedef struct fipsd_gw_snapshot_config {
    const char *pool_cidr, *lan_interface, *dns_upstream, *dns_listen;
    uint32_t dns_ttl;
    uint64_t pool_grace_period;
} fipsd_gw_snapshot_config;
/* A new snapshot with one reference; takes mappings. NULL on allocation failure. */
fipsd_gw_snapshot *fipsd_gw_snapshot_build(const fipsd_gw_pool_status *status, fipsd_gw_mapping_info *mappings, size_t nmappings,
                                           size_t nat_mappings, uint64_t uptime_secs, const fipsd_gw_snapshot_config *config);
void fipsd_gw_snapshot_release(fipsd_gw_snapshot *s);

/* The latest snapshot; NULL until the first tick. */
typedef struct fipsd_gw_snapshot_slot { nsync_mu mu; fipsd_gw_snapshot *current; } fipsd_gw_snapshot_slot;
void fipsd_gw_snapshot_slot_init(fipsd_gw_snapshot_slot *slot);
/* Publish s (taking its reference), releasing the one it replaces. */
void fipsd_gw_snapshot_publish(fipsd_gw_snapshot_slot *slot, fipsd_gw_snapshot *s);
/* A reference to the latest snapshot, or NULL. */
fipsd_gw_snapshot *fipsd_gw_snapshot_acquire(fipsd_gw_snapshot_slot *slot);
void fipsd_gw_snapshot_slot_clear(fipsd_gw_snapshot_slot *slot);

const char *fipsd_gw_mapping_state_str(fipsd_gw_mapping_state state);
/* The responses, owned. */
json_t *fipsd_gw_control_show_gateway(const fipsd_gw_snapshot *s);
json_t *fipsd_gw_control_show_mappings(const fipsd_gw_snapshot *s);
/* Dispatch a command against a snapshot (NULL: not yet initialized). */
json_t *fipsd_gw_control_dispatch(const char *command, const fipsd_gw_snapshot *s);
/* Answer one request line; the encoded response line (no newline), malloc'd. */
char *fipsd_gw_control_answer(const char *line, size_t len, fipsd_gw_snapshot_slot *slot);

/* The control socket service. */
typedef struct fipsd_gw_control {
    char path[256];
    int listener;
    uint64_t device, inode; bool bound;
    fipsd_gw_snapshot_slot *slot;
    atomic_bool stopping;
    atomic_uint connections;
} fipsd_gw_control;
/* Bind path under the shared FIPS access policy: create the parent, remove a
 * stale socket (never a live one or another file), mode 0770 and the fips
 * group. 0, or -1 with errno and a message in err. */
int fipsd_gw_control_bind(fipsd_gw_control *c, const char *path, fipsd_gw_snapshot_slot *slot, char *err, size_t cap);
/* Accept and answer connections until stopped; each connection is served on
 * a fiber of its own when spawn is given, in line otherwise. */
typedef int (*fipsd_gw_spawn_fn)(void *user, void (*fn)(void *), void *arg);
void fipsd_gw_control_accept_loop(fipsd_gw_control *c, fipsd_gw_spawn_fn spawn, void *user);
/* Serve one accepted connection (owns fd). */
void fipsd_gw_control_serve_fd(fipsd_gw_control *c, int fd);
void fipsd_gw_control_stop(fipsd_gw_control *c);
/* Close the listener and remove the socket file this service bound. */
void fipsd_gw_control_clear(fipsd_gw_control *c);
#endif
