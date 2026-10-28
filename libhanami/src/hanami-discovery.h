/* SPDX-License-Identifier: MIT — private Blossom server discovery for clone. */
#ifndef HANAMI_DISCOVERY_H
#define HANAMI_DISCOVERY_H
#include "hanami/hanami-config.h"
#include "hanami/hanami-nostr.h"
#include <blossom/capability.h>
#include <stdbool.h>

/* Owner's BUD-03 kind-10063 server list, `server` tags in listed order.
 * EOSE-completed snapshot; HANAMI_ERR_NOT_FOUND when no list exists.
 * Caller frees with hanami_discovery_free_list(). */
hanami_error_t hanami_nostr_fetch_blossom_servers(hanami_nostr_ctx_t *ctx,
    const char *pubkey, char ***out, size_t *count);
void hanami_discovery_free_list(char **urls, size_t count);

/* Positive Blossom evidence from libblossom's capability probe: server-info
 * JSON, an accepted BUD-02 upload, or a BUD-02 auth refusal. A bare HTTP
 * answer (any Git Smart HTTP host) is not evidence. */
bool hanami_caps_indicate_blossom(const blossom_server_capabilities_t *caps);

/* Select the Blossom API endpoint for a repository. Candidates, in order:
 * explicit config endpoint, then the owner's kind-10063 servers (queried on
 * ctx, then on fallback_relays when the first has no list). NIP-34 `clone`
 * URLs are Git remotes and are never candidates. Each candidate is
 * classified with blossom_probe_capabilities(); the first Blossom server wins.
 * Errors: HANAMI_ERR_CONFIG when no server is known, HANAMI_ERR_BLOSSOM when
 * no candidate is a Blossom server, or the server-list query error. The
 * libgit2 last-error message names the cause. */
hanami_error_t hanami_select_blossom_endpoint(hanami_nostr_ctx_t *ctx,
    const char *const *fallback_relays, const char *owner_pubkey,
    const char *repo_id, const hanami_config_t *config, char **out);
#endif
