/* SPDX-License-Identifier: MIT — private sealed Git assembly. */
#ifndef HANAMI_SEALED_H
#define HANAMI_SEALED_H
#include "hanami/hanami.h"
#include "hanami/hanami-index.h"
#include "hanami/hanami-nostr.h"
#include <vault/vault.h>
#include <nip34.h>

typedef struct hanami_sealed {
    unsigned refs;
    vault_ring keys;
    porthome_ring_t ring;
    hanami_index_t *index;
    blossom_client_t *client;
    hanami_nostr_ctx_t *nostr;
    char *endpoint;
    char **relays;
    hanami_signer_t signer;
    char owner[65], writer[65], base_writer[65], repo_id[65];
    porthome_head *head;
    nip34_repo_state_t *state;
    bool writable, lifted, allow_insecure;
} hanami_sealed;

hanami_error_t hanami_sealed_new(hanami_sealed **out, const char *endpoint,
    const char **relays, const char *repo_id, const char *owner,
    const hanami_signer_t *signer, const porthome_ring_t *ring,
    const hanami_config_t *config);
void hanami_sealed_retain(hanami_sealed *);
void hanami_sealed_release(hanami_sealed *);
hanami_error_t hanami_sealed_publish(hanami_sealed *, const nip34_ref_t *, size_t, const char *head);
hanami_error_t hanami_sealed_attach_odb(git_repository *, hanami_sealed *);
hanami_error_t hanami_sealed_odb_new(git_odb_backend **, hanami_sealed *);
hanami_error_t hanami_sealed_refdb_new(git_refdb_backend **, hanami_sealed *);
/* Borrowed while repository/ODB remains alive. No global repo/session registry. */
hanami_sealed *hanami_sealed_for_repo(git_repository *);
bool hanami_repo_private(git_repository *);
/* Caller owns returned event; exact kind/author/d snapshot, EOSE required. */
hanami_error_t hanami_nostr_fetch_head(hanami_nostr_ctx_t *, const uint8_t vault_id[32],
    const char *writer, NostrEvent **);
hanami_error_t hanami_nostr_sign_head(hanami_nostr_ctx_t *, NostrEvent *);
#endif
