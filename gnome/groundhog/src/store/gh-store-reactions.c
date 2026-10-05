#include "gh-store-reactions.h"

#include <sqlite3.h>

struct _GhStoreReactions {
  GObject parent_instance;
  GhStore *store;       /* borrowed */
  gboolean closed;
};

G_DEFINE_FINAL_TYPE(GhStoreReactions, gh_store_reactions, G_TYPE_OBJECT)

#define REACTION_AUTHOR_ROOM_LIMIT 64
#define REACTION_AUTHOR_ACCOUNT_LIMIT 256
#define REACTION_ROOM_LIMIT 512
#define REACTION_ACCOUNT_LIMIT 4096
#define REACTION_PENDING_SECONDS (7 * 24 * 60 * 60)

static gboolean
sql_error(sqlite3 *db, int rc, const gchar *what, GError **error)
{
  if (rc == SQLITE_OK || rc == SQLITE_DONE)
    return TRUE;
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s: %s", what, sqlite3_errmsg(db));
  return FALSE;
}

static gboolean
lookup_room(sqlite3 *db, const gchar *room_id, gboolean *known, GError **error)
{
  *known = FALSE;
  sqlite3_stmt *stmt = NULL;
  if (!sql_error(db, sqlite3_prepare_v2(db,
      "SELECT 1 FROM conversations WHERE backend_key = ? LIMIT 1", -1, &stmt, NULL),
      "prepare room lookup", error))
    return FALSE;
  sqlite3_bind_text(stmt, 1, room_id, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW)
    *known = TRUE;
  return rc == SQLITE_ROW || sql_error(db, rc, "lookup room", error);
}

static gboolean
prune_deferred(sqlite3 *db, GError **error)
{
  static const gchar *sql =
    "DELETE FROM pending_reactions WHERE received_at < CAST(strftime('%s','now') AS INTEGER) - "
    G_STRINGIFY(REACTION_PENDING_SECONDS) ";"
    "DELETE FROM reaction_tombstones WHERE received_at < CAST(strftime('%s','now') AS INTEGER) - "
    G_STRINGIFY(REACTION_PENDING_SECONDS) ";"
    "DELETE FROM pending_reactions WHERE NOT EXISTS (SELECT 1 FROM conversations c "
    "WHERE c.backend_key = pending_reactions.room_id);"
    "DELETE FROM reaction_tombstones WHERE NOT EXISTS (SELECT 1 FROM conversations c "
    "WHERE c.backend_key = reaction_tombstones.room_id);"
    "DELETE FROM pending_reactions WHERE reaction_msg_id IN ("
    "SELECT reaction_msg_id FROM (SELECT reaction_msg_id, ROW_NUMBER() OVER ("
    "PARTITION BY room_id, sender_pubkey ORDER BY arrival_seq DESC) AS n "
    "FROM pending_reactions) WHERE n > " G_STRINGIFY(REACTION_AUTHOR_ROOM_LIMIT) ");"
    "DELETE FROM pending_reactions WHERE reaction_msg_id IN ("
    "SELECT reaction_msg_id FROM (SELECT reaction_msg_id, ROW_NUMBER() OVER ("
    "PARTITION BY room_id ORDER BY arrival_seq DESC) AS n "
    "FROM pending_reactions) WHERE n > " G_STRINGIFY(REACTION_ROOM_LIMIT) ");"
    "DELETE FROM reaction_tombstones WHERE (reaction_msg_id, room_id, sender_pubkey) IN ("
    "SELECT reaction_msg_id, room_id, sender_pubkey FROM ("
    "SELECT reaction_msg_id, room_id, sender_pubkey, ROW_NUMBER() OVER ("
    "PARTITION BY room_id, sender_pubkey ORDER BY arrival_seq DESC) AS n "
    "FROM reaction_tombstones) WHERE n > " G_STRINGIFY(REACTION_AUTHOR_ROOM_LIMIT) ");"
    "DELETE FROM reaction_tombstones WHERE (reaction_msg_id, room_id, sender_pubkey) IN ("
    "SELECT reaction_msg_id, room_id, sender_pubkey FROM ("
    "SELECT reaction_msg_id, room_id, sender_pubkey, ROW_NUMBER() OVER ("
    "PARTITION BY room_id ORDER BY arrival_seq DESC) AS n "
    "FROM reaction_tombstones) WHERE n > " G_STRINGIFY(REACTION_ROOM_LIMIT) ");"
    "DELETE FROM pending_reactions WHERE arrival_seq IN ("
    "SELECT arrival_seq FROM (SELECT arrival_seq, ROW_NUMBER() OVER ("
    "PARTITION BY sender_pubkey ORDER BY arrival_seq DESC) AS n "
    "FROM pending_reactions) WHERE n > " G_STRINGIFY(REACTION_AUTHOR_ACCOUNT_LIMIT) ");"
    "DELETE FROM reaction_tombstones WHERE arrival_seq IN ("
    "SELECT arrival_seq FROM (SELECT arrival_seq, ROW_NUMBER() OVER ("
    "PARTITION BY sender_pubkey ORDER BY arrival_seq DESC) AS n "
    "FROM reaction_tombstones) WHERE n > " G_STRINGIFY(REACTION_AUTHOR_ACCOUNT_LIMIT) ");"
    /* An author's nth-newest row is its oldest when it has n rows left.
     * Descending n therefore drains the heaviest authors first, rebalancing
     * after each removal; arrival_seq breaks ties oldest-first. The newest
     * arrival survives rather than being rejected at a full account. */
    "DELETE FROM pending_reactions WHERE arrival_seq IN ("
    "SELECT arrival_seq FROM (SELECT arrival_seq, ROW_NUMBER() OVER ("
    "PARTITION BY sender_pubkey ORDER BY arrival_seq DESC) AS n "
    "FROM pending_reactions) ORDER BY n DESC, arrival_seq ASC "
    "LIMIT (SELECT max(0, count(*) - " G_STRINGIFY(REACTION_ACCOUNT_LIMIT) ") "
    "FROM pending_reactions));"
    "DELETE FROM reaction_tombstones WHERE arrival_seq IN ("
    "SELECT arrival_seq FROM (SELECT arrival_seq, ROW_NUMBER() OVER ("
    "PARTITION BY sender_pubkey ORDER BY arrival_seq DESC) AS n "
    "FROM reaction_tombstones) ORDER BY n DESC, arrival_seq ASC "
    "LIMIT (SELECT max(0, count(*) - " G_STRINGIFY(REACTION_ACCOUNT_LIMIT) ") "
    "FROM reaction_tombstones));";
  return sql_error(db, sqlite3_exec(db, sql, NULL, NULL, NULL), "prune reactions", error);
}

