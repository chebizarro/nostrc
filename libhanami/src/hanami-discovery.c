/*
 * hanami-discovery.c - Blossom API endpoint selection for hanami_clone
 *
 * NIP-34 `clone` tags are URLs for `git clone` (e.g. GRASP Smart HTTP), not
 * Blossom APIs, and NIP-34 defines no Blossom tag. Blossom servers come from
 * explicit configuration or the owner's BUD-03 kind-10063 server list.
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami-discovery.h"
#include "hanami/hanami.h"
#include <blossom/client.h>
#include <git2/errors.h>
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif
#include <stdlib.h>
#include <string.h>

void hanami_discovery_free_list(char **urls, size_t count)
{
    if (!urls) return;
    for (size_t i = 0; i < count; i++) free(urls[i]);
    free(urls);
}

bool hanami_caps_indicate_blossom(const blossom_server_capabilities_t *caps)
{
    if (!caps || !caps->reachable) return false;
    return caps->private_server != BLOSSOM_CAP_UNKNOWN || caps->max_size > 0 ||
           caps->server_tag_ok == BLOSSOM_CAP_YES ||
           caps->batch_ok != BLOSSOM_CAP_UNKNOWN || /* 2xx, or BUD-02 401 */
           caps->raw_random_ok == BLOSSOM_CAP_YES ||
           caps->png_shim_ok == BLOSSOM_CAP_YES;
}

static bool probe_is_blossom(const char *url, bool allow_insecure)
{
    blossom_client_opts_t opts = {
        .endpoint = url, .timeout_seconds = 30,
        .allow_insecure = allow_insecure, .user_agent = "libhanami/" HANAMI_VERSION_STRING
    };
    blossom_client_t *client = NULL;
    if (blossom_client_new(&opts, NULL, &client) != BLOSSOM_OK) return false;
    bool ok = blossom_probe_capabilities(client) == BLOSSOM_OK &&
              hanami_caps_indicate_blossom(blossom_client_get_capabilities(client));
    blossom_client_free(client);
    return ok;
}

static bool same_url(const char *a, const char *b)
{
    size_t la = strlen(a), lb = strlen(b);
    while (la && a[la - 1] == '/') la--;
    while (lb && b[lb - 1] == '/') lb--;
    return la == lb && strncmp(a, b, la) == 0;
}

static hanami_error_t fetch_list(hanami_nostr_ctx_t *ctx,
    const char *const *fallback_relays, const char *owner,
    char ***urls, size_t *count)
{
    hanami_error_t err = hanami_nostr_fetch_blossom_servers(ctx, owner, urls, count);
    if (err != HANAMI_ERR_NOT_FOUND || !fallback_relays || !fallback_relays[0])
        return err;
    hanami_nostr_ctx_t *repo_ctx = NULL;
    if (hanami_nostr_ctx_new(fallback_relays, NULL, &repo_ctx) != HANAMI_OK)
        return err;
    err = hanami_nostr_fetch_blossom_servers(repo_ctx, owner, urls, count);
    hanami_nostr_ctx_free(repo_ctx);
    return err;
}

hanami_error_t hanami_select_blossom_endpoint(hanami_nostr_ctx_t *ctx,
    const char *const *fallback_relays, const char *owner_pubkey,
    const char *repo_id, const hanami_config_t *config, char **out)
{
    if (!ctx || !owner_pubkey || !repo_id || !out) return HANAMI_ERR_INVALID_ARG;
    *out = NULL;
    bool insecure = hanami_config_get_allow_insecure(config);
    const char *configured = hanami_config_get_endpoint(config);
    if (configured && !*configured) configured = NULL;

    if (configured && probe_is_blossom(configured, insecure))
        return (*out = strdup(configured)) ? HANAMI_OK : HANAMI_ERR_NOMEM;

    char **listed = NULL;
    size_t listed_count = 0;
    hanami_error_t list_err = fetch_list(ctx, fallback_relays, owner_pubkey,
                                         &listed, &listed_count);
    size_t probed = configured ? 1 : 0;
    for (size_t i = 0; list_err == HANAMI_OK && i < listed_count; i++) {
        if (configured && same_url(listed[i], configured)) continue;
        bool seen = false;
        for (size_t j = 0; j < i && !seen; j++) seen = same_url(listed[i], listed[j]);
        if (seen) continue;
        probed++;
        if (probe_is_blossom(listed[i], insecure)) {
            *out = strdup(listed[i]);
            hanami_discovery_free_list(listed, listed_count);
            return *out ? HANAMI_OK : HANAMI_ERR_NOMEM;
        }
    }
    hanami_discovery_free_list(listed, listed_count);

    if (list_err != HANAMI_OK && list_err != HANAMI_ERR_NOT_FOUND) {
        git_error_set(GIT_ERROR_NET,
            "hanami: kind-10063 Blossom server list query for %s failed: %s",
            owner_pubkey, hanami_strerror(list_err));
        return list_err;
    }
    if (probed == 0) {
        git_error_set(GIT_ERROR_INVALID,
            "hanami: no Blossom server known for %s/%s: set hanami.endpoint or "
            "publish a BUD-03 kind-10063 server list (NIP-34 clone URLs are Git "
            "remotes, not Blossom APIs)", owner_pubkey, repo_id);
        return HANAMI_ERR_CONFIG;
    }
    git_error_set(GIT_ERROR_NET,
        "hanami: none of the %zu configured/kind-10063 servers for %s/%s is a "
        "reachable Blossom server", probed, owner_pubkey, repo_id);
    return HANAMI_ERR_BLOSSOM;
}
