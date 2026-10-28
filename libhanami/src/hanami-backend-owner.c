/* SPDX-License-Identifier: MIT */
#include "hanami-backend-owner.h"
#include <stdlib.h>

hanami_backend_owner *hanami_backend_owner_new(void)
{
    hanami_backend_owner *owner = calloc(1, sizeof(*owner));
    if (owner) atomic_init(&owner->refs, 1);
    return owner;
}

void hanami_backend_owner_retain(hanami_backend_owner *owner)
{
    if (owner) atomic_fetch_add_explicit(&owner->refs, 1, memory_order_relaxed);
}

void hanami_backend_owner_release(hanami_backend_owner *owner)
{
    if (!owner || atomic_fetch_sub_explicit(&owner->refs, 1, memory_order_acq_rel) != 1)
        return;
    hanami_nostr_ctx_free(owner->nostr);
    blossom_client_free(owner->client);
    hanami_index_close(owner->index);
    free(owner);
}
