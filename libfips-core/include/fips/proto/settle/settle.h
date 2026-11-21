#ifndef FIPS_PROTO_SETTLE_H
#define FIPS_PROTO_SETTLE_H
#include <fips/transport/types.h>
#include <fips/identity/mod.h>
#include <fips/proto/rekey/lease.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Partial settlement port at upstream 64f5b4d482cf: params, links, legs only.
 * No peer arbitration/table, restart gates or node integration is provided.
 * Storage belongs to the caller and must not overlap arguments/outputs. No
 * allocation, IO, clock reads or global state. Instants use fips/host.h.
 * Objects/slots are private-by-contract; inspect but do not mutate or copy
 * owning legs/leases. Caller synchronizes access. All fallible calls leave
 * state, output buffers and lease ownership unchanged on failure.
 * Address and transcript sizes are C-local bounds, not wire protocol limits.
 */
#define FIPS_SETTLE_MESSAGE_CAPACITY 4096
#define FIPS_SETTLE_DEFAULT_REPLAYS 8
#define FIPS_SETTLE_DEFAULT_COUNTED 8
#define FIPS_SETTLE_DEFAULT_SILENT 3

typedef struct fips_settle_params {
    fips_duration unheard, interval, silentbase, silentcap, horizon;
    fips_duration handshake, first, resend, retry;
    size_t replays, counted, pending, peers;
    uint32_t silent, resends;
    uint64_t backoff; /* IEEE binary64 bits, as upstream. */
} fips_settle_params;
FIPS_CORE_API fips_settle_params fips_settle_params_default(void);
FIPS_CORE_API fips_duration fips_settle_wait(const fips_settle_params *p, uint32_t sent);

typedef struct fips_settle_pair { fips_transport_id transport; fips_transport_addr addr; } fips_settle_pair;
typedef enum fips_settle_proven { FIPS_SETTLE_LIVE, FIPS_SETTLE_ACKED, FIPS_SETTLE_PROBING } fips_settle_proven;
typedef struct fips_settle_holder { bool peer; fips_node_addr identity; } fips_settle_holder;
typedef struct fips_settle_path { fips_settle_pair pair; fips_settle_proven state; } fips_settle_path;
typedef struct fips_settle_link {
    bool active, released, capable;
    uint64_t number;
    fips_settle_pair setup;
    fips_link_direction direction;
    fips_settle_holder holder;
    fips_settle_path *paths;
    size_t path_count;
} fips_settle_link;
typedef struct fips_settle_links {
    fips_settle_link *slots;
    size_t capacity, paths_per_link;
    uint64_t next;
} fips_settle_links;
/* paths has capacity * paths_per_link entries. Zero capacities permit NULL. */
FIPS_CORE_API fips_status fips_settle_links_init(fips_settle_links *s,
    fips_settle_link *slots, size_t capacity, fips_settle_path *paths, size_t paths_per_link);
FIPS_CORE_API bool fips_settle_pair_equal(const fips_settle_pair *a, const fips_settle_pair *b);
FIPS_CORE_API fips_status fips_settle_link_open(fips_settle_links *s, const fips_settle_pair *pair,
    fips_link_direction direction, fips_settle_holder holder, uint64_t *number);
FIPS_CORE_API const fips_settle_link *fips_settle_link_get(const fips_settle_links *s, uint64_t number);
FIPS_CORE_API void fips_settle_link_hold(fips_settle_links *s, uint64_t number, fips_settle_holder holder);
FIPS_CORE_API void fips_settle_link_capable(fips_settle_links *s, uint64_t number);
FIPS_CORE_API fips_status fips_settle_link_path(fips_settle_links *s, uint64_t number,
    const fips_settle_pair *pair, fips_settle_proven state);
/* Moves the first matching path, preserving state. Missing source arrives Live.
 * Like upstream, a move onto an existing destination may leave duplicate paths. */
FIPS_CORE_API fips_status fips_settle_link_moved(fips_settle_links *s, uint64_t number,
    const fips_settle_pair *from, const fips_settle_pair *to);
FIPS_CORE_API bool fips_settle_link_indexed(const fips_settle_links *s, uint64_t number,
    const fips_settle_pair *pair);
FIPS_CORE_API bool fips_settle_link_in_use(const fips_settle_links *s,
    const fips_settle_pair *pair, uint64_t except);
/* Caller-owned closes; retire needs at most path_count+1 entries, release 1.
 * On success count is written even when link is missing (zero). Retire's output
 * is setup then paths in insertion order, deduped, excluding other users. */
FIPS_CORE_API fips_status fips_settle_link_retire(fips_settle_links *s, uint64_t number,
    fips_settle_pair *closes, size_t capacity, size_t *count);
