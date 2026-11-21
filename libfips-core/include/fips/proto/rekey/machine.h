#ifndef FIPS_REKEY_MACHINE_H
#define FIPS_REKEY_MACHINE_H
#include <fips/proto/rekey/candidates.h>
#include <fips/proto/rekey/cycle.h>
#include <fips/proto/rekey/params.h>
#include <fips/proto/rekey/proof.h>
#include <fips/proto/rekey/reason.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Every call appends at most 48 actions (4 candidates, 3 epochs, 3 expiry
 * passes); fixed storage makes an output-capacity failure impossible mid-commit.
 * Output owners execute Release and clear every Delegate key with outputs_clear.
 * Start with {0}; clear before reuse. Never copy an owning output list.
 * Only items[0, count) are live: hosts may act on those in place but never
 * write past count, so outputs_clear wipes just that prefix and count. */
#define FIPS_REKEY_OUTPUTS_MAX 48u
typedef enum fips_rekey_msg_kind {
    FIPS_REKEY_MSG_Msg1,
    FIPS_REKEY_MSG_Msg2,
    FIPS_REKEY_MSG_Setup,
    FIPS_REKEY_MSG_Ack,
    FIPS_REKEY_MSG_Msg3
} fips_rekey_msg_kind;
typedef enum fips_rekey_output_kind {
    FIPS_REKEY_OUT_Send,
    FIPS_REKEY_OUT_Prompt,
    FIPS_REKEY_OUT_Deliver,
    FIPS_REKEY_OUT_Bind,
    FIPS_REKEY_OUT_Unbind,
    FIPS_REKEY_OUT_Delegate,
    FIPS_REKEY_OUT_Revoke,
    FIPS_REKEY_OUT_Release,
    FIPS_REKEY_OUT_Installed,
    FIPS_REKEY_OUT_Retired,
    FIPS_REKEY_OUT_Reverted,
    FIPS_REKEY_OUT_PeerRestarted,
    FIPS_REKEY_OUT_Rekeyed,
    FIPS_REKEY_OUT_ReplaceRequested,
    FIPS_REKEY_OUT_Reject,
    FIPS_REKEY_OUT_Abandoned,
    FIPS_REKEY_OUT_Teardown,
    /* Host arms a pending redial, then calls note_redial; no state recorded yet. */
    FIPS_REKEY_OUT_RedialRequested
} fips_rekey_output_kind;
typedef struct fips_rekey_output {
    fips_rekey_output_kind kind;
    fips_rekey_epoch_tag tag;
    bool has_tag;
    fips_rekey_role role;
    fips_rekey_slot slot;
    size_t begin, end;
    fips_rekey_transport transport;
    uint32_t index;
    fips_rekey_purpose purpose;
    fips_rekey_index_lease lease;
    fips_rekey_msg_kind message_kind;
    fips_rekey_route route;
    fips_rekey_evidence_kind evidence;
    fips_rekey_reason reason;
    fips_rekey_abandon abandon;
    fips_rekey_close close;
    fips_rekey_teardown teardown;
    fips_rekey_layer layer;
    uint8_t epoch[8];
    union {
        fips_rekey_wire msg;
        fips_rekey_recv_copy recv;
        fips_rekey_msg1_read tok;
    } data;
} fips_rekey_output;
typedef struct fips_rekey_outputs {
    fips_rekey_output items[FIPS_REKEY_OUTPUTS_MAX];
    size_t count;
} fips_rekey_outputs;
typedef enum fips_rekey_input_kind {
    FIPS_REKEY_IN_Reply,
    FIPS_REKEY_IN_Final,
    FIPS_REKEY_IN_Setup,
    FIPS_REKEY_IN_PeerMsg1,
    FIPS_REKEY_IN_PeerDone,
    FIPS_REKEY_IN_Frame,
    FIPS_REKEY_IN_WorkerFrame,
    FIPS_REKEY_IN_Delegated,
    FIPS_REKEY_IN_Undelegated,
    FIPS_REKEY_IN_Open,
    FIPS_REKEY_IN_DataSent,
    FIPS_REKEY_IN_PathFacts,
    FIPS_REKEY_IN_Poll,
    /* Trusted host completion ONLY after AEAD authentication with this tag.
     * buf contains plaintext at hint.body; len ends at the plaintext end.
     * The machine rechecks epoch and replay, then marks on its sole writer. */
    FIPS_REKEY_IN_AuthenticatedFrame
} fips_rekey_input_kind;
typedef enum fips_rekey_intent {
    FIPS_REKEY_INTENT_Rekey,
    FIPS_REKEY_INTENT_Fresh,
    FIPS_REKEY_INTENT_Mismatch
} fips_rekey_intent;
/* Fields selected by kind: Reply/Final/Setup bytes+len, Setup carrier+route;
 * Frame buf+len+hint (buf is plaintext-modified only after authentication);
 * Reply additionally on_link (FMP only); PeerMsg1 tok+on_link+reachable+heard; PeerDone leg+intent (consumed); reports tag/k/
 * counter; Open peer; PathFacts reachable/has_srtt/srtt. Other fields ignored. */
