#ifndef GH_REACTION_STORE_H
#define GH_REACTION_STORE_H

#include "gh-reaction.h"

G_BEGIN_DECLS

/* Persistence delegate: where reactions live durably. The store calls
 * admit() inside its caller's transaction (gh_store_begin/commit is already
 * held by the enclosing admission). remove() is for kind-5 deletions.
 * restore() loads all reactions for the account on startup. All callbacks
 * run on the main context. Any member may be NULL (memory only). */
typedef struct {
  gboolean (*admit)(gpointer data, GhReaction *reaction, GError **error);
  gboolean (*remove)(gpointer data, const gchar *reaction_rumor_id, GError **error);
  gboolean (*remove_by_sender)(gpointer data, const gchar *target_rumor_id,
                                const gchar *sender_pubkey, const gchar *emoji,
                                GError **error);
} GhReactionDelegate;

#define GH_TYPE_REACTION_STORE (gh_reaction_store_get_type())
G_DECLARE_FINAL_TYPE(GhReactionStore, gh_reaction_store, GH, REACTION_STORE, GObject)

/* One account's reactions across all backends: MLS, NIP-17 and NIP-29.
 * Uses the main context only. The store and model are borrowed: dispose
 * the reaction store before closing the store. */
GhReactionStore *gh_reaction_store_new(void);

/* Binds to account_pubkey (lowercase hex) or to none (NULL). A different
 * account clears everything. The delegate persists reactions; NULL for
 * memory only. */
void gh_reaction_store_set_account(GhReactionStore *self,
                                   const gchar *account_pubkey,
                                   const GhReactionDelegate *delegate,
                                   gpointer delegate_data,
                                   GDestroyNotify destroy);

/* The single admission call for an inbound reaction. Deduplicates by
 * reaction_rumor_id. Returns TRUE if the reaction was new. The delegate's
 * admit() is called before the in-memory model changes. */
gboolean gh_reaction_store_admit(GhReactionStore *self,
                                 GhReaction *reaction,
                                 GError **error);

/* Removes a reaction by its rumor id (a kind-5 deletion). Returns TRUE
 * if the reaction was found and removed. */
gboolean gh_reaction_store_remove(GhReactionStore *self,
                                  const gchar *reaction_rumor_id,
                                  GError **error);

/* Removes the account's own reaction with @emoji on @target_rumor_id.
 * Returns the removed reaction's rumor id (transfer full), or NULL if
 * not found. */
gchar *gh_reaction_store_remove_own(GhReactionStore *self,
                                    const gchar *target_rumor_id,
                                    const gchar *emoji,
                                    GError **error);

/* Looks up the reaction summary for a message. Returns a borrowed
 * GhReactionSummary that is live-updated as reactions arrive. The first
 * call for a target creates it. NULL only on bad arguments. */
GhReactionSummary *gh_reaction_store_lookup(GhReactionStore *self,
                                            const gchar *target_rumor_id);

/* Signal: "reaction-changed" (target_rumor_id: string) — emitted after
 * a reaction is added or removed, so the UI can update the affected
 * message row. */

G_END_DECLS
#endif
