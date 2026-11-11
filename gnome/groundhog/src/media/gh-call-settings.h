#ifndef GH_CALL_SETTINGS_H
#define GH_CALL_SETTINGS_H
#include <gio/gio.h>
G_BEGIN_DECLS
/* Credential identity is the exact configured TURN URL + username. Neither the
 * password nor a credential-bearing URL is ever stored in GSettings. */
void gh_call_turn_password_store_async(const gchar *url, const gchar *username,
                                      const gchar *password, GCancellable *cancel,
                                      GAsyncReadyCallback callback, gpointer data);
gboolean gh_call_turn_password_store_finish(GAsyncResult *result, GError **error);
void gh_call_turn_password_lookup_async(const gchar *url, const gchar *username,
                                       GCancellable *cancel,
                                       GAsyncReadyCallback callback, gpointer data);
gchar *gh_call_turn_password_lookup_finish(GAsyncResult *result, GError **error);
void gh_call_turn_password_free(gchar *password);
/* Validate without DNS or opening sockets; URLs must not contain credentials. */
gboolean gh_call_ice_url_validate(const gchar *url, gboolean turn, GError **error);
G_END_DECLS
#endif
