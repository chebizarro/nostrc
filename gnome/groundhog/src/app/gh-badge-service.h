#ifndef GH_BADGE_SERVICE_H
#define GH_BADGE_SERVICE_H
#include "gh-social-lists.h"
#include "gh-badge-types.h"
#include "gh-net-http.h"
#include <nostr/nip58/nip58.h>
G_BEGIN_DECLS
#define GH_TYPE_BADGE_SERVICE (gh_badge_service_get_type())
G_DECLARE_FINAL_TYPE(GhBadgeService, gh_badge_service, GH, BADGE_SERVICE, GObject)
/*
 * NIP-58 badges for one account (plan §3.12, nostrc-9lwl7.13). Main context
 * only; "changed" () follows every lookup, selection or binding change.
 *
 * Validation chain, in order, before anything is shown: the profile list's
 * signature and author; ordered consecutive a/e pairs (unmatched entries
 * ignored); the award fetched by exact id and signature-checked; the award
 * author equal to the issuer of the definition address; the displayed
 * account among the award's p tags; the award's a equal to the pair's; the
 * definition fetched by exact author/kind/d and chosen by NIP-01 ordering;
 * nostr_nip58_validate_relationship() over all three.
 *
 * Lookups are explicit (never from a Message Request, never on the send
 * path), on the configured discovery relays under CONTACT_DIRECTORY
 * (ephemeral AUTH, PD-12). A list's relay hints are reported, not
 * contacted, until the caller passes them as approved_relays. Verified
 * events are kept in social_event_cache so cached badges show offline.
 *
 * The own kind-10008 document lives in GhSocialLists (EXPLICIT_ONLY,
 * public-post). Selection edits are local; publish_async is the one
 * network write and the UI confirms it first. A canonical 10008 wins over a
 * legacy 30008 d=profile_badges even when empty; a legacy list only seeds
 * the selection until the next explicit publication, which publishes
 * canonical 10008 and never a deletion of the legacy event.
 */
/* Pure: every signature and relationship of one chain, or NULL. */
GhBadge *gh_badge_verify(const NostrNip58ProfileBadge *pair, const gchar *award_json,
                         const gchar *definition_json, const gchar *recipient);
GhBadgeService *gh_badge_service_new(GhSocialLists *lists, GhAccountStore *account_store,
                                     const GhListAdapterNetwork *network, GSettings *settings);
/* Explicit seam for a session store; close before releasing the borrowed store. */
void gh_badge_service_bind(GhBadgeService *self, GhStore *store, guint64 generation);
void gh_badge_service_close(GhBadgeService *self);
/* Tests only: artwork over a fake HTTP transport. */
void gh_badge_service_set_http_transport(GhBadgeService *self, const GhHttpTransport *transport,
                                         gpointer data);
GPtrArray *gh_badge_service_get_badges(GhBadgeService *self, const gchar *pubkey);
GhBadgeListKind gh_badge_service_get_list_kind(GhBadgeService *self, const gchar *pubkey);
GStrv gh_badge_service_dup_hints(GhBadgeService *self, const gchar *pubkey);
GStrv gh_badge_service_dup_sources(GhBadgeService *self);
void gh_badge_service_lookup_async(GhBadgeService *self, const gchar *pubkey,
                                   const gchar *const *approved_relays, GCancellable *cancellable,
                                   GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_badge_service_lookup_finish(GhBadgeService *self, GAsyncResult *result, GError **error);
GPtrArray *gh_badge_service_get_awards(GhBadgeService *self);
GPtrArray *gh_badge_service_get_selection(GhBadgeService *self, GhBadgeSelectionSource *source,
                                          GError **error);
gboolean gh_badge_service_set_selection(GhBadgeService *self, GPtrArray *badges, GError **error);
GStrv gh_badge_service_dup_targets(GhBadgeService *self);
void gh_badge_service_publish_async(GhBadgeService *self, GCancellable *cancellable,
                                    GAsyncReadyCallback callback, gpointer user_data);
gboolean gh_badge_service_publish_finish(GhBadgeService *self, GAsyncResult *result, GError **error);
gboolean gh_badge_service_get_artwork_enabled(GhBadgeService *self);
void gh_badge_service_load_artwork_async(GhBadgeService *self, const gchar *url,
                                         GCancellable *cancellable, GAsyncReadyCallback callback,
                                         gpointer user_data);
GdkTexture *gh_badge_service_load_artwork_finish(GhBadgeService *self, GAsyncResult *result,
                                                 GError **error);
GhBadgesBackend gh_badge_service_backend(GhBadgeService *self);
G_END_DECLS
#endif
