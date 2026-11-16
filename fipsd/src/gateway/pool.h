#ifndef FIPSD_GATEWAY_POOL_H
#define FIPSD_GATEWAY_POOL_H
/* The virtual IP pool, the port of upstream src/gateway/pool.rs
 * (nostrc-iq1n9.3): allocation, TTL and reclamation of virtual IPv6
 * addresses from a configured CIDR, keyed by node address, driven by a
 * caller-supplied clock and the conntrack snapshot each tick reads. Pure:
 * no I/O, no locks; the gateway serialises calls with a mutex. */
#include "gateway/conntrack.h"
#include "gateway/types.h"

/* Most live mappings the pool holds before it refuses new names. */
#define FIPSD_GW_MAPPING_CEILING 1000u
/* New mappings admitted in a burst, and per second once the burst is spent. */
#define FIPSD_GW_MAPPING_BURST 50u
#define FIPSD_GW_MAPPING_RATE 10u
/* Free offsets ahead of the cursor each state write covers, and the number
 * left before the durable mark below which the next tick asks for a write. */
#define FIPSD_GW_MARK_RESERVE 512u
#define FIPSD_GW_MARK_LOW_WATER 300u
/* Consecutive read snapshots after which the pool trusts what they do not
 * show, and consecutive unread ticks after which it stops trusting them. */
#define FIPSD_GW_MAX_ABSENT_READS 3u
#define FIPSD_GW_MAX_UNREAD_TICKS 3u
/* How long the pool must refuse nothing before its refusal warning is released. */
#define FIPSD_GW_BOUND_LOG_HOLD_SECS 60u
#define FIPSD_GW_DNS_NAME_MAX 256u

typedef enum fipsd_gw_pool_error {
    FIPSD_GW_POOL_OK = 0,
    FIPSD_GW_POOL_INVALID_CIDR,
    FIPSD_GW_POOL_EXHAUSTED,
    FIPSD_GW_POOL_INVALID_PREFIX,
    FIPSD_GW_POOL_AT_CEILING,
    FIPSD_GW_POOL_RATE_LIMITED,
    FIPSD_GW_POOL_AWAITING_MARK,
    FIPSD_GW_POOL_INSIDE_POOL
} fipsd_gw_pool_error;
/* The short kebab-case name for structured log fields. */
const char *fipsd_gw_pool_error_reason(fipsd_gw_pool_error error);
/* Upstream's Display text; count is the "in use" figure, addr the mesh
 * address inside the pool, cidr the text that did not parse. */
const char *fipsd_gw_pool_error_text(fipsd_gw_pool_error error, size_t count, const struct in6_addr *addr,
                                     const char *cidr, char *buf, size_t cap);

typedef enum fipsd_gw_mapping_state {
    FIPSD_GW_MAPPING_ALLOCATED, /* allocated via DNS, no NAT sessions yet */
    FIPSD_GW_MAPPING_ACTIVE,    /* active NAT sessions exist */
    FIPSD_GW_MAPPING_DRAINING   /* TTL expired but sessions remain */
} fipsd_gw_mapping_state;
typedef struct fipsd_gw_mapping {
    bool live;
    fips_node_addr node_addr;
    struct in6_addr virtual_ip, mesh_addr;
    char dns_name[FIPSD_GW_DNS_NAME_MAX];
    fipsd_gw_mapping_state state;
    fipsd_gw_instant created, last_referenced;
    bool has_drain_start; fipsd_gw_instant drain_start;
    uint32_t session_count;
    bool used;  /* a reply from mesh_addr was seen on a flow to virtual_ip; never cleared */
    bool blind; /* created while evidence was not trusted or before enough start reads */
    fipsd_gw_instant answer_deadline, last_answer;
} fipsd_gw_mapping;
typedef struct fipsd_gw_pool_status {
    size_t total, allocated, active, draining, free;
    uint64_t evicted;   /* mappings replaced at the ceiling since start */
    size_t releasing;   /* removed and waiting, or held from the previous run */
    uint64_t refused;   /* new names refused at a bound since start */
} fipsd_gw_pool_status;
typedef struct fipsd_gw_mapping_info {
    struct in6_addr virtual_ip, mesh_addr;
    fips_node_addr node_addr;
    char dns_name[FIPSD_GW_DNS_NAME_MAX];
    fipsd_gw_mapping_state state;
    uint32_t session_count;
    uint64_t age_secs, last_ref_secs;
    bool used;
} fipsd_gw_mapping_info;
typedef struct fipsd_gw_evicted { bool present; struct in6_addr virtual_ip, mesh_addr; } fipsd_gw_evicted;
typedef struct fipsd_gw_allocation {
    struct in6_addr virtual_ip;
    bool is_new;
    uint32_t ttl;
    fipsd_gw_evicted evicted;
} fipsd_gw_allocation;

/* What a pool keeps across a restart: no identities, only which offsets a
 * client may still hold an answer for, as sorted inclusive ranges. */
typedef struct fipsd_gw_pool_state {
    uint32_t version;
    char pool[64];
    uint32_t total, from, span;
    uint32_t (*held)[2]; size_t held_len;
    uint64_t hold_secs;
} fipsd_gw_pool_state;
void fipsd_gw_pool_state_clear(fipsd_gw_pool_state *state);
int fipsd_gw_pool_state_copy(fipsd_gw_pool_state *dst, const fipsd_gw_pool_state *src);
bool fipsd_gw_pool_state_eq(const fipsd_gw_pool_state *a, const fipsd_gw_pool_state *b);
/* How a pool begins: fresh at an offset, or from a previous run's state. */
typedef struct fipsd_gw_pool_begin { bool restored; uint32_t offset; fipsd_gw_pool_state state; } fipsd_gw_pool_begin;

