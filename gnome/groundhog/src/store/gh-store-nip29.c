#include "gh-store-nip29.h"

#include "gh-conversation-private.h"

#include <string.h>

#include <sqlite3.h>

/* The service's group record: four relay-signed snapshots (a 39002 of a big
 * group is the largest part), the timeline ring and a little state. */
#define MAX_STATE_JSON (8 * 1024 * 1024)
#define MAX_PAGE 1000

struct _GhStoreNip29 {
  GObject parent_instance;
  GhStore *store;  /* borrowed; NULL once closed */
  gchar *account;
  GWeakRef model;  /* the GhConversationStore it is the NIP-29 delegate of */
};

G_DEFINE_FINAL_TYPE(GhStoreNip29, gh_store_nip29, G_TYPE_OBJECT)

/* ---- Statements -------------------------------------------------------------- */

/* As in gh-store-conversations.c: nothing runs as if a transaction SQLite
 * abandoned (disk full, I/O error) were still open. */
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
  gh_store_set_sqlite_error(store, rc, "Preparing a group statement", error);
  return NULL;
}

#define BIND(expr)                                                              \
  G_STMT_START {                                                                \
    int bind_rc_ = (expr);                                                      \
    if (bind_rc_ != SQLITE_OK) {                                                \
      gh_store_set_sqlite_error(store, bind_rc_, "Binding a group value", error); \
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

static gboolean
invalid(GError **error, const gchar *message)
{
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, message);
  return FALSE;
}

static gboolean
lower_hex(const gchar *s, gsize length)
{
  if (!s || strlen(s) != length)
    return FALSE;
  for (gsize i = 0; i < length; i++)
    if (!g_ascii_isdigit(s[i]) && (s[i] < 'a' || s[i] > 'f'))
      return FALSE;
  return TRUE;
}

static gboolean lookup_room(GhStore *store, const gchar *room_id, gboolean *found, gint64 *id,
                            GError **error);
static gboolean update_read_state(GhStore *store, gint64 conversation_id, gint64 row,
                                  gboolean force, gint64 *out_unread, GError **error);

/* ---- Groups ------------------------------------------------------------------- */

void
gh_store_nip29_group_free(GhStoreNip29Group *group)
{
  if (!group)
    return;
  g_free(group->relay_url);
  g_free(group->group_id);
  g_free(group->relay_pubkey);
  g_free(group->state_json);
  g_free(group->title);
  g_free(group);
}

static gboolean
check_group(const gchar *relay_url, const gchar *group_id, const gchar *relay_pubkey,
            const gchar *state_json, const gchar *title, GError **error)
{
  g_autofree gchar *room = relay_url && group_id ? gh_message_nip29_room_id(relay_url, group_id)
                                                 : NULL;
  if (!room || !gh_message_nip29_room_split(room, NULL, NULL) ||
      strlen(room) > GH_STORE_MAX_BACKEND_KEY)
    return invalid(error, "A normalized relay URL and a NIP-29 group id are required");
  if (!relay_pubkey || (*relay_pubkey && !lower_hex(relay_pubkey, 64)))
    return invalid(error, "The relay key must be 64 lowercase hex or empty");
  if (state_json && (strnlen(state_json, MAX_STATE_JSON + 1) > MAX_STATE_JSON ||
                     !g_utf8_validate(state_json, -1, NULL)))
    return invalid(error, "The group record is too large or not UTF-8");
  if (title && (strnlen(title, GH_STORE_MAX_TITLE + 1) > GH_STORE_MAX_TITLE ||
                !g_utf8_validate(title, -1, NULL)))
    return invalid(error, "The group name is too long or not UTF-8");
  return TRUE;
}

gboolean
gh_store_nip29_save_group(GhStore *store, const gchar *relay_url, const gchar *group_id,
                          const gchar *relay_pubkey, const gchar *state_json,
                          const gchar *title, gint64 *out_conversation_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_group(relay_url, group_id, relay_pubkey, state_json, title, error))
    return FALSE;
  g_autofree gchar *key = gh_message_nip29_room_id(relay_url, group_id);
  gint64 id = 0;
  sqlite3_stmt *stmt = NULL;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!gh_store_ensure_conversation(store, GH_STORE_BACKEND_NIP29, key,
                                    GH_STORE_REQUEST_ACCEPTED, &id, error))
    goto fail;
  if (title) {
    stmt = prepare(store, "UPDATE conversations SET title = ?1 WHERE id = ?2", error);
    if (!stmt)
      goto fail;
    BIND(bind_text(stmt, 1, *title ? title : NULL));
    BIND(sqlite3_bind_int64(stmt, 2, id));
    if (!step_done(store, stmt, "Naming a group", error))
      goto fail;
    g_clear_pointer(&stmt, sqlite3_finalize);
  }
  stmt = prepare(store,
    "INSERT INTO nip29_groups (conversation_id, relay_url, group_id, relay_pubkey, "
    "snapshot_json) VALUES (?1, ?2, ?3, ?4, ?5) ON CONFLICT (conversation_id) DO UPDATE SET "
    "relay_url = excluded.relay_url, group_id = excluded.group_id, "
    "relay_pubkey = excluded.relay_pubkey, snapshot_json = excluded.snapshot_json", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, id));
  BIND(bind_text(stmt, 2, relay_url));
  BIND(bind_text(stmt, 3, group_id));
  BIND(bind_text(stmt, 4, relay_pubkey));
  BIND(bind_text(stmt, 5, state_json));
  if (!step_done(store, stmt, "Saving a group", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!gh_store_commit(store, error))
    return FALSE;
  if (out_conversation_id)
    *out_conversation_id = id;
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  gh_store_rollback(store);
  return FALSE;
}

