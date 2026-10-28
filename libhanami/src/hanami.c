/*
 * hanami.c - Library lifecycle and high-level API
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include "hanami/hanami.h"
#include "hanami-sealed.h"
#include "hanami-discovery.h"
#include "hanami-fetch.h"
#include "hanami-manifest.h"
#include "hanami-nostr-internal.h"
#include "hanami-index-private.h"
#include <git2/sys/odb_backend.h>
#include <git2/sys/refdb_backend.h>
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <ftw.h>
#include <limits.h>

static _Atomic int hanami_ref_count = 0;

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
    if (atomic_fetch_add(&hanami_ref_count, 1) > 0)
        return HANAMI_OK;

    /* Future: initialize curl, index, etc. */
    return HANAMI_OK;
}

void hanami_shutdown(void)
{
    if (atomic_fetch_sub(&hanami_ref_count, 1) > 1)
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
#include <blossom/client.h>
#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-refdb-backend.h"
#include "hanami-backend-owner.h"
#include "hanami/hanami-nostr.h"
#include <nip34.h>
#include <time.h>

/*
 * attach_blossom_backends:
 * Helper to attach Blossom ODB + Nostr RefDB backends to a repository.
 * This is shared by hanami_repo_open (for bare temp repos) and
 * hanami_clone (for working repos at a specific local path).
 */
static hanami_error_t attach_blossom_backends(
    git_repository *repo,
    const char *endpoint,
    const char **relay_urls,
    const char *repo_id,
    const char *owner_pubkey,
    const hanami_signer_t *signer,
    const hanami_config_t *config)
{
    if (!repo || !endpoint || !relay_urls || !repo_id || !owner_pubkey)
        return HANAMI_ERR_INVALID_ARG;

    hanami_backend_owner *owner = hanami_backend_owner_new();
    if (!owner) return HANAMI_ERR_NOMEM;

    /* 1. OID ↔ Blossom hash cache (<gitdir>/hanami/index.sqlite) */
    hanami_error_t err = hanami_repo_cache_open(repo, &owner->index);
    if (err != HANAMI_OK) goto done;

    /* 2. Create Blossom client */
    blossom_client_opts_t blossom_opts = {
        .endpoint = endpoint,
        .timeout_seconds = 30,
        .allow_insecure = hanami_config_get_allow_insecure(config),
        .user_agent = "libhanami/0.4"
    };
    err = blossom_client_new(&blossom_opts, signer, &owner->client);
    if (err != HANAMI_OK) goto done;

    /* 3. Create ODB backend */
    bool verify = config ? hanami_config_get_verify_on_read(config) : true;
    git_odb_backend *odb_be = NULL;
    err = hanami_owned_odb_backend_new(&odb_be, owner, verify);
    if (err != HANAMI_OK) goto done;

    /* 4. Attach ODB backend */
    git_odb *odb = NULL;
    if (git_repository_odb(&odb, repo) < 0 || git_odb_add_backend(odb, odb_be, 1) < 0) {
        odb_be->free(odb_be);
        git_odb_free(odb);
        err = HANAMI_ERR_LIBGIT2;
        goto done;
    }
    git_odb_free(odb);

    /* 5. Create Nostr context */
    err = hanami_nostr_ctx_new(
        (const char *const *)relay_urls, signer, &owner->nostr);
    if (err != HANAMI_OK) {
        /* Non-fatal: repo still works for local ops */
        err = HANAMI_OK;
        goto done;
    }

    /* 6. Create RefDB backend (if we have nostr context) */
    if (owner->nostr) {
        hanami_refdb_backend_opts_t refdb_opts = {
            .nostr_ctx = owner->nostr,
            .repo_id = repo_id,
            .owner_pubkey = owner_pubkey
        };
        git_refdb_backend *refdb_be = NULL;
        err = hanami_refdb_backend_new(&refdb_be, &refdb_opts);
        if (err == HANAMI_OK) {
            git_refdb *refdb = NULL;
            /* Ref writes publish objects' manifest rows before the 30618. */
            hanami_refdb_use_manifest(refdb_be, owner);
            if (git_repository_refdb(&refdb, repo) < 0 ||
                git_refdb_set_backend(refdb, refdb_be) < 0) {
                refdb_be->free(refdb_be);
                git_refdb_free(refdb);
                err = HANAMI_ERR_LIBGIT2;
                goto done;
            }
            git_refdb_free(refdb);
        } else {
            /* RefDB creation failed — free the nostr context */
            hanami_nostr_ctx_free(owner->nostr);
            owner->nostr = NULL;
            err = HANAMI_OK;
        }
    }

    /* 7. Make existing remote objects readable: import the owner's published
     * manifest chain. A found but invalid/unavailable chain fails visibly;
     * relay query failures stay non-fatal, as for the refdb's ref load. */
    if (owner->nostr) {
        nip34_repo_state_t *state = NULL;
        char manifest[65] = {0};
        if (hanami_nostr_fetch_state_manifest(owner->nostr, repo_id, owner_pubkey,
                                              &state, manifest) == HANAMI_OK && manifest[0]) {
            err = hanami_manifest_sync(owner->client, owner->index, repo_id, manifest, NULL);
            if (err != HANAMI_OK)
                git_error_set(GIT_ERROR_INVALID, "hanami: manifest chain %s for %s is "
                              "unavailable or invalid", manifest, repo_id);
        }
        nip34_repo_state_free(state);
    }

done:
    hanami_backend_owner_release(owner);
    return err;
}

static int remove_temp_entry(const char *path, const struct stat *st, int kind, struct FTW *ftw)
{
    (void)st; (void)kind; (void)ftw;
    return remove(path);
}

hanami_error_t hanami_repo_open(git_repository **out,
                                const char *endpoint,
                                const char **relay_urls,
                                const char *repo_id,
                                const char *owner_pubkey,
                                const hanami_signer_t *signer,
                                const hanami_config_t *config,
                                const porthome_ring_t *vault)
{
    if (!out || !endpoint || !relay_urls || !repo_id || !owner_pubkey)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* 1. Create bare repository in a unique temp directory */
    char tmpdir[PATH_MAX];
    const char *tmp_root = getenv("TMPDIR");
    if (!tmp_root || !*tmp_root) tmp_root = "/tmp";
    int n = snprintf(tmpdir, sizeof(tmpdir), "%s/hanami-repo-XXXXXX", tmp_root);
    if (n < 0 || (size_t)n >= sizeof(tmpdir)) return HANAMI_ERR_IO;
    if (!mkdtemp(tmpdir))
        return HANAMI_ERR_IO;

    git_repository *repo = NULL;
    if (git_repository_init(&repo, tmpdir, 1) < 0) {
        nftw(tmpdir, remove_temp_entry, 16, FTW_DEPTH | FTW_PHYS);
        return HANAMI_ERR_LIBGIT2;
    }

    /* 2. Attach Blossom ODB + Nostr RefDB backends */
    hanami_error_t err;
    if (vault) {
        hanami_sealed *sealed = NULL;
        err = hanami_sealed_new(&sealed, endpoint, relay_urls, repo_id,
                                owner_pubkey, signer, vault, config);
        if (err == HANAMI_OK) {
            err = hanami_sealed_attach_odb(repo, sealed);
            git_refdb_backend *backend = NULL;
            git_refdb *refdb = NULL;
            if (err == HANAMI_OK)
                err = hanami_sealed_refdb_new(&backend, sealed);
            if (err == HANAMI_OK) {
                if (git_repository_refdb(&refdb, repo) < 0 ||
                    git_refdb_set_backend(refdb, backend) < 0) {
                    backend->free(backend);
                    err = HANAMI_ERR_LIBGIT2;
                }
                git_refdb_free(refdb);
            }
            hanami_sealed_release(sealed);
        }
    } else {
        err = attach_blossom_backends(
            repo, endpoint, relay_urls, repo_id, owner_pubkey, signer, config);
    }
    if (err != HANAMI_OK) {
        git_repository_free(repo);
        nftw(tmpdir, remove_temp_entry, 16, FTW_DEPTH | FTW_PHYS);
        return err;
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
    if (!out || !nostr_uri || !local_path)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    /* nostr://<hex_pubkey>/<repo_id> (npub decoding is not supported). */
    char *owner_pubkey = NULL, *repo_id = NULL;
    hanami_error_t err = hanami_parse_nostr_uri(nostr_uri, &owner_pubkey, &repo_id);
    if (err != HANAMI_OK)
        return err;

    /* Discovery relays: configured relays, else public defaults. */
    const char *default_relays[] = {
        "wss://nos.lol",
        "wss://relay.nostr.band",
        NULL
    };
    const char *const *discovery_relays = default_relays;
    if (hanami_config_get_relay_count(config) > 0)
        discovery_relays = hanami_config_get_relays(config);

    hanami_nostr_ctx_t *ctx = NULL;
    nip34_repository_t *repo_info = NULL;
    char *endpoint = NULL;
    err = hanami_nostr_ctx_new(discovery_relays, signer, &ctx);

    /* Fetch repository announcement to discover relays */
    if (err == HANAMI_OK) {
        err = hanami_nostr_fetch_repo(ctx, repo_id, owner_pubkey, &repo_info);
        if (err == HANAMI_OK && !repo_info)
            err = HANAMI_ERR_NOT_FOUND;
    }

    /* Later snapshots (kind-10063, kind-30618) cover discovery ∪ announced
     * relays in one EOSE query, so the newest replaceable event wins across
     * both and a state missing from one set is not mistaken for "empty". */
    char **relays = NULL;
    hanami_nostr_ctx_t *repo_ctx = NULL;
    if (err == HANAMI_OK) {
        relays = hanami_relay_union(discovery_relays,
                                    (const char *const *)repo_info->relays);
        err = relays ? hanami_nostr_ctx_new((const char *const *)relays, signer, &repo_ctx)
                     : HANAMI_ERR_NOMEM;
    }
    /* ... ∪ the owner's NIP-65 write relays (bounded; nostrc-nd9yl). */
    if (err == HANAMI_OK)
        err = hanami_add_outbox_relays(&repo_ctx, &relays, owner_pubkey, signer);

    /* Blossom API endpoint: explicit config or the owner's BUD-03 kind-10063
     * list, classified by libblossom's probe. NIP-34 `clone` URLs are Git
     * remotes, never Blossom APIs (nostrc-g2g37). */
    if (err == HANAMI_OK)
        err = hanami_select_blossom_endpoint(repo_ctx, owner_pubkey, repo_id, config, &endpoint);

    /* Objects are verified and written locally; refs, remote "origin" and
     * the .git/hanami cache make the clone durable (nostrc-v2zyh). */
    if (err == HANAMI_OK)
        err = hanami_clone_materialize(out, nostr_uri, local_path, owner_pubkey, repo_id,
                                       endpoint, (const char *const *)relays, signer, config);

    hanami_nostr_ctx_free(repo_ctx);
    for (size_t i = 0; relays && relays[i]; i++) free(relays[i]);
    free(relays);
    free(endpoint);
    nip34_repository_free(repo_info);
    hanami_nostr_ctx_free(ctx);
    free(owner_pubkey);
    free(repo_id);
    return err;
}

/* =========================================================================
 * Push: objects, then manifest segment, then kind-30618 (nostrc-v2zyh)
 * ========================================================================= */

typedef struct {
    git_odb *odb;
    blossom_client_t *client;
    hanami_index_t *index;
    hanami_error_t err;
} push_upload_ctx_t;

/* git_odb_foreach callback: make every local object retrievable from the
 * Blossom server and record it in the repo cache. New rows stay unpublished
 * until a manifest segment covering them is accepted. */
static int push_upload_object_cb(const git_oid *oid, void *payload)
{
    push_upload_ctx_t *ctx = (push_upload_ctx_t *)payload;
    char oid_hex[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(oid_hex, sizeof(oid_hex), oid);

    git_odb_object *obj = NULL;
    if (git_odb_read(&obj, ctx->odb, oid) != 0) {
        ctx->err = HANAMI_ERR_LIBGIT2;
        git_error_set(GIT_ERROR_ODB, "hanami: cannot read local object %s", oid_hex);
        return -1;
    }
    const void *data = git_odb_object_data(obj);
    size_t size = git_odb_object_size(obj);

    char blossom_hash[65];
    bool exists = false;
    hanami_error_t err = blossom_client_address(ctx->client, data, size, blossom_hash);
    if (err == HANAMI_OK)
        err = blossom_client_head(ctx->client, blossom_hash, &exists);
    if (err == HANAMI_OK && !exists)
        err = blossom_client_put(ctx->client, (const uint8_t *)data, size,
                                 blossom_hash, NULL);
    /* An existing row naming another address the server lacks (e.g. a
     * different shim encoding) is repaired and republished; newer segments
     * win for cold readers. */
    hanami_index_entry_t known;
    bool repair = false;
    if (err == HANAMI_OK && hanami_index_get_by_oid(ctx->index, oid_hex, &known) == HANAMI_OK &&
        strcmp(known.blossom_hash, blossom_hash) != 0) {
        bool old_exists = false;
        err = blossom_client_head(ctx->client, known.blossom_hash, &old_exists);
        repair = err == HANAMI_OK && !old_exists;
        if (repair) err = hanami_index_unmark_published(ctx->index, oid_hex);
    }
    if (err == HANAMI_OK && (repair || !hanami_index_exists(ctx->index, oid_hex))) {
        hanami_index_entry_t entry = {0};
        snprintf(entry.git_oid, sizeof(entry.git_oid), "%s", oid_hex);
        snprintf(entry.blossom_hash, sizeof(entry.blossom_hash), "%s", blossom_hash);
        entry.type = git_odb_object_type(obj);
        entry.size = size;
        entry.timestamp = (int64_t)time(NULL);
        err = hanami_index_put(ctx->index, &entry);
    }
    git_odb_object_free(obj);
    if (err != HANAMI_OK) {
        ctx->err = err == HANAMI_ERR_INDEX ? err : HANAMI_ERR_UPLOAD;
        git_error_set(GIT_ERROR_NET, "hanami: cannot upload object %s: %s",
                      oid_hex, hanami_strerror(err));
        return -1;
    }
    return 0;
}

static void free_refs(nip34_ref_t *refs, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        free(refs[i].refname);
        free(refs[i].target);
    }
    free(refs);
}

/* Direct local refs (remote-tracking refs are not the repository's state)
 * plus HEAD, which a cold clone needs to check out. */
static hanami_error_t collect_push_refs(git_repository *repo, nip34_ref_t **out,
                                        size_t *count, char **head)
{
    *out = NULL; *count = 0; *head = NULL;
    git_reference_iterator *iter = NULL;
    if (git_reference_iterator_new(&iter, repo) < 0)
        return HANAMI_ERR_LIBGIT2;
    size_t cap = 16, n = 0;
    nip34_ref_t *refs = calloc(cap, sizeof(*refs));
    hanami_error_t err = refs ? HANAMI_OK : HANAMI_ERR_NOMEM;
    git_reference *ref = NULL;
    int rc = 0;
    while (err == HANAMI_OK && (rc = git_reference_next(&ref, iter)) == 0) {
        const char *name = git_reference_name(ref);
        if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC ||
            strncmp(name, "refs/remotes/", 13) == 0) {
            git_reference_free(ref);
            continue;
        }
        if (n == cap) {
            nip34_ref_t *grown = realloc(refs, cap * 2 * sizeof(*refs));
            if (!grown) { git_reference_free(ref); err = HANAMI_ERR_NOMEM; break; }
            refs = grown; cap *= 2;
        }
        char hex[GIT_OID_SHA1_HEXSIZE + 1];
        git_oid_tostr(hex, sizeof(hex), git_reference_target(ref));
        refs[n].refname = strdup(name);
        refs[n].target = strdup(hex);
        git_reference_free(ref);
        if (!refs[n].refname || !refs[n].target) err = HANAMI_ERR_NOMEM;
        n++;
    }
    git_reference_iterator_free(iter);
    if (err == HANAMI_OK && rc != GIT_ITEROVER) err = HANAMI_ERR_LIBGIT2;
    if (err == HANAMI_OK && git_reference_lookup(&ref, repo, "HEAD") == 0) {
        char buf[1100];
        if (git_reference_type(ref) == GIT_REFERENCE_SYMBOLIC)
            snprintf(buf, sizeof(buf), "ref: %s", git_reference_symbolic_target(ref));
        else
            git_oid_tostr(buf, sizeof(buf), git_reference_target(ref));
        git_reference_free(ref);
        if (!(*head = strdup(buf))) err = HANAMI_ERR_NOMEM;
    }
    if (err != HANAMI_OK) { free_refs(refs, n); free(*head); *head = NULL; return err; }
    *out = refs; *count = n;
    return HANAMI_OK;
}

hanami_error_t hanami_push_repo_state(git_repository *repo, blossom_client_t *client,
    hanami_nostr_ctx_t *ctx, const char *repo_id, const char *author)
{
    if (!repo || !client || !ctx || !repo_id || !author)
        return HANAMI_ERR_INVALID_ARG;
    hanami_index_t *cache = NULL;
    hanami_error_t err = hanami_repo_cache_open(repo, &cache);
    if (err != HANAMI_OK) return err;

    /* 1. Objects (every local object, so readers can resolve any ref). */
    git_odb *odb = NULL;
    if (git_repository_odb(&odb, repo) < 0) {
        hanami_index_close(cache);
        return HANAMI_ERR_LIBGIT2;
    }
    push_upload_ctx_t upload = { .odb = odb, .client = client, .index = cache, .err = HANAMI_OK };
    if (git_odb_foreach(odb, push_upload_object_cb, &upload) != 0 && upload.err == HANAMI_OK)
        upload.err = HANAMI_ERR_LIBGIT2;
    git_odb_free(odb);
    err = upload.err;

    /* 2-3. Manifest segment, then the tagged 30618 (relay OK true required). */
    nip34_ref_t *refs = NULL;
    size_t count = 0;
    char *head = NULL;
    if (err == HANAMI_OK) err = collect_push_refs(repo, &refs, &count, &head);
    if (err == HANAMI_OK)
        err = hanami_manifest_publish_state(client, ctx, cache, repo_id, author,
                                            refs, count, head);
    free_refs(refs, count);
    free(head);
    hanami_index_close(cache);
    return err;
}

hanami_error_t hanami_push_to_blossom_ex(git_repository *repo,
                                         const char *endpoint,
                                         const hanami_signer_t *signer,
                                         const char **relay_urls,
                                         const char *repo_id,
                                         const hanami_config_t *config)
{
    if (!repo || !endpoint || !signer || !signer->pubkey || !relay_urls || !repo_id)
        return HANAMI_ERR_INVALID_ARG;

    hanami_sealed *sealed = hanami_sealed_for_repo(repo);
    if (hanami_repo_private(repo))
        return HANAMI_ERR_AUTH;

    blossom_client_opts_t blossom_opts = {
        .endpoint = endpoint,
        .timeout_seconds = 60,
        .allow_insecure = (sealed && sealed->allow_insecure) ||
                          hanami_config_get_allow_insecure(config),
        .user_agent = "libhanami/" HANAMI_VERSION_STRING
    };
    blossom_client_t *client = NULL;
    hanami_error_t err = blossom_client_new(&blossom_opts, signer, &client);
    if (err != HANAMI_OK)
        return err;
    hanami_nostr_ctx_t *nostr_ctx = NULL;
    err = hanami_nostr_ctx_new((const char *const *)relay_urls, signer, &nostr_ctx);
    if (err == HANAMI_OK)
        err = hanami_push_repo_state(repo, client, nostr_ctx, repo_id, signer->pubkey);
    hanami_nostr_ctx_free(nostr_ctx);
    blossom_client_free(client);
    return err;
}

hanami_error_t hanami_push_to_blossom(git_repository *repo,
                                      const char *endpoint,
                                      const hanami_signer_t *signer,
                                      const char **relay_urls,
                                      const char *repo_id)
{
    return hanami_push_to_blossom_ex(repo, endpoint, signer, relay_urls, repo_id, NULL);
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

    /* Refs-only publication: carry the published manifest head forward so
     * cold readers can still locate objects. Readers reject refs it does
     * not cover; use hanami_push_to_blossom to publish new objects. */
    nip34_repo_state_t *published = NULL;
    char manifest[65] = {0};
    err = signer->pubkey
        ? hanami_nostr_fetch_state_manifest(ctx, repo_id, signer->pubkey, &published, manifest)
        : HANAMI_ERR_AUTH;
    if (err == HANAMI_ERR_NOT_FOUND) err = HANAMI_OK;
    /* Without a Blossom client the chain cannot be read here, so coverage is
     * established conservatively: every direct target must already be a
     * published ref/HEAD target. New commits need hanami_push_to_blossom. */
    for (size_t i = 0; err == HANAMI_OK && manifest[0] && i < ref_count; i++) {
        const char *target = nip_refs[i].target;
        if (!target || strncmp(target, "ref: ", 5) == 0) continue;
        bool known = published->head && !strcmp(published->head, target);
        for (size_t j = 0; !known && j < published->ref_count; j++)
            known = published->refs[j].target && !strcmp(published->refs[j].target, target);
        if (!known) {
            git_error_set(GIT_ERROR_INVALID, "hanami: %s -> %s is not covered by the published "
                          "manifest; use hanami_push_to_blossom", nip_refs[i].refname, target);
            err = HANAMI_ERR_INDEX;
        }
    }
    nip34_repo_state_free(published);
    if (err == HANAMI_OK)
        err = hanami_nostr_publish_state_manifest(ctx, repo_id, nip_refs, ref_count, NULL,
                                                  manifest[0] ? manifest : NULL);
    hanami_nostr_ctx_free(ctx);
    free(nip_refs);
    return err;
}
