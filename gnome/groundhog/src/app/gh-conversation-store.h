#ifndef GH_CONVERSATION_STORE_H
#define GH_CONVERSATION_STORE_H

#include "gh-conversation.h"

G_BEGIN_DECLS

typedef enum {
  GH_CONVERSATION_ADD_REJECTED,  /* another account's message, or no account */
  GH_CONVERSATION_ADD_NEW,       /* committed and inserted into its room */
  GH_CONVERSATION_ADD_DUPLICATE, /* rumor already known; seen keys committed */
  GH_CONVERSATION_ADD_FAILED     /* the delegate could not commit: nothing
                                  * changed, nothing is marked seen, so a later
                                  * delivery of the wrap retries it */
} GhConversationAddResult;

/* Persistence delegate: where admitted messages and the NIP-17 seen keys
 * live. The store calls admit() exactly once per admission and before its
 * in-memory model changes, so a durable backend can make that one call a
 * single transaction (charter §3.5 T-admit: insert the message ON CONFLICT DO
 * NOTHING, upsert the conversation, and record the wrap id and rumor id as
 * seen). admit() must be idempotent on the rumor id; wrap_id is NULL for a
 * local echo, which has no wrap yet. has_wrap() is the pre-check made before
 * any signer prompt. All callbacks run on the main context.
 *
 * Today's delegate is GhDmInbox's per-account GhNip17Seen file with messages
 * held in memory only; the encrypted store (G05) replaces it with its own
 * delegate and will extend this table with load, read-marker and acceptance
 * writes. Without a delegate the store is purely in-memory. */
typedef struct {
  gboolean (*has_wrap)(gpointer data, const gchar *wrap_id);
  gboolean (*has_rumor)(gpointer data, const gchar *rumor_id);
  gboolean (*admit)(gpointer data, GhMessage *message, const gchar *wrap_id,
                    GError **error);
} GhConversationDelegate;

#define GH_TYPE_CONVERSATION_STORE (gh_conversation_store_get_type())
G_DECLARE_FINAL_TYPE(GhConversationStore, gh_conversation_store, GH,
                     CONVERSATION_STORE, GObject)

/* The NIP-17 conversations of exactly one account: a GListModel of
 * GhConversation ordered by last activity (newest first, then room id).
 * Requests (gh_conversation_get_is_request) are included; the UI filters
 * them into their own list. Used from the main context only. */
GhConversationStore *gh_conversation_store_new(void);

/* Binds the store to account_pubkey (lowercase hex) or to none (NULL), with
 * the delegate that persists it (NULL for memory only). A different account
 * removes every conversation before anything of the next one appears; the
 * same account keeps its conversations and only swaps the delegate. The store
 * owns delegate_data and releases it with destroy when replaced or finalized. */
void gh_conversation_store_set_account(GhConversationStore *self,
                                       const gchar *account_pubkey,
                                       const GhConversationDelegate *delegate,
                                       gpointer delegate_data,
                                       GDestroyNotify destroy);
const gchar *gh_conversation_store_get_account(GhConversationStore *self);

/* The single admission call, for inbound messages (with the verified wrap id
 * that carried them) and the sender's local echo (wrap_id NULL) alike:
 * delegate commit first, then the in-memory model. NEW emits
 * "message-added" (conversation, message) once the room and the store order
 * are updated. On DUPLICATE the relays of message are merged into the stored
 * copy. REJECTED and FAILED set error. The store takes its own reference. */
GhConversationAddResult gh_conversation_store_admit(GhConversationStore *self,
                                                    GhMessage *message,
                                                    const gchar *wrap_id,
                                                    GError **error);
/* A local echo: gh_conversation_store_admit(self, message, NULL, error). */
GhConversationAddResult gh_conversation_store_add_message(GhConversationStore *self,
                                                          GhMessage *message,
                                                          GError **error);
/* Whether the delegate already committed this wrap; FALSE without one. */
gboolean gh_conversation_store_has_wrap(GhConversationStore *self, const gchar *wrap_id);
/* Whether the model holds the rumor. */
gboolean gh_conversation_store_has_message(GhConversationStore *self,
                                           const gchar *rumor_id);
/* Borrowed; NULL when absent. */
GhMessage *gh_conversation_store_lookup_message(GhConversationStore *self,
                                                const gchar *rumor_id);
GhConversation *gh_conversation_store_lookup(GhConversationStore *self,
                                             const gchar *room_id);

G_END_DECLS
#endif
