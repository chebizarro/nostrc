#include "gh-store-mls.h"

#include "gh-conversation-private.h"

#include <string.h>

#include <sqlite3.h>

#define MAX_PAGE 1000
/* Rows of an MLS conversation that are no messages: the account's own
 * Commits (kind 445, gh-mls-commits.c) and Welcome wraps (kind 444). */
#define LISTED_KIND GH_MESSAGE_MLS_KIND

struct _GhStoreMls {
  GObject parent_instance;
  GhStore *store;  /* borrowed; NULL once closed */
  gchar *account;
  GWeakRef model;  /* the GhConversationStore it is the MLS delegate of */
};

G_DEFINE_FINAL_TYPE(GhStoreMls, gh_store_mls, G_TYPE_OBJECT)

/* ---- Statements (as gh-store-nip29.c) --------------------------------------- */

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
  gh_store_set_sqlite_error(store, rc, "Preparing an encrypted-group statement", error);
  return NULL;
}

#define BIND(expr)                                                                  \
  G_STMT_START {                                                                    \
    int bind_rc_ = (expr);                                                          \
    if (bind_rc_ != SQLITE_OK) {                                                    \
      gh_store_set_sqlite_error(store, bind_rc_, "Binding an encrypted-group value", \
                                error);                                             \
      goto fail;                                                                    \
    }                                                                               \
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

/* ---- Rooms ----------------------------------------------------------------- */

gboolean
gh_store_mls_save_room(GhStore *store, const gchar *group_id_hex, const gchar *title,
                       gint64 *out_conversation_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *room_id = gh_message_mls_room_id(group_id_hex);
  if (!room_id)
    return invalid(error, "An MLS group id is lowercase hex");
  if (title && strnlen(title, 65536) > 65535)
    return invalid(error, "The group name is too long");
  gint64 conversation_id = 0;
  sqlite3_stmt *stmt = NULL;
  if (!gh_store_begin(store, error))
    return FALSE;
  if (!gh_store_ensure_conversation(store, GH_STORE_BACKEND_MLS, group_id_hex,
                                    GH_STORE_REQUEST_ACCEPTED, &conversation_id, error))
    goto fail;
  if (title) {
    stmt = prepare(store, "UPDATE conversations SET title = ?1 WHERE id = ?2", error);
    if (!stmt)
      goto fail;
    BIND(bind_text(stmt, 1, *title ? title : NULL));
    BIND(sqlite3_bind_int64(stmt, 2, conversation_id));
    if (!step_done(store, stmt, "Naming an encrypted group", error))
      goto fail;
    g_clear_pointer(&stmt, sqlite3_finalize);
  }
  if (!gh_store_commit(store, error))
    return FALSE;
  if (out_conversation_id)
    *out_conversation_id = conversation_id;
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  gh_store_rollback(store);
  return FALSE;
}

static gboolean
check_open(GhStoreMls *self, GError **error)
{
  if (self->store)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                      "The encrypted-group store is closed");
  return FALSE;
}

/* The conversation of an MLS room id ("mls:<hex>"); *found FALSE when it is
 * not stored. */
static gboolean
lookup_room(GhStore *store, const gchar *room_id, gboolean *found, gint64 *id, GError **error)
{
  *found = FALSE;
  *id = 0;
  g_autofree gchar *hex = NULL;
  if (!gh_message_mls_room_split(room_id, &hex))
    return TRUE;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id FROM conversations WHERE backend = 3 AND backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, hex));
  gboolean ok = step_row(store, stmt, found, "Looking up an encrypted group", error);
  if (ok && *found)
    *id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean
message_row(GhStore *store, gint64 conversation_id, const gchar *message_id, gint64 *row,
            GError **error)
{
  *row = 0;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id FROM messages WHERE conversation_id = ?1 AND backend_msg_id = ?2 AND kind = ?3",
    error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, message_id));
  BIND(sqlite3_bind_int(stmt, 3, LISTED_KIND));
  gboolean has_row = FALSE;
  gboolean ok = step_row(store, stmt, &has_row, "Looking up an encrypted-group message", error);
  if (ok && has_row)
    *row = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* ---- The delegate --------------------------------------------------------------- */

static gboolean
delegate_has_wrap(gpointer data, const gchar *wrap_id)
{
  (void)data;
  (void)wrap_id;
  return FALSE; /* kind-445 envelopes are deduplicated by libmarmot */
}

/* The seen set keys an MLS message by (group, id) (gh-store.h), and this
 * pre-check gets the id alone: T-admit (delegate_admit) is where a
 * duplicate within its group is recognized. */
static gboolean
delegate_has_rumor(gpointer data, const gchar *message_id)
{
  (void)data;
  (void)message_id;
  return FALSE;
}

