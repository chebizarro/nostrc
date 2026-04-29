/*
 * hanami.c - Library lifecycle and high-level API
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami.h"
#include <git2/sys/odb_backend.h>
#include <git2/sys/refdb_backend.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

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
#include <time.h>

hanami_error_t hanami_repo_open(git_repository **out,
                                const char *endpoint,
                                const char **relay_urls,
                                const char *repo_id,
                                const char *owner_pubkey,
                                const hanami_signer_t *signer,
                                const hanami_config_t *config)
{
    if (!out || !endpoint || !relay_urls || !repo_id || !owner_pubkey)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;
    hanami_error_t err;

    /* 1. Create bare repository in a unique temp directory */
    char tmpdir[] = "/tmp/hanami-repo-XXXXXX";
    if (!mkdtemp(tmpdir))
        return HANAMI_ERR_IO;

    git_repository *repo = NULL;
    if (git_repository_init(&repo, tmpdir, 1) < 0)
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
    bool verify = config ? hanami_config_get_verify_on_read(config) : true;
    hanami_odb_backend_opts_t odb_opts = {
        .index = index,
        .client = client,
        .verify_on_read = verify
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
    } else {
        /* ODB attach failed — free the backend to prevent leak */
        odb_be->free(odb_be);
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
            } else {
                /* RefDB attach failed — free the backend */
                refdb_be->free(refdb_be);
            }
        } else {
            /* RefDB creation failed — free the nostr context */
            hanami_nostr_ctx_free(nostr_ctx);
            nostr_ctx = NULL;
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

    /*
     * Parse nostr:// or naddr1... URI to extract owner pubkey + repo_id.
     * Supported formats:
     *   nostr://<hex_pubkey>/<repo_id>
     *   nostr://npub1.../<repo_id>  (future: bech32 decode)
     */
    const char *prefix = "nostr://";
    size_t prefix_len = 8;
    if (strncmp(nostr_uri, prefix, prefix_len) != 0)
        return HANAMI_ERR_INVALID_ARG;

    const char *after_prefix = nostr_uri + prefix_len;
    const char *slash = strchr(after_prefix, '/');
    if (!slash || slash == after_prefix || !*(slash + 1))
        return HANAMI_ERR_INVALID_ARG;

    size_t pubkey_len = (size_t)(slash - after_prefix);
    char *owner_pubkey = strndup(after_prefix, pubkey_len);
    if (!owner_pubkey)
        return HANAMI_ERR_NOMEM;

    const char *repo_id = slash + 1;

    /* Query relays for kind 30617 to discover clone URLs and Blossom endpoints.
     * We use a set of default relays if no config is provided. */
    const char *default_relays[] = {
        "wss://relay.damus.io",
        "wss://nos.lol",
        "wss://relay.nostr.band",
        NULL
    };

    hanami_nostr_ctx_t *ctx = NULL;
    hanami_error_t err = hanami_nostr_ctx_new(default_relays, signer, &ctx);
    if (err != HANAMI_OK) {
        free(owner_pubkey);
        return err;
    }

    /* Fetch repository announcement to discover endpoints */
    nip34_repository_t *repo_info = NULL;
    err = hanami_nostr_fetch_repo(ctx, repo_id, owner_pubkey, &repo_info);
    if (err != HANAMI_OK || !repo_info) {
        hanami_nostr_ctx_free(ctx);
        free(owner_pubkey);
        return (err != HANAMI_OK) ? err : HANAMI_ERR_NOT_FOUND;
    }

    /* Extract Blossom endpoint from clone URLs (first https:// URL) */
    const char *endpoint = NULL;
    for (size_t i = 0; i < repo_info->clone_count && repo_info->clone[i]; i++) {
        if (strncmp(repo_info->clone[i], "https://", 8) == 0) {
            endpoint = repo_info->clone[i];
            break;
        }
        if (strncmp(repo_info->clone[i], "http://", 7) == 0 && !endpoint) {
            endpoint = repo_info->clone[i];
        }
    }

    if (!endpoint) {
        nip34_repository_free(repo_info);
        hanami_nostr_ctx_free(ctx);
        free(owner_pubkey);
        return HANAMI_ERR_NOT_FOUND;
    }

    /* Use repo's relay list if available, otherwise keep defaults */
    const char **relay_urls = (const char **)default_relays;
    if (repo_info->relays && repo_info->relay_count > 0) {
        relay_urls = (const char **)repo_info->relays;
    }

    /* Open the repository with Blossom + Nostr backends */
    err = hanami_repo_open(out, endpoint, (const char **)relay_urls,
                           repo_id, owner_pubkey, signer, config);

    nip34_repository_free(repo_info);
    hanami_nostr_ctx_free(ctx);
    free(owner_pubkey);

    if (err != HANAMI_OK)
        return err;

    /* Clone into local_path: init a working repo and fetch */
    git_repository *local_repo = NULL;
    int rc = git_clone(&local_repo, endpoint, local_path, NULL);
    if (rc < 0) {
        /* Fallback: create local repo and set up remote for later fetch */
        rc = git_repository_init(&local_repo, local_path, 0);
        if (rc < 0) {
            git_repository_free(*out);
            *out = NULL;
            return HANAMI_ERR_LIBGIT2;
        }
    }

    /* The caller gets the Blossom-backed repo for remote operations.
     * The local_repo clone is at local_path for working tree access. */
    if (local_repo && local_repo != *out) {
        git_repository_free(local_repo);
    }

    return HANAMI_OK;
}

/* =========================================================================
 * Push helpers: upload all ODB objects to Blossom
 * ========================================================================= */

typedef struct {
    git_odb *odb;
    hanami_blossom_client_t *client;
    hanami_index_t *index;
    bool upload_failed;
} push_upload_ctx_t;

