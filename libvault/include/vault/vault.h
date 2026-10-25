/* SPDX-License-Identifier: MIT — experimental Layer-5 shared vault policy. */
#ifndef VAULT_VAULT_H
#define VAULT_VAULT_H
#include <vault/export.h>
#include <porthome/dir-node.h>
#include <porthome/head.h>
#include <blossom/client.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Preserve porthome's primitive errors, notably ERR_EPOCH (no key retries). */
#define VAULT_OK PORTHOME_OK
#define VAULT_ERR_ARG PORTHOME_ERR_ARG
#define VAULT_ERR_OOM PORTHOME_ERR_OOM
#define VAULT_ERR_EPOCH PORTHOME_ERR_EPOCH
#define VAULT_ERR_CONFLICT (-9)
#define VAULT_ERR_INCOMPLETE (-10)
#define VAULT_ERR_BUDGET (-11)
#define VAULT_ERR_IO (-12)
#define VAULT_ERR_AGAIN (-13)
#define VAULT_ERR_AUTH (-14)
#define VAULT_MAX_HEADS 256u
#define VAULT_MAX_OBJECTS 4096u
#define VAULT_MAX_HINTS 32u
#define VAULT_MAX_URL 2048u

/* init/copy require a fresh zeroed destination; dispose before replacing it.
 * Epochs are ordered, nonzero and distinct-keyed; append uses latest + 1. */
typedef struct {
    porthome_ring_entry_t *entries;
    size_t count;
} vault_ring;
VAULT_API int vault_ring_init(vault_ring *, uint64_t epoch, const uint8_t key[32]);
VAULT_API int vault_ring_copy(vault_ring *, const porthome_ring_t *);
VAULT_API int vault_ring_append(vault_ring *, const uint8_t key[32], uint64_t *epoch);
VAULT_API int vault_ring_append_random(vault_ring *, uint64_t *epoch);
VAULT_API int vault_ring_drop_before(vault_ring *, uint64_t epoch);
VAULT_API porthome_ring_t vault_ring_view(const vault_ring *);
VAULT_API void vault_ring_dispose(vault_ring *);

/* Pure, atomic proposal: inputs unchanged on success/failure. Caller supplies
 * its authenticated actor identity; equality to owner is NOT proof of custody.
 * Remote authority takes effect only after the owner's normal signed publish.
 * Node must be a share boundary. Exactly one of parent/root_head is required.
 * root_head must be the owner's previously verified head, not a writer's head.
 * Current canonical node address must match that reference. Unlisted readers
 * can still be revoked: readers[] is informational, never an access check.
 * Subtree: output parent entry has override/epoch/address; caller re-seals its
 * parent/ancestors. Root: output head is sealed ONLY with its new epoch key;
 * that key must be delivered separately by a grant/exporter. No old-key head.
 * New chunks use result.ring's latest key; old chunk IDs are deliberately kept.
 */
typedef struct {
    vault_ring ring;
    porthome_dir_node *node;
    uint8_t *sealed;
    size_t sealed_len;
    uint8_t address[32];
    int has_parent;
    porthome_node_entry parent;
    porthome_head *head;
    char *head_payload; /* Root proposal, NIP-44 sealed with new epoch only. */
} vault_revoke_result;
VAULT_API int vault_revoke(const uint8_t actor[32], const uint8_t member[32],
                           const porthome_dir_node *, const vault_ring *,
                           const porthome_node_entry *parent, const porthome_head *root_head,
                           uint64_t written_at, vault_revoke_result **);
VAULT_API void vault_revoke_result_free(vault_revoke_result *);

