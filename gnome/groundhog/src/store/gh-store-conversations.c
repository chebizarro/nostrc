#include "gh-store-conversations.h"

#include "gh-conversation-private.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

/* The legacy GhNip17Seen file (gh-nip17-inbox.c). */
#define SEEN_MAGIC "groundhog-nip17-seen 1 "
#define SEEN_LINE  67 /* "w ", "r " or "x " + 64 hex + "\n" */
/* A canonical room id: at most sender + GH_MESSAGE_MAX_RECIPIENTS pubkeys. */
#define MAX_ROOM_ID ((GH_MESSAGE_MAX_RECIPIENTS + 1) * 65 - 1)

struct _GhStoreConversations {
  GObject parent_instance;
  GhStore *store;  /* borrowed; NULL once closed */
  gchar *account;
  GWeakRef model;  /* the GhConversationStore it was attached to */
};

G_DEFINE_FINAL_TYPE(GhStoreConversations, gh_store_conversations, G_TYPE_OBJECT)

/* ---- Statements ------------------------------------------------------------- */

static gboolean
check_open(GhStoreConversations *self, GError **error)
{
  if (self->store)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                      "The conversation store is closed");
  return FALSE;
}

/* If SQLite abandoned the caller's transaction (disk full, I/O error),
 * nothing may run as if it were still open: each statement would autocommit
 * a fragment of an atomic operation. */
static sqlite3_stmt *
prepare(GhStore *store, const char *sql, GError **error)
{
  struct sqlite3 *db = gh_store_get_db(store);
  if (gh_store_get_transaction_depth(store) > 0 && sqlite3_get_autocommit(db)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "The store transaction was rolled back by an earlier error");
    return NULL;
  }
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    return stmt;
  sqlite3_finalize(stmt);
  gh_store_set_sqlite_error(store, rc, "Preparing a conversation statement", error);
  return NULL;
}

#define BIND(expr)                                                              \
  G_STMT_START {                                                                \
    int bind_rc_ = (expr);                                                      \
    if (bind_rc_ != SQLITE_OK) {                                                \
      gh_store_set_sqlite_error(store, bind_rc_, "Binding a conversation value", error); \
      goto fail;                                                                \
    }                                                                           \
  } G_STMT_END

static int
bind_text(sqlite3_stmt *stmt, int index, const gchar *text)
{
  if (!text)
    return sqlite3_bind_null(stmt, index);
  return sqlite3_bind_text(stmt, index, text, -1, SQLITE_STATIC);
}

static gboolean
step_row(GhStore *store, sqlite3_stmt *stmt, gboolean *has_row, const gchar *what,
         GError **error)
{
  int rc = sqlite3_step(stmt);
  *has_row = rc == SQLITE_ROW;
  if (rc == SQLITE_ROW || rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, what, error);
}

static gboolean
step_done(GhStore *store, sqlite3_stmt *stmt, const gchar *what, GError **error)
{
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  if (rc == SQLITE_ROW) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED, "%s: unexpected row", what);
    return FALSE;
  }
  return gh_store_set_sqlite_error(store, rc, what, error);
}

static gchar *
column_text(sqlite3_stmt *stmt, int column)
{
  const unsigned char *text = sqlite3_column_text(stmt, column);
  return text ? g_strdup((const gchar *)text) : NULL;
}

/* ---- Rooms and read state ------------------------------------------------------ */

static gboolean
is_hex64(const gchar *s)
{
  for (guint i = 0; i < 64; i++)
    if (!g_ascii_isdigit(s[i]) && (s[i] < 'a' || s[i] > 'f'))
      return FALSE;
  return TRUE;
}

/* A canonical NIP-17 room id of the account: sorted, unique lowercase hex
 * pubkeys joined by ',', one of them the account (gh_message_get_room_id()). */
static gboolean
check_room(GhStoreConversations *self, const gchar *room_id, GError **error)
{
  gsize len = room_id ? strnlen(room_id, MAX_ROOM_ID + 1) : 0;
  gboolean ok = len > 0 && len <= MAX_ROOM_ID && len % 65 == 64;
  gboolean has_account = FALSE;
  for (gsize off = 0; ok && off < len; off += 65) {
    ok = is_hex64(room_id + off) && (off + 64 == len || room_id[off + 64] == ',') &&
         (off == 0 || strncmp(room_id + off - 65, room_id + off, 64) < 0);
    has_account = has_account || strncmp(room_id + off, self->account, 64) == 0;
  }
  if (ok && has_account)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                      "Not a NIP-17 room of this account");
  return FALSE;
}

/* The stored room's id; *found FALSE when it is not stored. */
static gboolean
lookup_room(GhStore *store, const gchar *room_id, gboolean *found, gint64 *id,
            GError **error)
{
  *found = FALSE;
  *id = 0;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id FROM conversations WHERE backend = 1 AND backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, room_id));
  gboolean ok = step_row(store, stmt, found, "Looking up a conversation", error);
  if (ok && *found)
    *id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Whether the stored room is blocked (request_state BLOCKED); FALSE when it
 * is not stored. */
static gboolean
room_blocked(GhStore *store, const gchar *room_id, gboolean *blocked, GError **error)
{
  *blocked = FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT request_state FROM conversations WHERE backend = 1 AND backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, room_id));
  gboolean found = FALSE;
  gboolean ok = step_row(store, stmt, &found, "Reading a conversation's block", error);
  if (ok && found)
    *blocked = sqlite3_column_int64(stmt, 0) == GH_STORE_REQUEST_BLOCKED;
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* A stored message's place in the message order. */
typedef struct {
  gboolean has;
  gint64 row;         /* messages.id */
  gint64 created_at;
  gchar *id;          /* backend_msg_id (the rumor id) */
} Place;

static void
place_clear(Place *place)
{
  g_clear_pointer(&place->id, g_free);
  memset(place, 0, sizeof *place);
}

static void
place_set(Place *place, gint64 row, gint64 created_at, const gchar *id)
{
  gchar *copy = g_strdup(id);
  place_clear(place);
  place->has = TRUE;
  place->row = row;
  place->created_at = created_at;
  place->id = copy;
}


