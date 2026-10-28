/*
 * hanami-odb-backend.c - Custom git_odb_backend for Blossom object storage
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-index.h"
#include "hanami-sealed.h"
#include "hanami-backend-owner.h"
#include <openssl/crypto.h>
#include <blossom/client.h>

#include <git2/odb.h>
#include <git2/oid.h>
#include <git2/errors.h>
#if defined(__has_include)
#  if __has_include(<git2/sys/errors.h>)
#    include <git2/sys/errors.h>
#  endif
#endif
#include <git2/sys/odb_backend.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Extended backend struct (embeds git_odb_backend as first member)
 * ========================================================================= */

typedef struct {
    git_odb_backend parent;     /* Must be first — libgit2 casts to this */
    hanami_index_t *index;      /* Borrowed */
    blossom_client_t *client; /* Borrowed */
    bool verify_on_read;
    const porthome_ring_t *vault;
    hanami_sealed *sealed;
    hanami_backend_owner *owner;
} hanami_odb_blossom_t;

/* =========================================================================
 * Helpers
 * ========================================================================= */

/* Convert a git_oid to hex string. Out must be at least 65 bytes. */
static void oid_to_hex(const git_oid *oid, char *out, size_t out_size)
{
    git_oid_tostr(out, out_size, oid);
}

/* =========================================================================
 * Vtable: read
 * ========================================================================= */

static int blossom_odb_read(void **data_p, size_t *len_p, git_object_t *type_p,
                            git_odb_backend *_backend, const git_oid *oid)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)_backend;

    /* Look up Blossom hash from index */
    char oid_hex[65];
    oid_to_hex(oid, oid_hex, sizeof(oid_hex));

    hanami_index_entry_t entry;
    hanami_error_t err = hanami_index_get_by_oid(be->index, oid_hex, &entry);
    if (err != HANAMI_OK)
        return GIT_ENOTFOUND;

    /* Fetch from Blossom */
    uint8_t *raw_data = NULL;
    size_t raw_len = 0;
    err = blossom_client_get_content(be->client, entry.blossom_hash, &raw_data, &raw_len);
    if (err != HANAMI_OK) {
        return (err == HANAMI_ERR_NOT_FOUND) ? GIT_ENOTFOUND : -1;
    }

    /* libblossom verifies the wire address before returning content. */
    if (be->vault) {
        uint8_t *plain = NULL;
        size_t plain_len = 0;
        int rc = porthome_decrypt_chunk_v2(be->vault, entry.chunk_key_id,
            raw_data, raw_len, entry.size, &plain, &plain_len);
        free(raw_data);
        if (rc != PORTHOME_OK) {
            git_error_set(GIT_ERROR_ODB, "sealed object authentication/epoch failed (%d)", rc);
            return -1;
        }
        raw_data = plain;
        raw_len = plain_len;
        git_oid actual;
        if (git_odb_hash(&actual, raw_data, raw_len, entry.type) < 0 ||
            !git_oid_equal(&actual, oid)) {
            OPENSSL_cleanse(raw_data, raw_len);
            free(raw_data);
            git_error_set(GIT_ERROR_ODB, "sealed Git OID mismatch");
            return -1;
        }
    } else {
        /* The wire hash only proves the bytes match the index row; the row
         * itself comes from a published manifest. Always re-hash. */
        git_oid actual;
        if (git_odb_hash(&actual, raw_data, raw_len, entry.type) < 0 ||
            !git_oid_equal(&actual, oid)) {
            free(raw_data);
            git_error_set(GIT_ERROR_ODB, "Blossom object %s does not hash to Git OID %s",
                          entry.blossom_hash, oid_hex);
            return -1;
        }
    }

    /* Allocate libgit2-managed buffer and copy data */
    void *odb_buf = git_odb_backend_data_alloc(_backend, raw_len);
    if (!odb_buf) {
        free(raw_data);
        return -1;
    }
    if (raw_len) memcpy(odb_buf, raw_data, raw_len);
    if (be->vault) OPENSSL_cleanse(raw_data, raw_len);
    free(raw_data);

    *data_p = odb_buf;
    *len_p = raw_len;
    *type_p = entry.type;
    return 0;
}

/* =========================================================================
 * Vtable: read_header
 * ========================================================================= */

