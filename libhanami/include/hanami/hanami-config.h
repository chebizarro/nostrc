/*
 * hanami-config.h - Configuration system for libhanami
 *
 * SPDX-License-Identifier: MIT
 *
 * Configuration can come from:
 *   1. Programmatic API (hanami_config_set_* / hanami_config_init)
 *   2. .gitconfig [hanami] section
 *   3. Environment variables (HANAMI_ENDPOINT, HANAMI_RELAYS, etc.)
 *
 * Priority: programmatic > env var > gitconfig > default
 */

#ifndef HANAMI_CONFIG_H
#define HANAMI_CONFIG_H

#include "hanami-types.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * hanami_config_t (full definition in hanami-config.c)
 * Use hanami_config_new/hanami_config_free for lifecycle.
 */
typedef struct hanami_config hanami_config_t;

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/**
 * hanami_config_new:
 * @out: (out): created config with default values
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_config_new(hanami_config_t **out);

/**
 * hanami_config_free:
 * @config: (transfer full) (nullable): config to free
 */
void hanami_config_free(hanami_config_t *config);

/**
 * hanami_config_load_env:
 * @config: config to populate from environment variables
 *
 * Reads HANAMI_ENDPOINT, HANAMI_RELAYS, HANAMI_CACHE_DIR,
 * HANAMI_INDEX_BACKEND, HANAMI_UPLOAD_THRESHOLD,
 * HANAMI_PREFETCH_CONCURRENCY, HANAMI_VERIFY_ON_READ.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_config_load_env(hanami_config_t *config);

/**
 * hanami_config_load_gitconfig:
 * @config: config to populate from .gitconfig
 * @repo: (nullable): git repository — if non-NULL, reads repo-level config first
 *
 * Reads hanami.endpoint, hanami.relays, hanami.cache-dir, etc.
 *
 * Returns: HANAMI_OK on success (missing keys are not errors)
 */
hanami_error_t hanami_config_load_gitconfig(hanami_config_t *config,
                                            void *repo);

/* =========================================================================
 * Getters
 * ========================================================================= */

const char *hanami_config_get_endpoint(const hanami_config_t *config);
const char *const *hanami_config_get_relays(const hanami_config_t *config);
size_t hanami_config_get_relay_count(const hanami_config_t *config);
const char *hanami_config_get_cache_dir(const hanami_config_t *config);
const char *hanami_config_get_index_backend(const hanami_config_t *config);
size_t hanami_config_get_upload_threshold(const hanami_config_t *config);
int hanami_config_get_prefetch_concurrency(const hanami_config_t *config);
bool hanami_config_get_verify_on_read(const hanami_config_t *config);

/* =========================================================================
 * Setters
 * ========================================================================= */

hanami_error_t hanami_config_set_endpoint(hanami_config_t *config,
                                           const char *endpoint);
hanami_error_t hanami_config_set_relays(hanami_config_t *config,
                                         const char *const *relay_urls,
                                         size_t count);
hanami_error_t hanami_config_set_cache_dir(hanami_config_t *config,
                                            const char *cache_dir);
hanami_error_t hanami_config_set_index_backend(hanami_config_t *config,
                                                const char *backend);
void hanami_config_set_upload_threshold(hanami_config_t *config,
                                        size_t threshold);
void hanami_config_set_prefetch_concurrency(hanami_config_t *config,
                                            int concurrency);
void hanami_config_set_verify_on_read(hanami_config_t *config, bool verify);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_CONFIG_H */
