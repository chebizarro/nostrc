#ifndef FIPSD_PEER_BUDGET_H
#define FIPSD_PEER_BUDGET_H
#include <fips/transport/types.h>

/* Zero is unlimited at each policy limb; storage is ALWAYS bounded separately.
 * Pending transports count against connections; admitted peers against links
 * and peers. All operations run on the dispatcher. */
typedef struct fipsd_peer_caps { size_t peers, links, connections; } fipsd_peer_caps;
typedef struct fipsd_transport_caps { fips_transport_id transport; fipsd_peer_caps caps; } fipsd_transport_caps;
typedef struct fipsd_peer_budget fipsd_peer_budget;
typedef enum fipsd_peer_admission { FIPSD_ADMIT_OK, FIPSD_ADMIT_TOTAL,
    FIPSD_ADMIT_TRANSPORT, FIPSD_ADMIT_STORAGE, FIPSD_ADMIT_DUPLICATE,
    FIPSD_ADMIT_BUSY } fipsd_peer_admission;
typedef struct fipsd_peer_ticket {
    fipsd_peer_budget *owner;
    fips_transport_id transport;
    bool pending;
} fipsd_peer_ticket;
/* limits are copied, duplicate transport ids rejected. Unknown transports have
 * no per-transport policy cap but still pay total and bounded transport slots. */
fipsd_peer_budget *fipsd_peer_budget_create(fipsd_peer_caps total,
    const fipsd_transport_caps *limits, size_t count, size_t transport_capacity);
/* Requires all tickets returned; refuses otherwise. */
bool fipsd_peer_budget_destroy(fipsd_peer_budget *budget);
fipsd_peer_admission fipsd_peer_budget_acquire(fipsd_peer_budget *budget,
    fips_transport_id transport, bool pending, fipsd_peer_ticket *ticket);
fipsd_peer_admission fipsd_peer_budget_promote(fipsd_peer_ticket *ticket);
fipsd_peer_admission fipsd_peer_budget_rebind(fipsd_peer_ticket *ticket, fips_transport_id transport);
void fipsd_peer_budget_release(fipsd_peer_ticket *ticket);
fipsd_peer_caps fipsd_peer_budget_used(const fipsd_peer_budget *budget);

/* Upstream node/peer_budget.rs: thousandths, sub-ms carry, no reconnect bonus.
 * One bucket per admitted authenticated link; table bounds its cardinality. */
typedef struct fipsd_peer_tokens {
    uint64_t milli, capacity;
    uint32_t per_sec;
    fips_instant refill;
} fipsd_peer_tokens;
void fipsd_peer_tokens_init(fipsd_peer_tokens *bucket, uint32_t per_sec,
    uint32_t burst, uint32_t initial, fips_instant now);
uint64_t fipsd_peer_tokens_available(fipsd_peer_tokens *bucket, fips_instant now);
bool fipsd_peer_tokens_spend(fipsd_peer_tokens *bucket, uint64_t tokens, fips_instant now);
#endif
