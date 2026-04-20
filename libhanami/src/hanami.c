/*
 * hanami.c - Library lifecycle and high-level API stubs
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami.h"
#include <git2/sys/odb_backend.h>
#include <git2/sys/refdb_backend.h>
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

/* Configuration — implemented in hanami-config.c */

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

/* Transport — implemented in hanami-transport.c */

/* ODB backend — implemented in hanami-odb-backend.c */

/* RefDB backend — implemented in hanami-refdb-backend.c */

/* =========================================================================
 * High-level operations
 *
 * These functions orchestrate the internal components to provide
 * simple entry points for common Blossom + Nostr Git operations.
 * ========================================================================= */

#include "hanami/hanami-config.h"
#include "hanami/hanami-index.h"
#include "hanami/hanami-blossom-client.h"
#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-refdb-backend.h"
#include "hanami/hanami-nostr.h"
#include <nip34.h>

hanami_error_t hanami_repo_open(git_repository **out,
                                const char *endpoint,
                                const char **relay_urls,
                                const char *repo_id,
                                const char *owner_pubkey,
                                const hanami_signer_t *signer,
                                const hanami_config_t *config)
{
    (void)config; /* Future: use config for cache_dir, verify, etc. */

    if (!out || !endpoint || !relay_urls || !repo_id || !owner_pubkey)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;
    hanami_error_t err;

    /* 1. Create in-memory bare repository */
    git_repository *repo = NULL;
    if (git_repository_init(&repo, "/tmp/hanami-repo-XXXXXX", 1) < 0)
        return HANAMI_ERR_LIBGIT2;

    /* 2. Open index for OID ↔ Blossom hash mapping */
    hanami_index_t *index = NULL;
    err = hanami_index_open(&index, ":memory:", NULL);
    if (err != HANAMI_OK) {
        git_repository_free(repo);
        return err;
    }

    /* 3. Create Blossom client */
    hanami_blossom_client_opts_t blossom_opts = {
        .endpoint = endpoint,
        .timeout_seconds = 30,
        .user_agent = "libhanami/0.1"
    };
    hanami_blossom_client_t *client = NULL;
    err = hanami_blossom_client_new(&blossom_opts, signer, &client);
    if (err != HANAMI_OK) {
        hanami_index_close(index);
        git_repository_free(repo);
        return err;
    }

    /* 4. Create ODB backend */
    hanami_odb_backend_opts_t odb_opts = {
        .index = index,
        .client = client,
        .verify_on_read = true
    };
    git_odb_backend *odb_be = NULL;
    err = hanami_odb_backend_new(&odb_be, &odb_opts);
    if (err != HANAMI_OK) {
        hanami_blossom_client_free(client);
        hanami_index_close(index);
        git_repository_free(repo);
        return err;
    }

    /* 5. Attach ODB backend */
    git_odb *odb = NULL;
    if (git_repository_odb(&odb, repo) == 0) {
        git_odb_add_backend(odb, odb_be, 1);
        git_odb_free(odb);
    }

    /* 6. Create Nostr context */
    hanami_nostr_ctx_t *nostr_ctx = NULL;
    err = hanami_nostr_ctx_new(
        (const char *const *)relay_urls, signer, &nostr_ctx);
    if (err != HANAMI_OK) {
        /* Non-fatal: repo still works for local ops */
        nostr_ctx = NULL;
    }

    /* 7. Create RefDB backend (if we have nostr context) */
    if (nostr_ctx) {
        hanami_refdb_backend_opts_t refdb_opts = {
            .nostr_ctx = nostr_ctx,
            .repo_id = repo_id,
            .owner_pubkey = owner_pubkey
        };
        git_refdb_backend *refdb_be = NULL;
        err = hanami_refdb_backend_new(&refdb_be, &refdb_opts);
        if (err == HANAMI_OK) {
            git_refdb *refdb = NULL;
            if (git_repository_refdb(&refdb, repo) == 0) {
                git_refdb_set_backend(refdb, refdb_be);
                git_refdb_free(refdb);
            }
        }
    }

    *out = repo;
    return HANAMI_OK;
}

hanami_error_t hanami_clone(git_repository **out,
                            const char *nostr_uri,
                            const char *local_path,
                            const hanami_signer_t *signer,
                            const hanami_config_t *config)
{
    (void)config;

    if (!out || !nostr_uri || !local_path)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* TODO: Full implementation requires:
     * 1. Parse nostr:// URI to extract owner pubkey + repo_id
     * 2. Query relays for kind 30617 to discover Blossom endpoints
     * 3. Call hanami_repo_open with discovered endpoints
     * 4. Fetch all objects from Blossom
     * 5. Create working directory at local_path
     *
     * For now, return HANAMI_ERR_NOSTR until URI parsing is implemented.
     */
    (void)signer;
    return HANAMI_ERR_NOSTR;
}

hanami_error_t hanami_push_to_blossom(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id)
{
    if (!repo || !endpoint || !signer || !relay_urls || !repo_id)
        return HANAMI_ERR_INVALID_ARG;

    /* TODO: Full implementation requires:
     * 1. Walk all reachable objects in repo
     * 2. For each object, compute Blossom hash
     * 3. Check if blob exists on server (HEAD request)
     * 4. Upload missing blobs via PUT /upload
     * 5. Collect current ref state from repo
     * 6. Publish kind 30618 with updated refs
     */
    return HANAMI_OK;
}

hanami_error_t hanami_announce_repo(const char *repo_id,
                                    const char *name,
                                    const char *description,
                                    const char **clone_urls,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer)
{
    if (!repo_id || !name || !signer || !relay_urls)
        return HANAMI_ERR_INVALID_ARG;

    /* Create Nostr context */
    hanami_nostr_ctx_t *ctx = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(
        (const char *const *)relay_urls, signer, &ctx);
    if (err != HANAMI_OK)
        return err;

    err = hanami_nostr_publish_repo(ctx, repo_id, name, description,
                                     (const char *const *)clone_urls,
                                     NULL /* web_urls */);
    hanami_nostr_ctx_free(ctx);
    return err;
}

hanami_error_t hanami_publish_state(const char *repo_id,
                                    const hanami_ref_entry_t *refs,
                                    size_t ref_count,
                                    const char **relay_urls,
                                    const hanami_signer_t *signer)
{
    if (!repo_id || !relay_urls || !signer)
        return HANAMI_ERR_INVALID_ARG;

    /* Convert hanami_ref_entry_t to nip34_ref_t */
    nip34_ref_t *nip_refs = NULL;
    if (ref_count > 0 && refs) {
        nip_refs = calloc(ref_count, sizeof(nip34_ref_t));
        if (!nip_refs)
            return HANAMI_ERR_NOMEM;
        for (size_t i = 0; i < ref_count; i++) {
            nip_refs[i].refname = (char *)refs[i].refname;
            nip_refs[i].target = (char *)refs[i].target;
        }
    }

    /* Create Nostr context */
    hanami_nostr_ctx_t *ctx = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(
        (const char *const *)relay_urls, signer, &ctx);
    if (err != HANAMI_OK) {
        free(nip_refs);
        return err;
    }

    err = hanami_nostr_publish_state(ctx, repo_id, nip_refs, ref_count, NULL);
    hanami_nostr_ctx_free(ctx);
    free(nip_refs);
    return err;
}
