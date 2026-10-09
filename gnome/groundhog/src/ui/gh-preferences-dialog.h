#ifndef GH_PREFERENCES_DIALOG_H
#define GH_PREFERENCES_DIALOG_H

#include <adwaita.h>
#include "gh-agents-page.h"

G_BEGIN_DECLS

/*
 * GhPreferencesDialog: Groundhog's preferences (privacy charter §7.11, G17),
 * a composite AdwPreferencesDialog (data/ui/gh-preferences-dialog.blp) with
 * four pages, "privacy", "messages", "network" and "account". Each row is
 * bound both ways to one key of the org.nostr.Groundhog settings it is given:
 *
 *   Privacy   notifications-enabled, notification-privacy, sound-enabled,
 *             load-remote-images, link-previews, load-profile-pictures,
 *             filter-unknown-senders, only-join-verified-mls-groups,
 *             show-message-previews
 *   Messages  enter-sends, default-disappearing-seconds, retention-days,
 *             blossom-servers (the Attachments group, G22)
 *   Network   network-mode, tor-socks-address, discovery-relays
 *   Account   run-in-background
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
 *                     are not used yet (§4.2). With it the page says, for the
 *                     chosen mode, who can see the IP address: in Tor mode
 *                     what Tor hides and what it doesn't (§2.2 surface 5),
 *                     with the Tor address and the Tor status
 *                     (gh_preferences_dialog_set_tor_status())
 *
 * The application passes gh_features_for_preferences() (src/app/gh-features.h),
 * the one list of what this build performs; tests/check_privacy.py requires a
 * consumer outside this dialog for every key whose features that list has.
 *
 * Messages › Attachments (charter §6, §7.11, G22): the attachment servers in
 * the order they are tried, each with Move Up, Move Down and Remove; with
 * gh_preferences_dialog_set_attachments() also the largest file, the size of
 * the decrypted copies on this device with "Clear…" (after a confirmation),
 * and, on a server the account consented to upload to as itself (nostrc-dnsc),
 * a note and "Revoke". The group's description says what a server learns,
 * with or without the Tor clause as the build has Tor.
 *
 * Privacy › Blocked Conversations (hidden until gh_blocked_page_attach(),
 * gh-blocked-page.h, gives it the account store's blocks) lists blocked
 * conversations with Unblock.
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
  GH_PREFERENCES_FEATURE_ENCRYPTED_GROUPS = 1 << 9, /* encrypted groups (nostrc-6ukh) */
  /* The adopted-profile KeyPackage producer (nostrc-lf62): only then is the
   * older format a choice, so without it that switch isn't shown at all. */
  GH_PREFERENCES_FEATURE_ADOPTED_KEY_PACKAGES = 1 << 10,
} GhPreferencesFeatures;
#define GH_PREFERENCES_FEATURES_ALL ((GhPreferencesFeatures)((1 << 11) - 1))

/* Whether Tor answers at tor-socks-address (the app's network session). */
typedef enum {
  GH_PREFERENCES_TOR_STATUS_UNKNOWN,     /* not known: no status row */
  GH_PREFERENCES_TOR_STATUS_CHECKING,
  GH_PREFERENCES_TOR_STATUS_REACHABLE,
  GH_PREFERENCES_TOR_STATUS_UNREACHABLE, /* Groundhog connects nowhere */
} GhPreferencesTorStatus;

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
void gh_preferences_dialog_set_agent_start_func(GhPreferencesDialog *self,
                                                GhAgentsStartFunc start, gpointer user_data);
void gh_preferences_dialog_set_account(GhPreferencesDialog *self, const gchar *npub,
                                       const gchar *label);
void gh_preferences_dialog_set_account_picture(GhPreferencesDialog *self,
                                             GdkPaintable *picture);
/* The Tor status row, shown in Tor mode (with the TOR feature) unless the
 * status is UNKNOWN. */
void gh_preferences_dialog_set_tor_status(GhPreferencesDialog *self,
                                          GhPreferencesTorStatus status);
/* Whether people can invite the account to encrypted groups (its
 * GhMlsService KeyPackage state; nostrc-f8a5, nostrc-0bdg). */
