#ifndef FIPS_PROTO_FSP_QUORUM_H
#define FIPS_PROTO_FSP_QUORUM_H
#include <fips/identity/mod.h>
#ifdef __cplusplus
extern "C" {
#endif
#define FIPS_FSP_QUORUM_LINKS 2
#define FIPS_FSP_QUORUM_WINDOW_MS UINT64_C(15000)
typedef enum fips_fsp_quorum_kind { FIPS_FSP_Below, FIPS_FSP_Reached } fips_fsp_quorum_kind;
typedef struct fips_fsp_quorum_verdict { fips_fsp_quorum_kind kind; size_t distinct; } fips_fsp_quorum_verdict;
/* At quorum=2, only one live vote can remain per destination. Key the vote on
 * the authenticated link peer, NEVER the unauthenticated body reporter. */
typedef struct fips_fsp_quorum_entry { fips_node_addr dest, link_peer; uint64_t first_seen_ms; } fips_fsp_quorum_entry;
typedef struct fips_fsp_link_quorum {
    fips_fsp_quorum_entry *items;
    size_t len, capacity;
    uint64_t last_sweep_ms;
} fips_fsp_link_quorum;
/* Caller-owned bounded array, NULL only at capacity zero, len <= capacity.
 * No eviction of live destinations. Capacity refusal preserves state/verdict;
 * host may fail closed (no demotion) or grant more storage and retry.
 * now_ms is injected; elapsed subtraction saturates as upstream does. */
FIPS_CORE_API void fips_fsp_link_quorum_init(fips_fsp_link_quorum *q, fips_fsp_quorum_entry *storage, size_t capacity);
FIPS_CORE_API fips_status fips_fsp_link_quorum_record(fips_fsp_link_quorum *q, fips_node_addr dest, fips_node_addr link_peer, uint64_t now_ms, fips_fsp_quorum_verdict *out);
FIPS_CORE_API void fips_fsp_link_quorum_clear(fips_fsp_link_quorum *q, const fips_node_addr *dest);
#ifdef __cplusplus
}
#endif
#endif
