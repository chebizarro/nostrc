#ifndef GH_REACTION_STORE_H
#define GH_REACTION_STORE_H

#include "gh-reaction.h"

G_BEGIN_DECLS

/* Persistence delegate: where reactions live durably. The store calls
 * admit() before the model changes; a durable delegate may open a nested
 * transaction and defer reactions without showing them. remove() is for
 * local removals; delete_event() records verified kind-5 deletions.
 * restore() loads all reactions for the account on startup. All callbacks
 * run on the main context. Any member may be NULL (memory only). */
typedef struct {
  gboolean (*admit)(gpointer data, GhReaction *reaction, GError **error);
  gboolean (*remove)(gpointer data, const gchar *reaction_rumor_id, GError **error);
  gboolean (*remove_by_sender)(gpointer data, const gchar *target_rumor_id,
                                const gchar *sender_pubkey, const gchar *emoji,
                                GError **error);
  gboolean (*delete_event)(gpointer data, const gchar *reaction_rumor_id,
                           const gchar *sender_pubkey, const gchar *room_id,
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

/* A verified NIP-09 deletion. Records a bounded, author/room-scoped notice
 * even if the reaction has not arrived yet. */
gboolean gh_reaction_store_delete_event(GhReactionStore *self,
                                        const gchar *reaction_rumor_id,
                                        const gchar *sender_pubkey,
                                        const gchar *room_id,
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

/* W26 slice B review fix (F5): temporarily suspend and resume the delegate
 * so that bulk operations (restore loop) don't trigger redundant SQLite
 * statements. */
void gh_reaction_store_suspend_delegate(GhReactionStore *self);
void gh_reaction_store_resume_delegate(GhReactionStore *self);

/* W26 slice B review fix (F3): looks up the sender pubkey for a reaction by
 * its rumor id. Returns NULL if the reaction is unknown. The returned string
 * is borrowed from the reaction and valid as long as the store is alive and
 * the reaction has not been removed. */
const gchar *gh_reaction_store_get_sender(GhReactionStore *self,
                                          const gchar *reaction_rumor_id);

/* Signal: "reaction-changed" (target_rumor_id: string) — emitted after
 * a reaction is added or removed, so the UI can update the affected
 * message row. */

G_END_DECLS
#endif
