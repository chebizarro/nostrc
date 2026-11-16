#ifndef FIPSD_NOSTR_DRIVER_H
#define FIPSD_NOSTR_DRIVER_H
#include "nostr/advert.h"
#include "nostr/traversal.h"
#include "node/context.h"
#include "node/handlers/handlers.h"
typedef enum { FIPSD_REFETCH_REFRESHED, FIPSD_REFETCH_SAME_ADVERT, FIPSD_REFETCH_EVICTED, FIPSD_REFETCH_SKIPPED } fipsd_nostr_refetch_outcome;
/* Calls run on the rendezvous thread, never the node dispatcher. Borrowed
 * arguments expire on return. Implementations enqueue to the node, not mutate
 * its tables. Return false for capacity/policy/unsupported effects. */
typedef struct {
    void *user;
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
typedef struct {
    const fipsd_node_context *node;
    fipsd_node_handlers *handlers;
    const fipsd_transport_registry *transports;
} fipsd_nostr_node_driver;
fipsd_rendezvous_driver fipsd_nostr_node_driver_init(fipsd_nostr_node_driver *, const fipsd_node_context *,
    fipsd_node_handlers *, const fipsd_transport_registry *);
#endif