typedef struct fips_rekey_input {
    fips_rekey_input_kind kind;
    const uint8_t *bytes;
    uint8_t *buf;
    size_t len;
    fips_rekey_route_handle route;
    fips_node_addr carrier;
    const fips_rekey_msg1_read *tok;
    fips_rekey_leg *leg;
    fips_rekey_intent intent;
    fips_rekey_frame_hint hint;
    fips_rekey_epoch_tag tag;
    bool k, on_link, reachable, has_srtt;
    /* Held peering has authenticated a frame or was dialled by this node. */
    bool heard;
    uint64_t counter;
    fips_duration srtt;
    fips_peer_key peer;
} fips_rekey_input;
typedef struct fips_rekey_env {
    fips_rekey_index_source indices;
    fips_rekey_transport transport;
    fips_rekey_tag_source *tags;
    bool workers;
    uint16_t (*jitter_draw)(void *ctx, uint16_t bound);
    void *jitter_ctx;
    void *local;
    uint8_t *scratch;
    size_t scratch_capacity;
    /* Optional host audit hook, synchronously at every replay-window write.
     * No mutation/re-entry; ctx lifetime covers this step call. */
    void (*replay_mark_observer)(void *ctx);
    void *replay_mark_ctx;
} fips_rekey_env;
typedef struct fips_rekey_setup_facts {
    uint8_t peer_epoch[8];
    bool has_peer_epoch, k, peer_k, has_peer_k;
    fips_instant link_since, started, last_rx;
    fips_rekey_record record;
} fips_rekey_setup_facts;
typedef fips_rekey_setup_facts fips_rekey_carry;
typedef struct fips_rekey_epoch_view {
    bool present;
    fips_rekey_epoch_tag tag;
    bool k;
    fips_rekey_role role;
    fips_instant started, base;
    fips_rekey_recv_home recv;
    const uint8_t *ext;
} fips_rekey_epoch_view;
typedef struct fips_rekey_view {
    bool proven;
    fips_rekey_bond identity;
    bool has_peer_epoch;
    uint8_t peer_epoch[8];
    fips_instant last_rx, link_since;
    fips_rekey_epoch_view current, previous, pending;
    fips_rekey_cycle_view cycle;
    fips_rekey_wait wait;
    size_t candidates;
} fips_rekey_view;
typedef struct fips_rekey_binding {
    fips_rekey_transport transport;
    uint32_t index;
    fips_rekey_purpose purpose;
} fips_rekey_binding;
typedef enum fips_rekey_seal_refusal {
    FIPS_REKEY_SEAL_OK,
    FIPS_REKEY_SEAL_NotEstablished,
    FIPS_REKEY_SEAL_Exhausted,
    FIPS_REKEY_SEAL_NotSealable
} fips_rekey_seal_refusal;
typedef struct fips_rekey_sealed {
    fips_rekey_epoch_tag tag;
    bool k;
    uint64_t counter;
    fips_duration elapsed;
    fips_rekey_index theirs;
    fips_cipher_state key;
} fips_rekey_sealed;
typedef struct fips_rekey_sealed_prompt {
    fips_rekey_sealed sealed;
    fips_rekey_wire plaintext;
    bool has_route;
    fips_rekey_route_handle route;
} fips_rekey_sealed_prompt;
typedef struct fips_rekey_memory {
    uint8_t peer_epoch[8];
    bool has_peer_epoch, peer_k, has_peer_k;
    int32_t jitter;
    fips_instant damp, backoff;
    uint8_t abandons;
    fips_instant last_rx, rx_cur;
    bool sent_data;
    uint8_t silence;
    fips_instant link_since, last_cutover;
    fips_duration hs_rtt, srtt, setup_rto;
    bool has_hs_rtt, has_srtt, has_setup_rto, reachable, due;
} fips_rekey_memory;
/* Noncopyable, immovable caller-owned machine. Read-only fields outside module.
 * The profile and route referents outlive it. All times share one clock domain.
 * close wipes every secret and returns leases; destroy is emergency cleanup and
 * counts outstanding leases as leaks. After either, step/seal/hooks cannot move
 * state; a fresh constructor is required. No allocation, timers or OS calls. */
