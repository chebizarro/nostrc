/* SPDX-License-Identifier: MIT
 * Shared-vault head codec, §5.1/§5.2. Experimental, no fold or writer policy.
 */
#ifndef PORTHOME_HEAD_H
#define PORTHOME_HEAD_H
#include <porthome/crypto.h>
#ifdef __cplusplus
extern "C" {
#endif
struct _NostrEvent;
#define PORTHOME_HEAD_KIND 30078
#define PORTHOME_HEAD_MAX_PARENTS 64u
#define PORTHOME_HEAD_MAX_JSON 65535u
#define PORTHOME_HEAD_MAX_PAYLOAD 90000u
#define PORTHOME_HEAD_D_LEN 77u /* porthome/v2/ + hex(vault_id) + NUL */
typedef struct {
    uint8_t writer[32], root_manifest[32];
    uint64_t seq;
} porthome_head_parent;
typedef struct {
    uint8_t vault_id[32], root_manifest[32], ref_state[32];
    int has_ref_state;
    uint64_t epoch, seq, grants_rev, written_at;
    porthome_head_parent *parents; /* Decoded array is owned. */
    size_t parents_len;
} porthome_head;

/* Head plaintext is JSON with the exact field names from §5.2; bytes32 are
 * 64 lowercase hex chars. Encoder uses compact field order. Strict decoder
 * permits field reordering/JSON whitespace, refuses duplicate/unknown/missing
 * fields, escaped strings, non-uint/overflow numbers, wrong lengths and trailing
 * bytes. Epoch must be nonzero, written_at <= INT64_MAX. Sequence, parents and
 * grants_rev are data, not authorization/fold policy. ref_state is optional.
 * Decode allocates; dispose then free(). Encode borrows input and returns malloc
 * UTF-8 bytes of out_len, plus a convenience NUL (not included in out_len).
 * Outputs are reset on failure. */
PORTHOME_API int porthome_head_encode(const porthome_head *head, uint8_t **out, size_t *out_len);
PORTHOME_API int porthome_head_decode(const uint8_t *json, size_t len, porthome_head **out);
PORTHOME_API void porthome_head_dispose(porthome_head *head);
PORTHOME_API int porthome_head_d_tag(const uint8_t vault_id[32], char out[PORTHOME_HEAD_D_LEN]);

/* NIP-44 v2 content under convkey(head_key, BIP340_xonly_pub(head_key)).
 * Uses SV-1 head scalar derivation. Open selects only the requested epoch,
 * never retries another ring entry, and checks the decrypted body epoch.
 * Callers of bare open must bind vault_id to their expected vault. */
PORTHOME_API int porthome_head_seal(const porthome_head *head, const uint8_t node_key[32], char **out);
PORTHOME_API int porthome_head_open(const porthome_ring_t *ring, uint64_t epoch,
    const char *payload, porthome_head **out);

/* Builds an UNSIGNED kind-30078 event with writer pubkey, created_at=written_at,
 * exactly one d tag, and sealed content. Caller signs with their identity signer
 * then uses the normal relay publish path. No secret identity key is needed here.
 * Event caller uses nostr_event_free(). Head event open verifies the signature,
 * kind and exactly one correctly shaped d tag, then binds body vault_id/epoch.
 * Other tags are allowed. Writer authorization is explicitly SV-3, not this API.
 * libnostr's public event header is needed only by callers manipulating the event. */
PORTHOME_API int porthome_head_event_new(const porthome_head *head,
    const uint8_t node_key[32], const uint8_t writer[32], struct _NostrEvent **out);
PORTHOME_API int porthome_head_event_open(const struct _NostrEvent *event,
    const uint8_t vault_id[32], const porthome_ring_t *ring, uint64_t epoch, porthome_head **out);
#ifdef __cplusplus
}
#endif
#endif
