/* nd-token-store.h - Bearer token management via libsecret
 *
 * SPDX-License-Identifier: MIT
 *
 * Generates a random bearer token per account at first run, stores it
 * in the user's secret service (GNOME Keyring / KDE Wallet), and
 * validates incoming HTTP Basic auth passwords against it.
 *
 * The token is never persisted to disk -- it lives only in the secret
 * service. WebDAV clients (GOA/GNOME Calendar/etc.) send it as the
 * password in HTTP Basic auth.
 */
#ifndef ND_TOKEN_STORE_H
#define ND_TOKEN_STORE_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _NdTokenStore NdTokenStore;

/**
 * nd_token_store_new:
 *
 * Creates a new token store backed by libsecret.
 * Returns: (transfer full): a new token store, or NULL if libsecret
 *   is unavailable.
 */
NdTokenStore *nd_token_store_new(void);

/**
 * nd_token_store_free:
 * @store: (transfer full): the store to free
 */
void nd_token_store_free(NdTokenStore *store);

/**
 * nd_token_store_ensure_token:
 * @store: the token store
 * @account_id: unique account identifier (e.g. npub)
 * @error: (out) (optional): location for error
 *
 * Ensures a bearer token exists for @account_id. If none exists,
 * generates a 32-byte random token (base64url-encoded) and stores it.
 *
 * Returns: (transfer full) (nullable): the token string, or NULL on error.
 *   Caller frees with g_free().
 */
gchar *nd_token_store_ensure_token(NdTokenStore *store,
                                   const gchar  *account_id,
                                   GError      **error);

/**
 * nd_token_store_validate:
 * @store: the token store
 * @account_id: account to check against
 * @token: the bearer token to validate
 *
 * Returns: TRUE if @token matches the stored token for @account_id.
 */
gboolean nd_token_store_validate(NdTokenStore *store,
                                 const gchar  *account_id,
                                 const gchar  *token);

/**
 * nd_token_store_get_token:
 * @store: the token store
 * @account_id: the account
 * @error: (out) (optional): location for error
 *
 * Retrieves the existing token without creating one.
 *
 * Returns: (transfer full) (nullable): the token or NULL if not found.
 */
gchar *nd_token_store_get_token(NdTokenStore *store,
                                const gchar  *account_id,
                                GError      **error);

G_END_DECLS
#endif /* ND_TOKEN_STORE_H */