/* The stored row of the room's message rumor_id; 0 when not stored. */
static gboolean
find_message_row(GhStore *store, const gchar *room_id, const gchar *rumor_id,
                 gint64 *out_row, GError **error)
{
  *out_row = 0;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT m.id FROM messages m JOIN conversations c ON c.id = m.conversation_id "
    "WHERE c.backend = 1 AND c.backend_key = ?1 AND m.backend_msg_id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, room_id));
  BIND(bind_text(stmt, 2, rumor_id));
  gboolean has_row = FALSE;
  gboolean ok = step_row(store, stmt, &has_row, "Looking up a message", error);
  if (ok && has_row)
    *out_row = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* The read state after a stored message (see GhStoreReadState): an own
 * message accepts a message request and moves the read state (replying
 * implies having read what came before). One written on this device (its
 * local echo, wrap_id NULL, whether new or stored by T-enqueue) reads what
 * had arrived before it and sorts before it; one another device wrote,
 * first delivered now (stored), moves the reply boundary; a relay's copy of
 * an own message already stored changes nothing. The unread count follows.
 * It never lifts a block: only writing from this device does, in T-enqueue
 * (gh_store_enqueue()), which stored the message before its local echo gets
 * here. A self-copy a relay delivers (replayed, older, or written on another
 * device, which knows nothing of this device's block, P8) leaves the room
 * blocked. */
static gboolean
admit_read_state(GhStore *store, const gchar *room_id, gint64 message_row, gint64 seq,
                 GhMessage *message, const gchar *wrap_id, gboolean stored_now,
                 gint64 *out_unread, GError **error)
{
  gboolean found = FALSE;
  gint64 conversation_id = 0;
  if (!lookup_room(store, room_id, &found, &conversation_id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    return FALSE;
  }
  GhStoreReadMove move = GH_STORE_READ_RECOUNT;
  gboolean ok = TRUE;
  if (gh_message_is_self(message)) {
    move = !wrap_id ? GH_STORE_READ_LISTED : stored_now ? GH_STORE_READ_REPLY
                                                        : GH_STORE_READ_RECOUNT;
    sqlite3_stmt *stmt = prepare(store,
      "UPDATE conversations SET request_state = 0 WHERE id = ?1 AND request_state = 1", error);
    ok = stmt && sqlite3_bind_int64(stmt, 1, conversation_id) == SQLITE_OK &&
         step_done(store, stmt, "Accepting a conversation", error);
    sqlite3_finalize(stmt);
  }
  return ok && gh_store_read_state_update(store, conversation_id, move, message_row, seq,
                                          out_unread, error);
}

/* ---- The delegate ---------------------------------------------------------------- */

static gboolean
seen_contains(GhStoreConversations *self, GhStoreSeenNs ns, const gchar *id)
{
  if (!self->store || !id)
    return FALSE;
  gboolean seen = FALSE;
  g_autoptr(GError) error = NULL;
  if (gh_store_seen_contains(self->store, ns, id, &seen, &error))
    return seen;
  /* Not seen: the caller unwraps, and the commit then fails the same way,
   * so the wrap is deferred rather than lost. A malformed id is never seen. */
  if (!g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID))
    g_warning("Groundhog could not read the seen set: %s", error->message);
  return FALSE;
}

static gboolean
delegate_has_wrap(gpointer data, const gchar *wrap_id)
{
  return seen_contains(data, GH_STORE_SEEN_WRAP, wrap_id);
}

static gboolean
delegate_has_rumor(gpointer data, const gchar *rumor_id)
{
  return seen_contains(data, GH_STORE_SEEN_RUMOR, rumor_id);
}

static gboolean
delegate_has_rejected(gpointer data, const gchar *wrap_id)
{
  return seen_contains(data, GH_STORE_SEEN_REJECTED_WRAP, wrap_id);
}

static gboolean
delegate_add_rejected(gpointer data, const gchar *wrap_id, GError **error)
{
  GhStoreConversations *self = data;
  return check_open(self, error) &&
         gh_store_seen_add(self->store, GH_STORE_SEEN_REJECTED_WRAP, wrap_id, error);
}

/* The room name is bounded in the store; a longer subject is cut at a
 * character boundary (the message keeps it whole). */
static gchar *
bounded_title(const gchar *subject)
{
  if (!subject)
    return NULL;
  gsize len = strnlen(subject, GH_STORE_MAX_TITLE + 1);
  if (len <= GH_STORE_MAX_TITLE)
    return g_strdup(subject);
  const gchar *end = subject + GH_STORE_MAX_TITLE;
  while (end > subject && ((guchar)*end & 0xc0) == 0x80)
    end--;
  return g_strndup(subject, end - subject);
}

/* A message from someone else in a blocked room (G18): its wrap and rumor
 * ids are recorded as seen, so it is neither unwrapped nor offered again,
 * and nothing of it is stored. */
static gboolean
admit_blocked(GhStore *store, GhMessage *message, const gchar *wrap_id, GError **error)
{
  return (!wrap_id || gh_store_seen_add(store, GH_STORE_SEEN_WRAP, wrap_id, error)) &&
         gh_store_seen_add(store, GH_STORE_SEEN_RUMOR, gh_message_get_rumor_id(message), error);
}

/* T-admit (see the header): one transaction for the message, its room, its
 * seen keys and the room's read state. */
static gboolean
delegate_admit(gpointer data, GhMessage *message, const gchar *wrap_id,
               GhConversationCommit *commit, GError **error)
{
  GhStoreConversations *self = data;
  if (!check_open(self, error))
    return FALSE;
  if (g_strcmp0(gh_message_get_account(message), self->account) != 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The message belongs to another account");
    return FALSE;
  }
  GhStore *store = self->store;
  const gboolean own = gh_message_is_self(message);
  const gchar *room_id = gh_message_get_room_id(message);
  g_autofree gchar *title = bounded_title(gh_message_get_subject(message));
  GhStoreMessage m = {
    .backend = GH_STORE_BACKEND_NIP17,
    .backend_key = room_id,
    .backend_msg_id = gh_message_get_rumor_id(message),
    .wrap_id = wrap_id,
    .sender_pubkey = gh_message_get_sender(message),
    .kind = gh_message_get_kind(message),
    .created_at = gh_message_get_created_at(message),
    .direction = own ? GH_STORE_DIRECTION_OUT : GH_STORE_DIRECTION_IN,
    .body = gh_message_get_content(message),
    .raw_json = gh_message_get_rumor_json(message),
    .expires_at = gh_message_get_expires_at(message),
    .title = title,
    .participants = gh_message_get_participants(message),
    .unread = !own,
    .request_state = own ? GH_STORE_REQUEST_ACCEPTED : GH_STORE_REQUEST_PENDING,
  };
  GhStoreAdmitResult result = GH_STORE_ADMIT_DUPLICATE;
  gint64 message_row = 0;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!own) {
    gboolean blocked = FALSE;
    if (!room_blocked(store, room_id, &blocked, error))
      goto fail;
    if (blocked) {
      if (!admit_blocked(store, message, wrap_id, error))
        goto fail;
      commit->hidden = TRUE;
      return gh_store_commit(store, error);
    }
  }
  if (!gh_store_admit(store, &m, &result, &message_row, error))
    goto fail;
  /* A duplicate may still be a stored message (e.g. the outbox stored it
   * before this local echo); otherwise it was recorded as seen only. */
  if (result == GH_STORE_ADMIT_DUPLICATE &&
      !find_message_row(store, room_id, m.backend_msg_id, &message_row, error))
    goto fail;
  if (message_row > 0) {
    gint64 seq = 0;
    if (!gh_store_message_seq(store, message_row, &seq, error) ||
        !admit_read_state(store, room_id, message_row, seq, message, wrap_id,
                          result == GH_STORE_ADMIT_STORED, &commit->unread, error))
      goto fail;
    commit->seq = seq;
    /* Someone else's message in a blocked room returned above. An own one
     * is stored; the local echo of a message this device wrote is listed,
     * since its T-enqueue lifted the block, while a relay's self-copy in a
     * room that is still blocked stays unlisted with it. */
    gboolean blocked = FALSE;
    if (!room_blocked(store, room_id, &blocked, error))
      goto fail;
    commit->hidden = blocked;
  } else {
    commit->hidden = TRUE; /* seen only: expired, forgotten room, purged */
  }
  return gh_store_commit(store, error);

fail:
  gh_store_rollback(store);
  return FALSE;
}

/* The stored row of @message in the room, or with @newest and none, the
 * room's newest stored message; 0 when neither. */
static gboolean
room_message_row(GhStore *store, gint64 conversation_id, GhMessage *message, gboolean newest,
                 gint64 *out_row, GError **error)
{
  *out_row = 0;
  static const char *const queries[] = {
    "SELECT id FROM messages WHERE conversation_id = ?1 AND backend_msg_id = ?2",
    "SELECT id FROM messages WHERE conversation_id = ?1 "
    "ORDER BY created_at DESC, backend_msg_id DESC LIMIT 1",
  };
  sqlite3_stmt *stmt = NULL;
  for (guint i = 0; i < (newest ? 2u : 1u) && *out_row == 0; i++) {
    stmt = prepare(store, queries[i], error);
    if (!stmt)
      return FALSE;
    BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
    if (i == 0)
      BIND(bind_text(stmt, 2, gh_message_get_rumor_id(message)));
    gboolean has_row = FALSE;
    if (!step_row(store, stmt, &has_row, "Looking up a message", error))
      goto fail;
    if (has_row)
      *out_row = sqlite3_column_int64(stmt, 0);
    g_clear_pointer(&stmt, sqlite3_finalize);
  }
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Mark read or unread (gh_conversation_mark_read()/_unread()) in one
 * transaction; read state is local only (P8). */
static gboolean
persist_read_move(GhStoreConversations *self, GhConversation *conversation, GhMessage *message,
                  GhStoreReadMove move, GError **error)
{
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  gboolean found = FALSE;
  gint64 conversation_id = 0, row = 0;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!lookup_room(store, gh_conversation_get_room_id(conversation), &found,
                   &conversation_id, error))
    goto fail;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    goto fail;
  }
  /* Read: the listed newest message, or (not stored) the newest stored one;
   * a read position only moves forward. Unread: exactly that message. */
  if (!room_message_row(store, conversation_id, message, move == GH_STORE_READ_LISTED, &row,
                        error))
    goto fail;
  if (row > 0 &&
      !gh_store_read_state_update(store, conversation_id, move, row,
                                  (gint64)MIN(gh_conversation_get_read_seq(conversation),
                                              (guint64)G_MAXINT64),
                                  NULL, error))
    goto fail;
  return gh_store_commit(store, error);

fail:
  gh_store_rollback(store);
  return FALSE;
}

