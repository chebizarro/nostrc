/* SPDX-License-Identifier: MIT — private Blossom server discovery for clone. */
#ifndef HANAMI_DISCOVERY_H
#define HANAMI_DISCOVERY_H
#include "hanami/hanami-config.h"
#include "hanami/hanami-nostr.h"
#include <blossom/capability.h>
#include <stdbool.h>

/* Owner's BUD-03 kind-10063 server list, `server` tags in listed order.
 * EOSE-completed snapshot; HANAMI_ERR_NOT_FOUND only when every relay sent
 * EOSE without a list (see hanami_nostr_fetch_state for the other errors).
 * Caller frees with hanami_discovery_free_list(). */
hanami_error_t hanami_nostr_fetch_blossom_servers(hanami_nostr_ctx_t *ctx,
    const char *pubkey, char ***out, size_t *count);
/* Owner's NIP-65 kind-10002 write relays (`r` tags without a marker or with
 * "write"), ws/wss only, in listed order. Same EOSE snapshot semantics;
 * HANAMI_ERR_NOT_FOUND when no list exists. Free with
 * hanami_discovery_free_list(). */
hanami_error_t hanami_nostr_fetch_write_relays(hanami_nostr_ctx_t *ctx,
    const char *pubkey, char ***out, size_t *count);
void hanami_discovery_free_list(char **urls, size_t count);
/* Deduplicated NULL-terminated union of two relay lists (either nullable);
 * free with hanami_discovery_free_list(list, count) or element-wise. */
char **hanami_relay_union(const char *const *a, const char *const *b);

/* At most this many NIP-65 write relays are added to a clone's relay set. */
#define HANAMI_OUTBOX_RELAYS_MAX 4

/* NIP-65 outbox: read the owner's kind-10002 over *ctx (whose relays are
 * *relays) and append up to HANAMI_OUTBOX_RELAYS_MAX write relays not
 * already present, after the existing ones. When relays are added, *relays
 * and *ctx are replaced (new context with @signer). A missing or unreadable
 * list adds nothing: later snapshots over the same relays report relay
 * failures themselves. Returns HANAMI_OK, or HANAMI_ERR_NOMEM. */
hanami_error_t hanami_add_outbox_relays(hanami_nostr_ctx_t **ctx, char ***relays,
    const char *owner_pubkey, const hanami_signer_t *signer);

/* Positive Blossom evidence from libblossom's capability probe: server-info
 * JSON, an accepted BUD-02 upload, or a BUD-02 auth refusal. A bare HTTP
 * answer (any Git Smart HTTP host) is not evidence. */
bool hanami_caps_indicate_blossom(const blossom_server_capabilities_t *caps);

/* Select the Blossom API endpoint for a repository. Candidates, in order:
 * explicit config endpoint, then the owner's kind-10063 servers (one EOSE
 * snapshot over ctx's relays; callers pass discovery ∪ announcement ∪ NIP-65
 * write relays so the newest replaceable list wins across all). NIP-34 `clone`
 * URLs are Git remotes and are never candidates. Each candidate is
 * classified with blossom_probe_capabilities(); the first Blossom server wins.
 * Errors: HANAMI_ERR_CONFIG when no server is known, HANAMI_ERR_BLOSSOM when
 * no candidate is a Blossom server, or the server-list query error. The
 * libgit2 last-error message names the cause. */
hanami_error_t hanami_select_blossom_endpoint(hanami_nostr_ctx_t *ctx,
    const char *owner_pubkey,
    const char *repo_id, const hanami_config_t *config, char **out);
#endif
