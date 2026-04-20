/*
 * hanami.h - Public API for libhanami
 *
 * SPDX-License-Identifier: MIT
 *
 * libhanami: Blossom filesystem extension & tools for libgit2
 *
 * Provides custom libgit2 backends enabling Git repositories to use
 * Blossom servers as a decentralized, content-addressed object store
 * and Nostr events (NIP-34) as the coordination/ref layer.
 */

#ifndef HANAMI_H
#define HANAMI_H

#include "hanami-types.h"
#include <git2.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Library lifecycle
 * ========================================================================= */

/**
 * hanami_init:
 *
 * Initialize libhanami. Call after git_libgit2_init().
 * Safe to call multiple times (reference counted).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_init(void);

/**
 * hanami_shutdown:
 *
 * Shut down libhanami. Decrements reference count; resources are freed
 * when count reaches zero. Call before git_libgit2_shutdown().
 */
void hanami_shutdown(void);

/**
 * hanami_version:
 * @major: (out) (optional): major version
 * @minor: (out) (optional): minor version
 * @patch: (out) (optional): patch version
 *
 * Returns: version string (e.g. "0.1.0")
 */
const char *hanami_version(int *major, int *minor, int *patch);

/* =========================================================================
 * Configuration
 * ========================================================================= */

/**
 * hanami_config_default:
 * @config: (out): filled with default values
 *
 * Initialize a config struct with sensible defaults.
 */
void hanami_config_default(hanami_config_t *config);

/**
 * hanami_config_load:
 * @config: (out): filled from .gitconfig and environment
 *
 * Load configuration from git config ([hanami] section) and
 * environment variables (HANAMI_ENDPOINT, HANAMI_RELAYS, etc.).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_config_load(hanami_config_t *config);

/* =========================================================================
 * Transport registration
 * ========================================================================= */

/* hanami_transport_register — see hanami/hanami-transport.h for the full API */

/* =========================================================================
 * ODB backend
 * ========================================================================= */

/* hanami_odb_backend_new — see hanami/hanami-odb-backend.h for the full API */

/* hanami_refdb_backend_new — see hanami/hanami-refdb-backend.h for the full API */

/* =========================================================================
 * High-level operations
 * ========================================================================= */

/**
 * hanami_repo_open:
 * @out: (out): the opened git_repository
 * @endpoint: Blossom server URL
 * @relay_urls: NULL-terminated array of Nostr relay URLs
 * @repo_id: repository identifier
 * @owner_pubkey: repository owner's hex public key
 * @signer: (nullable): signer for write operations
 * @config: (nullable): configuration (uses defaults if NULL)
 *
 * Open or create a repository backed by Blossom ODB + Nostr RefDB.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_repo_open(git_repository **out,
                                const char *endpoint,
                                const char **relay_urls,
                                const char *repo_id,
                                const char *owner_pubkey,
                                const hanami_signer_t *signer,
                                const hanami_config_t *config);

/**
 * hanami_clone:
 * @out: (out): the cloned git_repository
 * @nostr_uri: nostr:// URI (e.g. "nostr://npub1.../repo-name")
 * @local_path: local directory to clone into
 * @signer: (nullable): signer for authenticated fetch
 * @config: (nullable): configuration (uses defaults if NULL)
 *
 * Clone a repository discovered via Nostr events, fetching objects
 * from Blossom servers listed in the repository announcement.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_clone(git_repository **out,
                            const char *nostr_uri,
                            const char *local_path,
                            const hanami_signer_t *signer,
                            const hanami_config_t *config);

/**
 * hanami_push_to_blossom:
 * @repo: the repository to push from
 * @endpoint: Blossom server URL
 * @signer: signer for upload authorization (required)
 * @relay_urls: NULL-terminated array of relay URLs for state publishing
 * @repo_id: repository identifier for NIP-34 state event
 *
 * Upload all local objects to Blossom and publish updated
 * repository state (kind 30618) to Nostr relays.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_push_to_blossom(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id);

/**
 * hanami_announce_repo:
 * @repo_id: repository identifier (d-tag)
 * @name: human-readable name
 * @description: brief description
 * @clone_urls: NULL-terminated array of clone URLs
 * @relay_urls: NULL-terminated array of relay URLs
 * @signer: signer for the announcement event (required)
 *
 * Publish a NIP-34 repository announcement (kind 30617).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_announce_repo(const char *repo_id,
                                    const char *name,
                                    const char *description,
                                    const char **clone_urls,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer);

/**
 * hanami_publish_state:
 * @repo_id: repository identifier (d-tag)
 * @refs: array of ref entries (refname → target)
 * @ref_count: number of entries
 * @relay_urls: NULL-terminated array of relay URLs
 * @signer: signer for the state event (required)
 *
 * Publish a NIP-34 repository state announcement (kind 30618).
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_publish_state(const char *repo_id,
                                    const hanami_ref_entry_t *refs,
                                    size_t ref_count,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_H */