/* T-admit of one decrypted group message (see the header). */
static gboolean
delegate_admit(gpointer data, GhMessage *message, const gchar *wrap_id,
               GhConversationCommit *commit, GError **error)
{
  GhStoreMls *self = data;
  if (!check_open(self, error))
    return FALSE;
  if (!gh_message_is_mls(message) ||
      g_strcmp0(gh_message_get_account(message), self->account) != 0)
    return invalid(error, "Not an encrypted-group message of this account");
  GhStore *store = self->store;
  const gboolean own = gh_message_is_self(message);
  const gchar *room_id = gh_message_get_room_id(message);
  GhStoreMessage m = {
    .backend = GH_STORE_BACKEND_MLS,
    .backend_key = gh_message_get_group_id(message),
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
  /* A duplicate may still be stored (a send stored its message first). */
  if (found && result == GH_STORE_ADMIT_DUPLICATE &&
      !message_row(store, conversation_id, m.backend_msg_id, &row, error))
    goto fail;
  if (found && row > 0) {
    GhStoreReadMove move = !own ? GH_STORE_READ_RECOUNT
                           : !wrap_id ? GH_STORE_READ_LISTED
                           : result == GH_STORE_ADMIT_STORED ? GH_STORE_READ_REPLY
                           : GH_STORE_READ_RECOUNT;
    gint64 seq = 0;
    if (!gh_store_message_seq(store, row, &seq, error) ||
        !gh_store_read_state_update(store, conversation_id, move, row, seq, &commit->unread,
                                    error))
      goto fail;
    commit->seq = seq;
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
  GhStoreMls *self = data;
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
      (row > 0 &&
       !gh_store_read_state_update(store, conversation_id, GH_STORE_READ_LISTED, row,
                                   (gint64)MIN(gh_conversation_get_read_seq(conversation),
                                               (guint64)G_MAXINT64), NULL, error)))
    goto fail;
  return gh_store_commit(store, error);
fail:
  gh_store_rollback(store);
  return FALSE;
}

static const GhConversationDelegate mls_delegate = {
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
 * NULL) or those before the listed room's floor, as gh-store-nip29.c does. */
static gboolean
restore_room(GhStoreMls *self, GhConversationStore *model, gint64 conversation_id,
             const gchar *group_hex, GhConversation *conversation, guint limit,
             guint *out_listed, GError **error)
{
  GhStore *store = self->store;
  const gint64 now = gh_clock_get_unix(gh_store_get_clock(store));
  g_autofree gchar *room_id = gh_message_mls_room_id(group_hex);
  Place cursor = { 0 };
  GhStoreReadState read = { 0 };
  g_autofree gchar *title = NULL;
  gint64 pinned_rank = 0;
  g_autoptr(GPtrArray) messages = g_ptr_array_new_with_free_func(g_object_unref);
  sqlite3_stmt *stmt = NULL;
  *out_listed = 0;
  if (!room_id)
    return TRUE; /* not a room this build can list */
  if (conversation) {
    const gchar *floor_id = NULL;
    if (!gh_conversation_get_floor(conversation, &cursor.created_at, &floor_id))
      return TRUE; /* nothing older */
    cursor.has = TRUE;
    cursor.id = g_strdup(floor_id);
  }
  stmt = prepare(store,
    "SELECT created_at, backend_msg_id, raw_json, expires_at, seq FROM messages "
    "WHERE conversation_id = ?1 AND kind = ?6 AND (?2 = 0 OR created_at < ?3 OR "
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
    BIND(sqlite3_bind_int(stmt, 6, LISTED_KIND));
    guint rows = 0;
    more = FALSE;
    while (TRUE) {
      gboolean has_row = FALSE;
      if (!step_row(store, stmt, &has_row, "Reading stored encrypted-group messages", error))
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
      GhMessage *message = gh_message_new_from_mls(self->account, group_hex,
        (const gchar *)sqlite3_column_text(stmt, 2), &bad);
      if (!message) {
        g_warning("Groundhog skipped a stored encrypted-group message that failed "
                  "verification: %s", bad->message);
        continue;
      }
      gh_message_set_seq(message, (guint64)MAX(sqlite3_column_int64(stmt, 4), 0));
      g_ptr_array_add(messages, message);
    }
  } while (!conversation && messages->len == 0 && more);
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!conversation && messages->len == 0)
    goto done;

  stmt = prepare(store, "SELECT title, pinned_rank FROM conversations WHERE id = ?1", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  if (!step_row(store, stmt, &has_row, "Reading an encrypted group", error))
    goto fail;
  if (has_row) {
    title = column_text(stmt, 0);
    pinned_rank = sqlite3_column_type(stmt, 1) == SQLITE_NULL
                    ? 0 : MAX(sqlite3_column_int64(stmt, 1), 1);
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!gh_store_read_state_load(store, conversation_id, &read, error))
    goto fail;
  GhConversationState state = {
    .accepted = TRUE,
    .has_marker = read.has_marker,
    .marker_created_at = read.marker_created_at,
    .marker_id = read.marker_id,
    .subject = title,
    .unread = (guint)CLAMP(read.unread, 0, (gint64)G_MAXUINT),
    .has_older = more && cursor.has,
    .floor_created_at = cursor.created_at,
    .floor_id = cursor.id,
    .read_seq = (guint64)MAX(read.read_seq, 0),
    .has_reply = read.has_reply,
    .reply_created_at = read.reply_created_at,
    .reply_id = read.reply_id,
    .pinned_rank = pinned_rank,
  };
  gh_conversation_store_restore(model, room_id, messages, &state);
  *out_listed = messages->len;
done:
  g_free(cursor.id);
  gh_store_read_state_clear(&read);
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  g_free(cursor.id);
  gh_store_read_state_clear(&read);
  return FALSE;
}

gboolean
gh_store_mls_attach(GhStoreMls *self, GhConversationStore *model, guint page_size,
                    GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_MLS(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(model), FALSE);
  if (!check_open(self, error))
    return FALSE;
  GhStore *store = self->store;
  guint limit = page_size ? MIN(page_size, MAX_PAGE) : GH_STORE_MLS_PAGE_SIZE;
  if (!gh_conversation_store_set_backend_delegate(model, GH_CONVERSATION_BACKEND_MLS,
                                                  self->account, &mls_delegate,
                                                  g_object_ref(self), g_object_unref)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "The conversation list belongs to another account");
    return FALSE;
  }
  g_weak_ref_set(&self->model, model);
  g_autoptr(GArray) ids = g_array_new(FALSE, FALSE, sizeof(gint64));
  g_autoptr(GPtrArray) groups = g_ptr_array_new_with_free_func(g_free);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id, backend_key FROM conversations c WHERE backend = 3 AND "
    "EXISTS (SELECT 1 FROM messages m WHERE m.conversation_id = c.id AND m.kind = ?1) "
    "ORDER BY last_activity DESC, backend_key", error);
  if (!stmt)
    return FALSE;
  if (sqlite3_bind_int(stmt, 1, LISTED_KIND) != SQLITE_OK) {
    gh_store_set_sqlite_error(store, SQLITE_ERROR, "Binding an encrypted-group value", error);
    sqlite3_finalize(stmt);
    return FALSE;
  }
  while (TRUE) {
    gboolean has_row = FALSE;
    if (!step_row(store, stmt, &has_row, "Listing encrypted groups", error)) {
      sqlite3_finalize(stmt);
      return FALSE;
    }
    if (!has_row)
      break;
    gint64 id = sqlite3_column_int64(stmt, 0);
    g_array_append_val(ids, id);
    g_ptr_array_add(groups, column_text(stmt, 1));
  }
  sqlite3_finalize(stmt);
  for (guint i = 0; i < ids->len; i++) {
    guint listed = 0;
    if (!restore_room(self, model, g_array_index(ids, gint64, i), g_ptr_array_index(groups, i),
                      NULL, limit, &listed, error))
      return FALSE;
  }
  return TRUE;
}

