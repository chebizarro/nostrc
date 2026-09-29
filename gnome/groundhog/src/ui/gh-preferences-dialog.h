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
 * No fake support (charter §7.1, §7.11; W13b review B2). A row whose feature
 * this build lacks (GhPreferencesFeatures, given at construction) is not
 * bound: it shows what the build does instead (off, or the first choice),
 * is insensitive and says "Not available in this version yet". The rows and
 * the features they need (the key_features table in gh-preferences-dialog.c):
 *
 *   NOTIFICATIONS     notifications-enabled; its content and sound rows and
 *                     the lock-screen note are hidden without it
 *   COMPOSER          enter-sends
 *   COMPOSER|EXPIRY   default-disappearing-seconds (its timer only applies
 *                     to messages the account sends)
 *   EXPIRY            retention-days
 *   REMOTE_IMAGES     load-remote-images
 *   PROFILE_PICTURES  load-profile-pictures
 *   LINK_PREVIEWS     link-previews
 *   REQUEST_FILTER    filter-unknown-senders; without it the row shows that
 *                     requests are always separated (on, insensitive)
 *   ATTACHMENTS       blossom-servers (the list is shown, not editable)
 *   TOR               tor-socks-address and the "tor" network mode: without
 *                     it the Tor choice is removed, the Tor address row stays
 *                     hidden and the Network page says that proxy settings
 *                     are not used yet (§4.2)
 *
 * The application passes gh_features_for_preferences() (src/app/gh-features.h),
 * the one list of what this build performs; tests/check_privacy.py requires a
 * consumer outside this dialog for every key whose features that list has.
 *
 * The Account page shows the account set with
 * gh_preferences_dialog_set_account(); with a forget function it offers
 * "Delete All Messages on This Device" (the prefs.delete-all action), which
 * asks for confirmation in an AdwAlertDialog and then runs the function for
 * that account: in the application, GhAccountStore's forget (charter §3.8,
 * ST-8), which also signs the account out on this device. The dialog cannot
 * be closed while that runs; the result is shown as a toast, which says so
 * when the messages were deleted but the storage key stayed in the keyring.
 */
#define GH_TYPE_PREFERENCES_DIALOG (gh_preferences_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhPreferencesDialog, gh_preferences_dialog, GH, PREFERENCES_DIALOG,
                     AdwPreferencesDialog)

/* What this build performs among the features some preferences control. */
typedef enum {
  GH_PREFERENCES_FEATURE_TOR              = 1 << 0, /* G09: network modes, Tor transport */
  GH_PREFERENCES_FEATURE_NOTIFICATIONS    = 1 << 1, /* G16: message notifications */
  GH_PREFERENCES_FEATURE_COMPOSER         = 1 << 2, /* G13: writing and sending messages */
  GH_PREFERENCES_FEATURE_REMOTE_IMAGES    = 1 << 3, /* a remote image loader */
  GH_PREFERENCES_FEATURE_PROFILE_PICTURES = 1 << 4, /* a profile picture fetcher */
  GH_PREFERENCES_FEATURE_LINK_PREVIEWS    = 1 << 5, /* a link preview fetcher */
  GH_PREFERENCES_FEATURE_REQUEST_FILTER   = 1 << 6, /* letting unknown senders through */
  GH_PREFERENCES_FEATURE_ATTACHMENTS      = 1 << 7, /* G21/G22: Blossom attachments */
  GH_PREFERENCES_FEATURE_EXPIRY           = 1 << 8, /* G07: the expiry and retention purge */
} GhPreferencesFeatures;
#define GH_PREFERENCES_FEATURES_ALL ((GhPreferencesFeatures)((1 << 9) - 1))

/* What a forget did. */
typedef enum {
  GH_PREFERENCES_FORGET_FAILED,   /* not everything was deleted; error says what */
  GH_PREFERENCES_FORGET_DELETED,  /* the messages and their storage key are gone */
  GH_PREFERENCES_FORGET_KEY_KEPT, /* the messages are gone, but the storage key item
                                   * could not be removed from the keyring (error
                                   * says why); it no longer opens anything */
} GhPreferencesForgetResult;

/* Deletes everything of the account npub stores on this device and signs it
 * out there; the result is read with the matching finish function. target is
 * the object given to gh_preferences_dialog_set_forget_func(). */
typedef void (*GhPreferencesForgetAsyncFunc)(GObject *target, const gchar *npub,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback, gpointer user_data);
typedef GhPreferencesForgetResult (*GhPreferencesForgetFinishFunc)(GObject *target,
                                                                   GAsyncResult *result,
                                                                   GError **error);

/* settings: org.nostr.Groundhog. features: what this build performs
 * (gh_features_for_preferences() in the application). */
GhPreferencesDialog *gh_preferences_dialog_new(GSettings *settings,
                                               GhPreferencesFeatures features);

/* Whether key's row is live in this build: TRUE when it has every feature it
 * needs (a key no feature governs always has); FALSE for a key without a
 * row. */
gboolean gh_preferences_dialog_get_key_available(GhPreferencesDialog *self, const gchar *key);

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

/* The text of the last deletion result toast (NULL before one), for tests. */
const gchar *gh_preferences_dialog_get_last_toast(GhPreferencesDialog *self);

/* The one reason every gated row gives (translated). */
const gchar *gh_preferences_dialog_not_available_text(void);

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
