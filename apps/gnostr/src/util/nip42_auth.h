#ifndef GNOSTR_NIP42_AUTH_H
#define GNOSTR_NIP42_AUTH_H

#include <glib.h>
#include "nostr_pool.h"

/**
 * gnostr_nip42_setup_pool_auth:
 * @pool: a #GNostrPool
 *
 * Sets up NIP-42 relay authentication on a pool. When any relay in the
 * pool receives an AUTH challenge, the default signer service will be
 * used to sign the kind 22242 auth event and send it back.
 *
 * Must be called after the signer service is initialized. Safe to call
 * multiple times (replaces previous handler).
 */
void gnostr_nip42_setup_pool_auth(GNostrPool *pool);

#endif /* GNOSTR_NIP42_AUTH_H */