typedef struct fips_rekey {
    bool live;
    const fips_rekey_profile *profile;
    fips_rekey_params params;
    fips_rekey_bond bond;
    fips_node_addr me;
    fips_rekey_epoch cur;
    fips_rekey_drain drain;
    fips_rekey_cycle cycle;
    fips_rekey_candidates cands;
    fips_rekey_record record;
    fips_rekey_memory mem;
    fips_noise_session sessions[3];
    bool used[3];
} fips_rekey;
/* Constructors require uninitialized/destroyed storage. On refusal no input or
 * output changes. established checks the bond then consumes first/ids/facts.
 * The first epoch's role is Initiator, exactly as the pinned constructor. */
FIPS_CORE_API fips_status fips_rekey_established(fips_rekey *m, const fips_rekey_profile *profile,
                                                 fips_rekey_params params, fips_rekey_bond bond,
                                                 fips_node_addr me, fips_rekey_completed *first,
                                                 fips_rekey_ids *ids, fips_rekey_setup_facts *facts,
                                                 fips_instant now, fips_rekey_env *env,
                                                 fips_rekey_outputs *out);
FIPS_CORE_API fips_status fips_rekey_opening_outbound(fips_rekey *m,
                                                      const fips_rekey_profile *profile,
                                                      fips_rekey_params params, fips_peer_key peer,
                                                      fips_node_addr me, fips_instant now,
                                                      fips_rekey_env *env, fips_rekey_outputs *out);
FIPS_CORE_API fips_status fips_rekey_opening_inbound(fips_rekey *m,
                                                     const fips_rekey_profile *profile,
                                                     fips_rekey_params params,
                                                     fips_node_addr claimed, fips_node_addr me,
                                                     fips_instant now, fips_rekey_input setup,
                                                     fips_rekey_env *env, fips_rekey_outputs *out);
/* Record only after the host has armed a RedialRequested output. */
FIPS_CORE_API void fips_rekey_note_redial(fips_rekey *m, fips_rekey_digest ephemeral);
FIPS_CORE_API fips_status fips_rekey_step(fips_rekey *m, fips_instant now, fips_rekey_input input,
                                          fips_rekey_env *env, fips_rekey_outputs *out);
FIPS_CORE_API bool fips_rekey_grows(const fips_rekey *m, fips_instant now, const uint8_t *msg1,
                                    size_t len, fips_node_addr carrier);
FIPS_CORE_API fips_instant fips_rekey_next_wake(const fips_rekey *m);
FIPS_CORE_API fips_rekey_seal_refusal fips_rekey_seal(fips_rekey *m, fips_instant now,
                                                      fips_rekey_sealed *out);
FIPS_CORE_API fips_rekey_seal_refusal fips_rekey_seal_prompt(fips_rekey *m, fips_instant now,
                                                             fips_rekey_epoch_tag tag,
                                                             fips_rekey_sealed_prompt *out);
FIPS_CORE_API fips_rekey_view fips_rekey_get_view(const fips_rekey *m);
FIPS_CORE_API size_t fips_rekey_bindings(const fips_rekey *m, fips_rekey_binding out[3]);
FIPS_CORE_API bool fips_rekey_close_machine(fips_rekey *m, fips_rekey_close why,
                                            fips_rekey_outputs *out, fips_rekey_carry *carry);
FIPS_CORE_API void fips_rekey_destroy(fips_rekey *m);
FIPS_CORE_API void fips_rekey_outputs_clear(fips_rekey_outputs *out);
FIPS_CORE_API void fips_rekey_sealed_clear(fips_rekey_sealed *sealed);
FIPS_CORE_API void fips_rekey_sealed_prompt_clear(fips_rekey_sealed_prompt *prompt);
#ifdef __cplusplus
}
#endif

#endif /* FIPS_REKEY_MACHINE_H */