/* Callback for git_odb_foreach — uploads each object to Blossom */
static int push_upload_object_cb(const git_oid *oid, void *payload)
{
    push_upload_ctx_t *ctx = (push_upload_ctx_t *)payload;
    if (ctx->upload_failed)
        return -1; /* stop iteration */

    git_odb_object *obj = NULL;
    if (git_odb_read(&obj, ctx->odb, oid) != 0)
        return 0; /* skip unreadable, continue */

    const void *data = git_odb_object_data(obj);
    size_t size = git_odb_object_size(obj);

    char blossom_hash[65];
    if (hanami_hash_blossom(data, size, blossom_hash) == HANAMI_OK) {
        /* Check if already on server */
        bool exists = false;
        hanami_blossom_head(ctx->client, blossom_hash, &exists);

        if (!exists) {
            hanami_error_t uerr = hanami_blossom_upload(
                ctx->client, (const uint8_t *)data, size,
                blossom_hash, NULL);
            if (uerr != HANAMI_OK) {
                ctx->upload_failed = true;
                git_odb_object_free(obj);
                return -1;
            }
        }

        /* Track in index */
        char oid_hex[65];
        git_oid_tostr(oid_hex, sizeof(oid_hex), oid);
        hanami_index_entry_t entry = {0};
        strncpy(entry.git_oid, oid_hex, sizeof(entry.git_oid) - 1);
        strncpy(entry.blossom_hash, blossom_hash,
                sizeof(entry.blossom_hash) - 1);
        entry.type = git_odb_object_type(obj);
        entry.size = size;
        entry.timestamp = (int64_t)time(NULL);
        hanami_index_put(ctx->index, &entry);
    }

    git_odb_object_free(obj);
    return 0;
}

hanami_error_t hanami_push_to_blossom(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id)
{
    if (!repo || !endpoint || !signer || !relay_urls || !repo_id)
        return HANAMI_ERR_INVALID_ARG;

    hanami_error_t err;

    /* 1. Create Blossom client for uploads */
    hanami_blossom_client_opts_t blossom_opts = {
        .endpoint = endpoint,
        .timeout_seconds = 60,
        .user_agent = "libhanami/0.1"
    };
    hanami_blossom_client_t *client = NULL;
    err = hanami_blossom_client_new(&blossom_opts, signer, &client);
    if (err != HANAMI_OK)
        return err;

    /* 2. Open in-memory index for hash tracking */
    hanami_index_t *index = NULL;
    err = hanami_index_open(&index, ":memory:", NULL);
    if (err != HANAMI_OK) {
        hanami_blossom_client_free(client);
        return err;
    }

    /* 3. Walk all reachable objects and upload to Blossom.
     *
     * We use git_odb_foreach to enumerate every object in the ODB.
     * This covers commits, trees, blobs, and tags — the revwalk-only
     * approach missed trees and blobs entirely (see nostrc-6xd). */
    git_odb *odb = NULL;
    if (git_repository_odb(&odb, repo) < 0) {
        hanami_index_close(index);
        hanami_blossom_client_free(client);
        return HANAMI_ERR_LIBGIT2;
    }

    push_upload_ctx_t upload_ctx = {
        .odb = odb,
        .client = client,
        .index = index,
        .upload_failed = false
    };
    git_odb_foreach(odb, push_upload_object_cb, &upload_ctx);
    bool upload_failed = upload_ctx.upload_failed;

    git_odb_free(odb);
    hanami_index_close(index);
    hanami_blossom_client_free(client);

    if (upload_failed)
        return HANAMI_ERR_UPLOAD;

    /* 4. Collect current ref state from repo */
    git_reference_iterator *ref_iter = NULL;
    if (git_reference_iterator_new(&ref_iter, repo) < 0)
        return HANAMI_ERR_LIBGIT2;

    /* Count refs first */
    size_t ref_cap = 16;
    size_t ref_count = 0;
    nip34_ref_t *refs = calloc(ref_cap, sizeof(nip34_ref_t));
    if (!refs) {
        git_reference_iterator_free(ref_iter);
        return HANAMI_ERR_NOMEM;
    }

    git_reference *ref = NULL;
    while (git_reference_next(&ref, ref_iter) == 0) {
        if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC) {
            git_reference_free(ref);
            continue;
        }

        if (ref_count >= ref_cap) {
            ref_cap *= 2;
            nip34_ref_t *tmp = realloc(refs, ref_cap * sizeof(nip34_ref_t));
            if (!tmp) {
                git_reference_free(ref);
                break;
            }
            refs = tmp;
        }

        refs[ref_count].refname = strdup(git_reference_name(ref));
        char oid_hex[41] = {0};
        git_oid_tostr(oid_hex, sizeof(oid_hex), git_reference_target(ref));
        refs[ref_count].target = strdup(oid_hex);
        ref_count++;
        git_reference_free(ref);
    }
    git_reference_iterator_free(ref_iter);

    /* 5. Publish kind 30618 with updated refs */
    hanami_nostr_ctx_t *nostr_ctx = NULL;
    err = hanami_nostr_ctx_new((const char *const *)relay_urls, signer, &nostr_ctx);
    if (err != HANAMI_OK) {
        for (size_t i = 0; i < ref_count; i++) {
            free(refs[i].refname);
            free(refs[i].target);
        }
        free(refs);
        return err;
    }

    err = hanami_nostr_publish_state(nostr_ctx, repo_id, refs, ref_count, NULL);

    hanami_nostr_ctx_free(nostr_ctx);
    for (size_t i = 0; i < ref_count; i++) {
        free(refs[i].refname);
        free(refs[i].target);
    }
    free(refs);

    return err;
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
