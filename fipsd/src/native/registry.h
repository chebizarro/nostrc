#ifndef FIPSD_NATIVE_REGISTRY_H
#define FIPSD_NATIVE_REGISTRY_H
/* Which local ports the native API holds, and where an inbound datagram goes
 * (upstream src/native/registry.rs). Decisions over tables only: no I/O, no
 * clock read, no sockets. The caller passes the time in and performs whatever
 * the decision names, which is what lets the delivery rule be tested without
 * a wire and lets the same deliver serve the debug `arrive` command and the
 * real receive path. Sinks and arrival mailboxes are opaque to the registry.
 * The registry is the node's, not a connection's; api.c guards it with one
 * lock that is never held across fiber I/O. */
#include <fips/identity/mod.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How long a flow stays pending between the announcement and the listener
 * fiber wiring it: a hop inside the daemon, not a client round trip. */
#define FIPSD_NATIVE_PENDING_DEADLINE_MS 5000u
/* The fraction of max_flows one peer's inbound flows may hold, as a divisor. */
#define FIPSD_NATIVE_PEER_SHARE 4u

typedef struct fipsd_native_flow_key {
    fips_node_addr peer; /* the far end, by wire address */
    uint16_t remote;     /* the far end's port */
    uint16_t local;      /* this node's port */
} fipsd_native_flow_key;
bool fipsd_native_flow_key_eq(const fipsd_native_flow_key *a, const fipsd_native_flow_key *b);
typedef struct fipsd_native_limits { size_t per_flow, backlog, max_flows; } fipsd_native_limits;
/* Inbound flows one peer may hold at once, pending and accepted: a quarter of
 * max_flows, never zero. */
size_t fipsd_native_limits_per_peer(const fipsd_native_limits *limits);

typedef enum fipsd_native_registry_error {
    FIPSD_NATIVE_REG_OK = 0,
    FIPSD_NATIVE_REG_PORT_TAKEN,     /* EADDRINUSE */
    FIPSD_NATIVE_REG_FLOW_TAKEN,     /* EADDRINUSE */
    FIPSD_NATIVE_REG_NO_PORT,        /* EADDRNOTAVAIL */
    FIPSD_NATIVE_REG_TOO_MANY_FLOWS, /* EMFILE */
    FIPSD_NATIVE_REG_BACKLOG_FULL,   /* ECONNREFUSED */
    FIPSD_NATIVE_REG_NO_PENDING,     /* ECONNREFUSED */
    FIPSD_NATIVE_REG_PORT_PROTOCOL,  /* EADDRNOTAVAIL */
    FIPSD_NATIVE_REG_PORT_STANDARD   /* EADDRNOTAVAIL */
} fipsd_native_registry_error;
/* A refusal with what its message names. */
typedef struct fipsd_native_refusal {
    fipsd_native_registry_error error;
    uint16_t port, local, remote;
    uint64_t flow;
    size_t max;
} fipsd_native_refusal;
const char *fipsd_native_registry_errno(fipsd_native_registry_error error);
void fipsd_native_refusal_message(const fipsd_native_refusal *r, char *out, size_t cap);

/* One datagram, owned by whoever holds the pointer; wiped on free (L4). */
typedef struct fipsd_native_datagram { size_t len; uint8_t bytes[]; } fipsd_native_datagram;
fipsd_native_datagram *fipsd_native_datagram_new(const uint8_t *data, size_t len);
void fipsd_native_datagram_free(fipsd_native_datagram *d);

/* What a listener is told when a new peer arrives on its port. */
typedef struct fipsd_native_arrival {
    uint64_t flow;
    fipsd_native_flow_key key;
    fips_xonly_pubkey pubkey; /* captured where the session authenticated it */
} fipsd_native_arrival;
typedef enum fipsd_native_delivery_kind {
    FIPSD_NATIVE_DELIVER_FLOW,    /* an established flow owns it: send on `sink` */
    FIPSD_NATIVE_DELIVER_ARRIVED, /* a listener owns the port: announce `arrival` on `arrivals`, then hold */
    FIPSD_NATIVE_DELIVER_PENDING, /* already announced: hold against `flow` */
    FIPSD_NATIVE_DELIVER_DROP
} fipsd_native_delivery_kind;
typedef enum fipsd_native_drop_cause {
    FIPSD_NATIVE_DROP_NO_PORT, FIPSD_NATIVE_DROP_BACKLOG_FULL, FIPSD_NATIVE_DROP_TOO_MANY_FLOWS,
    FIPSD_NATIVE_DROP_QUEUE_FULL, FIPSD_NATIVE_DROP_PEER_SHARE
} fipsd_native_drop_cause;
typedef struct fipsd_native_delivery {
    fipsd_native_delivery_kind kind;
    void *sink, *arrivals;
    uint64_t flow;
    fipsd_native_arrival arrival;
    fipsd_native_drop_cause cause;
} fipsd_native_delivery;