gboolean
gh_store_mls_load_older(GhStoreMls *self, GhConversation *conversation, guint limit,
                        guint *out_loaded, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_MLS(self), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  if (out_loaded)
    *out_loaded = 0;
  if (!check_open(self, error))
    return FALSE;
  if (limit == 0 || limit > MAX_PAGE)
    return invalid(error, "A page holds 1 to 1000 messages");
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  const gchar *room_id = gh_conversation_get_room_id(conversation);
  g_autofree gchar *hex = NULL;
  if (!model || gh_conversation_store_lookup(model, room_id) != conversation ||
      !gh_message_mls_room_split(room_id, &hex))
    return invalid(error, "The encrypted group is not listed by the attached model");
  gboolean found = FALSE;
  gint64 id = 0;
  if (!lookup_room(self->store, room_id, &found, &id, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "No such encrypted group");
    return FALSE;
  }
  guint listed = 0;
  if (!restore_room(self, model, id, hex, conversation, limit, &listed, error))
    return FALSE;
  if (out_loaded)
    *out_loaded = listed;
  return TRUE;
}

/* ---- Object ---------------------------------------------------------------------- */

GhStoreMls *
gh_store_mls_new(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  GhStoreMls *self = g_object_new(GH_TYPE_STORE_MLS, NULL);
  self->store = store;
  self->account = g_strdup(gh_store_get_account_pubkey(store));
  return self;
}

void
gh_store_mls_close(GhStoreMls *self)
{
  g_return_if_fail(GH_IS_STORE_MLS(self));
  self->store = NULL;
  g_autoptr(GhConversationStore) model = g_weak_ref_get(&self->model);
  g_weak_ref_set(&self->model, NULL);
  if (model)
    gh_conversation_store_clear_backend_delegate(model, GH_CONVERSATION_BACKEND_MLS, self);
}

static void
gh_store_mls_finalize(GObject *object)
{
  GhStoreMls *self = GH_STORE_MLS(object);
  g_weak_ref_clear(&self->model);
  g_free(self->account);
  G_OBJECT_CLASS(gh_store_mls_parent_class)->finalize(object);
}

static void
gh_store_mls_class_init(GhStoreMlsClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gh_store_mls_finalize;
}

static void
gh_store_mls_init(GhStoreMls *self)
{
  g_weak_ref_init(&self->model, NULL);
}
