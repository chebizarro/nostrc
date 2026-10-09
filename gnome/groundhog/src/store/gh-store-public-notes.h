#ifndef GH_STORE_PUBLIC_NOTES_H
#define GH_STORE_PUBLIC_NOTES_H

#include "gh-store.h"

G_BEGIN_DECLS

/* A bounded, signed public event admitted after an explicit reference lookup.
 * No relay hint is a source of trust or an implicit network destination. */
typedef struct {
  gchar *id;
  gchar *pubkey;
  gint kind;
  gchar *dtag;
  gint64 created_at;
  gchar *content;
  gchar *preview; /* bounded, validated text computed off the row-bind path */
  gchar *event_json;
} GhPublicNote;

GhPublicNote *gh_public_note_from_signed_json(const gchar *json, GError **error);
void gh_public_note_free(GhPublicNote *note);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GhPublicNote, gh_public_note_free)

/* At most 1,024 signed events, each at most 64 KiB, in the encrypted account
 * store. The caller's account generation must be checked before put/load. */
gboolean gh_store_public_note_put(GhStore *store, const GhPublicNote *note,
                                  GError **error);
GPtrArray *gh_store_public_notes_load(GhStore *store, GError **error);

G_END_DECLS
#endif
