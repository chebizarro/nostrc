#ifndef GH_PUBLIC_NOTE_POST_H
#define GH_PUBLIC_NOTE_POST_H

#include <gio/gio.h>
#include <nostr-gtk-1.0/gn-nostr-reference.h>
#include "gh-store-public-notes.h"

G_BEGIN_DECLS

typedef struct {
  gchar *unsigned_json; /* exact bytes shown at confirmation and sent to signer */
  gchar *account;
  GStrv write_relays;    /* NIP-65 write destinations frozen at review */
} GhPublicPostSnapshot;

GhPublicPostSnapshot *gh_public_post_snapshot_new(const GhPublicNote *original,
    const GnNostrReference *reference, gboolean quote, const gchar *comment,
    const gchar *account_pubkey, gint64 created_at,
    const gchar *const *write_relays, GError **error);
/* Refuse a signer result whose canonical NIP-01 event differs from the reviewed bytes. */
gboolean gh_public_post_snapshot_matches_signed(const GhPublicPostSnapshot *snapshot,
    const gchar *signed_json, GError **error);
void gh_public_post_snapshot_free(GhPublicPostSnapshot *snapshot);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhPublicPostSnapshot, gh_public_post_snapshot_free)

G_END_DECLS
#endif
