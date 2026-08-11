#ifndef GN_CONCORD_CONTROL_PLANE_H
#define GN_CONCORD_CONTROL_PLANE_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* The Control Plane fold (CORD-04).
 *
 * A Community's authoritative state — its metadata, its Channel definitions,
 * its Roster and its Banlist — lives as versioned editions on one plane, and
 * the fold is what turns a bag of editions into current state. The invite
 * bundle a member joined with is a *join-time snapshot* (CORD-02 §6); this is
 * the authority that supersedes it.
 *
 * Deliberately free of any host, relay or GObject dependency: a fold is pure
 * derivation over bytes, so it is exercisable with no network and no signer.
 */

typedef struct _GnConcordControlPlane GnConcordControlPlane;

/* One folded ChannelMetadata entity (CORD-03 §2). The `key` is never here —
 * keys travel in invites and rekeys, never on the Control Plane — so a
 * private Channel a member holds no key for is still *listed*, just not
 * readable. */
typedef struct {
  gchar *channel_id;
  gchar *name;
  gboolean is_private;
  gboolean deleted;
} GnConcordControlChannel;

/* @owner_pubkey_hex is proven by the community_id itself (CORD-02 §1), which
 * is what makes the Roster fold non-circular: rank resolution starts at an
 * actor whose authority comes from outside the fold. */
GnConcordControlPlane *gn_concord_control_plane_new(
    const char *community_id_hex, const char *owner_pubkey_hex);
void gn_concord_control_plane_free(GnConcordControlPlane *self);

/* Ingests one kind-1059 wrap from the plane's address. @address_hex is the
 * control_pk a member holds from their invite, or — on a legacy, pre-split
 * Community — the `concord/control` derivation's own pk, which was the
 * address and the signer both (CORD-02 §5). @conv_key is always the
 * community_root-derived read key's conversation key.
 *
 * Returns TRUE when a new edition was accepted into the plane. Acceptance is
 * not authorization: an edition is judged by its sealed actor's rank at fold
 * time, never at ingest, because the Grant that authorizes it may arrive
 * later (CORD-04 §5). */
gboolean gn_concord_control_plane_ingest_wrap(GnConcordControlPlane *self,
                                              const uint8_t conv_key[32],
                                              const char *address_hex,
                                              const char *wrap_json);

/* Folded state. Each getter folds on demand and caches until the next
 * accepted edition. */
const char *gn_concord_control_plane_get_name(GnConcordControlPlane *self);
const char *gn_concord_control_plane_get_description(
    GnConcordControlPlane *self);
/* NULL-terminated; NULL when no metadata edition has been folded. */
const char *const *gn_concord_control_plane_get_relays(
    GnConcordControlPlane *self, guint *n_relays);
/* (element-type GnConcordControlChannel) (transfer none): every Channel the
 * plane defines, deleted ones included — a caller decides what to display. */
GPtrArray *gn_concord_control_plane_get_channels(GnConcordControlPlane *self);

/* CORD-04 §4: every honest client drops *every* event from a banned npub —
 * message, reaction, edit, or authority action — so a banned member vanishes
 * entirely. */
gboolean gn_concord_control_plane_is_banned(GnConcordControlPlane *self,
                                            const char *pubkey_hex);

/* The folded Roster, exposed for the UI and for tests. Position is
 * lower-is-higher: the owner is 0 and a roleless member is
 * CONCORD_POSITION_LAST. */
guint64 gn_concord_control_plane_get_permissions(GnConcordControlPlane *self,
                                                 const char *pubkey_hex);
guint32 gn_concord_control_plane_get_position(GnConcordControlPlane *self,
                                              const char *pubkey_hex);

/* Editions held, and editions parked awaiting the Grant they cite (CORD-04
 * §5 block-until-synced). Diagnostics, and the assertion surface tests need. */
guint gn_concord_control_plane_count_editions(GnConcordControlPlane *self);
guint gn_concord_control_plane_count_parked(GnConcordControlPlane *self);

G_END_DECLS
#endif