/* Authenticated per-chunk lengths come from the manifest's size_plain, not
 * padding buckets or file totals. Borrowed mutable node/ring must remain
 * unchanged except by this task.
 * put() success means durable, hash-verified acceptance. read() returns malloc
 * bytes. No implicit retries: any I/O failure makes the task terminal; reconcile
 * pending_address externally before constructing another task. Never deletes
 * old objects. Costs reserve download+upload payload bytes BEFORE any I/O.
 * Lazy runs a prefix; eager requires budget for all remaining work before I/O.
 * The task does NOT drop history; drop_before is an explicit grant decision
 * after a complete subtree inventory shows no references to the old epoch.
 */
typedef struct {
    porthome_dir_node *node;
    const vault_ring *ring;
    size_t entry, chunk;
} vault_reseal_item;
typedef struct {
    void *ctx;
    int (*read)(void *, const uint8_t address[32], uint8_t **, size_t *);
    int (*put)(void *, const uint8_t *, size_t, const uint8_t address[32]);
} vault_blob_io;
typedef struct vault_reseal_task vault_reseal_task;
VAULT_API int vault_reseal_task_new(const vault_reseal_item *, size_t, int eager,
                                    vault_reseal_task **);
VAULT_API int vault_reseal_task_run(vault_reseal_task *, uint64_t bandwidth_budget,
                                    const vault_blob_io *);
VAULT_API size_t vault_reseal_task_completed(const vault_reseal_task *);
VAULT_API uint64_t vault_reseal_task_reserved(const vault_reseal_task *);
VAULT_API const uint8_t *vault_reseal_task_pending_address(const vault_reseal_task *);
VAULT_API void vault_reseal_task_free(vault_reseal_task *);

/* A manifest object inventory closes an immutable snapshot. Every address is
 * checked BEFORE AEAD, then child node ID/epoch/address bindings are checked.
 * The caller supplies exact per-node rings (including independently overridden
 * child history); libvault never guesses keys. Missing objects fail closed.
 */
typedef struct {
    const uint8_t *sealed;
    size_t sealed_len;
    porthome_ring_t ring;
    uint64_t epoch;
} vault_manifest_object;
typedef struct vault_record vault_record;
VAULT_API int vault_record_open(const struct _NostrEvent *, const uint8_t vault_id[32],
                                const porthome_ring_t *root_ring, uint64_t epoch,
                                const vault_manifest_object *, size_t count, vault_record **);
VAULT_API const porthome_head *vault_record_head(const vault_record *);
VAULT_API const porthome_dir_node *vault_record_root(const vault_record *);
VAULT_API void vault_record_free(vault_record *);

typedef enum {
    VAULT_HEAD_ACCEPTED,
    VAULT_HEAD_NONWRITER,
    VAULT_HEAD_STALE,
    VAULT_HEAD_POLICY,
    VAULT_HEAD_DUPLICATE
} vault_head_disposition;
typedef struct {
    char *path_enc; /* slash-joined encrypted components; owned */
    size_t other_record;
    int in_owner, in_other;
    uint8_t writer[32], root_manifest[32];
    uint64_t seq;
} vault_path_conflict;
typedef struct {
    size_t view; /* input index, SIZE_MAX if no authoritative view */
    size_t *frontier;
    size_t frontier_len;
    vault_head_disposition *dispositions;
    size_t records_len;
    vault_path_conflict *conflicts;
    size_t conflicts_len;
    uint8_t (*retained_roots)[32];
    size_t retained_roots_len;
} vault_fold_result;
/* anchor is trusted owner metadata, not a policy supplied by an arbitrary
 * writer. Newer OWNER-signed records can advance grants_rev/writer policy.
 * Readers never admit a head. Stale/nonwriter/policy-forging heads are dropped
 * from the view. Parent tuples match writer+seq+root, not seq alone. Supply the
 * referenced signed history closure; missing ancestors/cycles return INCOMPLETE
 * or CONFLICT with retention/disposition data, never a fabricated frontier.
 * One frontier -> that view. Multiple -> latest unambiguous owner head (even
 * if an ancestor); absent/forked owner -> CONFLICT, no arbitrary LWW fallback.
 * All verified input roots and parent roots are retained, including ignored
 * heads; fold performs no writes/deletions. Result indices refer to input order.
 */