static gboolean
delegate_mark_read(gpointer data, GhConversation *conversation, GhMessage *last_read,
                   GError **error)
{
  return persist_read_move(data, conversation, last_read, GH_STORE_READ_LISTED, error);
}

static gboolean
delegate_mark_unread(gpointer data, GhConversation *conversation, GhMessage *first_unread,
                     GError **error)
{
  return persist_read_move(data, conversation, first_unread, GH_STORE_READ_UNREAD, error);
}

static gboolean
delegate_accept(gpointer data, GhConversation *conversation, GError **error)
{
  GhStoreConversations *self = data;
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  /* Only a request is accepted here; a blocked room stays blocked. */
  sqlite3_stmt *stmt = prepare(store,
    "UPDATE conversations SET request_state = 0 "
    "WHERE backend = 1 AND backend_key = ?1 AND request_state = 1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, gh_conversation_get_room_id(conversation)));
  gboolean ok = step_done(store, stmt, "Accepting a conversation", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean
delegate_is_blocked(gpointer data, const gchar *room_id)
{
  GhStoreConversations *self = data;
  gboolean blocked = FALSE;
  g_autoptr(GError) error = NULL;
  if (!self->store || !check_room(self, room_id, NULL))
    return FALSE;
  if (!room_blocked(self->store, room_id, &blocked, &error))
    g_warning("Groundhog could not read a conversation's block: %s", error->message);
  return blocked;
}

static gboolean unblock_room(GhStoreConversations *self, const gchar *room_id, gboolean accept,
                             GError **error);

/* New Message to someone blocked (G18): starting a conversation accepts it. */
static gboolean
delegate_unblock(gpointer data, const gchar *room_id, GError **error)
{
  return unblock_room(data, room_id, TRUE, error);
}

static gboolean
delegate_reset_latest(gpointer data, GhConversation *conversation, GError **error)
{
  return gh_store_conversations_reset_latest(data, conversation, error);
}

static const GhConversationDelegate store_delegate = {
  .has_wrap = delegate_has_wrap,
  .has_rumor = delegate_has_rumor,
  .admit = delegate_admit,
  .has_rejected = delegate_has_rejected,
  .add_rejected = delegate_add_rejected,
  .mark_read = delegate_mark_read,
  .accept = delegate_accept,
  .is_blocked = delegate_is_blocked,
  .unblock = delegate_unblock,
  .mark_unread = delegate_mark_unread,
  .reset_latest = delegate_reset_latest,
};

/* ---- Restore ------------------------------------------------------------------------ */

/* The room's durable state for GhConversationState. */
typedef struct {
  gint64 request_state;
  gchar *title;
  gint64 pinned_rank;
  gint64 timer_seconds;
  gint64 timer_changed_at;
  GhStoreReadState read;
} RoomState;

static void
room_state_clear(RoomState *state)
{
  g_clear_pointer(&state->title, g_free);
  gh_store_read_state_clear(&state->read);
}

static gboolean
load_room_state(GhStore *store, gint64 conversation_id, RoomState *state, GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
    "SELECT request_state, title, pinned_rank, disappearing_s, timer_changed_at "
    "FROM conversations WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  if (!step_row(store, stmt, &has_row, "Reading a conversation", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    goto fail;
  }
  state->request_state = sqlite3_column_int64(stmt, 0);
  state->title = column_text(stmt, 1);
  state->pinned_rank = sqlite3_column_type(stmt, 2) == SQLITE_NULL
                         ? 0 : MAX(sqlite3_column_int64(stmt, 2), 1);
  state->timer_seconds = sqlite3_column_int64(stmt, 3);
  state->timer_changed_at = sqlite3_column_int64(stmt, 4);
  sqlite3_finalize(stmt);
  return gh_store_read_state_load(store, conversation_id, &state->read, error);
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* The model's view of a stored room's state (borrows from state). */
static GhConversationState
conversation_state(const RoomState *state, gboolean has_older, gint64 floor_created_at,
                   const gchar *floor_id)
{
  return (GhConversationState){
    .accepted = state->request_state == GH_STORE_REQUEST_ACCEPTED,
    .has_marker = state->read.has_marker,
    .marker_created_at = state->read.marker_created_at,
    .marker_id = state->read.marker_id,
    .subject = state->title,
    .unread = (guint)CLAMP(state->read.unread, 0, (gint64)G_MAXUINT),
    .has_older = has_older,
    .floor_created_at = floor_created_at,
    .floor_id = floor_id,
    .read_seq = (guint64)MAX(state->read.read_seq, 0),
    .has_reply = state->read.has_reply,
    .reply_created_at = state->read.reply_created_at,
    .reply_id = state->read.reply_id,
    .pinned_rank = state->pinned_rank,
    .timer_seconds = state->timer_seconds,
    .timer_changed_at = state->timer_changed_at,
  };
}

/* Lists a page of a stored room: its newest messages (conversation NULL) or
 * those before the listed room's floor. Pages past messages that cannot be
 * listed (expired, or failing verification) so a new room gets at least one
 * message when it has any. */
static gboolean
restore_room(GhStoreConversations *self, GhConversationStore *model, gint64 conversation_id,
             const gchar *room_id, GhConversation *conversation, guint limit,
             gboolean reset, guint *out_listed, GError **error)
{
  GhStore *store = self->store;
  const gint64 now = gh_clock_get_unix(gh_store_get_clock(store));
  Place cursor = { 0 };
  const gchar *floor_id = NULL;
  gint64 floor_created_at = 0;
  if (conversation) {
    if (!gh_conversation_get_floor(conversation, &floor_created_at, &floor_id)) {
      *out_listed = 0;
      return TRUE; /* nothing older */
    }
    place_set(&cursor, 0, floor_created_at, floor_id);
  }
  g_autoptr(GPtrArray) messages = g_ptr_array_new_with_free_func(g_object_unref);
  RoomState state = { 0 };
  sqlite3_stmt *stmt = prepare(store,
    "SELECT created_at, backend_msg_id, raw_json, expires_at, seq FROM messages "
    "WHERE conversation_id = ?1 AND (?2 = 0 OR created_at < ?3 OR "
    "(created_at = ?3 AND backend_msg_id < ?4)) "
    "ORDER BY created_at DESC, backend_msg_id DESC LIMIT ?5", error);
  if (!stmt)
    goto fail;
  gboolean more = FALSE;
  do {
    sqlite3_reset(stmt);
    BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
    BIND(sqlite3_bind_int(stmt, 2, cursor.has));
    BIND(sqlite3_bind_int64(stmt, 3, cursor.created_at));
    BIND(bind_text(stmt, 4, cursor.has ? cursor.id : ""));
    BIND(sqlite3_bind_int64(stmt, 5, (gint64)limit + 1));
    guint rows = 0;
    gboolean has_row = FALSE;
    more = FALSE;
    while (TRUE) {
      if (!step_row(store, stmt, &has_row, "Reading stored messages", error))
        goto fail;
      if (!has_row)
        break;
      if (++rows > limit) {
        more = TRUE; /* the extra row only tells that older ones exist */
        break;
      }
      place_set(&cursor, 0, sqlite3_column_int64(stmt, 0),
                (const gchar *)sqlite3_column_text(stmt, 1));
      gint64 expires_at = sqlite3_column_int64(stmt, 3);
      if (expires_at > 0 && expires_at <= now)
        continue; /* expired; the purge (G07) deletes it */
      g_autoptr(GError) invalid = NULL;
      GhMessage *message = gh_message_new_from_rumor(self->account,
        (const gchar *)sqlite3_column_text(stmt, 2), &invalid);
      if (!message || g_strcmp0(gh_message_get_room_id(message), room_id) != 0) {
        g_warning("Groundhog skipped a stored message that failed verification: %s",
                  invalid ? invalid->message : "it belongs to another room");
        g_clear_object(&message);
        continue;
      }
      gh_message_set_expires_at(message, expires_at);
      gh_message_set_seq(message, (guint64)MAX(sqlite3_column_int64(stmt, 4), 0));
      g_ptr_array_add(messages, message);
    }
  } while (!conversation && messages->len == 0 && more);
  g_clear_pointer(&stmt, sqlite3_finalize);

  if (!conversation && messages->len == 0) {
    place_clear(&cursor);
    *out_listed = 0;
    return TRUE; /* nothing listable */
  }
  if (!load_room_state(store, conversation_id, &state, error))
    goto fail;
  GhConversationState restored = conversation_state(&state, more && cursor.has,
                                                    cursor.created_at, cursor.id);
  if (reset) {
    GhConversation *existing = gh_conversation_store_lookup(model, room_id);
    if (existing)
      gh_conversation_window_clear(existing);
  }
  GhConversation *listed = gh_conversation_store_restore(model, room_id, messages, &restored);
  if (listed)
    gh_conversation_window_enable(listed);
  *out_listed = messages->len;
  room_state_clear(&state);
  place_clear(&cursor);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  room_state_clear(&state);
  place_clear(&cursor);
  return FALSE;
}

gboolean
gh_store_conversations_attach(GhStoreConversations *self, GhConversationStore *model,
                              guint page_size, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(model), FALSE);
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  guint limit = page_size ? MIN(page_size, GH_STORE_CONVERSATIONS_MAX_PAGE_SIZE)
                          : GH_STORE_CONVERSATIONS_PAGE_SIZE;
  gh_conversation_store_set_account(model, self->account, &store_delegate,
                                    g_object_ref(self), g_object_unref);
  g_weak_ref_set(&self->model, model);

  g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
  g_autoptr(GPtrArray) rooms = g_ptr_array_new_with_free_func(g_free);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id, backend_key FROM conversations c WHERE backend = 1 AND request_state != 2 AND "
    "EXISTS (SELECT 1 FROM messages m WHERE m.conversation_id = c.id) "
    "ORDER BY last_activity DESC, backend_key", error);
  if (!stmt)
    return FALSE;
  gboolean has_row = FALSE;
  while (TRUE) {
    if (!step_row(store, stmt, &has_row, "Listing conversations", error)) {
      sqlite3_finalize(stmt);
      return FALSE;
    }
    if (!has_row)
      break;
    gint64 id = sqlite3_column_int64(stmt, 0);
    g_array_append_val(ids, id);
    g_ptr_array_add(rooms, column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);
  for (guint i = 0; i < ids->len; i++) {
    guint listed = 0;
    if (!restore_room(self, model, g_array_index(ids, gint64, i),
                      g_ptr_array_index(rooms, i), NULL, limit, FALSE, &listed, error))
      return FALSE;
  }
  return TRUE;
}

gboolean
gh_store_conversations_load_older(GhStoreConversations *self, GhConversation *conversation,
                                  guint limit, guint *out_loaded, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  if (out_loaded)
    *out_loaded = 0;
  if (!check_open(self, error))
    return FALSE;
  if (limit == 0 || limit > GH_STORE_CONVERSATIONS_MAX_PAGE_SIZE) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                "A page holds 1 to %u messages", GH_STORE_CONVERSATIONS_MAX_PAGE_SIZE);
    return FALSE;
  }
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  if (!model || g_strcmp0(gh_conversation_store_get_account(model), self->account) != 0 ||
      gh_conversation_store_lookup(model, room_id) != conversation) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The conversation is not listed by the attached model");
    return FALSE;
  }
  gboolean found = FALSE;
  gint64 conversation_id = 0;
  if (!lookup_room(self->store, room_id, &found, &conversation_id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    return FALSE;
  }
  guint listed = 0;
  if (!restore_room(self, model, conversation_id, room_id, conversation, limit, FALSE, &listed, error))
    return FALSE;
  if (out_loaded)
    *out_loaded = listed;
  return TRUE;
}