FIPS_CORE_API fips_status fips_settle_link_release(fips_settle_links *s, uint64_t number,
    const fips_settle_pair *pair, bool keep, fips_settle_pair *closes, size_t capacity, size_t *count);
typedef bool (*fips_settle_expects)(void *ctx, uint64_t link, fips_node_addr *peer);
typedef struct fips_settle_placement {
    bool established, has_expected, has_unanswered, on_link;
    fips_node_addr expected;
    uint64_t unanswered;
} fips_settle_placement;
/* Derived index: setup unless released, plus Live/Acked paths. Newest number
 * wins naming; fallback expected identity is first held proven path by number.
 * on_link asks about held_link, not necessarily the newest named link. */
FIPS_CORE_API fips_settle_placement fips_settle_place(const fips_settle_links *s,
    const fips_settle_pair *pair, uint64_t held_link, fips_settle_expects expects, void *ctx);

typedef struct fips_settle_leg {
    bool active, answer, has_peer, has_held, has_next, retry;
    uint64_t link, held;
    fips_node_addr peer;
    uint8_t msg1[FIPS_SETTLE_MESSAGE_CAPACITY], msg2[FIPS_SETTLE_MESSAGE_CAPACITY];
    size_t msg1_len, msg2_len;
    uint32_t sent, resends;
    fips_instant next, deadline;
    fips_rekey_index_lease lease;
} fips_settle_leg;
typedef struct fips_settle_legs { fips_settle_leg *slots; size_t capacity; } fips_settle_legs;
typedef struct fips_settle_start {
    uint64_t link, held;
    bool has_peer, has_held, has_cap;
    fips_node_addr peer;
    const uint8_t *msg1;
    size_t msg1_len;
    uint32_t cap;
    fips_rekey_index_lease *lease; /* consumed only on success */
} fips_settle_start;
FIPS_CORE_API void fips_settle_legs_init(fips_settle_legs *s, fips_settle_leg *slots, size_t capacity);
/* answer=false dials; answer=true stores msg2 and never schedules resends.
 * Replacing a leg moves the old owning value to old; otherwise old is zeroed.
 * A live old must be taken/released before reusing that output. */
FIPS_CORE_API fips_status fips_settle_leg_open(fips_settle_legs *s, const fips_settle_start *start,
    bool answer, const uint8_t *msg2, size_t msg2_len, fips_instant now,
    const fips_settle_params *p, fips_settle_leg *old);
FIPS_CORE_API const fips_settle_leg *fips_settle_leg_get(const fips_settle_legs *s, uint64_t link);
FIPS_CORE_API bool fips_settle_leg_take(fips_settle_legs *s, uint64_t link, fips_settle_leg *out);
/* Borrowed stored answer; NULL means no answer (use msg2_len for empty bytes). */
FIPS_CORE_API const uint8_t *fips_settle_leg_stored(const fips_settle_legs *s, uint64_t link,
    const uint8_t *msg1, size_t msg1_len, size_t *msg2_len);
FIPS_CORE_API void fips_settle_leg_resent(fips_settle_legs *s, uint64_t link,
    fips_instant now, const fips_settle_params *p);
FIPS_CORE_API void fips_settle_leg_unsent(fips_settle_legs *s, uint64_t link,
    fips_instant now, const fips_settle_params *p);
FIPS_CORE_API size_t fips_settle_legs_answering(const fips_settle_legs *s);
FIPS_CORE_API bool fips_settle_legs_next_wake(const fips_settle_legs *s, fips_instant *out);
typedef struct fips_settle_leg_out {
    bool reaped, dialed, has_peer;
    uint64_t link;
    fips_node_addr peer;
    fips_rekey_index_lease lease; /* owning on Reaped only; host must release */
    const uint8_t *bytes; /* Resend borrows live leg until next mutation */
    size_t length, close_offset, close_count;
} fips_settle_leg_out;
/* Reap in ascending link order, then emit resends in ascending link order.
 * close_all reaps every leg regardless of time and emits no resends (close).
 * Buffer preflight requires the sum of (1+path_count) of reaped links, even
 * when shared/deduped closes need fewer entries. Failure is transactional.
 * Execute/consume owning outputs before reusing their array. Poll itself does
 * not commit resends: report sent/unsent explicitly, as upstream does. */
FIPS_CORE_API fips_status fips_settle_legs_poll(fips_settle_legs *s, fips_settle_links *links,
    fips_instant now, bool close_all, fips_settle_leg_out *out, size_t capacity, size_t *count,
    fips_settle_pair *closes, size_t close_capacity, size_t *close_count);
#ifdef __cplusplus
}
#endif
#endif