VAULT_API int vault_fold(const porthome_vault_info *anchor, const vault_record *const *, size_t,
                         vault_fold_result **);
VAULT_API void vault_fold_result_free(vault_fold_result *);

/* Grant body JSON: §6.2, cap=read|write. Ring and hint arrays decoded as owned.
 * Hints are advisory, not service authority. URL strings are unescaped ASCII
 * ws(s)/http(s) URLs without control, whitespace, quote or backslash bytes.
 * Strict named-field JSON like porthome heads; no duplicate/unknown fields.
 * encode() returns malloc'd secret plaintext: caller must wipe before freeing.
 * Rumor kind30804 is NEVER signed or published bare. Carrier owns transport.
 */
typedef enum { VAULT_CAP_READ = 1, VAULT_CAP_WRITE = 2 } vault_cap;
typedef struct {
    uint8_t vault_id[32], node_id[32], owner[32], root_manifest[32];
    vault_ring ring;
    vault_cap cap;
    char **relays, **blossom;
    size_t relays_len, blossom_len;
    uint64_t issued_at;
} vault_grant;
VAULT_API int vault_grant_encode(const vault_grant *, char **);
VAULT_API int vault_grant_decode(const char *, vault_grant **);
VAULT_API void vault_grant_free(vault_grant *);
VAULT_API int vault_grant_rumor_new(const vault_grant *, struct _NostrEvent **);
VAULT_API int vault_grant_rumor_open(const struct _NostrEvent *, vault_grant **);
/* Rumors contain key material: use this instead of bare nostr_event_free(). */
VAULT_API void vault_grant_rumor_free(struct _NostrEvent *);
typedef struct vault_grant_carrier {
    void *ctx;
    int (*send)(void *, const uint8_t recipient[32], const struct _NostrEvent *rumor);
    /* Receive returns owned rumor and an AUTHENTICATED sender. No echo-derived
     * authority: implementations must verify the carrier, e.g. NIP-59 in SV-4. */
    int (*receive)(void *, struct _NostrEvent **rumor, uint8_t authenticated_sender[32]);
} vault_grant_carrier;
VAULT_API int vault_grant_send(const vault_grant_carrier *, const uint8_t recipient[32],
                               const vault_grant *);
VAULT_API int vault_grant_receive(const vault_grant_carrier *, vault_grant **);

/* 10319 self-encrypted service list. Public tags empty, writer is own identity.
 * Uses standard NIP-44 self-ECDH; real signed event verification before decode.
 * This event MAY be published by the caller's normal ACK-aware relay path.
 */
typedef struct {
    char **relays, **blossom;
    size_t relays_len, blossom_len;
} vault_service_list;
VAULT_API int vault_service_list_encode(const vault_service_list *, char **);
VAULT_API int vault_service_list_decode(const char *, vault_service_list **);
VAULT_API void vault_service_list_free(vault_service_list *);
VAULT_API int vault_service_list_event_new(const vault_service_list *,
                                           const uint8_t identity_sk[32], uint64_t written_at,
                                           struct _NostrEvent **);
VAULT_API int vault_service_list_event_open(const struct _NostrEvent *,
                                            const uint8_t identity_sk[32], vault_service_list **);

/* Advertisements are classification, NOT proof of allowlist/privacy behavior.
 * UNKNOWN/NO never promote to a private service. Relay JSON is a NIP-11 doc;
 * Blossom result comes from libblossom's real capability probe/cache. */
VAULT_API int vault_vsp_relay_classify(const char *nip11, blossom_capability_state_t *);
VAULT_API blossom_capability_state_t
vault_vsp_blossom_classify(const blossom_server_capabilities_t *);
VAULT_API blossom_error_t vault_vsp_probe_blossom(blossom_client_t *, blossom_capability_state_t *);
#ifdef __cplusplus
}
#endif
#endif
