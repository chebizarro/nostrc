#ifndef GN_CONCORD_GUESTBOOK_H
#define GN_CONCORD_GUESTBOOK_H

#include <glib.h>
#include <stdint.h>

G_BEGIN_DECLS

/* The Guestbook Plane fold (CORD-02 §5).
 *
 * One plane per Community, keyed by the community_root and necessarily
 * member-*writable*, unlike Control, because Joins and Leaves are each
 * member's own word. It carries membership motion and nothing else: never
 * messages, never authority.
 *
 * It is deliberately *off-consensus* — nothing in Control or Chat depends on
 * it — so it loads last and can lag without harm.
 *
 * Like the Control Plane fold, this is pure derivation over bytes: no host, no
 * relay, no GObject.
 */

typedef struct _GnConcordGuestbook GnConcordGuestbook;

typedef enum {
  GN_CONCORD_MEMBER_ABSENT = 0,
  GN_CONCORD_MEMBER_PRESENT,
  GN_CONCORD_MEMBER_DEPARTED
} GnConcordMemberState;

GnConcordGuestbook *gn_concord_guestbook_new(void);
void gn_concord_guestbook_free(GnConcordGuestbook *self);

/* Ingests one kind-1059 wrap from the plane. @conv_key and @address_hex both
 * derive from the community_root, so any member can publish here — which is
 * the point, and why the fold trusts the *seal's* npub for authorship and
 * nothing else. Returns TRUE when the fold changed. */
gboolean gn_concord_guestbook_ingest_wrap(GnConcordGuestbook *self,
                                          const uint8_t conv_key[32],
                                          const char *address_hex,
                                          const char *wrap_json);

/* CORD-02 §5: the npub whose Refounding minted this epoch — the Rotator whose
 * seal carried the base rekey (CORD-06 §1). A snapshot (kind 3312) is that
 * npub's *secondhand* attestation of who was present, so it is honored from
 * them and from nobody else; with no refounder set, the fold honors none,
 * which is the correct reading of an epoch nobody has proven they minted.
 *
 * The fold buffers nothing it refused, so a snapshot dropped for want of a
 * Rotator is folded only if the same bytes are ingested again: a client
 * learns the Rotator from the rekey blob before it fetches the new epoch's
 * Guestbook. */
void gn_concord_guestbook_set_refounder(GnConcordGuestbook *self,
                                        const char *pubkey_hex);

/* Records that @pubkey_hex was seen authoring at @order_key (CORD-02 §5's
 * ms basis). An author seen publishing is *observably present*, auto-included
 * even if their Join never arrived — but observation only counts forward, so
 * a departed member's old history can never resurrect them. */
void gn_concord_guestbook_observe(GnConcordGuestbook *self,
                                  const char *pubkey_hex, gint64 order_key);

GnConcordMemberState gn_concord_guestbook_get_state(GnConcordGuestbook *self,
                                                    const char *pubkey_hex);

/* (element-type utf8) (transfer container): every present member, sorted, so
 * two clients render one order. The caller subtracts the Banlist to reach the
 * Complete Memberlist. */
GPtrArray *gn_concord_guestbook_get_members(GnConcordGuestbook *self);

/* An authorized Kick marks its target departed. The caller resolves authority
 * against the Control Plane Roster, since the Guestbook holds none itself: a
 * Kick is honored only if its signer holds KICK and outranks the target. */
typedef gboolean (*GnConcordKickAuthorityFunc)(const char *actor,
                                               const char *target,
                                               gpointer user_data);
void gn_concord_guestbook_set_kick_authority(
    GnConcordGuestbook *self, GnConcordKickAuthorityFunc authority,
    gpointer user_data);

/* The receiver's clock, in the ms basis, for the future-drop rule. Tests set
 * it; production leaves it at the real clock. */
void gn_concord_guestbook_set_clock(GnConcordGuestbook *self, gint64 now_ms);

G_END_DECLS
#endif
