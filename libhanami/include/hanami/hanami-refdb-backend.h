/*
 * hanami-refdb-backend.h - Custom git_refdb_backend storing refs via NIP-34
 *
 * SPDX-License-Identifier: MIT
 *
 * Git refs (branches, tags, HEAD) are stored as NIP-34 kind 30618 repository
 * state events on Nostr relays. This backend implements git_refdb_backend so
 * libgit2 ref operations (checkout, branch, log) work transparently against
 * Nostr-backed repositories.
 */

#ifndef HANAMI_REFDB_BACKEND_H
#define HANAMI_REFDB_BACKEND_H

#include "hanami-types.h"
#include <git2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct hanami_nostr_ctx hanami_nostr_ctx_t;

/**
 * hanami_refdb_backend_opts_t:
 *
 * Configuration for creating a Nostr-backed refdb.
 */
typedef struct {
    hanami_nostr_ctx_t *nostr_ctx;   /**< Nostr context for relay operations (borrowed) */
    const char *repo_id;             /**< NIP-34 d-tag repo identifier (required) */
    const char *owner_pubkey;        /**< Hex pubkey of the repo owner (required) */
} hanami_refdb_backend_opts_t;

/**
 * hanami_refdb_backend_new:
 * @out: (out): created backend. Ownership transfers to libgit2 via git_refdb_set_backend().
 * @opts: configuration
 *
 * Create a git_refdb_backend that stores refs via NIP-34 kind 30618 events.
 * On creation, the latest state is fetched from relays (if reachable). Writes
 * create new signed 30618 events and publish them to relays.
 *
 * Reflog operations are not supported (return GIT_ENOTFOUND).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_refdb_backend_new(git_refdb_backend **out,
                                        const hanami_refdb_backend_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_REFDB_BACKEND_H */
