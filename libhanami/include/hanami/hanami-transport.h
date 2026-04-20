/*
 * hanami-transport.h - Blossom transport plugin for libgit2
 *
 * SPDX-License-Identifier: MIT
 *
 * Registers a custom "blossom://" URL scheme with libgit2 so that
 * `git clone blossom://...` and `git push blossom://...` work
 * transparently against Blossom + Nostr backends.
 *
 * URL format:
 *   blossom://<endpoint>/<owner_pubkey>/<repo_id>
 *
 * Example:
 *   blossom://blossom.example.com/aabb...ccdd/my-repo
 */

#ifndef HANAMI_TRANSPORT_H
#define HANAMI_TRANSPORT_H

#include "hanami-types.h"
#include <git2/types.h>
#include <git2/transport.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct hanami_nostr_ctx hanami_nostr_ctx_t;
typedef struct hanami_blossom_client hanami_blossom_client_t;

/**
 * hanami_transport_opts_t:
 *
 * Configuration for the Blossom transport.
 */
typedef struct {
    hanami_nostr_ctx_t *nostr_ctx;           /**< Nostr context (borrowed) */
    hanami_blossom_client_t *blossom_client;  /**< Blossom client (borrowed) */
} hanami_transport_opts_t;

/**
 * hanami_transport_register:
 * @opts: transport options (borrowed — caller retains ownership)
 *
 * Register the "blossom://" transport with libgit2.
 * Must be called after git_libgit2_init().
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_transport_register(const hanami_transport_opts_t *opts);

/**
 * hanami_transport_unregister:
 *
 * Unregister the "blossom://" transport.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_transport_unregister(void);

/**
 * hanami_transport_parse_url:
 * @url: blossom:// URL to parse
 * @endpoint: (out): allocated "https://<host>" string (caller frees)
 * @owner_pubkey: (out): allocated owner pubkey hex (caller frees)
 * @repo_id: (out): allocated repo identifier (caller frees)
 *
 * Parse a blossom:// URL into its components.
 *
 * Returns: HANAMI_OK on success, HANAMI_ERR_INVALID_ARG on parse failure
 */
hanami_error_t hanami_transport_parse_url(const char *url,
                                           char **endpoint,
                                           char **owner_pubkey,
                                           char **repo_id);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_TRANSPORT_H */
