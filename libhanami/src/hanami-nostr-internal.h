/* Internal signed-state helpers for the GRASP transaction. */
#ifndef HANAMI_NOSTR_INTERNAL_H
#define HANAMI_NOSTR_INTERNAL_H
#include "hanami/hanami-nostr.h"
#include "nostr-event.h"

hanami_error_t hanami_nostr_fetch_state_event(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const char *author, NostrEvent **out);
hanami_error_t hanami_nostr_create_state(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t count,
    const char *head, int64_t minimum_time, NostrEvent **out);
/* As create_state, plus ["hanami-manifest", manifest] when non-NULL. */
hanami_error_t hanami_nostr_create_state_tagged(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t count,
    const char *head, const char *manifest, int64_t minimum_time, NostrEvent **out);
/* hanami_nostr_publish_state with the manifest tag: same strictly advancing
 * created_at and relay-acceptance (OK true, bounded NIP-42 retry) rules. */
hanami_error_t hanami_nostr_publish_state_manifest(hanami_nostr_ctx_t *ctx,
    const char *repo_id, const nip34_ref_t *refs, size_t count,
    const char *head, const char *manifest);
/* Signer pubkey of ctx, NULL without a signer. */
const char *hanami_nostr_ctx_signer_pubkey(const hanami_nostr_ctx_t *ctx);
#endif
