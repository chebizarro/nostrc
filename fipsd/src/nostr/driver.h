#ifndef FIPSD_NOSTR_DRIVER_H
#define FIPSD_NOSTR_DRIVER_H
#include "nostr/advert.h"
#include "nostr/traversal.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
struct fipsd_nostr_runtime;
struct fipsd_traversal_io;
typedef enum { FIPSD_REFETCH_REFRESHED, FIPSD_REFETCH_SAME_ADVERT, FIPSD_REFETCH_EVICTED, FIPSD_REFETCH_SKIPPED } fipsd_nostr_refetch_outcome;
/* Calls run on the rendezvous thread, never the node dispatcher. Borrowed
 * arguments expire on return. Implementations enqueue to the node, not mutate
 * its tables. Return false for capacity/policy/unsupported effects. */
typedef struct {
    void *user;
    bool (*prepare)(void *, const fipsd_config *, struct fipsd_nostr_runtime *);
    int (*start)(void *, fipsd_runtime *, const fips_identity *);
    void (*stop)(void *);
    void (*destroy)(void *);
    void (*signal_result)(void *, const char *session, bool accepted, bool repair);
    bool (*dial_candidate)(void *, const char *author_hex, const fipsd_nostr_advert *);
    bool (*local_advert)(void *, fipsd_nostr_advert *);
    /* 8hvs6.3 traversal machine and UDP punch-packet classifier attachment seam.
     * No fallback transport or fabricated successful traversal when absent. */
    bool (*traversal_offer)(void *, const fipsd_nostr_traversal *);
    bool (*traversal_answer)(void *, const fipsd_nostr_traversal *);
    void (*advert_cached)(void *, const char *author_hex, bool live);
    void (*signal_admission)(void *, const char *session_id, bool admitted);
    void (*boundary)(void *, const char *relay, bool live, const char *reason);
    void (*publish_result)(void *, const char *relay, int kind, bool accepted);
    void (*refetch_result)(void *, const char *peer, fipsd_nostr_refetch_outcome);
} fipsd_rendezvous_driver;
struct fipsd_nostr_adverts;
/* local_advert snapshots the registry in order (upstream
 * advert_transport_snapshot) with each instance's advertise_on_nostr,
 * public, external_addr and advertised_port. While started, a fiber turns
 * advertised TCP/Tor listener binds and Tor onion publication into
 * fipsd_nostr_runtime_refresh_advert; the actor republishes only a changed set. */
typedef struct {
    const fipsd_node_context *node;
    fipsd_node_handlers *handlers;
    const fipsd_transport_registry *transports;
    struct fipsd_traversal_io *traversal;
    struct fipsd_nostr_adverts *adverts;
} fipsd_nostr_node_driver;
/* The transport already owns the socket/NAT mapping: handoff carries a
 * stable local tuple and numeric peer, never a newly bound private socket. */
typedef struct {
    char session_id[FIPSD_NOSTR_SESSION_MAX + 1];
    fips_peer_identity peer;
    fips_transport_id transport;
    fips_transport_addr local_addr, remote_addr;
} fipsd_established_traversal;
bool fipsd_nostr_node_driver_established(fipsd_nostr_node_driver *, const fipsd_established_traversal *);
fipsd_rendezvous_driver fipsd_nostr_node_driver_init(fipsd_nostr_node_driver *, const fipsd_node_context *,
    fipsd_node_handlers *, const fipsd_transport_registry *);
#endif
