#ifndef GN_CONCORD_COMMUNITY_SERVICE_H
#define GN_CONCORD_COMMUNITY_SERVICE_H

#include <gio/gio.h>
#include <glib-object.h>
#include <gnostr-plugin-api.h>

#include "model/gn-concord-community-item.h"
#include "model/gn-concord-message-item.h"

G_BEGIN_DECLS

typedef enum {
  GN_CONCORD_UPDATE_MEMBERSHIP = 1u << 0,
  GN_CONCORD_UPDATE_CHANNELS = 1u << 1,
  GN_CONCORD_UPDATE_MESSAGES = 1u << 2
} GnConcordUpdateFlags;

#define GN_TYPE_CONCORD_COMMUNITY_SERVICE (gn_concord_community_service_get_type())
G_DECLARE_FINAL_TYPE(GnConcordCommunityService, gn_concord_community_service,
                     GN, CONCORD_COMMUNITY_SERVICE, GObject)

GnConcordCommunityService *gn_concord_community_service_new(
    GnostrPluginContext *context);
/* Deterministic no-network constructor used by the plugin's service tests:
 * no host storage, no subscriptions, no signer. */
GnConcordCommunityService *gn_concord_community_service_new_offline(
    const char *user_pubkey);

GListModel *gn_concord_community_service_get_model(
    GnConcordCommunityService *self);
GnConcordCommunityItem *gn_concord_community_service_lookup_community(
    GnConcordCommunityService *self, const char *community_id);
/* The Chat Plane fold for one Channel, ordered oldest-first by
 * created_at * 1000 + ms (CORD-02 §4). */
GListModel *gn_concord_community_service_get_messages(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id);
const char *gn_concord_community_service_get_current_pubkey(
    GnConcordCommunityService *self);

/* Adopts a decrypted CORD-05 §1 CommunityInvite bundle as a membership.
 * Refuses a bundle whose owner proof fails to reproduce the community_id, one
 * past its `expires_at`, and one exceeding the §1 bounds. */
gboolean gn_concord_community_service_accept_bundle(
    GnConcordCommunityService *self, const char *bundle_json, GError **error);

/* Follows an invite URL: `$BASE/invite/<naddr>#<fragment>`. Only the naddr and
 * the fragment are protocol, so any base — or a bare `<naddr>#<fragment>` —
 * is accepted. Fetches the kind-33301 bundle at the naddr's coordinate,
 * decrypts it with the fragment's token, and adopts it. */
void gn_concord_community_service_accept_invite_async(
    GnConcordCommunityService *self, const char *invite_uri,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data);
gboolean gn_concord_community_service_accept_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error);

/* Ingests one kind-1059 stream wrap: opens the wrap under the Channel's
 * conversation key, verifies the seal, and folds the rumor. Returns TRUE when
 * a new message was added. Public so the service tests can drive the whole
 * decrypt path without a relay. */
gboolean gn_concord_community_service_ingest_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id, const char *wrap_json);

/* Ingests one kind-1059 wrap from the Community's Control Plane: opens it
 * under the community_root-derived read key, verifies the plaintext seal, and
 * folds the CORD-04 edition inside. Returns TRUE when a new edition was
 * accepted — acceptance is not authorization, which the fold judges by the
 * sealed actor's rank. Public so the service tests can drive the whole fold
 * without a relay. */
gboolean gn_concord_community_service_ingest_control_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *wrap_json);

/* A member's folded rank and permissions in one Community's owner-rooted
 * Roster. Position is lower-is-higher: the owner is 0 and a member the Roster
 * ranks nowhere is CONCORD_POSITION_LAST (CORD-04 §3). */
guint32 gn_concord_community_service_get_position(
    GnConcordCommunityService *self, const char *community_id,
    const char *pubkey_hex);
guint64 gn_concord_community_service_get_permissions(
    GnConcordCommunityService *self, const char *community_id,
    const char *pubkey_hex);

/* Ingests one kind-1059 wrap from the Community's Guestbook Plane and folds
 * the Join, Leave or Kick inside. Public so the service tests can drive the
 * coalesce without a relay. */
gboolean gn_concord_community_service_ingest_guestbook_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *wrap_json);

/* (element-type utf8) (transfer container): the Complete Memberlist — the
 * coalesced Guestbook, merged with observed authors, minus the Banlist
 * (CORD-02 §5). The strings belong to the service. */
GPtrArray *gn_concord_community_service_get_members(
    GnConcordCommunityService *self, const char *community_id);

/* Announces a Leave on the Guestbook, then drops the membership and
 * tombstones its Community List entry. The membership survives a Leave that
 * never reached a relay: this device would otherwise go quiet while every
 * other one still believes it is here. */
void gn_concord_community_service_leave_async(GnConcordCommunityService *self,
                                              const char *community_id,
                                              GCancellable *cancellable,
                                              GAsyncReadyCallback callback,
                                              gpointer user_data);
gboolean gn_concord_community_service_leave_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error);

/* Opens one standard NIP-59 giftwrap carrying a Direct Invite (CORD-05 §6)
 * and returns the CommunityInvite bundle inside, which
 * gn_concord_community_service_accept_bundle() then validates exactly as a
 * fetched one. Returns NULL for a giftwrap that is not an invite — an inbox
 * carries other people's traffic. */
void gn_concord_community_service_open_direct_invite_async(
    GnConcordCommunityService *self, const char *wrap_json,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data);
gchar *gn_concord_community_service_open_direct_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error);

/* Indexes this npub's Direct Invites and emits ::invite-offered for each one
 * that opens. Nothing joins, subscribes or announces presence on the strength
 * of an invite arriving — accepting is a separate, explicit act. */
void gn_concord_community_service_refresh_direct_invites(
    GnConcordCommunityService *self);

void gn_concord_community_service_refresh(GnConcordCommunityService *self);
void gn_concord_community_service_refresh_channel(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id);
void gn_concord_community_service_shutdown(GnConcordCommunityService *self);

void gn_concord_community_service_publish_message_async(
    GnConcordCommunityService *self, const char *community_id,
    const char *channel_id, const char *content, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean gn_concord_community_service_publish_message_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error);

G_END_DECLS
#endif