GPtrArray *
gh_store_nip29_list_groups(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT g.conversation_id, g.relay_url, g.group_id, g.relay_pubkey, g.snapshot_json, "
    "c.title FROM nip29_groups g JOIN conversations c ON c.id = g.conversation_id "
    "ORDER BY g.conversation_id", error);
  if (!stmt)
    return NULL;
  g_autoptr(GPtrArray) groups =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_nip29_group_free);
  while (TRUE) {
    gboolean has_row = FALSE;
    if (!step_row(store, stmt, &has_row, "Listing groups", error)) {
      sqlite3_finalize(stmt);
      return NULL;
    }
    if (!has_row)
      break;
    GhStoreNip29Group *group = g_new0(GhStoreNip29Group, 1);
    group->conversation_id = sqlite3_column_int64(stmt, 0);
    group->relay_url = column_text(stmt, 1);
    group->group_id = column_text(stmt, 2);
    group->relay_pubkey = column_text(stmt, 3);
    group->state_json = column_text(stmt, 4);
    group->title = column_text(stmt, 5);
    g_ptr_array_add(groups, group);
  }
  sqlite3_finalize(stmt);
  return g_steal_pointer(&groups);
}

gboolean
gh_store_nip29_delete_group(GhStore *store, gint64 conversation_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store, "DELETE FROM nip29_groups WHERE conversation_id = ?1",
                               error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  if (!step_done(store, stmt, "Deleting a group", error))
    goto fail;
  sqlite3_finalize(stmt);
  if (sqlite3_changes(gh_store_get_db(store)) == 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such group");
    return FALSE;
  }
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_nip29_get_room(GhStore *store, gint64 conversation_id, gchar **out_relay_url,
                        gchar **out_group_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  gchar *key = NULL;
  gboolean has_row = FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT backend_key FROM conversations WHERE id = ?1 AND backend = 2", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  if (!step_row(store, stmt, &has_row, "Looking up a group", error))
    goto fail;
  key = has_row ? column_text(stmt, 0) : NULL;
  sqlite3_finalize(stmt);
  gboolean found = key && gh_message_nip29_room_split(key, out_relay_url, out_group_id);
  g_free(key);
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such group");
    return FALSE;
  }
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_nip29_enqueue_operation(GhStore *store, gint64 conversation_id, const gchar *op_id,
                                 const gchar *unsigned_json, gint64 *out_outbox_id,
                                 GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!lower_hex(op_id, 32))
    return invalid(error, "The operation id must be 32 lowercase hex");
  if (!unsigned_json || strnlen(unsigned_json, GH_STORE_MAX_EVENT_JSON + 1) >
                          GH_STORE_MAX_EVENT_JSON || !g_utf8_validate(unsigned_json, -1, NULL))
    return invalid(error, "The operation's event is missing, too large or not UTF-8");
  if (gh_store_is_read_only(store)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                        "The message storage is read-only");
    return FALSE;
  }
  sqlite3_stmt *stmt = NULL;
  gboolean has_row = FALSE;
  gint64 id = 0;
  if (!gh_store_begin(store, error))
    return FALSE;
  /* Idempotent on op_id, as gh_store_enqueue(). */
  stmt = prepare(store, "SELECT id FROM outbox WHERE op_id = ?1", error);
  if (!stmt)
    goto fail;
  BIND(bind_text(stmt, 1, op_id));
  if (!step_row(store, stmt, &has_row, "Looking up an operation", error))
    goto fail;
  if (has_row) {
    id = sqlite3_column_int64(stmt, 0);
    g_clear_pointer(&stmt, sqlite3_finalize);
    gh_store_rollback(store);
    if (out_outbox_id)
      *out_outbox_id = id;
    return TRUE;
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  stmt = prepare(store, "SELECT backend FROM conversations WHERE id = ?1", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  if (!step_row(store, stmt, &has_row, "Looking up a group", error))
    goto fail;
  if (!has_row || sqlite3_column_int64(stmt, 0) != GH_STORE_BACKEND_NIP29) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such group");
    goto fail;
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  stmt = prepare(store,
    "INSERT INTO outbox (conversation_id, op_id, backend, state, rumor_json, created_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, op_id));
  BIND(sqlite3_bind_int64(stmt, 3, GH_STORE_BACKEND_NIP29));
  BIND(sqlite3_bind_int64(stmt, 4, GH_STORE_OUTBOX_QUEUED));
  BIND(bind_text(stmt, 5, unsigned_json));
  BIND(sqlite3_bind_int64(stmt, 6, gh_clock_get_unix(gh_store_get_clock(store))));
  if (!step_done(store, stmt, "Queueing a group operation", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  id = sqlite3_last_insert_rowid(gh_store_get_db(store));
  if (!gh_store_commit(store, error))
    return FALSE;
  if (out_outbox_id)
    *out_outbox_id = id;
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  gh_store_rollback(store);
  return FALSE;
}

/* ---- Read state ---------------------------------------------------------------- */

static gboolean
check_open(GhStoreNip29 *self, GError **error)
{
  if (self->store)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                      "The group store is closed");
  return FALSE;
}

static gboolean
lookup_room(GhStore *store, const gchar *room_id, gboolean *found, gint64 *id, GError **error)
{
  *found = FALSE;
  *id = 0;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id FROM conversations WHERE backend = 2 AND backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, room_id));
  gboolean ok = step_row(store, stmt, found, "Looking up a group room", error);
  if (ok && *found)
    *id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* The stored row of message_id in the room; 0 when not stored. */
static gboolean
message_row(GhStore *store, gint64 conversation_id, const gchar *message_id, gint64 *row,
            GError **error)
{
  *row = 0;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id FROM messages WHERE conversation_id = ?1 AND backend_msg_id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, message_id));
  gboolean has_row = FALSE;
  gboolean ok = step_row(store, stmt, &has_row, "Looking up a group message", error);
  if (ok && has_row)
    *row = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Moves the read marker to row when it sorts after the current one (or
 * always, when force); then recomputes unread_count as the messages from
 * others after the marker. A marker whose row is gone counts as none. */
static gboolean
update_read_state(GhStore *store, gint64 conversation_id, gint64 row, gboolean force,
                  gint64 *out_unread, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  if (row > 0) {
    stmt = prepare(store,
      "UPDATE conversations SET last_read_msg = ?2 WHERE id = ?1 AND (?3 OR "
      "last_read_msg IS NULL OR NOT EXISTS (SELECT 1 FROM messages r WHERE r.id = last_read_msg) "
      "OR EXISTS (SELECT 1 FROM messages r, messages m WHERE r.id = last_read_msg AND m.id = ?2 "
      "AND (m.created_at > r.created_at OR (m.created_at = r.created_at AND "
      "m.backend_msg_id > r.backend_msg_id))))", error);
    if (!stmt)
      return FALSE;
    BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
    BIND(sqlite3_bind_int64(stmt, 2, row));
    BIND(sqlite3_bind_int(stmt, 3, force));
    if (!step_done(store, stmt, "Moving a group's read marker", error))
      goto fail;
    g_clear_pointer(&stmt, sqlite3_finalize);
  }
  stmt = prepare(store,
    "UPDATE conversations SET unread_count = (SELECT count(*) FROM messages m "
    "WHERE m.conversation_id = ?1 AND m.direction = 0 AND NOT EXISTS ("
    "SELECT 1 FROM conversations c JOIN messages r ON r.id = c.last_read_msg WHERE c.id = ?1 "
    "AND (m.created_at < r.created_at OR (m.created_at = r.created_at AND "
    "m.backend_msg_id <= r.backend_msg_id)))) WHERE id = ?1 RETURNING unread_count", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  if (!step_row(store, stmt, &has_row, "Counting a group's unread messages", error))
    goto fail;
  if (out_unread)
    *out_unread = has_row ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_nip29_delete_message(GhStore *store, const gchar *room_id, const gchar *event_id,
                              GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!lower_hex(event_id, 64))
    return invalid(error, "A group event id is 64 lowercase hex");
  gboolean found = FALSE;
  gint64 conversation_id = 0;
  sqlite3_stmt *stmt = NULL;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!lookup_room(store, room_id, &found, &conversation_id, error))
    goto fail;
  if (!found) {
    gh_store_rollback(store);
    return TRUE;
  }
  stmt = prepare(store,
    "DELETE FROM messages WHERE conversation_id = ?1 AND backend_msg_id = ?2", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, event_id));
  if (!step_done(store, stmt, "Deleting a group message", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!update_read_state(store, conversation_id, 0, FALSE, NULL, error))
    goto fail;
  return gh_store_commit(store, error);
fail:
  sqlite3_finalize(stmt);
  gh_store_rollback(store);
  return FALSE;
}

/* ---- The delegate ----------------------------------------------------------------- */

static gboolean
delegate_has_wrap(gpointer data, const gchar *wrap_id)
{
  (void)data;
  (void)wrap_id;
  return FALSE; /* group events have no wraps */
}

static gboolean
delegate_has_rumor(gpointer data, const gchar *event_id)
{
  GhStoreNip29 *self = data;
  gboolean seen = FALSE;
  g_autoptr(GError) error = NULL;
  if (!self->store || !event_id)
    return FALSE;
  if (gh_store_seen_contains(self->store, GH_STORE_SEEN_NIP29_EVENT, event_id, &seen, &error))
    return seen;
  if (!g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID))
    g_warning("Groundhog could not read the group seen set: %s", error->message);
  return FALSE;
}

