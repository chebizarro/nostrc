/*
 * hanami.c - Library lifecycle and high-level API stubs
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami.h"
#include <stdlib.h>
#include <string.h>

#define HANAMI_VERSION_MAJOR 0
#define HANAMI_VERSION_MINOR 1
#define HANAMI_VERSION_PATCH 0
#define HANAMI_VERSION_STRING "0.1.0"

static int hanami_ref_count = 0;

/* =========================================================================
 * Error strings
 * ========================================================================= */

const char *hanami_strerror(hanami_error_t err)
{
    switch (err) {
    case HANAMI_OK:                return "success";
    case HANAMI_ERR_NOMEM:         return "out of memory";
    case HANAMI_ERR_INVALID_ARG:   return "invalid argument";
    case HANAMI_ERR_NOT_FOUND:     return "not found";
    case HANAMI_ERR_NETWORK:       return "network error";
    case HANAMI_ERR_AUTH:          return "authentication error";
    case HANAMI_ERR_HASH_MISMATCH: return "hash mismatch";
    case HANAMI_ERR_INDEX:         return "index error";
    case HANAMI_ERR_LIBGIT2:       return "libgit2 error";
    case HANAMI_ERR_NOSTR:         return "nostr error";
    case HANAMI_ERR_CONFIG:        return "configuration error";
    case HANAMI_ERR_IO:            return "I/O error";
    case HANAMI_ERR_BLOSSOM:       return "blossom server error";
    case HANAMI_ERR_UPLOAD:        return "upload failed";
    case HANAMI_ERR_TIMEOUT:       return "operation timed out";
    default:                       return "unknown error";
    }
}

/* =========================================================================
 * Library lifecycle
 * ========================================================================= */

hanami_error_t hanami_init(void)
{
    if (hanami_ref_count++ > 0)
        return HANAMI_OK;

    /* Future: initialize curl, index, etc. */
    return HANAMI_OK;
}

void hanami_shutdown(void)
{
    if (--hanami_ref_count > 0)
        return;

    /* Future: cleanup curl, index, etc. */
}

const char *hanami_version(int *major, int *minor, int *patch)
{
    if (major) *major = HANAMI_VERSION_MAJOR;
    if (minor) *minor = HANAMI_VERSION_MINOR;
    if (patch) *patch = HANAMI_VERSION_PATCH;
    return HANAMI_VERSION_STRING;
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

void hanami_config_default(hanami_config_t *config)
{
    if (!config)
        return;

    memset(config, 0, sizeof(*config));
    config->endpoint = NULL;
    config->relay_urls = NULL;
    config->cache_dir = NULL;
    config->index_backend = "sqlite";
    config->upload_threshold = 0;
    config->prefetch_concurrency = 4;
    config->verify_on_read = true;
}

hanami_error_t hanami_config_load(hanami_config_t *config)
{
    if (!config)
        return HANAMI_ERR_INVALID_ARG;

    hanami_config_default(config);

    /* TODO: Read from .gitconfig [hanami] section */
    /* TODO: Override from environment variables */

    return HANAMI_OK;
}

/* =========================================================================
 * Blob descriptor
 * ========================================================================= */

void hanami_blob_descriptor_free(hanami_blob_descriptor_t *desc)
{
    if (!desc)
        return;
    free(desc->url);
    free(desc->mime_type);
    free(desc);
}

/* =========================================================================
 * Transport registration (stub)
 * ========================================================================= */

hanami_error_t hanami_transport_register(void)
{
    /* TODO: git_transport_register("blossom", hanami_transport_new, NULL) */
    return HANAMI_OK;
}

/* ODB backend — implemented in hanami-odb-backend.c */

/* RefDB backend — implemented in hanami-refdb-backend.c */

/* =========================================================================
 * High-level operations (stubs)
 * ========================================================================= */

hanami_error_t hanami_repo_open(git_repository **out,
                                const char *endpoint,
                                const char **relay_urls,
                                const char *repo_id,
                                const char *owner_pubkey,
                                const hanami_signer_t *signer,
                                const hanami_config_t *config)
{
    (void)endpoint;
    (void)relay_urls;
    (void)repo_id;
    (void)owner_pubkey;
    (void)signer;
    (void)config;

    if (!out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;
    /* TODO: Create bare repo, attach Blossom ODB + Nostr RefDB */
    return HANAMI_OK;
}

hanami_error_t hanami_clone(git_repository **out,
                            const char *nostr_uri,
                            const char *local_path,
                            const hanami_signer_t *signer,
                            const hanami_config_t *config)
{
    (void)nostr_uri;
    (void)local_path;
    (void)signer;
    (void)config;

    if (!out)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;
    /* TODO: Resolve URI, discover repo, fetch objects, reconstruct */
    return HANAMI_OK;
}

hanami_error_t hanami_push_to_blossom(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id)
{
    (void)repo;
    (void)endpoint;
    (void)signer;
    (void)relay_urls;
    (void)repo_id;

    /* TODO: Walk objects, upload missing, publish state */
    return HANAMI_OK;
}

hanami_error_t hanami_announce_repo(const char *repo_id,
                                    const char *name,
                                    const char *description,
                                    const char **clone_urls,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer)
{
    (void)repo_id;
    (void)name;
    (void)description;
    (void)clone_urls;
    (void)relay_urls;
    (void)signer;

    /* TODO: Create and publish kind 30617 event */
    return HANAMI_OK;
}

hanami_error_t hanami_publish_state(const char *repo_id,
                                    const hanami_ref_entry_t *refs,
                                    size_t ref_count,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer)
{
    (void)repo_id;
    (void)refs;
    (void)ref_count;
    (void)relay_urls;
    (void)signer;

    /* TODO: Create and publish kind 30618 event */
    return HANAMI_OK;
}
