/**
 * GnostrLogin - Login Dialog for NIP-55L and NIP-46 Authentication
 *
 * Provides sign-in options:
 * 1. NIP-55L: Local signer via D-Bus (gnostr-signer)
 * 2. NIP-46: Remote signer via bunker:// URI
 */

#ifndef GNOSTR_LOGIN_H
#define GNOSTR_LOGIN_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_LOGIN (gnostr_login_get_type())

G_DECLARE_FINAL_TYPE(GnostrLogin, gnostr_login, GNOSTR, LOGIN, GtkWindow)

/**
 * gnostr_login_new:
 * @parent: (nullable): The parent window
 *
 * Creates a new login dialog.
 *
 * Returns: (transfer full): A new #GnostrLogin
 */
GnostrLogin *gnostr_login_new(GtkWindow *parent);

G_END_DECLS

#endif /* GNOSTR_LOGIN_H */