gboolean
gh_store_conversations_load_newer(GhStoreConversations *self, GhConversation *conversation,
                                  guint limit, guint *out_loaded, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  if (out_loaded)
    *out_loaded = 0;
  if (!check_open(self, error))
    return FALSE;
  if (limit == 0 || limit > GH_STORE_CONVERSATIONS_MAX_PAGE_SIZE) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Invalid page size");
    return FALSE;
  }
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  if (!model || gh_conversation_store_lookup(model, room_id) != conversation ||
      gh_conversation_get_backend(conversation) != GH_CONVERSATION_BACKEND_NIP17) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The conversation is not listed by the attached model");
    return FALSE;
  }
  gint64 at = 0, conversation_id = 0;
  const gchar *edge_id = NULL;
  gboolean found = FALSE;
  if (!gh_conversation_get_newer_cursor(conversation, &at, &edge_id) ||
      !lookup_room(self->store, room_id, &found, &conversation_id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    return FALSE;
  }
  GhStore *store = self->store;
  g_autoptr(GPtrArray) messages = g_ptr_array_new_with_free_func(g_object_unref);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT raw_json, expires_at, seq FROM messages WHERE conversation_id = ?1 "
    "AND (created_at > ?2 OR (created_at = ?2 AND backend_msg_id > ?3)) "
    "ORDER BY created_at ASC, backend_msg_id ASC", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(sqlite3_bind_int64(stmt, 2, at));
  BIND(bind_text(stmt, 3, edge_id));
  gboolean more = FALSE, has_row = FALSE;
  gint64 now = gh_clock_get_unix(gh_store_get_clock(self->store));
  while (TRUE) {
    if (!step_row(self->store, stmt, &has_row, "Reading newer stored messages", error)) {
      sqlite3_finalize(stmt);
      return FALSE;
    }
    if (!has_row)
      break;
    if (messages->len == limit) {
      more = TRUE;
      break;
    }
    gint64 expires_at = sqlite3_column_int64(stmt, 1);
    if (expires_at > 0 && expires_at <= now)
      continue;
    g_autoptr(GError) invalid = NULL;
    GhMessage *message = gh_message_new_from_rumor(self->account,
      (const gchar *)sqlite3_column_text(stmt, 0), &invalid);
    if (!message || g_strcmp0(gh_message_get_room_id(message), room_id) != 0) {
      g_warning("Groundhog skipped a stored message that failed verification: %s",
                invalid ? invalid->message : "it belongs to another room");
      g_clear_object(&message);
      continue;
    }
    gh_message_set_expires_at(message, expires_at);
    gh_message_set_seq(message, (guint64)MAX(sqlite3_column_int64(stmt, 2), 0));
    g_ptr_array_add(messages, message);
  }
  sqlite3_finalize(stmt);
  gh_conversation_window_add_newer(conversation, messages, more);
  if (out_loaded)
    *out_loaded = messages->len;
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_conversations_reset_latest(GhStoreConversations *self, GhConversation *conversation,
                                    GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  if (!check_open(self, error))
    return FALSE;
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  if (!model || gh_conversation_store_lookup(model, room_id) != conversation) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The conversation is not listed by the attached model");
    return FALSE;
  }
  gint64 id = 0;
  gboolean found = FALSE;
  if (!lookup_room(self->store, room_id, &found, &id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    return FALSE;
  }
  guint listed = 0;
  return restore_room(self, model, id, room_id, NULL, GH_CONVERSATION_WINDOW_OPEN,
                      TRUE, &listed, error);
}

/* ---- Drafts and forget ---------------------------------------------------------------- */

gboolean
gh_store_conversations_get_draft(GhStoreConversations *self, const gchar *room_id,
                                 gchar **out_draft, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(out_draft != NULL, FALSE);
  *out_draft = NULL;
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  gboolean found = FALSE;
  gint64 id = 0;
  if (!lookup_room(self->store, room_id, &found, &id, error))
    return FALSE;
  return !found || gh_store_get_draft(self->store, id, out_draft, error);
}

gboolean
gh_store_conversations_set_draft(GhStoreConversations *self, const gchar *room_id,
                                 const gchar *draft, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  GhStore *store = self->store;
  if (draft && !*draft)
    draft = NULL;
  gboolean found = FALSE;
  gint64 id = 0;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!lookup_room(store, room_id, &found, &id, error))
    goto fail;
  if (!found && !draft) {
    gh_store_rollback(store);
    return TRUE; /* nothing to clear */
  }
  /* Composing to someone is this account's own intent: a room it creates is
   * accepted (an existing request keeps its state). */
  if ((!found && !gh_store_ensure_conversation(store, GH_STORE_BACKEND_NIP17, room_id,
                                               GH_STORE_REQUEST_ACCEPTED, &id, error)) ||
      !gh_store_set_draft(store, id, draft, error))
    goto fail;
  return gh_store_commit(store, error);
fail:
  gh_store_rollback(store);
  return FALSE;
}

