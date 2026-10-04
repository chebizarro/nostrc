#ifndef GH_STORE_REACTIONS_H
#define GH_STORE_REACTIONS_H

#include "gh-reaction-store.h"
#include "gh-store.h"

G_BEGIN_DECLS

/* GhStoreReactions: the encrypted store's persistence delegate for
 * GhReactionStore (W26 slice B, nostrc-191r). Reactions live in the
 * account's GhStore (SQLCipher); nothing is written anywhere else.
 * GTK-free; used on the store's thread (the main context).
 *
 * Each admission inserts a row in `reactions`, or defers it in
 * `pending_reactions` until its target exists in the same room. Admission and
 * deletion open nested transactions when the caller owns one. Restore loads
 * attached reactions and reconciles eligible pending rows on startup.
 *
 * The store is borrowed: call gh_store_reactions_close() before
 * gh_store_close(). */

#define GH_TYPE_STORE_REACTIONS (gh_store_reactions_get_type())
G_DECLARE_FINAL_TYPE(GhStoreReactions, gh_store_reactions, GH, STORE_REACTIONS, GObject)

GhStoreReactions *gh_store_reactions_new(GhStore *store);

/* Binds a GhReactionStore to the store's account with this delegate and
 * restores the stored reactions. */
gboolean gh_store_reactions_attach(GhStoreReactions *self,
                                   GhReactionStore *model,
                                   GError **error);

/* Reconcile only pending rows whose exact room and target are now stored.
 * NULL room/target scans eligible rows at startup. */
gboolean gh_store_reactions_reconcile(GhStoreReactions *self, GhReactionStore *model,
                                      const gchar *room_id, const gchar *target_id,
                                      GError **error);

/* Detaches from the store. Idempotent. */
void gh_store_reactions_close(GhStoreReactions *self);

G_END_DECLS
#endif