typedef enum {
  GH_PREFERENCES_KEY_PACKAGE_UNKNOWN,   /* not known (no running service): no row */
  GH_PREFERENCES_KEY_PACKAGE_NO_RELAYS, /* no relay list to be found through: [Set Up] */
  GH_PREFERENCES_KEY_PACKAGE_PUBLISHING,
  GH_PREFERENCES_KEY_PACKAGE_PUBLISHED,
  GH_PREFERENCES_KEY_PACKAGE_FAILED,
  /* The account's relay list exists but names no relay it publishes to
   * (nostrc-0bdg re-review R3): [Add Relays], which the relay step offers
   * to add to that list. */
  GH_PREFERENCES_KEY_PACKAGE_NO_WRITE_RELAYS,
  /* Published; its due replacement waits for pending invitations (A5). */
  GH_PREFERENCES_KEY_PACKAGE_HELD,
  /* The account-proof signer request is pending (nostrc-q74l). */
  GH_PREFERENCES_KEY_PACKAGE_IDENTITY_WAITING,
  /* The user declined the account-proof request (nostrc-q74l). */
  GH_PREFERENCES_KEY_PACKAGE_IDENTITY_DECLINED,
  /* The signer failed or returned something else for the account proof
   * (e.g. the daemon's 300 s TTL expired; nostrc-q74l). */
  GH_PREFERENCES_KEY_PACKAGE_IDENTITY_FAILED,
} GhPreferencesKeyPackage;

/* Network › Encrypted Groups, shown with the ENCRYPTED_GROUPS feature unless
 * the state is UNKNOWN. NO_RELAYS and NO_WRITE_RELAYS say, honestly, that
 * nobody can invite the account and why, with [Set Up] / [Add Relays]: it
 * closes the dialog and activates win.setup-inbox
 * (GH_STATUS_ACTION_SETUP_INBOX), the onboarding relay step, which offers a
 * new kind-10002 relay list, or adding the chosen relays to the existing one
 * as write relays (so it never leads back here unchanged). */
void gh_preferences_dialog_set_key_package_state(GhPreferencesDialog *self,
                                                 GhPreferencesKeyPackage state);
GhPreferencesKeyPackage gh_preferences_dialog_get_key_package_state(GhPreferencesDialog *self);

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

/* The attachment server list's rules (the Attachments group and the first
 * use's server choice, G22), GTK-free. add: url normalized
 * (gh_preferences_normalize_server_url()) and appended, unless it is
 * invalid (G_IO_ERROR_INVALID_ARGUMENT), already listed in any spelling
 * (G_IO_ERROR_EXISTS) or the list holds 16 (G_IO_ERROR_NO_SPACE); errors
 * are translated. remove: every entry equal to url dropped. move: the entry
 * at index swapped with its neighbour delta places away (a copy, unchanged
 * when either is out of range). Each returns a new list. */
GStrv gh_preferences_server_list_add(const gchar *const *servers, const gchar *url,
                                     gboolean allow_onion, GError **error);
GStrv gh_preferences_server_list_remove(const gchar *const *servers, const gchar *url);
GStrv gh_preferences_server_list_move(const gchar *const *servers, guint index, gint delta);

/* The account's attachments, for the Attachments group (G22). */
typedef struct {
  guint64 max_file_size; /* the largest file that can be sent (0: not shown) */
  /* The bytes of decrypted files kept on this device; FALSE (with error)
   * while no account's storage is open. */
  gboolean (*get_cache_size)(gpointer data, gint64 *out_bytes, GError **error);
  /* Deletes them. */
  gboolean (*clear_cache)(gpointer data, GError **error);
  /* Whether the account consented to upload to server as itself, and taking
   * that back (both nullable). */
  gboolean (*get_consent)(gpointer data, const gchar *server);
  gboolean (*revoke_consent)(gpointer data, const gchar *server, GError **error);
} GhPreferencesAttachments;

/* attachments (copied; NULL removes them) with data, freed with destroy
 * (nullable) when replaced or the dialog goes. */
void gh_preferences_dialog_set_attachments(GhPreferencesDialog *self,
                                           const GhPreferencesAttachments *attachments,
                                           gpointer data, GDestroyNotify destroy);
/* Reads the cache size and the consents again (e.g. another account's
 * storage opened). */
void gh_preferences_dialog_refresh_attachments(GhPreferencesDialog *self);

/* Network › Where People Reach You (nostrc-mi1z): the account's published
 * relay lists read from GhAccountRelays. inbox_relays and write_relays are
 * NULL-terminated URL arrays (both nullable; NULL hides the section). The
 * section shows once either is non-NULL. "Change Relays…" emits the
 * "change-relays" signal so the application can open the onboarding relay
 * step. */
void gh_preferences_dialog_set_published_relays(GhPreferencesDialog *self,
                                                const gchar *const *inbox_relays,
                                                const gchar *const *write_relays);

/* Inline relay editing (nostrc-mi1z phase 2): "add-relay" and
 * "remove-relay" carry the kind (10050 or 10002) and the relay URL. The
 * application handles the check-sign-publish flow and refreshes the
 * section from GhAccountRelays when it settles. */

G_END_DECLS
#endif