/* T-admit of one group event (see the header). */
static gboolean
delegate_admit(gpointer data, GhMessage *message, const gchar *wrap_id,
               GhConversationCommit *commit, GError **error)
{
  GhStoreNip29 *self = data;
  (void)wrap_id; /* the event is its own carrier; nothing else to record */
  if (!check_open(self, error))
    return FALSE;
  if (!gh_message_is_nip29(message) ||
      g_strcmp0(gh_message_get_account(message), self->account) != 0)
    return invalid(error, "Not a group message of this account");
  GhStore *store = self->store;
  const gboolean own = gh_message_is_self(message);
  const gchar *room_id = gh_message_get_room_id(message);
  GhStoreMessage m = {
    .backend = GH_STORE_BACKEND_NIP29,
    .backend_key = room_id,
    .backend_msg_id = gh_message_get_rumor_id(message),
    .sender_pubkey = gh_message_get_sender(message),
    .kind = gh_message_get_kind(message),
    .created_at = gh_message_get_created_at(message),
    .direction = own ? GH_STORE_DIRECTION_OUT : GH_STORE_DIRECTION_IN,
    .body = gh_message_get_content(message),
    .raw_json = gh_message_get_rumor_json(message),
    .expires_at = gh_message_get_expires_at(message),
    .unread = !own,
    .request_state = GH_STORE_REQUEST_ACCEPTED,
  };
  GhStoreAdmitResult result = GH_STORE_ADMIT_DUPLICATE;
  gint64 row = 0, conversation_id = 0;
  gboolean found = FALSE;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!gh_store_admit(store, &m, &result, &row, error) ||
      !lookup_room(store, room_id, &found, &conversation_id, error))
    goto fail;
  /* A duplicate may still be stored (the outbox stored the echo first). */
  if (found && result == GH_STORE_ADMIT_DUPLICATE &&
      !message_row(store, conversation_id, m.backend_msg_id, &row, error))
    goto fail;
  if (found && row > 0) {
    if (!update_read_state(store, conversation_id, own ? row : 0, FALSE, &commit->unread, error))
      goto fail;
  } else {
    commit->hidden = TRUE; /* seen only: expired, forgotten room, purged */
  }
  return gh_store_commit(store, error);