typedef struct fipsd_native_flow_view {
    uint64_t flow;
    fipsd_native_flow_key key;
    fips_xonly_pubkey pubkey;
    bool established;
    size_t queued;
    uint64_t at;
} fipsd_native_flow_view;
typedef struct fipsd_native_listener_view { uint16_t port; size_t backlog; } fipsd_native_listener_view;

typedef struct fipsd_native_registry fipsd_native_registry;
/* `random(span, user)` returns a value in [0, span) for the ephemeral port
 * search start; NULL uses libsodium's uniform generator. */
fipsd_native_registry *fipsd_native_registry_create(const fipsd_native_limits *limits,
                                                    uint32_t (*random)(uint32_t span, void *user), void *user);
void fipsd_native_registry_destroy(fipsd_native_registry *r);
size_t fipsd_native_registry_flow_count(const fipsd_native_registry *r);
size_t fipsd_native_registry_port_count(const fipsd_native_registry *r);
/* Every flow, established and pending, ordered by identifier; `queued` reads
 * an established flow's sink depth. Returns the total count; fills up to cap. */
size_t fipsd_native_registry_flows(const fipsd_native_registry *r, fipsd_native_flow_view *out, size_t cap,
                                   size_t (*queued)(void *sink, void *user), void *user);
size_t fipsd_native_registry_listeners(const fipsd_native_registry *r, fipsd_native_listener_view *out, size_t cap);
/* Bind a listener to `port` (has_port) or an ephemeral one; reports the port held. */
fipsd_native_registry_error fipsd_native_registry_listen(fipsd_native_registry *r, bool has_port, uint16_t port,
                                                         void *arrivals, uint16_t *held, fipsd_native_refusal *why);
/* Open a flow to `peer` on `remote` from `local` (has_local) or an ephemeral port. */
fipsd_native_registry_error fipsd_native_registry_connect(fipsd_native_registry *r, const fips_xonly_pubkey *peer,
                                                          uint16_t remote, bool has_local, uint16_t local, void *sink,
                                                          uint64_t now, uint16_t *held, uint64_t *flow,
                                                          fipsd_native_refusal *why);
bool fipsd_native_registry_key_of(const fipsd_native_registry *r, uint64_t flow, fipsd_native_flow_key *out);
/* Decide where an inbound datagram goes: pending flow, established flow,
 * listener (announcing a new pending flow), or a drop. */
fipsd_native_delivery fipsd_native_registry_deliver(fipsd_native_registry *r, const fips_node_addr *peer,
                                                    const fips_xonly_pubkey *pubkey, uint16_t src, uint16_t dst, uint64_t now);
/* Hold a datagram for a pending flow; true took ownership, false means the
 * caller keeps it (queue full or no such flow). */
bool fipsd_native_registry_hold(fipsd_native_registry *r, uint64_t flow, fipsd_native_datagram *d);
/* Accept a pending flow: its key, its peer and what it held (an owned array
 * of owned datagrams the caller frees with fipsd_native_datagram_free). */
fipsd_native_registry_error fipsd_native_registry_accept(fipsd_native_registry *r, uint64_t flow, void *sink, uint64_t now,
                                                         fipsd_native_flow_key *key, fips_xonly_pubkey *pubkey,
                                                         fipsd_native_datagram ***held, size_t *held_count);
/* Discard a pending flow and anything it held. */
fipsd_native_registry_error fipsd_native_registry_reject(fipsd_native_registry *r, uint64_t flow, fipsd_native_flow_key *key);
/* Release an established flow and the port it owned, if it owned one. True
 * when a flow was actually removed. */
bool fipsd_native_registry_release(fipsd_native_registry *r, const fipsd_native_flow_key *key);
/* Release a listener, its port and every flow still pending on it; accepted
 * flows on that port survive. */
void fipsd_native_registry_release_listener(fipsd_native_registry *r, uint16_t port);
/* Discard pending flows past FIPSD_NATIVE_PENDING_DEADLINE_MS; returns how
 * many went, writing their identifiers up to cap. */
size_t fipsd_native_registry_expire(fipsd_native_registry *r, uint64_t now, uint64_t *expired, size_t cap);
#endif
