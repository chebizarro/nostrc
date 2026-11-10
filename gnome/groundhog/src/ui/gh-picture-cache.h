#pragma once
#include <adwaita.h>
#include "gh-web-content.h"

G_BEGIN_DECLS

/* Profile pictures across the UI (owner decisions W32 and D2, charter
 * PD-2 as amended): one cache per window decides, for every surface
 * (message rows, the sidebar, Conversation Info, Requests, New Message, the
 * account switcher, Preferences, member rows and the invitee picker), whether a contact's picture may be shown,
 * and serves one download per URL to all of them. A pubkey is eligible when
 *
 *   master_enabled AND NOT hidden AND (own OR followed OR explicit_consent)
 *
 * where master_enabled is the load-profile-pictures preference
 * (gh_picture_cache_bind_settings; nothing is ever fetched while it is off),
 * own identities come from the account switcher (gh_picture_cache_set_own),
 * followed and hidden from the injected policy (gh_picture_cache_set_policy;
 * the follow list is read only while the preference is on, and hidden
 * accounts are ineligible regardless), and explicit consent is given once
 * per contact and kept in the account's encrypted store
 * (contacts.picture_allowed_at, schema v9; P8: local only). Account-derived
 * eligibility never writes consent. Turning the preference off cancels
 * every load, drops every texture and clears every consent
 * (gh_picture_cache_revoke_all); turning it on again makes own and followed
 * pictures eligible, while everyone else needs fresh consent. */
#define GH_TYPE_PICTURE_CACHE (gh_picture_cache_get_type())
G_DECLARE_FINAL_TYPE(GhPictureCache, gh_picture_cache, GH, PICTURE_CACHE, GObject)

/* web may be NULL (no fetching: tests, no network). The master switch starts
 * on until settings are bound (tests drive it with gh_picture_cache_set_master). */
GhPictureCache *gh_picture_cache_new(GhWebContent *web);
/* End the owning window's binding without changing stored consent. */
void gh_picture_cache_close(GhPictureCache *self);
/* Follows the load-profile-pictures key of settings (the master switch):
 * off right away when it is off, revoked when it turns off. Borrowed with a
 * reference. */
void gh_picture_cache_bind_settings(GhPictureCache *self, GSettings *settings);
void gh_picture_cache_set_master(GhPictureCache *self, gboolean enabled);
gboolean gh_picture_cache_get_master(GhPictureCache *self);
/* Where consent is kept (the account's encrypted store, through the app):
 * list() returns every allowed pubkey, set() records or clears (0) one,
 * clear() revokes all. The backend is copied. NULL backend between
 * accounts: no consent, and the cache is emptied. */
typedef struct {
  GStrv (*list)(gpointer data, GError **error);
  gboolean (*set)(gpointer data, const gchar *pubkey, gint64 allowed_at, GError **error);
  gboolean (*clear)(gpointer data, GError **error);
} GhPictureConsentBackend;
void gh_picture_cache_set_consent(GhPictureCache *self, const GhPictureConsentBackend *backend,
                                  gpointer data);
/* Account policy, copied; either callback may be NULL (never followed, never
 * hidden). is_hidden is the account hide decision: hidden accounts show no picture. */
typedef struct {
  gboolean (*is_followed)(gpointer data, const gchar *pubkey);
  gboolean (*is_hidden)(gpointer data, const gchar *pubkey);
} GhPicturePolicy;
void gh_picture_cache_set_policy(GhPictureCache *self, const GhPicturePolicy *policy,
                                 gpointer data);
/* The account's own identities (hex), eligible without consent; replaces
 * the previous set. */
void gh_picture_cache_set_own(GhPictureCache *self, const gchar *const *pubkeys);
/* The policy's answers changed for pubkey (NULL: for anyone, e.g. the follow
 * list): pictures of pubkeys no longer eligible are dropped and their loads
 * cancelled; "picture-changed" follows for each, then "policy-changed". */
void gh_picture_cache_policy_changed(GhPictureCache *self, const gchar *pubkey);

/* Eligible now (the formula above). */
gboolean gh_picture_cache_is_allowed(GhPictureCache *self, const gchar *pubkey);
/* Explicit per-contact consent only, regardless of the master switch. */
gboolean gh_picture_cache_has_consent(GhPictureCache *self, const gchar *pubkey);
/* Whether the policy follows pubkey, under the master switch (the Requests
 * page says "You follow this account" only then). */
gboolean gh_picture_cache_is_followed(GhPictureCache *self, const gchar *pubkey);
/* Records explicit consent for pubkey (the user chose Allow). */
void gh_picture_cache_allow(GhPictureCache *self, const gchar *pubkey);
void gh_picture_cache_revoke_all(GhPictureCache *self);
/* The picture at uri for pubkey: the texture when loaded, else NULL. When
 * eligible with a uri, a fetch starts (once per URL; a failed URL is not
 * retried until the next gh_picture_cache_allow for any contact) and
 * "picture-changed" (pubkey) follows. A completion applies only to the
 * exact entry that started it (same cache epoch and entry), so a load that
 * was revoked or rebound never resurfaces. Borrowed. */
GdkTexture *gh_picture_cache_get(GhPictureCache *self, const gchar *pubkey, const gchar *uri);
/* Shared cached-identity source for surfaces without a message-row URI.
 * The source is referenced; only eligible pubkeys are looked up. */
typedef gchar *(*GhPictureUriFunc)(const gchar *pubkey, gpointer source);
void gh_picture_cache_set_uri_source(GhPictureCache *self, GhPictureUriFunc func, GObject *source);
GdkTexture *gh_picture_cache_get_for(GhPictureCache *self, const gchar *pubkey);
/* Keeps a contact avatar in step with the same per-window eligibility and URI
 * source, including revocation while its dialog stays open. */
void gh_picture_cache_bind_avatar(GhPictureCache *self, AdwAvatar *avatar, const gchar *pubkey);

/* The state of uri: TRUE while a fetch is under way. */
gboolean gh_picture_cache_is_loading(GhPictureCache *self, const gchar *uri);
gboolean gh_picture_cache_has_failed(GhPictureCache *self, const gchar *uri);

/* Signals: "picture-changed" (gchar *pubkey): that pubkey's picture arrived,
 * failed, or may no longer (or may now) be shown; "policy-changed" (): the
 * master switch, the own set or the policy changed, so every surface should
 * ask again for whom it shows. */

G_END_DECLS
