/* nd-token-store.h - Bearer token management (file-backed, fail-closed)
 *
 * SPDX-License-Identifier: MIT
 *
 * The authoritative bearer token lives in `<config_dir>/token`
 * (default $XDG_CONFIG_HOME/nostr-dav/token): 256 random bits,
 * base64url-encoded, mode 0600, created atomically with O_EXCL
 * semantics. An existing token file with group/other permission bits,
 * the wrong owner, or a group/world-writable parent directory is an
 * error — callers must refuse to start rather than chmod-and-continue.
 *
 * When built with libsecret and asked to, a freshly minted token is
 * also mirrored into the user's keyring (best effort; validation never
 * consults the keyring).
 *
 * WebDAV clients send the token as the password in HTTP Basic auth.
 * Validation is constant-time and fails closed: no token loaded means
 * every request is rejected.
 */
#ifndef ND_TOKEN_STORE_H
#define ND_TOKEN_STORE_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_TOKEN_STORE_ERROR (nd_token_store_error_quark())
GQuark nd_token_store_error_quark(void);

typedef enum {
  ND_TOKEN_STORE_ERROR_IO = 1,
  ND_TOKEN_STORE_ERROR_PERMISSIONS,
  ND_TOKEN_STORE_ERROR_INVALID,
  ND_TOKEN_STORE_ERROR_ACCOUNT
} NdTokenStoreError;

typedef struct _NdTokenStore NdTokenStore;

/**
 * nd_token_store_default_dir:
 *
 * Returns: (transfer full): $XDG_CONFIG_HOME/nostr-dav
 */
gchar *nd_token_store_default_dir(void);

/**
 * nd_token_store_new:
 * @config_dir: directory holding the `token` file (created 0700 if
 *   missing)
 * @mirror_to_keyring: also store newly minted tokens via libsecret
 *   (ignored when built without libsecret)
 *
 * No file I/O happens until nd_token_store_ensure_token().
 *
 * Returns: (transfer full): a new token store.
 */
NdTokenStore *nd_token_store_new(const gchar *config_dir,
                                 gboolean     mirror_to_keyring);

void nd_token_store_free(NdTokenStore *store);

/**
 * nd_token_store_get_path:
 *
 * Returns: (transfer none): path of the token file.
 */
const gchar *nd_token_store_get_path(NdTokenStore *store);

/**
 * nd_token_store_ensure_token:
 * @store: the token store
 * @account_id: account the token authorizes (v1: a single account)
 * @error: (out) (optional): location for error
 *
 * Loads the token file, minting it on first run. Binds @store to
 * @account_id; a later call with a different account fails with
 * %ND_TOKEN_STORE_ERROR_ACCOUNT.
 *
 * Returns: (transfer full) (nullable): the token, or NULL on error.
 */
gchar *nd_token_store_ensure_token(NdTokenStore *store,
                                   const gchar  *account_id,
                                   GError      **error);

/**
 * nd_token_store_has_token:
 *
 * Returns: TRUE if a token for @account_id has been loaded.
 */
gboolean nd_token_store_has_token(NdTokenStore *store,
                                  const gchar  *account_id);

/**
 * nd_token_store_validate:
 *
 * Returns: TRUE only if a token for @account_id is loaded and equals
 *   @token (constant-time comparison).
 */
gboolean nd_token_store_validate(NdTokenStore *store,
                                 const gchar  *account_id,
                                 const gchar  *token);

G_END_DECLS
#endif /* ND_TOKEN_STORE_H */