gboolean
gh_store_conversations_forget(GhStoreConversations *self, const gchar *room_id,
                              GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  gint64 id = 0;
  if (!gh_store_find_conversation(self->store, GH_STORE_BACKEND_NIP17, room_id, &id, error) ||
      !gh_store_forget_conversation(self->store, id, error))
    return FALSE;
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (model && g_strcmp0(gh_conversation_store_get_account(model), self->account) == 0)
    gh_conversation_store_remove(model, room_id);
  return TRUE;
}

/* After a committed block: the deleted messages leave the WAL now (the
 * nested forget could not checkpoint) and the room leaves the model. */
static void
unlist_blocked(GhStoreConversations *self, const gchar *room_id)
{
  g_autoptr(GError) checkpoint = NULL;
  if (gh_store_get_transaction_depth(self->store) == 0 &&
      !gh_store_checkpoint(self->store, &checkpoint))
    g_message("Groundhog will clear a blocked conversation from its journal later: %s",
              checkpoint->message);
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (model && g_strcmp0(gh_conversation_store_get_account(model), self->account) == 0)
    gh_conversation_store_remove(model, room_id);
}

gboolean
gh_store_conversations_block_and_forget(GhStoreConversations *self, const gchar *room_id, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  GhStore *store = self->store;
  const guint depth = gh_store_get_transaction_depth(store);
  gint64 id = 0;
  sqlite3_stmt *stmt = NULL;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!gh_store_find_conversation(store, GH_STORE_BACKEND_NIP17, room_id, &id, error))
    goto fail;
  stmt = prepare(store, "UPDATE conversations SET request_state = 2 WHERE id = ?1", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, id));
  if (!step_done(store, stmt, "Blocking a conversation", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  /* Nested: the forget is part of this transaction. */
  if (!gh_store_forget_conversation(store, id, error) || !gh_store_commit(store, error))
    goto fail;
  unlist_blocked(self, room_id);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  /* A failed commit has already rolled back. */
  if (gh_store_get_transaction_depth(store) > depth)
    gh_store_rollback(store);
  return FALSE;
}

gboolean
gh_store_conversations_is_blocked(GhStoreConversations *self, const gchar *room_id,
                                  gboolean *out_blocked, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(out_blocked != NULL, FALSE);
  *out_blocked = FALSE;
  return check_open(self, error) && check_room(self, room_id, error) &&
         room_blocked(self->store, room_id, out_blocked, error);
}

/* ---- Purge ------------------------------------------------------------------------------ */

/* The stored name of a room follows its newest subject (see delegate_admit):
 * after a purge it is the newest remaining message's subject, or none, so a
 * purged message's subject does not outlive it. */
static gboolean
refresh_title(GhStoreConversations *self, gint64 conversation_id, gboolean *out_changed,
              GError **error)
{
  GhStore *store = self->store;
  g_autofree gchar *title = NULL;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT raw_json FROM messages WHERE conversation_id = ?1 AND "
    "instr(raw_json, '\"subject\"') > 0 ORDER BY created_at DESC, backend_msg_id DESC", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  while (!title) {
    if (!step_row(store, stmt, &has_row, "Reading stored subjects", error))
      goto fail;
    if (!has_row)
      break;
    g_autoptr(GhMessage) message = gh_message_new_from_rumor(self->account,
      (const gchar *)sqlite3_column_text(stmt, 0), NULL);
    if (message && gh_message_get_subject(message))
      title = bounded_title(gh_message_get_subject(message));
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  stmt = prepare(store, "UPDATE conversations SET title = ?2 WHERE id = ?1 AND title IS NOT ?2",
                 error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, title));
  if (!step_done(store, stmt, "Renaming a conversation", error))
    goto fail;
  *out_changed = *out_changed || sqlite3_changes(gh_store_get_db(store)) > 0;
  sqlite3_finalize(stmt);
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Whether the room stores a message (before the place (created_at, id) when
 * id is not NULL). */
static gboolean
room_has_messages(GhStore *store, gint64 conversation_id, gint64 created_at, const gchar *id,
                  gboolean *out_exists, GError **error)
{
  *out_exists = FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT EXISTS (SELECT 1 FROM messages WHERE conversation_id = ?1 AND "
    "(?3 IS NULL OR (created_at, backend_msg_id) < (?2, ?3)))", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(sqlite3_bind_int64(stmt, 2, created_at));
  BIND(bind_text(stmt, 3, id));
  gboolean has_row = FALSE;
  gboolean ok = step_row(store, stmt, &has_row, "Reading stored messages", error);
  *out_exists = ok && has_row && sqlite3_column_int(stmt, 0) != 0;
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* A listed room after a purge: unlisted when nothing of it is stored any
 * more (as a restart would), otherwise given its durable state again (read
 * marker, unread count, name) with the history floor it has, and when every
 * loaded message went, its newest remaining page. */
static gboolean
refresh_room(GhStoreConversations *self, GhConversationStore *model, const gchar *room_id,
             GError **error)
{
  GhStore *store = self->store;
  GhConversation *conversation = gh_conversation_store_lookup(model, room_id);
  if (!conversation)
    return TRUE;
  gboolean found = FALSE, has_messages = FALSE;
  gint64 id = 0;
  if (!lookup_room(store, room_id, &found, &id, error) ||
      (found && !room_has_messages(store, id, 0, NULL, &has_messages, error)))
    return FALSE;
  if (!has_messages) {
    gh_conversation_store_remove(model, room_id);
    return TRUE;
  }
  const gchar *floor_ref = NULL;
  gint64 floor_created_at = 0;
  gboolean has_older = gh_conversation_get_floor(conversation, &floor_created_at, &floor_ref);
  /* Restoring the state replaces the room's own copy of the floor id. */
  g_autofree gchar *floor_id = g_strdup(floor_ref);
  if (has_older &&
      !room_has_messages(store, id, floor_created_at, floor_id, &has_older, error))
    return FALSE;
  RoomState state = { 0 };
  if (!load_room_state(store, id, &state, error)) {
    room_state_clear(&state);
    return FALSE;
  }
  GhConversationState restored = conversation_state(&state, has_older, floor_created_at,
                                                    floor_id);
  gh_conversation_store_restore(model, room_id, NULL, &restored);
  room_state_clear(&state);
  guint listed = 0;
  if (g_list_model_get_n_items(G_LIST_MODEL(conversation)) == 0 && has_older &&
      !restore_room(self, model, id, room_id, conversation, GH_STORE_CONVERSATIONS_PAGE_SIZE,
                    FALSE, &listed, error))
    return FALSE;
  return TRUE;
}

/* A read-only store (STORE_CORRUPT) deletes nothing: messages whose time
 * has come only leave the attached model, and retention waits for a store
 * that can be written. The next expiry is the earliest later one stored. */
static gboolean
hide_expired(GhStoreConversations *self, GhStorePurgeStats *stats, GStrvBuilder *ids,
             GError **error)
{
  GhStore *store = self->store;
  const gint64 now = gh_clock_get_unix(gh_store_get_clock(store));
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (model && g_strcmp0(gh_conversation_store_get_account(model), self->account) == 0) {
    g_autoptr(GPtrArray) expired = g_ptr_array_new_with_free_func(g_free);
    g_autoptr(GPtrArray) rooms = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(model)); i++) {
      g_autoptr(GhConversation) conversation = g_list_model_get_item(G_LIST_MODEL(model), i);
      gboolean touched = FALSE;
      for (guint j = 0; j < g_list_model_get_n_items(G_LIST_MODEL(conversation)); j++) {
        g_autoptr(GhMessage) message = g_list_model_get_item(G_LIST_MODEL(conversation), j);
        gint64 expires_at = gh_message_get_expires_at(message);
        if (expires_at > 0 && expires_at <= now) {
          g_ptr_array_add(expired, g_strdup(gh_message_get_rumor_id(message)));
          touched = TRUE;
        }
      }
      if (touched)
        g_ptr_array_add(rooms, g_strdup(gh_conversation_get_room_id(conversation)));
    }
    for (guint i = 0; i < expired->len; i++) {
      gh_conversation_store_remove_message(model, g_ptr_array_index(expired, i));
      g_strv_builder_add(ids, g_ptr_array_index(expired, i));
    }
    stats->n_expired = expired->len;
    for (guint i = 0; i < rooms->len; i++) {
      GhConversation *conversation = gh_conversation_store_lookup(model, g_ptr_array_index(rooms, i));
      if (conversation && g_list_model_get_n_items(G_LIST_MODEL(conversation)) == 0 &&
          !gh_conversation_get_has_older(conversation))
        gh_conversation_store_remove(model, g_ptr_array_index(rooms, i));
    }
  }
  sqlite3_stmt *stmt = prepare(store,
    "SELECT min(expires_at) FROM messages WHERE expires_at > ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, now));
  gboolean has_row = FALSE;
  gboolean ok = step_row(store, stmt, &has_row, "Reading the next expiry", error);
  if (ok && has_row)
    stats->next_expires_at = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_conversations_purge(GhStoreConversations *self, gint64 retention_cutoff,
                             GhStorePurgeStats *out_stats, GStrv *out_purged, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (out_stats)
    *out_stats = (GhStorePurgeStats){ 0 };
  if (out_purged)
    *out_purged = NULL;
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  GhStorePurgeStats stats = { 0 };
  if (gh_store_is_read_only(store)) {
    g_autoptr(GStrvBuilder) hidden = g_strv_builder_new();
    if (!hide_expired(self, &stats, hidden, error))
      return FALSE;
    if (out_stats)
      *out_stats = stats;
    if (out_purged)
      *out_purged = g_strv_builder_end(hidden);
    return TRUE;
  }
  g_autoptr(GPtrArray) purged = NULL;
  if (!gh_store_purge_full(store, retention_cutoff, &purged, &stats, error))
    return FALSE;
  if (out_stats)
    *out_stats = stats;

  /* The NIP-17 rooms it touched (keys borrowed from purged), in order. */
  g_autoptr(GPtrArray) rooms = g_ptr_array_new();
  g_autoptr(GHashTable) seen_rooms = g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(GStrvBuilder) ids = g_strv_builder_new();
  for (guint i = 0; i < purged->len; i++) {
    GhStorePurgedMessage *message = g_ptr_array_index(purged, i);
    if (message->backend != GH_STORE_BACKEND_NIP17)
      continue;
    g_strv_builder_add(ids, message->backend_msg_id);
    if (g_hash_table_add(seen_rooms, message->backend_key))
      g_ptr_array_add(rooms, message->backend_key);
  }

  /* The purge is committed: what follows only brings names and the model in
   * line, so a failure there is reported but the model still drops every
   * purged message. */
  g_autoptr(GError) sync_error = NULL;
  gboolean renamed = FALSE;
  if (rooms->len > 0 && gh_store_begin(store, &sync_error)) {
    gboolean ok = TRUE;
    for (guint i = 0; ok && i < rooms->len; i++) {
      gboolean found = FALSE;
      gint64 id = 0;
      ok = lookup_room(store, g_ptr_array_index(rooms, i), &found, &id, &sync_error) &&
           (!found || refresh_title(self, id, &renamed, &sync_error));
    }
    if (!ok)
      gh_store_rollback(store);
    else if (!gh_store_commit(store, &sync_error))
      renamed = FALSE;
  }
  /* A new name must not leave the purged one in the WAL; the purge's own
   * checkpoint decided that this is not too soon. */
  if (renamed && stats.checkpointed && !sync_error)
    (void)gh_store_checkpoint(store, NULL);

  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (model && g_strcmp0(gh_conversation_store_get_account(model), self->account) == 0) {
    for (guint i = 0; i < purged->len; i++) {
      GhStorePurgedMessage *message = g_ptr_array_index(purged, i);
      if (message->backend == GH_STORE_BACKEND_NIP17)
        gh_conversation_store_remove_message(model, message->backend_msg_id);
    }
    for (guint i = 0; i < rooms->len; i++) {
      g_autoptr(GError) room_error = NULL;
      if (!refresh_room(self, model, g_ptr_array_index(rooms, i), &room_error) && !sync_error)
        sync_error = g_steal_pointer(&room_error);
    }
  }
  if (sync_error)
    g_warning("Groundhog could not update conversations after a purge: %s",
              sync_error->message);
  if (out_purged)
    *out_purged = g_strv_builder_end(ids);
  return TRUE;
}

/* ---- Pins and timer changes (nostrc-qp24.86, qp24.83) ----------------------------------- */

/* The attached model's listing of room_id, or NULL. */
static GhConversation *
listed_room(GhStoreConversations *self, GhConversationStore **out_model, const gchar *room_id)
{
  *out_model = g_weak_ref_get(&self->model);
  if (!*out_model || g_strcmp0(gh_conversation_store_get_account(*out_model), self->account) != 0)
    return NULL;
  return gh_conversation_store_lookup(*out_model, room_id);
}

static void
pin_listed(GhStoreConversations *self, const gchar *room_id, gint64 rank)
{
  g_autoptr(GhConversationStore) model = NULL;
  GhConversation *conversation = listed_room(self, &model, room_id);
  if (conversation)
    gh_conversation_store_pin(model, conversation, rank);
}

gboolean
gh_store_conversations_set_pinned(GhStoreConversations *self, const gchar *room_id,
                                  gboolean pinned, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  GhStore *store = self->store;
  gboolean found = FALSE;
  gint64 id = 0, rank = 0;
  sqlite3_stmt *stmt = NULL;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!lookup_room(store, room_id, &found, &id, error))
    goto fail;
  if (!found && !pinned) {
    gh_store_rollback(store);
    return TRUE; /* nothing stored, nothing pinned */
  }
  /* Pinning is the account's own choice: a room it creates is accepted. A new
   * pin goes after the others; a pinned room keeps its place. */
  if ((!found && !gh_store_ensure_conversation(store, GH_STORE_BACKEND_NIP17, room_id,
                                               GH_STORE_REQUEST_ACCEPTED, &id, error)))
    goto fail;
  stmt = prepare(store, pinned
    ? "UPDATE conversations SET pinned_rank = COALESCE(pinned_rank, (SELECT "
      "COALESCE(MAX(pinned_rank), 0) + 1 FROM conversations WHERE backend = 1)) WHERE id = ?1 "
      "RETURNING pinned_rank"
    : "UPDATE conversations SET pinned_rank = NULL WHERE id = ?1 RETURNING 0", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, id));
  gboolean has_row = FALSE;
  if (!step_row(store, stmt, &has_row, pinned ? "Pinning a conversation"
                                              : "Unpinning a conversation", error))
    goto fail;
  rank = has_row ? MAX(sqlite3_column_int64(stmt, 0), 0) : 0;
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!gh_store_commit(store, error))
    return FALSE;
  pin_listed(self, room_id, rank);
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  gh_store_rollback(store);
  return FALSE;
}

gboolean
gh_store_conversations_sync_timer(GhStoreConversations *self, const gchar *room_id,
                                  GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  gboolean found = FALSE;
  gint64 id = 0, seconds = 0, changed_at = 0;
  if (!lookup_room(self->store, room_id, &found, &id, error))
    return FALSE;
  if (found && !gh_store_get_timer_change(self->store, id, &seconds, &changed_at, error))
    return FALSE;
  g_autoptr(GhConversationStore) model = NULL;
  GhConversation *conversation = listed_room(self, &model, room_id);
  if (conversation)
    gh_conversation_set_timer_change(conversation, seconds, changed_at);
  return TRUE;
}

/* ---- Notification state ---------------------------------------------------------------- */

gboolean
gh_store_conversations_get_notify_state(GhStoreConversations *self, const gchar *room_id,
                                        GhStoreNotifyState *out_state, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(out_state != NULL, FALSE);
  *out_state = (GhStoreNotifyState){ 0 };
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  GhStore *store = self->store;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT muted_until, request_state FROM conversations "
    "WHERE backend = 1 AND backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, room_id));
  gboolean found = FALSE;
  gboolean ok = step_row(store, stmt, &found, "Reading a conversation's notification state",
                         error);
  if (ok && found) {
    out_state->muted_until = MAX(sqlite3_column_int64(stmt, 0), 0);
    out_state->blocked = sqlite3_column_int64(stmt, 1) == GH_STORE_REQUEST_BLOCKED;
  }
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_conversations_set_muted_until(GhStoreConversations *self, const gchar *room_id,
                                       gint64 muted_until, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  if (muted_until < 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "A mute cannot end before 1970");
    return FALSE;
  }
  GhStore *store = self->store;
  gboolean found = FALSE;
  gint64 id = 0;
  if (!lookup_room(store, room_id, &found, &id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "The conversation is not stored");
    return FALSE;
  }
  sqlite3_stmt *stmt = prepare(store, "UPDATE conversations SET muted_until = ?2 WHERE id = ?1",
                               error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, id));
  BIND(sqlite3_bind_int64(stmt, 2, muted_until));
  gboolean ok = step_done(store, stmt, "Muting a conversation", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Blocked, or back from blocked: accepted when @accept (the user starts a
 * conversation with them, G18), else a request again unless the account
 * wrote in the room (unblocking alone is not accepting, charter PT-8). */
static gboolean
update_block(GhStore *store, gint64 id, gboolean blocked, gboolean accept, GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
    "UPDATE conversations SET request_state = CASE WHEN ?2 THEN 2 WHEN ?3 THEN 0 "
    "  WHEN EXISTS (SELECT 1 FROM messages WHERE conversation_id = ?1 AND direction = 1) "
    "  THEN 0 ELSE 1 END WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, id));
  BIND(sqlite3_bind_int(stmt, 2, blocked));
  BIND(sqlite3_bind_int(stmt, 3, accept));
  gboolean ok = step_done(store, stmt, blocked ? "Blocking a conversation"
                                                : "Unblocking a conversation", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Sets or lifts the block of a stored room and brings the attached model in
 * line: a blocked room leaves it, an unblocked one is listed again with its
 * newest page (none when nothing of it is stored, e.g. after Block on a
 * request, which forgot it). */
static gboolean
change_block(GhStoreConversations *self, const gchar *room_id, gboolean blocked,
             gboolean accept, GError **error)
{
  if (!check_open(self, error) || !check_room(self, room_id, error))
    return FALSE;
  gboolean found = FALSE;
  gint64 id = 0;
  if (!lookup_room(self->store, room_id, &found, &id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "The conversation is not stored");
    return FALSE;
  }
  if (!update_block(self->store, id, blocked, accept, error))
    return FALSE;

  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  if (!model || g_strcmp0(gh_conversation_store_get_account(model), self->account) != 0)
    return TRUE;
  if (blocked) {
    gh_conversation_store_remove(model, room_id);
    return TRUE;
  }
  guint listed = 0;
  return restore_room(self, model, id, room_id, NULL, GH_STORE_CONVERSATIONS_PAGE_SIZE, FALSE, &listed,
                      error);
}

static gboolean
unblock_room(GhStoreConversations *self, const gchar *room_id, gboolean accept, GError **error)
{
  return change_block(self, room_id, FALSE, accept, error);
}

gboolean
gh_store_conversations_set_blocked(GhStoreConversations *self, const gchar *room_id,
                                   gboolean blocked, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  return change_block(self, room_id, !!blocked, FALSE, error);
}

void
gh_store_blocked_room_free(GhStoreBlockedRoom *room)
{
  if (!room)
    return;
  g_free(room->room_id);
  g_free(room);
}

GPtrArray *
gh_store_conversations_list_blocked(GhStoreConversations *self, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), NULL);
  if (!check_open(self, error))
    return NULL;
  GhStore *store = self->store;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT backend_key, MAX(last_activity, forgotten_before) AS activity, "
    "  EXISTS (SELECT 1 FROM messages m WHERE m.conversation_id = c.id) "
    "FROM conversations c WHERE backend = 1 AND request_state = 2 "
    "ORDER BY activity DESC, backend_key", error);
  if (!stmt)
    return NULL;
  g_autoptr(GPtrArray) rooms =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_blocked_room_free);
  gboolean has_row = FALSE;
  while (TRUE) {
    if (!step_row(store, stmt, &has_row, "Listing blocked conversations", error)) {
      sqlite3_finalize(stmt);
      return NULL;
    }
    if (!has_row)
      break;
    GhStoreBlockedRoom *room = g_new0(GhStoreBlockedRoom, 1);
    room->room_id = column_text(stmt, 0);
    room->last_activity = sqlite3_column_int64(stmt, 1);
    room->has_messages = sqlite3_column_int(stmt, 2) != 0;
    g_ptr_array_add(rooms, room);
  }
  sqlite3_finalize(stmt);
  return g_steal_pointer(&rooms);
}

