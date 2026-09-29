#ifndef GH_STORE_CONVERSATIONS_H
#define GH_STORE_CONVERSATIONS_H

#include "gh-conversation-store.h"
#include "gh-store.h"

G_BEGIN_DECLS

/* GhStoreConversations: the encrypted store's persistence delegate for
 * GhConversationStore (privacy charter §3.3, §3.5, §8.2 G05). NIP-17 rooms,
 * their messages, the seen set, read markers, message-request state and
 * drafts live in the account's GhStore (SQLCipher); nothing is written
 * anywhere else. GTK-free; used on the store's thread (the main context).
 *
 * Admission is one transaction (T-admit). For every admitted message the
 * delegate runs, inside one gh_store_begin()/gh_store_commit():
 *   - gh_store_admit(): the wrap id and rumor id into `seen`, and unless the
 *     rumor was seen before, the message and its room (created as a request
 *     for someone else's message, accepted for an own one), participants,
 *     title and last activity;
 *   - the read state: an own message moves the read marker to itself and
 *     accepts the room; unread_count is recomputed as the messages from
 *     others after the marker.
 * A crash anywhere before the commit leaves neither the message nor its seen
 * keys; after it, both (ST-6). A rumor seen before is never stored again, so
 * a second wrap of it only adds its wrap id (ST-7), and purged, forgotten or
 * expired messages do not return from backfill (EX-6). A message already
 * expired on arrival (its expires_at, from gh_message_get_expires_at(), is not
 * in the future by the store's GhClock) and one older than a forgotten room's
 * tombstone are recorded in `seen` only and reported hidden: the model never
 * shows them (charter §3.7, EX-4, ST-9). The rejected-wrap namespace is
 * `seen` ns GH_STORE_SEEN_REJECTED_WRAP.
 *
 * Read state. conversations.last_read_msg is the last read message and
 * unread_count the messages from others after it, kept equal by every write
 * here. If the marker's row is gone (purged), the marker is taken to lie just
 * before the newest unread_count messages.
 *
 * Restore. gh_store_conversations_attach() binds a GhConversationStore to the
 * store's account with this delegate and lists every NIP-17 room that has a
 * stored message: its newest page of messages (each rumor verified again
 * from the stored JSON; expired ones skipped), request state, read marker,
 * unread count (including older, unloaded messages) and stored name, in store
 * order. Older messages load on request, a page at a time.
 *
 * Lifetime. The store is borrowed: call gh_store_conversations_close() before
 * gh_store_close(). After close every delegate call fails or answers "not
 * seen", admissions fail (GH_CONVERSATION_ADD_FAILED, so nothing is marked
 * seen anywhere) and the other functions fail with GH_STORE_ERROR_STATE.
 * The GhConversationStore holds a reference while this is its delegate.
 *
 * Errors are GH_STORE_ERROR (a full disk stays GH_STORE_ERROR_FULL). */

#define GH_TYPE_STORE_CONVERSATIONS (gh_store_conversations_get_type())
G_DECLARE_FINAL_TYPE(GhStoreConversations, gh_store_conversations, GH,
                     STORE_CONVERSATIONS, GObject)

/* Newest messages listed per room by gh_store_conversations_attach() when
 * page_size is 0, and the largest page any call loads. */
#define GH_STORE_CONVERSATIONS_PAGE_SIZE     50
#define GH_STORE_CONVERSATIONS_MAX_PAGE_SIZE 1000
/* The largest legacy seen file imported (an oversized one is refused). */
#define GH_STORE_CONVERSATIONS_MAX_SEEN_FILE (32 * 1024 * 1024)

/* A delegate over @store, which must stay open until
 * gh_store_conversations_close(). An ephemeral store ("Continue Without
 * Saving Messages") works the same and keeps nothing after closing. */
GhStoreConversations *gh_store_conversations_new(GhStore *store);

/* Binds @model to the store's account with this delegate (replacing any
 * other; another account's rooms are dropped first) and restores the stored
 * rooms, each with its newest @page_size messages (0: the default). On a
 * read error the rooms restored so far stay listed and the delegate stays
 * bound. */
gboolean gh_store_conversations_attach(GhStoreConversations *self,
                                       GhConversationStore *model, guint page_size,
                                       GError **error);
/* Lists up to @limit (1 to GH_STORE_CONVERSATIONS_MAX_PAGE_SIZE) older
 * messages of a room of the attached model; *out_loaded (nullable) is how
 * many were listed. Nothing to do (no older messages) is success with 0. */