fail:
  gh_store_rollback(store);
  return FALSE;
}

static gboolean
delegate_mark_read(gpointer data, GhConversation *conversation, GhMessage *last_read,
                   GError **error)
{
  GhStoreNip29 *self = data;
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  gboolean found = FALSE;
  gint64 conversation_id = 0, row = 0;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!lookup_room(store, gh_conversation_get_room_id(conversation), &found, &conversation_id,
                   error))
    goto fail;
  if (!found) {
    gh_store_rollback(store);
    return TRUE; /* a memory-only room */
  }
  if (!message_row(store, conversation_id, gh_message_get_rumor_id(last_read), &row, error) ||
      (row > 0 && !update_read_state(store, conversation_id, row, TRUE, NULL, error)))
    goto fail;
  return gh_store_commit(store, error);
fail:
  gh_store_rollback(store);
  return FALSE;
}

static const GhConversationDelegate group_delegate = {
  .has_wrap = delegate_has_wrap,
  .has_rumor = delegate_has_rumor,
  .admit = delegate_admit,
  .mark_read = delegate_mark_read,
};

/* ---- Restore ------------------------------------------------------------------------ */

typedef struct {
  gboolean has;
  gint64 created_at;
  gchar *id;
} Place;

/* Lists a page of a stored group room: its newest messages (conversation
 * NULL) or those before the listed room's floor, as gh-store-conversations.c
 * does for NIP-17 rooms. */