/* ---- Legacy seen file ------------------------------------------------------------------ */

gchar *
gh_store_conversations_legacy_seen_path(const gchar *state_dir, const gchar *account_pubkey)
{
  g_return_val_if_fail(account_pubkey != NULL, NULL);
  g_autofree gchar *dir = state_dir ? g_strdup(state_dir)
                                    : g_build_filename(g_get_user_state_dir(), "groundhog",
                                                       "nip17", NULL);
  g_autofree gchar *name = g_strconcat(account_pubkey, ".seen", NULL);
  return g_build_filename(dir, name, NULL);
}

/* Reads a private regular file without following a symlink. *out_missing
 * when there is none. */
static gboolean
read_seen_file(const gchar *path, gchar **out_contents, gsize *out_length,
               gboolean *out_missing, GError **error)
{
  *out_contents = NULL;
  *out_length = 0;
  *out_missing = FALSE;
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
  if (fd < 0) {
    int saved = errno;
    if (saved == ENOENT) {
      *out_missing = TRUE;
      return TRUE;
    }
    g_set_error(error, GH_STORE_ERROR,
                saved == ELOOP ? GH_STORE_ERROR_PERMISSIONS : GH_STORE_ERROR_FAILED,
                "Cannot open the legacy seen file: %s", g_strerror(saved));
    return FALSE;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid()) {
    close(fd);
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS,
                        "The legacy seen file is not a regular file of this user");
    return FALSE;
  }
  if (st.st_size > GH_STORE_CONVERSATIONS_MAX_SEEN_FILE) {
    close(fd);
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The legacy seen file is too large");
    return FALSE;
  }
  GByteArray *bytes = g_byte_array_sized_new((guint)st.st_size + 1);
  guint8 buffer[16384];
  gboolean ok = TRUE;
  while (ok) {
    ssize_t n = read(fd, buffer, sizeof buffer);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                  "Cannot read the legacy seen file: %s", g_strerror(errno));
      ok = FALSE;
    } else if (n == 0) {
      break;
    } else if (bytes->len + (gsize)n > GH_STORE_CONVERSATIONS_MAX_SEEN_FILE) {
      g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                          "The legacy seen file is too large");
      ok = FALSE;
    } else {
      g_byte_array_append(bytes, buffer, (guint)n);
    }
  }
  close(fd);
  if (!ok) {
    g_byte_array_unref(bytes);
    return FALSE;
  }
  *out_length = bytes->len;
  g_byte_array_append(bytes, (const guint8 *)"", 1);
  *out_contents = (gchar *)g_byte_array_free(bytes, FALSE);
  return TRUE;
}

