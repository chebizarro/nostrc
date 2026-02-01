/**
 * GnostrLogin - Login Dialog for NIP-55L and NIP-46 Authentication
 *
 * Provides sign-in options:
 * 1. NIP-55L: Local signer via D-Bus (gnostr-signer)
 * 2. NIP-46: Remote signer via bunker:// URI
 */

#ifndef GNOSTR_LOGIN_H
#define GNOSTR_LOGIN_H

#include <adwaita.h>
#include "nostr/nip46/nip46_client.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_LOGIN (gnostr_login_get_type())

G_DECLARE_FINAL_TYPE(GnostrLogin, gnostr_login, GNOSTR, LOGIN, AdwBin)

/**
 * gnostr_login_new:
 *
 * Creates a new login widget.
 * This widget should be embedded in a window or dialog for presentation.
 *
 * Returns: (transfer full): A new #GnostrLogin
 */
GnostrLogin *gnostr_login_new(void);

/**
 * gnostr_login_take_nip46_session:
 * @self: The login dialog
 *
 * Takes ownership of the NIP-46 session created during login.
 * After calling this, the login dialog no longer owns the session.
 *
 * Returns: (transfer full) (nullable): The NIP-46 session, or NULL if none
 */
NostrNip46Session *gnostr_login_take_nip46_session(GnostrLogin *self);

G_END_DECLS

#endif /* GNOSTR_LOGIN_H */
