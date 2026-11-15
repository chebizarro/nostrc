#ifndef FIPS_NODE_SESSION_H
#define FIPS_NODE_SESSION_H
#include <fips/proto/fmp/rekey.h>
#include <fips/proto/fsp/rekey.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Host serialization boundary; all callbacks are synchronous and non-reentrant.
 * send sees a complete FMP/FSP envelope, never a borrowed handshake state.
 * coords is called afresh per FSP Send (including retries), and optionally for
 * Prompt; false omits CP for prompts, but refuses Setup/Ack. route names the
 * arrival/fallback coordinate handle, not a pointer into the received packet.
 * effect runs for EVERY non-wire action, in original order; Delegate keys are
 * borrowed for that call only (clone explicitly if retained). Release is always
 * consumed and returned with release, even if a send/codec fails. No retry is
 * invented: the machine alone owns resend timing. A failure does not stop later
 * cleanup/effects. outputs is consumed and cleared on all paths. */
typedef struct fips_session_executor {
    void *ctx;
    bool (*coords)(void *, fips_rekey_route, fips_rekey_route_handle,
                   fips_fsp_coords *src, fips_fsp_coords *dest);
    fips_status (*send)(void *, const fips_rekey_output *, const uint8_t *, size_t);
    void (*effect)(void *, const fips_rekey_output *, const uint8_t *plaintext, size_t);
    void (*release)(void *, uint32_t index);
    uint8_t *wire;
    size_t capacity;
    fips_rekey_route_handle arrival; /* current ingress route for Send::Arrival */
} fips_session_executor;
FIPS_CORE_API fips_status fips_session_execute(fips_rekey *machine, fips_instant now,
    fips_rekey_outputs *out, const uint8_t *input, size_t len,
    const fips_session_executor *executor);
/* Seals plaintext with the already reserved counter; consumes no machine state.
 * K/index come ONLY from sealed. Never auto-seal a bare clone. No coords here;
 * callers needing CP use the executor's prompt path or the wire codec. */
FIPS_CORE_API fips_status fips_session_encode_sealed(const fips_rekey_profile *profile,
    const fips_rekey_sealed *sealed, const uint8_t *plaintext, size_t len,
    uint8_t *wire, size_t capacity, size_t *written);

/* init requires fresh/destroyed storage; never reinitialize a live table.
 * Use separate tables for link and session hosts. Open/answer are FSP-only;
 * established may also import dormant FMP machines for the link binding.
 * Caller allocates entries. Stable slots: never compact/copy a live machine.
 * Table/entry storage, shared tag source, local keys and route backing storage
 * outlive machines. All APIs are single-writer; no implicit clock, allocation,
 * background poll or index cache. Lookups derive bindings from the machines,
 * including Attempt/Pending/Previous, so cutovers cannot leave stale indices.
 * Capacity refuses unchanged. Eviction is EXPLICIT: oldest last_activity at or
 * before cutoff, never a busy/rekeying/draining machine; close returns all
 * effects/leases for immediate execution BEFORE any later open reuses its slot.
 * No hidden eviction of an authenticated peer to admit an untrusted setup. */
typedef struct fips_session_entry {
    bool occupied;
    fips_node_addr peer;
    fips_instant created, last_activity;
    fips_rekey machine;
} fips_session_entry;
typedef struct fips_session_table {
    fips_session_entry *entries;
    size_t count, capacity;
} fips_session_table;
FIPS_CORE_API void fips_session_table_init(fips_session_table *table,
    fips_session_entry *entries, size_t capacity);
FIPS_CORE_API fips_session_entry *fips_session_table_by_peer(fips_session_table *, fips_node_addr peer);
FIPS_CORE_API fips_session_entry *fips_session_table_by_index(fips_session_table *,
    fips_rekey_transport transport, uint32_t index);
FIPS_CORE_API fips_status fips_session_table_open(fips_session_table *, fips_peer_key peer,
    fips_node_addr me, fips_rekey_params params, fips_instant now,
    fips_rekey_env *, fips_rekey_outputs *, fips_session_entry **entry);
/* setup is the strictly decoded bare XK Setup, never an authenticated claim.
 * Before admission to an EXISTING entry, the host must Poll at next_wake when
 * due and execute that batch first (which may request teardown/removal). Then
 * re-lookup and admit the setup. This preserves deadline/start-before-setup
 * ordering without merging output owners or executing implicit I/O here. */
FIPS_CORE_API fips_status fips_session_table_answer(fips_session_table *, fips_node_addr claimed,
    fips_node_addr me, fips_rekey_params params, fips_instant now, fips_rekey_input setup,
    fips_rekey_env *, fips_rekey_outputs *, fips_session_entry **entry);
/* Import a completed first IK/XK handshake directly into its final stable slot.
 * FMP is bound for lookups/output execution but remains dormant until explicitly
 * driven by a future link host. This function does not poll or enable it. */
FIPS_CORE_API fips_status fips_session_table_established(fips_session_table *,
    const fips_rekey_profile *, fips_node_addr me, fips_rekey_params params,
    fips_rekey_completed *, fips_rekey_ids *, fips_rekey_setup_facts *, fips_instant now,
    fips_rekey_env *, fips_rekey_outputs *, fips_session_entry **entry);
FIPS_CORE_API bool fips_session_table_remove(fips_session_table *, fips_session_entry *,
    fips_rekey_close, fips_rekey_outputs *, fips_rekey_carry *);
FIPS_CORE_API bool fips_session_table_evict(fips_session_table *, fips_instant cutoff,
    fips_rekey_outputs *, fips_rekey_carry *);
/* Explicit application activity only, not MMP/probes/unauthenticated receipt. */
FIPS_CORE_API void fips_session_note_activity(fips_session_entry *, fips_instant now);
FIPS_CORE_API size_t fips_session_table_half_open(const fips_session_table *);
#ifdef __cplusplus
}
#endif
#endif
