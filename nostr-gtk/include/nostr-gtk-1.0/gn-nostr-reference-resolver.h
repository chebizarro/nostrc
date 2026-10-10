/* GnNostrReferenceResolver: how a NIP-18/NIP-21 reference card finds the
 * note it points at (nostrc-8xfib.6). Ported from gnostr's timeline-factory
 * NDB lookups so the card itself owns no storage and no network: gnostr
 * implements it over nostrdb, Groundhog over its verified public-note cache
 * and consented relay search.
 *
 * Contract:
 *  - lookup_local() and display_name() are synchronous, local-only and must
 *    never touch the network. They run on the row-bind path, so keep them
 *    cheap.
 *  - fetch_async() is the only way to reach the network. Widgets call it only
 *    from an explicit user action (or the application calls it under its own
 *    policy); never from bind or map.
 *  - can_fetch() FALSE (locked store, offline, no consent) hides fetching.
 *  - Results are signed event JSON; the card verifies them and checks they
 *    are what the reference points at before showing anything. */
#ifndef GN_NOSTR_REFERENCE_RESOLVER_H
#define GN_NOSTR_REFERENCE_RESOLVER_H

#include <gio/gio.h>
#include <nostr-gtk-1.0/gn-nostr-reference.h>

G_BEGIN_DECLS

#define GN_TYPE_NOSTR_REFERENCE_RESOLVER (gn_nostr_reference_resolver_get_type())
G_DECLARE_INTERFACE(GnNostrReferenceResolver, gn_nostr_reference_resolver,
                    GN, NOSTR_REFERENCE_RESOLVER, GObject)

struct _GnNostrReferenceResolverInterface {
  GTypeInterface parent_iface;

  gchar *(*lookup_local)(GnNostrReferenceResolver *self, const GnNostrReference *reference);
  gchar *(*display_name)(GnNostrReferenceResolver *self, const gchar *pubkey_hex);
  gboolean (*can_fetch)(GnNostrReferenceResolver *self);
  void (*fetch_async)(GnNostrReferenceResolver *self, const GnNostrReference *reference,
                      GCancellable *cancellable, GAsyncReadyCallback callback,
                      gpointer user_data);
  gchar *(*fetch_finish)(GnNostrReferenceResolver *self, GAsyncResult *result,
                         GError **error);

  gpointer padding[8];
};

/* Signed event JSON from local storage, or NULL. */
gchar *gn_nostr_reference_resolver_lookup_local(GnNostrReferenceResolver *self,
                                                 const GnNostrReference *reference);
/* Local profile name for @pubkey_hex, or NULL. */
gchar *gn_nostr_reference_resolver_display_name(GnNostrReferenceResolver *self,
                                                 const gchar *pubkey_hex);
/* FALSE when unimplemented. */
gboolean gn_nostr_reference_resolver_can_fetch(GnNostrReferenceResolver *self);
void gn_nostr_reference_resolver_fetch_async(GnNostrReferenceResolver *self,
                                             const GnNostrReference *reference,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data);
gchar *gn_nostr_reference_resolver_fetch_finish(GnNostrReferenceResolver *self,
                                                GAsyncResult *result, GError **error);

/* A note the reference resolved to: verified, and matching the reference. */
typedef struct {
  GnNostrEventInfo *event;
  gchar *event_json;
  gchar *author_name;      /* resolver display name, or NULL */
} GnNostrResolvedNote;

void gn_nostr_resolved_note_free(GnNostrResolvedNote *note);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GnNostrResolvedNote, gn_nostr_resolved_note_free)

/* Verifies @event_json (cached, see gn_nostr_event_verify_cache_clear()) and
 * checks it is what @reference points at; @resolver (optional) names the
 * author. NULL when it does not verify or match. */
GnNostrResolvedNote *gn_nostr_resolved_note_new(const GnNostrReference *reference,
                                                const gchar *event_json,
                                                GnNostrReferenceResolver *resolver);
/* lookup_local() followed by gn_nostr_resolved_note_new(). */
GnNostrResolvedNote *gn_nostr_reference_resolver_resolve_local(GnNostrReferenceResolver *self,
                                                               const GnNostrReference *reference);

G_END_DECLS
#endif