static gboolean
restore_room(GhStoreNip29 *self, GhConversationStore *model, gint64 conversation_id,
             const gchar *room_id, GhConversation *conversation, guint limit,
             guint *out_listed, GError **error)
{
  GhStore *store = self->store;
  const gint64 now = gh_clock_get_unix(gh_store_get_clock(store));
  g_autofree gchar *relay_url = NULL;
  Place cursor = { 0 }, marker = { 0 };
  g_autofree gchar *title = NULL;
  gint64 unread = 0;
  g_autoptr(GPtrArray) messages = g_ptr_array_new_with_free_func(g_object_unref);
  sqlite3_stmt *stmt = NULL;
  *out_listed = 0;
  if (!gh_message_nip29_room_split(room_id, &relay_url, NULL))
    return TRUE; /* not a room this build can list */
  if (conversation) {
    const gchar *floor_id = NULL;
    if (!gh_conversation_get_floor(conversation, &cursor.created_at, &floor_id))
      return TRUE; /* nothing older */
    cursor.has = TRUE;
    cursor.id = g_strdup(floor_id);
  }
  stmt = prepare(store,
    "SELECT created_at, backend_msg_id, raw_json, expires_at FROM messages "
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
    more = FALSE;
    while (TRUE) {
      gboolean has_row = FALSE;
      if (!step_row(store, stmt, &has_row, "Reading stored group messages", error))
        goto fail;
      if (!has_row)
        break;
      if (++rows > limit) {
        more = TRUE;
        break;
      }
      cursor.has = TRUE;
      cursor.created_at = sqlite3_column_int64(stmt, 0);
      g_free(cursor.id);
      cursor.id = column_text(stmt, 1);
      gint64 expires_at = sqlite3_column_int64(stmt, 3);
      if (expires_at > 0 && expires_at <= now)
        continue;
      g_autoptr(GError) bad = NULL;
      GhMessage *message = gh_message_new_from_nip29_event(self->account, relay_url,
        (const gchar *)sqlite3_column_text(stmt, 2), &bad);
      if (!message || g_strcmp0(gh_message_get_room_id(message), room_id) != 0) {
        g_warning("Groundhog skipped a stored group message that failed verification: %s",
                  bad ? bad->message : "it belongs to another group");
        g_clear_object(&message);
        continue;
      }
      g_ptr_array_add(messages, message);
    }
  } while (!conversation && messages->len == 0 && more);
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!conversation && messages->len == 0)
    goto done;

  stmt = prepare(store,
    "SELECT c.title, c.unread_count, r.created_at, r.backend_msg_id FROM conversations c "
    "LEFT JOIN messages r ON r.id = c.last_read_msg AND r.conversation_id = c.id "
    "WHERE c.id = ?1", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  if (!step_row(store, stmt, &has_row, "Reading a group room", error))
    goto fail;
  if (has_row) {
    title = column_text(stmt, 0);
    unread = sqlite3_column_int64(stmt, 1);
    marker.has = sqlite3_column_type(stmt, 3) != SQLITE_NULL;
    marker.created_at = sqlite3_column_int64(stmt, 2);
    marker.id = column_text(stmt, 3);
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  GhConversationState state = {
    .accepted = TRUE,
    .has_marker = marker.has,
    .marker_created_at = marker.created_at,
    .marker_id = marker.id,
    .subject = title,
    .unread = (guint)CLAMP(unread, 0, (gint64)G_MAXUINT),
    .has_older = more && cursor.has,
    .floor_created_at = cursor.created_at,
    .floor_id = cursor.id,
  };
  gh_conversation_store_restore(model, room_id, messages, &state);
  *out_listed = messages->len;
done:
  g_free(cursor.id);
  g_free(marker.id);
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  g_free(cursor.id);
  g_free(marker.id);
  return FALSE;
}

gboolean
gh_store_nip29_attach(GhStoreNip29 *self, GhConversationStore *model, guint page_size,
                      GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_NIP29(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(model), FALSE);
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  guint limit = page_size ? MIN(page_size, MAX_PAGE) : GH_STORE_NIP29_PAGE_SIZE;
  if (!gh_conversation_store_set_backend_delegate(model, GH_CONVERSATION_BACKEND_NIP29,
                                                  self->account, &group_delegate,
                                                  g_object_ref(self), g_object_unref)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "The conversation list belongs to another account");
    return FALSE;
  }
  g_weak_ref_set(&self->model, model);
  g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
  g_autoptr(GPtrArray) rooms = g_ptr_array_new_with_free_func(g_free);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id, backend_key FROM conversations c WHERE backend = 2 AND "
    "EXISTS (SELECT 1 FROM messages m WHERE m.conversation_id = c.id) "
    "ORDER BY last_activity DESC, backend_key", error);
  if (!stmt)
    return FALSE;
  while (TRUE) {
    gboolean has_row = FALSE;
    if (!step_row(store, stmt, &has_row, "Listing group rooms", error)) {
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
    if (!restore_room(self, model, g_array_index(ids, gint64, i), g_ptr_array_index(rooms, i),
                      NULL, limit, &listed, error))
      return FALSE;
  }
  return TRUE;
}