typedef struct fipsd_gw_pool fipsd_gw_pool;
/* Addresses in a pool: offsets 1..=total, at most 2^16 - 1; a /128 is refused. */
fipsd_gw_pool_error fipsd_gw_pool_total(const char *cidr, uint32_t *total);
/* Pool size below which one LAN host naming new names can exhaust the pool. */
size_t fipsd_gw_small_pool_threshold(uint64_t ttl_secs, uint64_t grace_secs);
fipsd_gw_pool *fipsd_gw_pool_new(const char *cidr, uint64_t ttl_secs, uint64_t grace_secs, fipsd_gw_pool_error *error);
fipsd_gw_pool *fipsd_gw_pool_with_limits(const char *cidr, uint64_t ttl_secs, uint64_t grace_secs, size_t ceiling,
                                         uint32_t burst, uint32_t rate, fipsd_gw_pool_error *error);
/* The pool the gateway runs with: refuses new names until a state write is
 * confirmed or has failed, and holds a restored state's offsets. */
fipsd_gw_pool *fipsd_gw_pool_start(const char *cidr, uint64_t ttl_secs, uint64_t grace_secs,
                                   const fipsd_gw_pool_begin *start, fipsd_gw_instant now, fipsd_gw_pool_error *error);
void fipsd_gw_pool_free(fipsd_gw_pool *pool);
void fipsd_gw_pool_network(const fipsd_gw_pool *pool, struct in6_addr *network, uint8_t *prefix_len);
/* Allocate a virtual IP for node_addr at now; idempotent for a mapped name. */
fipsd_gw_pool_error fipsd_gw_pool_allocate_at(fipsd_gw_pool *pool, const fips_node_addr *node_addr,
                                              const struct in6_addr *mesh_addr, const char *dns_name,
                                              fipsd_gw_instant now, fipsd_gw_allocation *out);
/* Refresh a used mapping's TTL clock, never creating one. */
bool fipsd_gw_pool_refresh_at(fipsd_gw_pool *pool, const fips_node_addr *node_addr, fipsd_gw_instant now);
/* The periodic tick: events for the NAT and network modules are appended. */
int fipsd_gw_pool_tick(fipsd_gw_pool *pool, fipsd_gw_instant now, const fipsd_gw_ct_snapshot *conntrack, fipsd_gw_events *events);
void fipsd_gw_pool_get_status(const fipsd_gw_pool *pool, fipsd_gw_pool_status *out);
/* Live mappings. */
size_t fipsd_gw_pool_len(const fipsd_gw_pool *pool);
/* Every live mapping, malloc'd; -1 on allocation failure. */
int fipsd_gw_pool_mapping_info(const fipsd_gw_pool *pool, fipsd_gw_instant now, fipsd_gw_mapping_info **out, size_t *count);
const fipsd_gw_mapping *fipsd_gw_pool_lookup_virtual_ip(const fipsd_gw_pool *pool, const struct in6_addr *virtual_ip);
const fipsd_gw_mapping *fipsd_gw_pool_lookup_node(const fipsd_gw_pool *pool, const fips_node_addr *node_addr);
/* The state to write before issuing further, if one is due. */
bool fipsd_gw_pool_mark_request(const fipsd_gw_pool *pool, fipsd_gw_pool_state *out);
int fipsd_gw_pool_shutdown_state(const fipsd_gw_pool *pool, fipsd_gw_pool_state *out);
bool fipsd_gw_pool_carry_state(const fipsd_gw_pool *pool, fipsd_gw_pool_state *out);
void fipsd_gw_pool_confirm_mark(fipsd_gw_pool *pool, const fipsd_gw_pool_state *state);
void fipsd_gw_pool_mark_failed(fipsd_gw_pool *pool);
void fipsd_gw_pool_nat_removed(fipsd_gw_pool *pool, const struct in6_addr *virtual_ip, const struct in6_addr *mesh_addr);
void fipsd_gw_pool_set_evidence(fipsd_gw_pool *pool, bool available);

/* Test seams: the private fields the ported tests read. */
uint64_t fipsd_gw_pool_test_tokens(const fipsd_gw_pool *pool);
size_t fipsd_gw_pool_test_free_len(const fipsd_gw_pool *pool);
bool fipsd_gw_pool_test_free_contains(const fipsd_gw_pool *pool, uint32_t offset);
uint32_t fipsd_gw_pool_test_total(const fipsd_gw_pool *pool);
uint64_t fipsd_gw_pool_test_cursor(const fipsd_gw_pool *pool);
uint32_t fipsd_gw_pool_test_start(const fipsd_gw_pool *pool);
uint32_t fipsd_gw_pool_test_offset_at(const fipsd_gw_pool *pool, uint64_t position);
uint64_t fipsd_gw_pool_test_ring_distance(const fipsd_gw_pool *pool, uint32_t from, uint32_t to);
bool fipsd_gw_pool_test_addr_offset(const fipsd_gw_pool *pool, const struct in6_addr *addr, uint32_t *offset);
void fipsd_gw_pool_test_offset_addr(const fipsd_gw_pool *pool, uint32_t offset, struct in6_addr *out);
uint64_t fipsd_gw_pool_test_refused_total(const fipsd_gw_pool *pool);
bool fipsd_gw_pool_test_refusal_open(const fipsd_gw_pool *pool, uint64_t *count);
uint64_t fipsd_gw_pool_test_evicted_total(const fipsd_gw_pool *pool);
bool fipsd_gw_pool_test_evidence_trusted(const fipsd_gw_pool *pool);
size_t fipsd_gw_pool_test_mappings_len(const fipsd_gw_pool *pool);
#endif
