#ifndef GH_CALL_SETTINGS_H
#define GH_CALL_SETTINGS_H
#include <gio/gio.h>
G_BEGIN_DECLS
/* Credential identity is the exact configured TURN URL + username. Neither the
 * password nor a credential-bearing URL is ever stored in GSettings.
 * Native macOS Keychain by default, Secret Service elsewhere (and in macOS
 * builds configured with GROUNDHOG_STORE_KEY_DEFAULT_BACKEND=secret-service).
 * Secret-store work runs off the UI thread; callbacks use the caller's main
 * context. Free returned passwords with gh_call_turn_password_free(). */
void gh_call_turn_password_store_async(const gchar *url, const gchar *username,
                                      const gchar *password, GCancellable *cancel,
                                      GAsyncReadyCallback callback, gpointer data);
gboolean gh_call_turn_password_store_finish(GAsyncResult *result, GError **error);
void gh_call_turn_password_lookup_async(const gchar *url, const gchar *username,
                                       GCancellable *cancel,
                                       GAsyncReadyCallback callback, gpointer data);
gchar *gh_call_turn_password_lookup_finish(GAsyncResult *result, GError **error);
/* Delete is idempotent. Lookup of a missing item returns G_IO_ERROR_NOT_FOUND;
 * locked/denied Keychain access is an error, never a missing credential.
 * Store replaces only the exact URL + username item. Empty passwords are refused. */
void gh_call_turn_password_delete_async(const gchar *url, const gchar *username,
                                       GCancellable *cancel,
                                       GAsyncReadyCallback callback, gpointer data);
gboolean gh_call_turn_password_delete_finish(GAsyncResult *result, GError **error);
void gh_call_turn_password_free(gchar *password);
/* Validate without DNS or opening sockets; URLs must not contain credentials. */
gboolean gh_call_ice_url_validate(const gchar *url, gboolean turn, GError **error);
G_END_DECLS
#endif