gboolean
gh_store_nip29_load_older(GhStoreNip29 *self, GhConversation *conversation, guint limit,
                          guint *out_loaded, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_NIP29(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  if (out_loaded)
    *out_loaded = 0;
  if (!check_open(self, error))
    return FALSE;
  if (limit == 0 || limit > MAX_PAGE)
    return invalid(error, "A page holds 1 to 1000 messages");
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  if (!model || gh_conversation_store_lookup(model, room_id) != conversation ||
      gh_conversation_get_backend(conversation) != GH_CONVERSATION_BACKEND_NIP29)
    return invalid(error, "The group room is not listed by the attached model");
  gboolean found = FALSE;
  gint64 id = 0;
  if (!lookup_room(self->store, room_id, &found, &id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such group room");
    return FALSE;
  }
  guint listed = 0;
  if (!restore_room(self, model, id, room_id, conversation, limit, &listed, error))
    return FALSE;
  if (out_loaded)
    *out_loaded = listed;
  return TRUE;
}

/* ---- Object ---------------------------------------------------------------------- */

GhStoreNip29 *
gh_store_nip29_new(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  GhStoreNip29 *self = g_object_new(GH_TYPE_STORE_NIP29, NULL);
  self->store = store;
  self->account = g_strdup(gh_store_get_account_pubkey(store));
  return self;
}

void
gh_store_nip29_close(GhStoreNip29 *self)
{
  g_return_if_fail(GH_IS_STORE_NIP29(self));
  self->store = NULL;
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  g_weak_ref_set(&self->model, NULL);
  if (model)
    gh_conversation_store_clear_backend_delegate(model, GH_CONVERSATION_BACKEND_NIP29, self);
}

static void
gh_store_nip29_finalize(GObject *object)
{
  GhStoreNip29 *self = GH_STORE_NIP29(object);
  g_weak_ref_clear(&self->model);
  g_free(self->account);
  G_OBJECT_CLASS(gh_store_nip29_parent_class)->finalize(object);
}

static void
gh_store_nip29_class_init(GhStoreNip29Class *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_store_nip29_finalize;
}

static void
gh_store_nip29_init(GhStoreNip29 *self)
{
  g_weak_ref_init(&self->model, NULL);
}