gboolean gh_store_conversations_load_older(GhStoreConversations *self,
                                           GhConversation *conversation, guint limit,
                                           guint *out_loaded, GError **error);

/* The composer draft of a room (NULL: none). @room_id is a canonical NIP-17
 * room id of the account (gh_message_get_room_id()). Setting a draft for a
 * room that is not stored yet creates it, as accepted; NULL or "" clears. */
gboolean gh_store_conversations_get_draft(GhStoreConversations *self, const gchar *room_id,
                                          gchar **out_draft, GError **error);
gboolean gh_store_conversations_set_draft(GhStoreConversations *self, const gchar *room_id,
                                          const gchar *draft, GError **error);

/* Forget conversation (charter §3.8, ST-9): gh_store_forget_conversation()
 * (messages, participants, outbox and draft deleted; a tombstone refuses
 * older backfill) and the room is unlisted from the attached model. A later
 * message starts it again with only that message. NOT_FOUND when the room
 * is not stored. */
gboolean gh_store_conversations_forget(GhStoreConversations *self, const gchar *room_id,
                                       GError **error);

/* T-purge with the attached model kept in step (charter §3.7, G07):
 * gh_store_purge_full() deletes what expired (and, with @retention_cutoff >
 * 0, what was received before it). Then each NIP-17 room it touched gets
 * its stored name back from the newest remaining subject, and in the
 * attached model every purged message leaves its room: a room with nothing
 * stored any more is unlisted, any other takes its durable read marker,
 * unread count and name again (loading its newest remaining page if every
 * listed message went). *out_purged (nullable, NULL-terminated) receives the
 * purged NIP-17 rumor ids, so what showed them (notifications) can be
 * withdrawn. Fails only if the purge itself did; a later failure to update
 * names or rooms is logged, and the model still drops every purged
 * message. A read-only store (STORE_CORRUPT) deletes nothing: expired
 * messages only leave the model (reported as n_expired and in *out_purged),
 * and retention waits for a writable store. */
gboolean gh_store_conversations_purge(GhStoreConversations *self, gint64 retention_cutoff,
                                      GhStorePurgeStats *out_stats, GStrv *out_purged,
                                      GError **error);

/* ---- Legacy seen file (charter §3.2, ST-12) -----------------------------------
 * Before the encrypted store, GhDmInbox kept the account's seen keys in a
 * 0600 flat file: <state_dir>/<account>.seen (Groundhog 0.6.0) or, in a build
 * without the store, <state_dir>/<acct>.seen (gh_nip17_seen_file_name()). A
 * header line "groundhog-nip17-seen 1 <account>", then one "w <wrap id>",
 * "r <rumor id>" or "x <rejected wrap id>" line each. */
typedef struct {
  guint rejected;  /* "x" ids newly added to GH_STORE_SEEN_REJECTED_WRAP */
  guint dropped;   /* "w" and "r" lines, never imported (see below) */
} GhStoreSeenImport;

/* <state_dir>/<account_pubkey>.seen; NULL state_dir means GhDmInbox's
 * default, $XDG_STATE_HOME/groundhog/nip17. */
gchar *gh_store_conversations_legacy_seen_path(const gchar *state_dir,
                                               const gchar *account_pubkey);
/* Imports the rejected-wrap ids ("x") of the legacy file at @path into
 * `seen` in one transaction, then deletes the file. Its "w" and "r" keys are
 * dropped: the inbox that wrote them kept the messages in memory only, so
 * importing them would record as seen messages that no store holds and hide
 * them for good although the relays still have them (W13 review B1). Left
 * out, those wraps are fetched, unwrapped and stored again by the first
 * encrypted-store session (whatever the relays still hold); a rejected id
 * never hides a message and spares a signer approval. Idempotent: keys
 * already present are kept, and a crash between the commit and the unlink
 * imports nothing new next time. A missing file is success with nothing
 * imported. A symlink, a file of another user or not a regular file
 * (PERMISSIONS), another account's file (FOREIGN), and a malformed or
 * oversized one (INVALID) are refused and left in place; a torn final line
 * is ignored, as GhDmInbox did. If only the deletion fails, the keys stay
 * imported, *out_stats counts them and the error (GH_STORE_ERROR_FAILED)
 * says so; calling again deletes the file. */
gboolean gh_store_conversations_import_seen_file(GhStoreConversations *self,
                                                 const gchar *path,
                                                 GhStoreSeenImport *out_stats,
                                                 GError **error);

/* Detaches from the store; see "Lifetime" above. Idempotent. */
void gh_store_conversations_close(GhStoreConversations *self);

G_END_DECLS
#endif
