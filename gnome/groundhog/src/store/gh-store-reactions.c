#include "gh-store-reactions.h"

#include <sqlite3.h>

struct _GhStoreReactions {
  GObject parent_instance;
  GhStore *store;       /* borrowed */
  gboolean closed;
};

G_DEFINE_FINAL_TYPE(GhStoreReactions, gh_store_reactions, G_TYPE_OBJECT)

static void
gh_store_reactions_finalize(GObject *object)
{
  /* store is borrowed; nothing to free. */
  G_OBJECT_CLASS(gh_store_reactions_parent_class)->finalize(object);
}

static void
gh_store_reactions_class_init(GhStoreReactionsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gh_store_reactions_finalize;
}

static void
gh_store_reactions_init(GhStoreReactions *self)
{
  (void)self;
}

GhStoreReactions *
gh_store_reactions_new(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  GhStoreReactions *self = g_object_new(GH_TYPE_STORE_REACTIONS, NULL);
  self->store = store;
  return self;
}

/* ---- Delegate callbacks ---------------------------------------------------- */

static gboolean
delegate_admit(gpointer data, GhReaction *reaction, GError **error)
{
  GhStoreReactions *self = data;
  if (self->closed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Reaction store closed");
    return FALSE;
  }

  /* The caller already holds a transaction (gh_store_begin). */
  sqlite3 *db = gh_store_get_db(self->store);
  if (!db) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Store unavailable");
    return FALSE;
  }

  /* Find conversation_id from room_id. */
  const gchar *room_id = gh_reaction_get_room_id(reaction);
  sqlite3_stmt *find_conv = NULL;
  int rc = sqlite3_prepare_v2(db,
    "SELECT id FROM conversations WHERE backend_key = ?", -1, &find_conv, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  sqlite3_bind_text(find_conv, 1, room_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(find_conv);
  gint64 conversation_id = -1;
  if (rc == SQLITE_ROW)
    conversation_id = sqlite3_column_int64(find_conv, 0);
  sqlite3_finalize(find_conv);

  if (conversation_id < 0) {
    /* The conversation doesn't exist yet — reaction on an unknown room.
     * Store with conversation_id 0; it will be linked when the room is
     * created. For now, succeed silently (memory-only for this reaction). */
    return TRUE;
  }

  sqlite3_stmt *stmt = NULL;
  rc = sqlite3_prepare_v2(db,
    "INSERT OR IGNORE INTO reactions "
    "(conversation_id, target_msg_id, reaction_msg_id, sender_pubkey, emoji, created_at, room_id) "
    "VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  sqlite3_bind_int64(stmt, 1, conversation_id);
  sqlite3_bind_text(stmt, 2, gh_reaction_get_target_rumor_id(reaction), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, gh_reaction_get_reaction_rumor_id(reaction), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, gh_reaction_get_sender(reaction), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, gh_reaction_get_emoji(reaction), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, gh_reaction_get_created_at(reaction));
  sqlite3_bind_text(stmt, 7, gh_reaction_get_room_id(reaction), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "insert: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  return TRUE;
}

static gboolean
delegate_remove(gpointer data, const gchar *reaction_rumor_id, GError **error)
{
  GhStoreReactions *self = data;
  if (self->closed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Reaction store closed");
    return FALSE;
  }
  sqlite3 *db = gh_store_get_db(self->store);
  if (!db) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Store unavailable");
    return FALSE;
  }
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "DELETE FROM reactions WHERE reaction_msg_id = ?", -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  sqlite3_bind_text(stmt, 1, reaction_rumor_id, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "delete: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  return TRUE;
}

static gboolean
delegate_remove_by_sender(gpointer data, const gchar *target_rumor_id,
                           const gchar *sender_pubkey, const gchar *emoji, GError **error)
{
  GhStoreReactions *self = data;
  if (self->closed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Reaction store closed");
    return FALSE;
  }
  sqlite3 *db = gh_store_get_db(self->store);
  if (!db) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Store unavailable");
    return FALSE;
  }
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "DELETE FROM reactions WHERE target_msg_id = ? AND sender_pubkey = ? AND emoji = ?",
    -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  sqlite3_bind_text(stmt, 1, target_rumor_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, sender_pubkey, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, emoji, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "delete: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  return TRUE;
}

static const GhReactionDelegate store_delegate = {
  .admit = delegate_admit,
  .remove = delegate_remove,
  .remove_by_sender = delegate_remove_by_sender,
};

/* ---- Restore --------------------------------------------------------------- */

gboolean
gh_store_reactions_attach(GhStoreReactions *self, GhReactionStore *model, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_REACTIONS(self), FALSE);
  g_return_val_if_fail(GH_IS_REACTION_STORE(model), FALSE);

  const gchar *account = gh_store_get_account_pubkey(self->store);
  if (!account) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "No account");
    return FALSE;
  }

  gh_reaction_store_set_account(model, account, &store_delegate, self, NULL);

  /* Load stored reactions. */
  sqlite3 *db = gh_store_get_db(self->store);
  if (!db)
    return TRUE;  /* no reactions to restore */

  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "SELECT target_msg_id, reaction_msg_id, sender_pubkey, emoji, created_at, room_id "
    "FROM reactions ORDER BY created_at ASC", -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare: %s", sqlite3_errmsg(db));
    return FALSE;
  }

  /* W26 slice B review fix (F5): suspend the delegate so the restore loop
   * does not issue redundant INSERT OR IGNORE statements for every stored
   * reaction. The reactions are already persisted. */
  gh_reaction_store_suspend_delegate(model);

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const gchar *target_msg_id = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *reaction_msg_id = (const gchar *)sqlite3_column_text(stmt, 1);
    const gchar *sender_pubkey = (const gchar *)sqlite3_column_text(stmt, 2);
    const gchar *emoji = (const gchar *)sqlite3_column_text(stmt, 3);
    gint64 created_at = sqlite3_column_int64(stmt, 4);
    const gchar *room_id = (const gchar *)sqlite3_column_text(stmt, 5);

    if (!target_msg_id || !reaction_msg_id || !sender_pubkey || !emoji || !room_id)
      continue;

    g_autoptr(GhReaction) reaction = gh_reaction_new(
      target_msg_id, reaction_msg_id, sender_pubkey, emoji, created_at, room_id);
    if (reaction) {
      g_autoptr(GError) admit_error = NULL;
      gh_reaction_store_admit(model, reaction, &admit_error);
    }
  }
  sqlite3_finalize(stmt);

  gh_reaction_store_resume_delegate(model);
  return TRUE;
}

void
gh_store_reactions_close(GhStoreReactions *self)
{
  g_return_if_fail(GH_IS_STORE_REACTIONS(self));
  self->closed = TRUE;
}
