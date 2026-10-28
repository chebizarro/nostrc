/* SPDX-License-Identifier: MIT — private public-repo backend ownership. */
#ifndef HANAMI_BACKEND_OWNER_H
#define HANAMI_BACKEND_OWNER_H

#include "hanami/hanami-index.h"
#include "hanami/hanami-nostr.h"
#include <blossom/client.h>
#include <git2/sys/odb_backend.h>
#include <git2/sys/refdb_backend.h>
#include <stdatomic.h>

/* Each attached backend retains the resources it borrows. The assembly keeps
 * one reference until attachment and manifest import have finished. */
typedef struct hanami_backend_owner {
    atomic_uint refs;
    hanami_index_t *index;
    blossom_client_t *client;
    hanami_nostr_ctx_t *nostr;
} hanami_backend_owner;

hanami_backend_owner *hanami_backend_owner_new(void);
void hanami_backend_owner_retain(hanami_backend_owner *owner);
void hanami_backend_owner_release(hanami_backend_owner *owner);
hanami_error_t hanami_owned_odb_backend_new(git_odb_backend **out,
    hanami_backend_owner *owner, bool verify_on_read);
void hanami_refdb_use_manifest(git_refdb_backend *backend,
    hanami_backend_owner *owner);

#endif