static gboolean
seen_file_invalid(GError **error, const gchar *reason)
{
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
              "Not a NIP-17 seen file: %s", reason);
  return FALSE;
}

/* Checks the header and every line; *out_entries is where the entries start
 * and *out_end where the complete lines end (a torn final line is left out). */
static gboolean
parse_seen_file(GhStoreConversations *self, const gchar *contents, gsize length,
                gsize *out_entries, gsize *out_end, GError **error)
{
  const gsize magic = strlen(SEEN_MAGIC);
  const gsize header = magic + 64 + 1;
  if (memchr(contents, '\0', length))
    return seen_file_invalid(error, "it holds a NUL byte");
  if (length < header || memcmp(contents, SEEN_MAGIC, magic) != 0 ||
      !is_hex64(contents + magic) || contents[magic + 64] != '\n')
    return seen_file_invalid(error, "no seen-set header");
  if (memcmp(contents + magic, self->account, 64) != 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FOREIGN,
                        "The legacy seen file belongs to another account");
    return FALSE;
  }
  gsize end = header;
  while (end < length) {
    const gchar *line = contents + end;
    const gchar *newline = memchr(line, '\n', length - end);
    if (!newline)
      break; /* a torn final append, ignored like GhNip17Seen did */
    if (newline - line != SEEN_LINE - 1 ||
        (line[0] != 'w' && line[0] != 'r' && line[0] != 'x') || line[1] != ' ' ||
        !is_hex64(line + 2))
      return seen_file_invalid(error, "a malformed entry");
    end += SEEN_LINE;
  }
  *out_entries = header;
  *out_end = end;
  return TRUE;
}