static int blossom_odb_read_header(size_t *len_p, git_object_t *type_p,
                                   git_odb_backend *_backend,
                                   const git_oid *oid)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)_backend;

    char oid_hex[65];
    oid_to_hex(oid, oid_hex, sizeof(oid_hex));

    hanami_index_entry_t entry;
    hanami_error_t err = hanami_index_get_by_oid(be->index, oid_hex, &entry);
    if (err != HANAMI_OK)
        return GIT_ENOTFOUND;

    *len_p = entry.size;
    *type_p = entry.type;
    return 0;
}

/* =========================================================================
 * Vtable: write
 * ========================================================================= */

static int blossom_odb_write(git_odb_backend *_backend, const git_oid *oid,
                             const void *data, size_t len, git_object_t type)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)_backend;

    uint8_t *sealed = NULL;
    const uint8_t *upload = data;
    size_t upload_len = len;
    uint32_t key_id = 0;
    if (be->vault) {
        const porthome_ring_entry_t *key = &be->vault->entries[be->vault->count - 1];
        if (key->epoch > UINT32_MAX ||
            porthome_encrypt_chunk_v2(key->node_key, data, len, &sealed,
                                      &upload_len, NULL) != PORTHOME_OK)
            return -1;
        upload = sealed;
        key_id = (uint32_t)key->epoch;
    }
    char blossom_hash[65];
    hanami_error_t err = blossom_client_address(be->client, upload, upload_len, blossom_hash);
    if (err != HANAMI_OK) {
        free(sealed);
        return -1;
    }

    err = blossom_client_put(be->client, upload, upload_len, blossom_hash, NULL);
    free(sealed);
    if (err != HANAMI_OK)
        return -1;

    /* Store OID ↔ Blossom hash mapping */
    char oid_hex[65];
    oid_to_hex(oid, oid_hex, sizeof(oid_hex));

    hanami_index_entry_t entry = {0};
    snprintf(entry.git_oid, sizeof(entry.git_oid), "%s", oid_hex);
    snprintf(entry.blossom_hash, sizeof(entry.blossom_hash), "%s", blossom_hash);
    entry.type = type;
    entry.size = len;
    entry.chunk_key_id = key_id;
    entry.timestamp = (int64_t)time(NULL);

    err = hanami_index_put(be->index, &entry);
    if (err != HANAMI_OK)
        return -1;

    return 0;
}

/* =========================================================================
 * Vtable: exists
 * ========================================================================= */

static int blossom_odb_exists(git_odb_backend *_backend, const git_oid *oid)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)_backend;

    char oid_hex[65];
    oid_to_hex(oid, oid_hex, sizeof(oid_hex));

    /* Check local index first (fast path) */
    if (hanami_index_exists(be->index, oid_hex))
        return 1;

    /* Not in index — object not known to this backend */
    return 0;
}

/* =========================================================================
 * Vtable: exists_prefix
 * ========================================================================= */

static int blossom_odb_exists_prefix(git_oid *out, git_odb_backend *_backend,
                                     const git_oid *short_oid, size_t len)
{
    /* Prefix matching is not supported on Blossom (content-addressed by full hash).
     * Other backends in the ODB stack (e.g. local pack/loose) handle prefix lookups. */
    (void)out;
    (void)_backend;
    (void)short_oid;
    (void)len;
    return GIT_ENOTFOUND;
}

/* =========================================================================
 * Vtable: freshen
 * ========================================================================= */

static int blossom_odb_freshen(git_odb_backend *_backend, const git_oid *oid)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)_backend;

    char oid_hex[65];
    oid_to_hex(oid, oid_hex, sizeof(oid_hex));

    /* Update timestamp in index */
    hanami_index_entry_t entry;
    if (hanami_index_get_by_oid(be->index, oid_hex, &entry) == HANAMI_OK) {
        entry.timestamp = (int64_t)time(NULL);
        hanami_index_put(be->index, &entry);
        return 0;
    }

    return GIT_ENOTFOUND;
}

/* =========================================================================
 * Vtable: free
 * ========================================================================= */

static void blossom_odb_free(git_odb_backend *_backend)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)_backend;
    hanami_sealed_release(be->sealed);
    hanami_backend_owner_release(be->owner);
    free(be);
}

