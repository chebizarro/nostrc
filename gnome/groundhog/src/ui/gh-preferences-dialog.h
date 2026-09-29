#ifndef GH_PREFERENCES_DIALOG_H
#define GH_PREFERENCES_DIALOG_H

#include <adwaita.h>

G_BEGIN_DECLS

/*
 * GhPreferencesDialog: Groundhog's preferences (privacy charter §7.11, G17),
 * a composite AdwPreferencesDialog (data/ui/gh-preferences-dialog.blp) with
 * four pages, "privacy", "messages", "network" and "account". Each row is
 * bound both ways to one key of the org.nostr.Groundhog settings it is given:
 *
 *   Privacy   notifications-enabled, notification-privacy, sound-enabled,
 *             load-remote-images, link-previews, load-profile-pictures,
 *             filter-unknown-senders, show-message-previews
 *   Messages  enter-sends, default-disappearing-seconds, retention-days
 *   Network   network-mode, tor-socks-address, discovery-relays,
 *             blossom-servers
 *   Account   signer-method, run-in-background
 *
 * Switches follow their key directly. A choice row shows the key's value; a
 * value that none of its choices has (set elsewhere, e.g. retention-days=90)
 * is shown as an extra item and never rewritten until the user picks another
 * choice. Addresses and lists are written only when the user applies a valid
 * entry: relay URLs must be wss:// (ws:// only for this computer, or a .onion
 * with Tor), attachment servers https://, and neither may carry credentials,
 * a query or a fragment.
 *
 * Tor (§4.2) is G09's: without tor_available the Tor choice is removed, the
 * Tor address row stays hidden and the Network page says that proxy settings
 * are not used yet. The Account page shows the account set with
 * gh_preferences_dialog_set_account(); with a forget function it offers
 * "Delete All Messages on This Device" (the prefs.delete-all action), which
 * asks for confirmation in an AdwAlertDialog and then runs the function for
 * that account: in the application, GhAccountStore's forget (charter §3.8,
 * ST-8), which also signs the account out on this device. The dialog cannot
 * be closed while that runs; the result is shown as a toast.
 */
#define GH_TYPE_PREFERENCES_DIALOG (gh_preferences_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhPreferencesDialog, gh_preferences_dialog, GH, PREFERENCES_DIALOG,
                     AdwPreferencesDialog)

/* Deletes everything of the account npub stores on this device and signs it
 * out there; the result is read with the matching finish function. target is
 * the object given to gh_preferences_dialog_set_forget_func(). */
typedef void (*GhPreferencesForgetAsyncFunc)(GObject *target, const gchar *npub,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback, gpointer user_data);
typedef gboolean (*GhPreferencesForgetFinishFunc)(GObject *target, GAsyncResult *result,
                                                  GError **error);

/* settings: org.nostr.Groundhog. tor_available: G09's network modes exist
 * (GROUNDHOG_HAVE_TOR in the application). */
GhPreferencesDialog *gh_preferences_dialog_new(GSettings *settings, gboolean tor_available);

/* The active account; npub NULL when there is none. label is its display
 * name, or NULL to show a shortened npub. */
void gh_preferences_dialog_set_account(GhPreferencesDialog *self, const gchar *npub,
                                       const gchar *label);
/* Enables "Delete All Messages on This Device"; target is referenced. */
void gh_preferences_dialog_set_forget_func(GhPreferencesDialog *self,
                                           GhPreferencesForgetAsyncFunc forget_async,
                                           GhPreferencesForgetFinishFunc forget_finish,
                                           GObject *target);

/* The widget bound to a preference key: its row, or for discovery-relays and
 * blossom-servers the list of their entries. NULL for other keys. */
GtkWidget *gh_preferences_dialog_get_key_widget(GhPreferencesDialog *self, const gchar *key);

/* The URL rules of the list editors, with translated G_IO_ERROR_INVALID_ARGUMENT
 * messages. Each returns the URL to store: scheme and host lowercased, trailing
 * slashes dropped. allow_onion permits plain ws:// or http:// to a .onion host
 * (Tor mode). */
gchar *gh_preferences_normalize_relay_url(const gchar *url, gboolean allow_onion,
                                          GError **error);
gchar *gh_preferences_normalize_server_url(const gchar *url, gboolean allow_onion,
                                           GError **error);
/* host:port with a port in 1-65535, e.g. the default 127.0.0.1:9050. */
gboolean gh_preferences_validate_socks_address(const gchar *address, GError **error);

G_END_DECLS
#endif
