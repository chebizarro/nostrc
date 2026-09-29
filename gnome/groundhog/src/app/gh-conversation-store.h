#ifndef GH_CONVERSATION_STORE_H
#define GH_CONVERSATION_STORE_H

#include "gh-conversation.h"

G_BEGIN_DECLS

typedef enum {
  GH_CONVERSATION_ADD_REJECTED,  /* another account's message, or no account */
  GH_CONVERSATION_ADD_NEW,       /* committed and inserted into its room */
  GH_CONVERSATION_ADD_DUPLICATE, /* rumor already known; seen keys committed */
  GH_CONVERSATION_ADD_FAILED,    /* the delegate could not commit: nothing
                                  * changed, nothing is marked seen, so a later
                                  * delivery of the wrap retries it */
  GH_CONVERSATION_ADD_HIDDEN     /* committed but never shown: expired on
                                  * arrival (charter §3.7, EX-4), older than a
                                  * forgotten room's tombstone, or in a room
                                  * the user blocked (G18/G19) */
} GhConversationAddResult;

/* What one delegate commit did, beyond success. The store initializes it to
 * { FALSE, -1 } before calling admit(). */
typedef struct {
  /* The rumor is recorded as seen but stored as no visible message (expired
   * on arrival, forgotten room, purged): the model must not show it. */
  gboolean hidden;
  /* The room's durable unread count after the commit, or -1 when the
   * delegate keeps none; the model then counts unloaded history with it. */
  gint64 unread;
} GhConversationCommit;

/* Persistence delegate: where admitted messages and the NIP-17 seen keys
 * live. The store calls admit() exactly once per admission and before its
 * in-memory model changes, so a durable backend can make that one call a
 * single transaction (charter §3.5 T-admit: insert the message ON CONFLICT DO
 * NOTHING, upsert the conversation, and record the wrap id and rumor id as
 * seen). admit() must be idempotent on the rumor id; wrap_id is NULL for a
 * local echo, which has no wrap yet. has_wrap() is the pre-check made before
 * any signer prompt. All callbacks run on the main context.
 *
 * The optional members may be NULL:
 *   - has_rejected()/add_rejected(): the rejected-wrap namespace, wraps
 *     finally rejected after a signer call (never a seen message), checked
 *     before any signer call like has_wrap();
 *   - mark_read()/accept(): persist gh_conversation_mark_read() (the last
 *     read message) and gh_conversation_accept() of a listed room. They run
 *     after the model changed; a failure is logged and the model keeps the
 *     change for this session;
 *   - is_blocked()/unblock(): a room the user blocked (G18/G19), which the
 *     model does not list although the delegate keeps it. is_blocked() tells
 *     whether room_id is one; unblock() lifts its block, accepting it, and
 *     lists what it kept of the room through gh_conversation_store_restore()
 *     (nothing when it kept no message). Both or neither.
 *
 * A memory-only GhDmInbox installs a delegate that keeps the messages' seen
 * keys in memory with them (only rejected wrap ids reach a file). The
 * encrypted store's delegate
 * (gh-store-conversations.h, G05) commits everything to the account's
 * GhStore and restores the rooms when it is attached. Without a delegate the
 * store is purely in-memory. */
typedef struct {
  gboolean (*has_wrap)(gpointer data, const gchar *wrap_id);
  gboolean (*has_rumor)(gpointer data, const gchar *rumor_id);
  gboolean (*admit)(gpointer data, GhMessage *message, const gchar *wrap_id,
                    GhConversationCommit *commit, GError **error);
  gboolean (*has_rejected)(gpointer data, const gchar *wrap_id);
  gboolean (*add_rejected)(gpointer data, const gchar *wrap_id, GError **error);
  gboolean (*mark_read)(gpointer data, GhConversation *conversation,
                        GhMessage *last_read, GError **error);
  gboolean (*accept)(gpointer data, GhConversation *conversation, GError **error);
  gboolean (*is_blocked)(gpointer data, const gchar *room_id);
  gboolean (*unblock)(gpointer data, const gchar *room_id, GError **error);
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
/* The bound account; the read-only "account" property notifies each change,
 * after the previous account's rooms were removed. */
const gchar *gh_conversation_store_get_account(GhConversationStore *self);

/* The single admission call, for inbound messages (with the verified wrap id
 * that carried them) and the sender's local echo (wrap_id NULL) alike:
 * delegate commit first, then the in-memory model. NEW emits
 * "message-added" (conversation, message) once the room and the store order
 * are updated. On DUPLICATE the relays of message are merged into the stored
 * copy. HIDDEN changes nothing in the model. A wrap-delivered rumor the
 * delegate already holds is DUPLICATE; a local echo of a rumor the delegate
 * stored earlier (e.g. by the durable outbox) is listed. When a durably
 * stored room's older history is not loaded and message sorts before it, the
 * message is committed into that history: NEW, the unread count updates, and
 * it is listed (without "message-added") when that history is loaded.
 * REJECTED and FAILED set error. The store takes its own reference. */
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
/* The delegate's rejected-wrap namespace (see GhConversationDelegate): FALSE,
 * and recording succeeds without effect, when it keeps none. */
gboolean gh_conversation_store_has_rejected(GhConversationStore *self,
                                            const gchar *wrap_id);
gboolean gh_conversation_store_record_rejected(GhConversationStore *self,
                                               const gchar *wrap_id, GError **error);
/* Whether the model holds the rumor. */
gboolean gh_conversation_store_has_message(GhConversationStore *self,
                                           const gchar *rumor_id);
/* Borrowed; NULL when absent. */
GhMessage *gh_conversation_store_lookup_message(GhConversationStore *self,
                                                const gchar *rumor_id);
GhConversation *gh_conversation_store_lookup(GhConversationStore *self,
                                             const gchar *room_id);

/* The most people besides the account a conversation the user starts may
 * have (charter §7.9: NIP-17 conversations of up to 10; larger groups are
 * encrypted groups). Rooms others start are not limited here. */
#define GH_CONVERSATION_MAX_PEERS 10

/* Opens the NIP-17 room of the bound account with peers (64-character hex
 * pubkeys in any case; duplicates and the account itself are ignored; NULL
 * or none: a note to self), the way New Message starts a conversation
 * (charter §7.9, G18): a listed room is returned (a message request is
 * accepted, since starting a conversation accepts it). A room the user
 * blocked is unblocked (the delegate's unblock(): starting a conversation
 * with them again is choosing to hear from them, and New Message says so
 * first, gh_conversation_store_is_blocked()) and returned with the history
 * it kept. Otherwise an empty, accepted room is listed at the top of the
 * store order and returned. An empty room is kept in memory only (nothing
 * is sent or stored until a message is) and calls no delegate. Borrowed;
 * NULL with error without a bound account (G_IO_ERROR_NOT_INITIALIZED), for
 * a malformed pubkey or more than GH_CONVERSATION_MAX_PEERS peers
 * (G_IO_ERROR_INVALID_ARGUMENT), or when unblocking fails (the delegate's
 * error). */
GhConversation *gh_conversation_store_open_room(GhConversationStore *self,
                                                const gchar *const *peers,
                                                GError **error);
/* Whether gh_conversation_store_open_room() with peers would unblock a room
 * the user blocked. FALSE without such a delegate or for invalid peers. */
gboolean gh_conversation_store_is_blocked(GhConversationStore *self,
                                          const gchar *const *peers);

G_END_DECLS
#endif