typedef struct { git_oid *oids; size_t count, capacity; } odb_visit;
static porthome_index_status visit_oid(void *payload, const porthome_index_entry *entry)
{
    odb_visit *v = payload;
    if (v->count == v->capacity) {
        size_t capacity = v->capacity ? v->capacity * 2 : 32;
        git_oid *grown = realloc(v->oids, capacity * sizeof(*grown));
        if (!grown) return PORTHOME_INDEX_ERR_OOM;
        v->oids = grown; v->capacity = capacity;
    }
    if (git_oid_fromstr(&v->oids[v->count], entry->git_oid) < 0)
        return PORTHOME_INDEX_ERR_STORAGE;
    v->count++;
    return PORTHOME_INDEX_OK;
}
static int blossom_odb_foreach(git_odb_backend *backend, git_odb_foreach_cb callback, void *payload)
{
    hanami_odb_blossom_t *be = (hanami_odb_blossom_t *)backend;
    odb_visit v = {0};
    int rc = porthome_index_iterate(hanami_index_porthome(be->index), visit_oid, &v);
    /* Snapshot first: libgit2's visitor may itself write objects/index rows. */
    for (size_t i = 0; rc == 0 && i < v.count; i++) rc = callback(&v.oids[i], payload);
    free(v.oids);
    return rc;
}

/* =========================================================================
 * Constructor
 * ========================================================================= */

hanami_error_t hanami_odb_backend_new(git_odb_backend **out,
                                      const hanami_odb_backend_opts_t *opts)
{
    if (!out || !opts)
        return HANAMI_ERR_INVALID_ARG;
    if (!opts->index || !opts->client)
        return HANAMI_ERR_INVALID_ARG;

    *out = NULL;

    hanami_odb_blossom_t *be = calloc(1, sizeof(*be));
    if (!be)
        return HANAMI_ERR_NOMEM;

    if (git_odb_init_backend(&be->parent, GIT_ODB_BACKEND_VERSION) < 0) {
        free(be);
        return HANAMI_ERR_LIBGIT2;
    }

    be->index = opts->index;
    be->client = opts->client;
    be->verify_on_read = opts->verify_on_read;

    /* Wire up vtable */
    be->parent.read = blossom_odb_read;
    be->parent.read_header = blossom_odb_read_header;
    be->parent.write = blossom_odb_write;
    be->parent.exists = blossom_odb_exists;
    be->parent.exists_prefix = blossom_odb_exists_prefix;
    be->parent.freshen = blossom_odb_freshen;
    be->parent.free = blossom_odb_free;
    be->parent.foreach = blossom_odb_foreach;

    /* Not implemented (optional):
     * read_prefix, writestream, readstream, refresh, foreach, writepack, writemidx
     */

    *out = &be->parent;
    return HANAMI_OK;
}

hanami_error_t hanami_owned_odb_backend_new(git_odb_backend **out,
    hanami_backend_owner *owner, bool verify_on_read)
{
    hanami_odb_backend_opts_t opts = {
        .index = owner->index, .client = owner->client,
        .verify_on_read = verify_on_read
    };
    hanami_error_t err = hanami_odb_backend_new(out, &opts);
    if (err == HANAMI_OK) {
        ((hanami_odb_blossom_t *)*out)->owner = owner;
        hanami_backend_owner_retain(owner);
    }
    return err;
}

hanami_error_t hanami_sealed_odb_new(git_odb_backend **out, hanami_sealed *sealed)
{
    hanami_odb_backend_opts_t opts = {
        .index = sealed->index, .client = sealed->client,
        .verify_on_read = true
    };
    hanami_error_t err = hanami_odb_backend_new(out, &opts);
    if (err == HANAMI_OK) {
        ((hanami_odb_blossom_t *)*out)->sealed = sealed;
        ((hanami_odb_blossom_t *)*out)->vault = &sealed->ring;
        hanami_sealed_retain(sealed);
    }
    return err;
}

hanami_sealed *hanami_sealed_for_repo(git_repository *repo)
{
    git_odb *odb = NULL;
    if (!repo || git_repository_odb(&odb, repo) < 0) return NULL;
    hanami_sealed *sealed = NULL;
    for (size_t i = 0; i < git_odb_num_backends(odb); i++) {
        git_odb_backend *be = NULL;
        if (git_odb_get_backend(&be, odb, i) == 0 && be->free == blossom_odb_free) {
            sealed = ((hanami_odb_blossom_t *)be)->sealed;
            if (sealed) break;
        }
    }
    git_odb_free(odb);
    return sealed;
}