gboolean
gh_store_conversations_import_seen_file(GhStoreConversations *self, const gchar *path,
                                        GhStoreSeenImport *out_stats, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_CONVERSATIONS(self), FALSE);
  g_return_val_if_fail(path != NULL, FALSE);
  GhStoreSeenImport stats = { 0 };
  if (out_stats)
    *out_stats = stats;
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  g_autofree gchar *contents = NULL;
  gsize length = 0, entries = 0, end = 0;
  gboolean missing = FALSE;
  if (!read_seen_file(path, &contents, &length, &missing, error))
    return FALSE;
  if (missing)
    return TRUE;
  if (!parse_seen_file(self, contents, length, &entries, &end, error))
    return FALSE;

  const gint64 now = gh_clock_get_unix(gh_store_get_clock(store));
  sqlite3_stmt *stmt = NULL;
  if (!gh_store_begin(store, error))
    return FALSE;
  stmt = prepare(store,
    "INSERT INTO seen (ns, id, first_seen) VALUES (?1, ?2, ?3) "
    "ON CONFLICT (ns, id) DO NOTHING", error);
  if (!stmt)
    goto fail;
  for (gsize off = entries; off < end; off += SEEN_LINE) {
    const gchar *line = contents + off;
    /* Only the rejected namespace: "w"/"r" keys of a memory-only inbox
     * would hide messages no store holds (see the header). */
    if (line[0] != 'x') {
      stats.dropped++;
      continue;
    }
    sqlite3_reset(stmt);
    BIND(sqlite3_bind_int64(stmt, 1, GH_STORE_SEEN_REJECTED_WRAP));
    BIND(sqlite3_bind_text(stmt, 2, line + 2, 64, SQLITE_STATIC));
    BIND(sqlite3_bind_int64(stmt, 3, now));
    if (!step_done(store, stmt, "Importing a rejected wrap id", error))
      goto fail;
    if (sqlite3_changes(gh_store_get_db(store)) > 0)
      stats.rejected++;
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!gh_store_commit(store, error))
    return FALSE;
  if (out_stats)
    *out_stats = stats;
  /* Imported: the plaintext file goes. A failure is reported; the keys stay
   * imported and a retry deletes the file. */
  if (unlink(path) != 0 && errno != ENOENT) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Imported the legacy seen file but could not delete it: %s",
                g_strerror(errno));
    return FALSE;
  }
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  gh_store_rollback(store);
  return FALSE;
}

/* ---- Object ------------------------------------------------------------------------------ */

GhStoreConversations *
gh_store_conversations_new(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  GhStoreConversations *self = g_object_new(GH_TYPE_STORE_CONVERSATIONS, NULL);
  self->store = store;
  self->account = g_strdup(gh_store_get_account_pubkey(store));
  return self;
}

void
gh_store_conversations_close(GhStoreConversations *self)
{
  g_return_if_fail(GH_IS_STORE_CONVERSATIONS(self));
  self->store = NULL;
}

static void
gh_store_conversations_finalize(GObject *object)
{
  GhStoreConversations *self = GH_STORE_CONVERSATIONS(object);
  g_weak_ref_clear(&self->model);
  g_free(self->account);
  G_OBJECT_CLASS(gh_store_conversations_parent_class)->finalize(object);
}

static void
gh_store_conversations_class_init(GhStoreConversationsClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_store_conversations_finalize;
}

static void
gh_store_conversations_init(GhStoreConversations *self)
{
  g_weak_ref_init(&self->model, NULL);
}
