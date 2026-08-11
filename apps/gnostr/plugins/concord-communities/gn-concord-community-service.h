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

/* One live link from the creator's own Invite List (CORD-05 §4). The signing
 * secret never leaves the service: a caller gets what it takes to show, copy
 * and retire a link, never to mint a bundle at its coordinate. */
typedef struct {
  gchar *token;        /* hex; the link's merge key across the creator's devices */
  gchar *community_id;
  gchar *url;          /* the shareable link */
  gchar *label;        /* the link's human name, or NULL */
  gint64 created_at;   /* unix ms */
  gint64 expires_at;   /* unix ms; 0 when the link never expires */
} GnConcordInviteLink;

void gn_concord_invite_link_free(GnConcordInviteLink *link);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnConcordInviteLink, gn_concord_invite_link_free)

/* (element-type GnConcordInviteLink) (transfer full): this npub's live links
 * for one Community, newest first. Empty before the Invite List has been
 * read — an unread List is not an empty one. */
GPtrArray *gn_concord_community_service_get_invites(
    GnConcordCommunityService *self, const char *community_id);

/* Mints a shareable invite link (CORD-05 §2): a fresh link signer, the
 * token-encrypted kind-33301 bundle at its coordinate, the creator's Registry
 * edition naming that coordinate (§5), and the kind-13303 Invite List entry
 * that syncs the link to this npub's other devices (§4) — in that order, so a
 * Registry never names a coordinate with no bundle behind it.
 *
 * @label is the link's optional human name ("Reddit", "Conf 2026"), echoed by
 * an accepting joiner's Guestbook Join. @expires_at is unix ms, or 0 for a
 * link that never expires.
 *
 * Gated by CREATE_INVITE, and by holding the Community's control_root: the
 * Registry edition is a Control Plane write. Returns the URL. */
void gn_concord_community_service_create_invite_async(
    GnConcordCommunityService *self, const char *community_id,
    const char *label, gint64 expires_at, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gchar *gn_concord_community_service_create_invite_finish(
    GnConcordCommunityService *self, GAsyncResult *result, GError **error);

/* Retires a link (CORD-05 §2): its coordinate is re-posted as a vsk-9
 * revocation tombstone — exactly as durable as the bundle it replaces, unlike
 * a best-effort relay deletion — then dropped from the Registry and
 * tombstoned in the Invite List, where a tombstone always beats an entry so a
 * stale device can never resurrect it. */
void gn_concord_community_service_revoke_invite_async(
    GnConcordCommunityService *self, const char *community_id,
    const char *token_hex, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean gn_concord_community_service_revoke_invite_finish(
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

/* (element-type utf8) (transfer none): the aggregate active-set of live invite
 * links, folded from every creator's Registry (CORD-05 §5). Each entry is a
 * link signer pubkey — a coordinate, never a token — so a member can see that
 * links exist without being able to use one. */
GPtrArray *gn_concord_community_service_get_invite_links(
    GnConcordCommunityService *self, const char *community_id);
/* That set *is* the Public/Private source of truth: non-empty means a live
 * link exists and the Community is Public (CORD-05 §5). */
gboolean gn_concord_community_service_is_public(
    GnConcordCommunityService *self, const char *community_id);

/* Ingests one kind-1059 wrap from the Community's Guestbook Plane and folds
 * the Join, Leave or Kick inside. Public so the service tests can drive the
 * coalesce without a relay. */
gboolean gn_concord_community_service_ingest_guestbook_wrap(
    GnConcordCommunityService *self, const char *community_id,
    const char *wrap_json);

/* Names the npub whose Refounding minted this Community's current epoch — the
 * Rotator whose seal carried the base rekey (CORD-06 §1) — which is the only
 * npub whose kind-3312 Guestbook snapshots this client honors (CORD-02 §5).
 *
 * Until the Refounding path lands (nostrc-vhtt) nothing in the service can
 * learn a Rotator, so no snapshot is folded: a member entering a new epoch
 * and finding their own state absent republishes a Join, which makes an
 * unfolded snapshot a blip rather than a disappearance. */
void gn_concord_community_service_set_refounder(
    GnConcordCommunityService *self, const char *community_id,
    const char *pubkey_hex);

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
