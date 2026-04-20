/*
 * hanami-odb-backend.c - Custom git_odb_backend for Blossom object storage
 *
 * SPDX-License-Identifier: MIT
 */

#include "hanami/hanami-odb-backend.h"
#include "hanami/hanami-index.h"
#include "hanami/hanami-blossom-client.h"

#include <git2/odb.h>
#include <git2/oid.h>
#include <git2/errors.h>
#include <git2/sys/odb_backend.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Extended backend struct (embeds git_odb_backend as first member)
 * ========================================================================= */

typedef struct {
    git_odb_backend parent;     /* Must be first — libgit2 casts to this */
    hanami_index_t *index;      /* Borrowed */
    hanami_blossom_client_t *client; /* Borrowed */
    bool verify_on_read;
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
    err = hanami_blossom_get(be->client, entry.blossom_hash, &raw_data, &raw_len);
    if (err != HANAMI_OK) {
        return (err == HANAMI_ERR_NOT_FOUND) ? GIT_ENOTFOUND : -1;
    }

    /* Optional: verify content hash */
    if (be->verify_on_read) {
        char verify_hash[65];
        if (hanami_hash_blossom(raw_data, raw_len, verify_hash) == HANAMI_OK) {
            if (strcmp(verify_hash, entry.blossom_hash) != 0) {
                free(raw_data);
                return -1; /* Hash mismatch — data corruption */
            }
        }
    }

    /* Allocate libgit2-managed buffer and copy data */
    void *odb_buf = git_odb_backend_data_alloc(_backend, raw_len);
    if (!odb_buf) {
        free(raw_data);
        return -1;
    }
    memcpy(odb_buf, raw_data, raw_len);
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

    /* Compute Blossom hash (SHA-256 of raw content) */
    char blossom_hash[65];
    hanami_error_t err = hanami_hash_blossom(data, len, blossom_hash);
    if (err != HANAMI_OK)
        return -1;

    /* Upload to Blossom */
    err = hanami_blossom_upload(be->client, (const uint8_t *)data, len,
                                blossom_hash, NULL);
    if (err != HANAMI_OK)
        return -1;

    /* Store OID ↔ Blossom hash mapping */
    char oid_hex[65];
    oid_to_hex(oid, oid_hex, sizeof(oid_hex));

    hanami_index_entry_t entry = {0};
    strncpy(entry.git_oid, oid_hex, sizeof(entry.git_oid) - 1);
    strncpy(entry.blossom_hash, blossom_hash, sizeof(entry.blossom_hash) - 1);
    entry.type = type;
    entry.size = len;
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
    /* For now, we don't support prefix matching on Blossom.
     * The local backends in the stack should handle this. */
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
    /* We don't own index or client — just free the backend struct */
    free(_backend);
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

    /* Not implemented (optional):
     * read_prefix, writestream, readstream, refresh, foreach, writepack, writemidx
     */

    *out = &be->parent;
    return HANAMI_OK;
}