static gboolean
delete_pending(sqlite3 *db, const gchar *rid, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  if (!sql_error(db, sqlite3_prepare_v2(db,
      "DELETE FROM pending_reactions WHERE reaction_msg_id = ?", -1, &stmt, NULL),
      "prepare pending delete", error))
    return FALSE;
  sqlite3_bind_text(stmt, 1, rid, -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return sql_error(db, rc, "delete pending", error);
}

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
delegate_admit_inner(gpointer data, GhReaction *reaction, GError **error)
{
  GhStoreReactions *self = data;
  if (self->closed) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Reaction store closed");
    return FALSE;
  }

  /* delegate_admit() opened a transaction, nested when the caller owns one. */
  sqlite3 *db = gh_store_get_db(self->store);
  if (!db) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Store unavailable");
    return FALSE;
  }

  /* A reaction only belongs to the room that owns its target. For NIP-17,
   * room_id is the canonical set of the reaction author and its p-tags, so
   * this also requires the author and recipients to match the target room.
   * Never update the target-only in-memory summary for an unknown room. */
  const gchar *room_id = gh_reaction_get_room_id(reaction);
  gboolean known = FALSE;
  if (!lookup_room(db, room_id, &known, error))
    return FALSE;
  if (!known)
    return FALSE;
  if (!prune_deferred(db, error))
    return FALSE;
  sqlite3_stmt *tombstone = NULL;
  int rc = sqlite3_prepare_v2(db,
    "SELECT 1 FROM reaction_tombstones WHERE reaction_msg_id = ? AND room_id = ? "
    "AND sender_pubkey = ?", -1, &tombstone, NULL);
  if (!sql_error(db, rc, "prepare tombstone lookup", error))
    return FALSE;
  sqlite3_bind_text(tombstone, 1, gh_reaction_get_reaction_rumor_id(reaction), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(tombstone, 2, room_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(tombstone, 3, gh_reaction_get_sender(reaction), -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(tombstone);
  sqlite3_finalize(tombstone);
  if (rc == SQLITE_ROW)
    return FALSE;
  if (rc != SQLITE_DONE)
    return sql_error(db, rc, "lookup tombstone", error);
  sqlite3_stmt *find_conv = NULL;
  rc = sqlite3_prepare_v2(db,
    "SELECT c.id FROM conversations c JOIN messages m ON m.conversation_id = c.id "
    "WHERE c.backend_key = ? AND m.backend_msg_id = ?", -1, &find_conv, NULL);
  if (rc != SQLITE_OK) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "prepare: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  sqlite3_bind_text(find_conv, 1, room_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(find_conv, 2, gh_reaction_get_target_rumor_id(reaction),
                    -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(find_conv);
  gint64 conversation_id = -1;
  if (rc == SQLITE_ROW)
    conversation_id = sqlite3_column_int64(find_conv, 0);
  sqlite3_finalize(find_conv);

  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "lookup target: %s", sqlite3_errmsg(db));
    return FALSE;
  }
  if (conversation_id < 0) {
    sqlite3_stmt *pending = NULL;
    rc = sqlite3_prepare_v2(db,
      "INSERT OR IGNORE INTO pending_reactions "
      "(reaction_msg_id, room_id, target_msg_id, sender_pubkey, emoji, created_at, received_at) "
      "VALUES (?, ?, ?, ?, ?, ?, CAST(strftime('%s','now') AS INTEGER))",
      -1, &pending, NULL);
    if (!sql_error(db, rc, "prepare pending reaction", error))
      return FALSE;
    sqlite3_bind_text(pending, 1, gh_reaction_get_reaction_rumor_id(reaction), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(pending, 2, room_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(pending, 3, gh_reaction_get_target_rumor_id(reaction), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(pending, 4, gh_reaction_get_sender(reaction), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(pending, 5, gh_reaction_get_emoji(reaction), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(pending, 6, gh_reaction_get_created_at(reaction));
    rc = sqlite3_step(pending);
    sqlite3_finalize(pending);
    if (!sql_error(db, rc, "insert pending reaction", error) || !prune_deferred(db, error))
      return FALSE;
    return FALSE;
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
  return delete_pending(db, gh_reaction_get_reaction_rumor_id(reaction), error);
}

static gboolean
delegate_delete_event_inner(gpointer data, const gchar *rid, const gchar *sender,
                            const gchar *room, GError **error)
{
  GhStoreReactions *self = data;
  sqlite3 *db = self->closed ? NULL : gh_store_get_db(self->store);
  if (!db) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Reaction store closed");
    return FALSE;
  }
  gboolean known = FALSE;
  if (!lookup_room(db, room, &known, error))
    return FALSE;
  if (!known)
    return TRUE;
  if (!prune_deferred(db, error))
    return FALSE;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "INSERT INTO reaction_tombstones (reaction_msg_id, room_id, sender_pubkey, received_at) "
    "VALUES (?, ?, ?, CAST(strftime('%s','now') AS INTEGER)) "
    "ON CONFLICT(reaction_msg_id, room_id, sender_pubkey) DO UPDATE SET "
    "received_at = excluded.received_at, arrival_seq = excluded.arrival_seq",
    -1, &stmt, NULL);
  if (!sql_error(db, rc, "prepare deletion notice", error))
    return FALSE;
  sqlite3_bind_text(stmt, 1, rid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, room, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, sender, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (!sql_error(db, rc, "write deletion notice", error))
    return FALSE;
  rc = sqlite3_prepare_v2(db,
    "DELETE FROM pending_reactions WHERE reaction_msg_id = ? AND room_id = ? "
    "AND sender_pubkey = ?", -1, &stmt, NULL);
  if (!sql_error(db, rc, "prepare pending deletion", error))
    return FALSE;
  sqlite3_bind_text(stmt, 1, rid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, room, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, sender, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (!sql_error(db, rc, "remove pending reaction", error))
    return FALSE;
  rc = sqlite3_prepare_v2(db,
    "DELETE FROM reactions WHERE reaction_msg_id = ? AND room_id = ? AND sender_pubkey = ?",
    -1, &stmt, NULL);
  if (!sql_error(db, rc, "prepare reaction deletion", error))
    return FALSE;
  sqlite3_bind_text(stmt, 1, rid, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, room, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, sender, -1, SQLITE_TRANSIENT);
  rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return sql_error(db, rc, "remove reaction", error) && prune_deferred(db, error);
}

static gboolean
delegate_admit(gpointer data, GhReaction *reaction, GError **error)
{
  GhStoreReactions *self = data;
  if (!gh_store_begin(self->store, error))
    return FALSE;
  g_autoptr(GError) local_error = NULL;
  gboolean admitted = delegate_admit_inner(data, reaction, &local_error);
  if (local_error) {
    gh_store_rollback(self->store);
    g_propagate_error(error, g_steal_pointer(&local_error));
    return FALSE;
  }
  if (!gh_store_commit(self->store, error))
    return FALSE;
  return admitted;
}

static gboolean
delegate_delete_event(gpointer data, const gchar *rid, const gchar *sender,
                      const gchar *room, GError **error)
{
  GhStoreReactions *self = data;
  if (!gh_store_begin(self->store, error))
    return FALSE;
  if (!delegate_delete_event_inner(data, rid, sender, room, error)) {
    gh_store_rollback(self->store);
    return FALSE;
  }
  return gh_store_commit(self->store, error);
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
  .delete_event = delegate_delete_event,
};

gboolean
gh_store_reactions_reconcile(GhStoreReactions *self, GhReactionStore *model,
                             const gchar *room_id, const gchar *target_id, GError **error)
{
  g_return_val_if_fail(GH_IS_STORE_REACTIONS(self), FALSE);
  g_return_val_if_fail(GH_IS_REACTION_STORE(model), FALSE);
  sqlite3 *db = self->closed ? NULL : gh_store_get_db(self->store);
  if (!db) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Reaction store closed");
    return FALSE;
  }
  if (!prune_deferred(db, error))
    return FALSE;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "SELECT p.target_msg_id, p.reaction_msg_id, p.sender_pubkey, p.emoji, "
    "p.created_at, p.room_id FROM pending_reactions p "
    "JOIN conversations c ON c.backend_key = p.room_id "
    "JOIN messages m ON m.conversation_id = c.id AND m.backend_msg_id = p.target_msg_id "
    "WHERE (?1 IS NULL OR p.room_id = ?1) AND (?2 IS NULL OR p.target_msg_id = ?2) "
    "ORDER BY p.created_at, p.reaction_msg_id", -1, &stmt, NULL);
  if (!sql_error(db, rc, "prepare pending reconciliation", error))
    return FALSE;
  sqlite3_bind_text(stmt, 1, room_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, target_id, -1, SQLITE_TRANSIENT);
  g_autoptr(GPtrArray) ready = g_ptr_array_new_with_free_func(g_object_unref);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const gchar *target = (const gchar *)sqlite3_column_text(stmt, 0);
    const gchar *rid = (const gchar *)sqlite3_column_text(stmt, 1);
    const gchar *sender = (const gchar *)sqlite3_column_text(stmt, 2);
    const gchar *emoji = (const gchar *)sqlite3_column_text(stmt, 3);
    const gchar *room = (const gchar *)sqlite3_column_text(stmt, 5);
    GhReaction *reaction = gh_reaction_new(target, rid, sender, emoji,
                                           sqlite3_column_int64(stmt, 4), room);
    if (reaction)
      g_ptr_array_add(ready, reaction);
  }
  sqlite3_finalize(stmt);
  if (!sql_error(db, rc, "read pending reactions", error))
    return FALSE;
  for (guint i = 0; i < ready->len; i++) {
    g_autoptr(GError) admit_error = NULL;
    gh_reaction_store_admit(model, g_ptr_array_index(ready, i), &admit_error);
    if (admit_error) {
      g_propagate_error(error, g_steal_pointer(&admit_error));
      return FALSE;
    }
  }
  return TRUE;
}

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
    "SELECT r.target_msg_id, r.reaction_msg_id, r.sender_pubkey, r.emoji, "
    "r.created_at, r.room_id FROM reactions r "
    "JOIN conversations c ON c.id = r.conversation_id "
    "JOIN messages m ON m.conversation_id = c.id AND m.backend_msg_id = r.target_msg_id "
    "WHERE c.backend_key = r.room_id ORDER BY r.created_at ASC, r.reaction_msg_id ASC",
    -1, &stmt, NULL);
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
  return gh_store_reactions_reconcile(self, model, NULL, NULL, error);
}

void
gh_store_reactions_close(GhStoreReactions *self)
{
  g_return_if_fail(GH_IS_STORE_REACTIONS(self));
  self->closed = TRUE;
}
