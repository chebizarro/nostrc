/*
 * hanami-odb-backend.h - Custom git_odb_backend for Blossom object storage
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements the libgit2 ODB backend vtable to transparently store and
 * retrieve Git objects on Blossom servers. On write, objects are uploaded
 * to Blossom and the OID ↔ SHA-256 mapping is recorded. On read, the
 * mapping index is consulted and objects are fetched from Blossom.
 *
 * Designed to be stacked with other backends:
 *   [local loose (priority 3)] → [local pack (priority 2)] → [blossom (priority 1)]
 */

#ifndef HANAMI_ODB_BACKEND_H
#define HANAMI_ODB_BACKEND_H

#include "hanami-types.h"
#include <git2/odb.h>
#include <git2/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations for internal types */
typedef struct hanami_index hanami_index_t;
typedef struct hanami_blossom_client hanami_blossom_client_t;

/**
 * hanami_odb_backend_opts_t:
 * Options for creating a Blossom ODB backend.
 */
typedef struct {
    /** OID ↔ Blossom hash mapping index (required, borrowed — caller owns lifetime) */
    hanami_index_t *index;

    /** Blossom HTTP client (required, borrowed — caller owns lifetime) */
    hanami_blossom_client_t *client;

    /** Verify content hash on read (default: true) */
    bool verify_on_read;
} hanami_odb_backend_opts_t;

/**
 * hanami_odb_backend_new:
 * @out: (out): receives the new git_odb_backend pointer
 * @opts: configuration options (index and client required)
 *
 * Create a new Blossom ODB backend implementing the git_odb_backend vtable.
 * The returned backend can be added to a git_odb with git_odb_add_backend().
 *
 * The caller retains ownership of opts->index and opts->client; they must
 * outlive the backend.
 *
 * Returns: HANAMI_OK on success
 */
hanami_error_t hanami_odb_backend_new(git_odb_backend **out,
                                      const hanami_odb_backend_opts_t *opts);

#ifdef __cplusplus
}
#endif

#endif /* HANAMI_ODB_BACKEND_H */
