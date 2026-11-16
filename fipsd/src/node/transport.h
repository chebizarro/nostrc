#ifndef FIPSD_TRANSPORT_H
#define FIPSD_TRANSPORT_H
#include "runtime.h"
#include <fips/transport/types.h>

#define FIPSD_TRANSPORT_MAX 32u
typedef struct fipsd_node_context fipsd_node_context;
typedef enum fipsd_transport_state {
    FIPSD_TRANSPORT_STOPPED, FIPSD_TRANSPORT_ABSENT,
    FIPSD_TRANSPORT_BOUND, FIPSD_TRANSPORT_FAILED
} fipsd_transport_state;
typedef enum fipsd_transport_event_kind {
    FIPSD_TRANSPORT_PACKET, FIPSD_TRANSPORT_DISCOVERED,
    FIPSD_TRANSPORT_CONNECTED, FIPSD_TRANSPORT_DISCONNECTED,
    FIPSD_TRANSPORT_BECAME_ABSENT, FIPSD_TRANSPORT_BECAME_PRESENT,
    FIPSD_TRANSPORT_EXITED, FIPSD_TRANSPORT_MEDIUM_CHANGED
} fipsd_transport_event_kind;
typedef struct fipsd_transport_event {
    fipsd_transport_event_kind kind;
    fips_transport_id id;
    fips_transport_addr address;
    size_t size;
    uint8_t bytes[];
} fipsd_transport_event;

typedef struct fipsd_transport fipsd_transport;
struct fipsd_netmon_event;
typedef struct fipsd_transport_ops {
    /* On a fiber. Nonblocking start: launch binder/receive tasks through rt.
     * Missing interface is success with state ABSENT, not a startup failure.
     * Binder owns detach/rebind; supervisor never restarts a transport.
     * Send malloc'd events via events with root cancellation; failed send
     * keeps ownership. No borrowed packets. All tasks must stop on root done.
     * Partial failure must unwind itself, including any spawned tasks. */
    int (*start_async)(fipsd_transport *, fipsd_runtime *,
                       const fipsd_node_context *, GoChannel *events);
    /* Prompt, idempotent cancellation/unbind request; no blocking join.
     * Runtime's tracked-fiber grace is the join boundary. */
    void (*stop)(fipsd_transport *);
    fipsd_transport_state (*state)(const fipsd_transport *);
    uint16_t (*mtu)(const fipsd_transport *, const fips_transport_addr *);
    /* bytes/to are borrowed through return ONLY. An async/buffered transport
     * must copy or take its own owned message before returning (packet slabs
     * can be wiped/reused immediately). No retained caller buffer pointers. */
    int (*send)(fipsd_transport *, const fips_transport_addr *, const uint8_t *, size_t);
    int (*discover)(fipsd_transport *);
    int (*connect_async)(fipsd_transport *, const fips_transport_addr *);
    void (*close_connection)(fipsd_transport *, const fips_transport_addr *);
    /* Dispatcher notification to binder; callback must enqueue/coalesce work,
     * not race binder-owned descriptor state. Snapshot is loaded from context. */
    void (*network_changed)(fipsd_transport *, const struct fipsd_netmon_event *);
    /* Dispatcher-only, AFTER Noise/AEAD authentication. Datagram transports
     * may promote a numeric endpoint into a connected socket; never called
     * merely because an unauthenticated datagram arrived. Best effort. */
    void (*authenticated)(fipsd_transport *, const fips_transport_addr *, fips_link_direction);
    bool (*local_addr)(const fipsd_transport *, fips_transport_addr *);
} fipsd_transport_ops;
struct fipsd_transport {
    const fipsd_transport_ops *ops;
    fips_transport_id id;
    const char *type, *name; /* borrowed stable metadata */
    bool auto_connect, accept_connections;
    void *user;
    /* Optional interface binding, NULL for unbound transports (including
     * loopback by default). Borrowed immutable name, set before registration
     * and valid through runtime join; binder still owns descriptors/rebinding.
     * Loopback tests may supply a fake name without touching host interfaces. */
    const char *interface_name;
    /* Append-only shared instance; NULL for legacy/custom implementations. */
    struct fipsd_transport_shared *shared;
};
typedef struct fipsd_transport_registry {
    fipsd_transport *items[FIPSD_TRANSPORT_MAX];
    size_t count;
    bool sealed;
} fipsd_transport_registry;
/* Registration is single-threaded, before lifecycle start. Borrowed objects
 * outlive runtime join. Duplicate ids/names, incomplete vtables and late adds
 * are refused without mutation. No hidden default/network transports. */
int fipsd_transport_register(fipsd_transport_registry *, fipsd_transport *);
fipsd_transport *fipsd_transport_find(const fipsd_transport_registry *, fips_transport_id);
#endif
