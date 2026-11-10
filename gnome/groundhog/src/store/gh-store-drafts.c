/* NIP-37 draft synchronization state (plan §3.4 schema v13, §3.7). The SQL
 * lives beside the schema; the store's own transactions call the hooks at
 * the top, GhDraftSync the rest. No jansson here (the store library does not
 * link it): the JSON arrays of identifiers and relay URLs are written and
 * read by the two small helpers below. */
#include "gh-store-drafts.h"

#include <sodium.h>
#include <sqlite3.h>
#include <string.h>

#define IDENTIFIER_BYTES 32

static sqlite3_stmt *
prepare(GhStore *store, const gchar *sql, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  gint rc = sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    return stmt;
  sqlite3_finalize(stmt);
  gh_store_set_sqlite_error(store, rc, "Preparing draft sync storage", error);
  return NULL;
}

static gboolean
bind_text(sqlite3_stmt *stmt, gint index, const gchar *text)
{
  return (text ? sqlite3_bind_text(stmt, index, text, -1, SQLITE_TRANSIENT)
               : sqlite3_bind_null(stmt, index)) == SQLITE_OK;
}

/* Steps a statement that returns no rows and finalizes it. */
static gboolean
run(GhStore *store, sqlite3_stmt *stmt, GError **error)
{
  gint rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || gh_store_set_sqlite_error(store, rc, "Saving draft sync state", error);
}

static gboolean
bind_failed(GhStore *store, sqlite3_stmt *stmt, GError **error)
{
  sqlite3_finalize(stmt);
  return gh_store_set_sqlite_error(store, SQLITE_MISUSE, "Binding draft sync state", error);
}

static const gchar *
column_text(sqlite3_stmt *stmt, gint column)
{
  return (const gchar *) sqlite3_column_text(stmt, column);
}

/* ---- JSON arrays of strings (identifiers, relay URLs) ---------------------------------- */

static gchar *
json_strv(const gchar *const *values)
{
  GString *out = g_string_new("[");
  for (guint i = 0; values && values[i]; i++) {
    g_string_append_c(out, i ? ',' : '"');
    if (i)
      g_string_append_c(out, '"');
    for (const gchar *p = values[i]; *p; p++) {
      if (*p == '"' || *p == '\\')
        g_string_append_printf(out, "\\%c", *p);
      else if ((guchar) *p < 0x20)
        g_string_append_printf(out, "\\u%04x", (guchar) *p);
      else
        g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
  }
  g_string_append_c(out, ']');
  return g_string_free(out, FALSE);
}

/* Parses what json_strv() writes (and any JSON array of strings whose
 * escapes are the simple ones). Anything else yields an empty array. */
static GStrv
parse_strv(const gchar *json)
{
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  const gchar *p = json;
  while (p && *p && *p != '[')
    p++;
  if (!p || *p != '[')
    return g_strv_builder_end(builder);
  p++;
  for (;;) {
    while (*p == ' ' || *p == ',')
      p++;
    if (*p != '"')
      break;
    p++;
    GString *value = g_string_new(NULL);
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) {
        p++;
        if (*p == 'u' && strlen(p) >= 5) {
          gchar hex[5] = { p[1], p[2], p[3], p[4], 0 };
          g_string_append_unichar(value, (gunichar) g_ascii_strtoull(hex, NULL, 16));
          p += 5;
          continue;
        }
        g_string_append_c(value, *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p);
      } else
        g_string_append_c(value, *p);
      p++;
    }
    g_strv_builder_add(builder, value->str);
    g_string_free(value, TRUE);
    if (*p == '"')
      p++;
  }
  return g_strv_builder_end(builder);
}

/* ---- Row types --------------------------------------------------------------------------- */

void
gh_store_draft_state_free(GhStoreDraftState *state)
{
  if (!state)
    return;
  g_free(state->room_key);
  g_free(state->draft_d);
  g_strfreev(state->aliases);
  g_free(state->last_event_id);
  g_free(state);
}

void
gh_store_draft_publication_free(GhStoreDraftPublication *publication)
{
  if (!publication)
    return;
  g_free(publication->room_key);
  g_free(publication->d);
  g_free(publication->event_json);
  g_strfreev(publication->targets);
  g_free(publication->outcomes_json);
  g_free(publication);
}

void
gh_store_draft_conflict_free(GhStoreDraftConflict *conflict)
{
  if (!conflict)
    return;
  g_free(conflict->room_key);
  g_free(conflict->text);
  g_free(conflict->source_event_id);
  g_free(conflict);
}

#define STATE_COLUMNS \
  "room_key, draft_d, aliases_json, local_revision, dirty, last_event_id, last_created_at, " \
  "cleared_at, expires_at"

static GhStoreDraftState *
state_from_row(sqlite3_stmt *stmt)
{
  GhStoreDraftState *state = g_new0(GhStoreDraftState, 1);
  state->room_key = g_strdup(column_text(stmt, 0));
  state->draft_d = g_strdup(column_text(stmt, 1));
  state->aliases = parse_strv(column_text(stmt, 2) ? column_text(stmt, 2) : "[]");
  state->local_revision = sqlite3_column_int64(stmt, 3);
  state->dirty = sqlite3_column_int(stmt, 4) != 0;
  state->last_event_id = g_strdup(column_text(stmt, 5));
  state->last_created_at = sqlite3_column_int64(stmt, 6);
  state->cleared_at = sqlite3_column_int64(stmt, 7);
  state->expires_at = sqlite3_column_int64(stmt, 8);
  return state;
}

/* ---- Hooks inside the caller's transaction ------------------------------------------ */

static gchar *
room_of_conversation(GhStore *store, gint64 conversation_id, gboolean *ok, GError **error)
{
  *ok = FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT backend_key FROM conversations WHERE id = ?1 AND backend = 1", error);
  if (!stmt)
    return NULL;
  if (sqlite3_bind_int64(stmt, 1, conversation_id) != SQLITE_OK) {
    bind_failed(store, stmt, error);
    return NULL;
  }
  gint rc = sqlite3_step(stmt);
  gchar *room = rc == SQLITE_ROW ? g_strdup(column_text(stmt, 0)) : NULL;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Looking up a conversation's room", error);
    return NULL;
  }
  *ok = TRUE;
  return room;
}

gboolean
gh_store_drafts_touch(GhStore *store, gint64 conversation_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO draft_sync_state(room_key, local_revision, dirty) "
    "SELECT backend_key, 1, 1 FROM conversations WHERE id = ?1 AND backend = 1 "
    "ON CONFLICT(room_key) DO UPDATE SET local_revision = local_revision + 1, dirty = 1", error);
  if (!stmt)
    return FALSE;
  if (sqlite3_bind_int64(stmt, 1, conversation_id) != SQLITE_OK)
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

static gboolean
queue_blank(GhStore *store, const gchar *room_key, const gchar *d, gint64 revision, GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
    "INSERT OR REPLACE INTO draft_publications(room_key, d, local_revision, event_json, "
    "targets_json, outcomes_json, state) VALUES (?1, ?2, ?3, '', '[]', '{}', 0)", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, d) ||
      sqlite3_bind_int64(stmt, 3, revision) != SQLITE_OK)
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

gboolean
gh_store_drafts_clear(GhStore *store, gint64 conversation_id, gint64 now, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  gboolean ok = FALSE;
  g_autofree gchar *room_key = room_of_conversation(store, conversation_id, &ok, error);
  if (!ok)
    return FALSE;
  if (!room_key)
    return TRUE; /* not a NIP-17 room */
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(store, room_key, error);
  if (!state && error && *error)
    return FALSE;
  gboolean identified = state && (state->draft_d || state->aliases[0]);
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO draft_sync_state(room_key, local_revision, dirty, cleared_at) VALUES (?1, 1, ?2, ?3) "
    "ON CONFLICT(room_key) DO UPDATE SET local_revision = local_revision + 1, dirty = ?2, "
    "cleared_at = MAX(cleared_at, ?3)", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || sqlite3_bind_int(stmt, 2, identified) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 3, now) != SQLITE_OK)
    return bind_failed(store, stmt, error);
  if (!run(store, stmt, error))
    return FALSE;
  if (!identified)
    return TRUE;
  g_autoptr(GhStoreDraftState) after = gh_store_drafts_get(store, room_key, error);
  if (!after)
    return FALSE;
  if (state->draft_d && !queue_blank(store, room_key, state->draft_d, after->local_revision, error))
    return FALSE;
  for (guint i = 0; state->aliases[i]; i++)
    if (!queue_blank(store, room_key, state->aliases[i], after->local_revision, error))
      return FALSE;
  return TRUE;
}

/* ---- State ----------------------------------------------------------------------------- */

GhStoreDraftState *
gh_store_drafts_get(GhStore *store, const gchar *room_key, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(room_key != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT " STATE_COLUMNS " FROM draft_sync_state WHERE room_key = ?1", error);
  if (!stmt)
    return NULL;
  if (!bind_text(stmt, 1, room_key)) {
    bind_failed(store, stmt, error);
    return NULL;
  }
  gint rc = sqlite3_step(stmt);
  GhStoreDraftState *state = rc == SQLITE_ROW ? state_from_row(stmt) : NULL;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    gh_store_set_sqlite_error(store, rc, "Reading draft sync state", error);
  return state;
}

GPtrArray *
gh_store_drafts_list(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT " STATE_COLUMNS " FROM draft_sync_state ORDER BY room_key", error);
  if (!stmt)
    return NULL;
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify) gh_store_draft_state_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    g_ptr_array_add(rows, state_from_row(stmt));
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Listing draft sync state", error);
    g_ptr_array_unref(rows);
    return NULL;
  }
  return rows;
}

gchar *
gh_store_drafts_find_room(GhStore *store, const gchar *identifier, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(identifier != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT room_key FROM draft_sync_state WHERE draft_d = ?1 OR instr(aliases_json, ?2) > 0",
    error);
  if (!stmt)
    return NULL;
  g_autofree gchar *quoted = g_strdup_printf("\"%s\"", identifier);
  if (!bind_text(stmt, 1, identifier) || !bind_text(stmt, 2, quoted)) {
    bind_failed(store, stmt, error);
    return NULL;
  }
  gint rc = sqlite3_step(stmt);
  gchar *room = rc == SQLITE_ROW ? g_strdup(column_text(stmt, 0)) : NULL;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    gh_store_set_sqlite_error(store, rc, "Looking up a draft identifier", error);
  return room;
}

gboolean
gh_store_drafts_get_room(GhStore *store, const gchar *room_key, gboolean *out_blocked,
                         gint64 *out_forgotten_before, gchar **out_draft, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL, FALSE);
  if (out_blocked)
    *out_blocked = FALSE;
  if (out_forgotten_before)
    *out_forgotten_before = 0;
  if (out_draft)
    *out_draft = NULL;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT request_state, forgotten_before, draft FROM conversations "
    "WHERE backend = 1 AND backend_key = ?1", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key))
    return bind_failed(store, stmt, error);
  gint rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    if (out_blocked)
      *out_blocked = sqlite3_column_int(stmt, 0) == GH_STORE_REQUEST_BLOCKED;
    if (out_forgotten_before)
      *out_forgotten_before = sqlite3_column_int64(stmt, 1);
    if (out_draft)
      *out_draft = g_strdup(column_text(stmt, 2));
  }
  sqlite3_finalize(stmt);
  return rc == SQLITE_ROW || rc == SQLITE_DONE ||
         gh_store_set_sqlite_error(store, rc, "Reading a room for draft sync", error);
}

gboolean
gh_store_drafts_prune(GhStore *store, gint64 now, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "DELETE FROM draft_sync_state WHERE dirty = 0 AND expires_at > 0 AND expires_at < ?1 "
    "AND room_key NOT IN (SELECT room_key FROM draft_publications)", error);
  if (!stmt)
    return FALSE;
  if (sqlite3_bind_int64(stmt, 1, now) != SQLITE_OK)
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

GPtrArray *
gh_store_drafts_list_local(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT backend_key FROM conversations WHERE backend = 1 AND draft IS NOT NULL "
    "ORDER BY backend_key", error);
  if (!stmt)
    return NULL;
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    g_ptr_array_add(rows, g_strdup(column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Listing local drafts", error);
    g_ptr_array_unref(rows);
    return NULL;
  }
  return rows;
}

gboolean
gh_store_drafts_mark_dirty(GhStore *store, const gchar *room_key, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO draft_sync_state(room_key, local_revision, dirty) VALUES (?1, 1, 1) "
    "ON CONFLICT(room_key) DO UPDATE SET local_revision = local_revision + 1, dirty = 1", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key))
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

gboolean
gh_store_drafts_mark_clean(GhStore *store, const gchar *room_key, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "UPDATE draft_sync_state SET dirty = 0 WHERE room_key = ?1", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key))
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

/* ---- Identifiers ----------------------------------------------------------------------- */

static gboolean
save_identifiers(GhStore *store, const gchar *room_key, const gchar *draft_d,
                 const gchar *const *aliases, GError **error)
{
  g_autofree gchar *aliases_json = json_strv(aliases);
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO draft_sync_state(room_key, draft_d, aliases_json) VALUES (?1, ?2, ?3) "
    "ON CONFLICT(room_key) DO UPDATE SET draft_d = ?2, aliases_json = ?3", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, draft_d) ||
      !bind_text(stmt, 3, aliases_json))
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

gboolean
gh_store_drafts_assign_identifier(GhStore *store, const gchar *room_key, gchar **out_d,
                                  GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL, FALSE);
  g_return_val_if_fail(out_d != NULL, FALSE);
  *out_d = NULL;
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(store, room_key, error);
  if (!state && error && *error)
    return FALSE;
  if (state && state->draft_d) {
    *out_d = g_steal_pointer(&state->draft_d);
    return TRUE;
  }
  guint8 raw[IDENTIFIER_BYTES];
  gchar hex[2 * IDENTIFIER_BYTES + 1];
  randombytes_buf(raw, sizeof raw);
  sodium_bin2hex(hex, sizeof hex, raw, sizeof raw);
  sodium_memzero(raw, sizeof raw);
  const gchar *none[] = { NULL };
  if (!save_identifiers(store, room_key, hex,
                        state ? (const gchar *const *) state->aliases : none, error))
    return FALSE;
  *out_d = g_strdup(hex);
  return TRUE;
}

gboolean
gh_store_drafts_add_identifier(GhStore *store, const gchar *room_key, const gchar *identifier,
                               GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && identifier != NULL, FALSE);
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(store, room_key, error);
  if (!state && error && *error)
    return FALSE;
  if (state && (!g_strcmp0(state->draft_d, identifier) ||
                g_strv_contains((const gchar *const *) state->aliases, identifier)))
    return TRUE;
  const gchar *head = identifier;
  g_autoptr(GStrvBuilder) aliases = g_strv_builder_new();
  if (state) {
    for (guint i = 0; state->aliases[i]; i++)
      g_strv_builder_add(aliases, state->aliases[i]);
    if (state->draft_d) {
      /* The lowest identifier carries future revisions; the other is blanked
       * once a newer head is accepted (§3.7 import rule 6). */
      if (strcmp(state->draft_d, identifier) < 0) {
        head = state->draft_d;
        g_strv_builder_add(aliases, identifier);
      } else
        g_strv_builder_add(aliases, state->draft_d);
    }
  }
  g_auto(GStrv) list = g_strv_builder_end(aliases);
  return save_identifiers(store, room_key, head, (const gchar *const *) list, error);
}

gboolean
gh_store_drafts_remove_alias(GhStore *store, const gchar *room_key, const gchar *identifier,
                             GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && identifier != NULL, FALSE);
  g_autoptr(GhStoreDraftState) state = gh_store_drafts_get(store, room_key, error);
  if (!state)
    return !(error && *error);
  g_autoptr(GStrvBuilder) aliases = g_strv_builder_new();
  for (guint i = 0; state->aliases[i]; i++)
    if (g_strcmp0(state->aliases[i], identifier))
      g_strv_builder_add(aliases, state->aliases[i]);
  g_auto(GStrv) list = g_strv_builder_end(aliases);
  return save_identifiers(store, room_key, state->draft_d, (const gchar *const *) list, error);
}

/* ---- Remote revisions ---------------------------------------------------------------- */

gboolean
gh_store_drafts_apply_remote(GhStore *store, const gchar *room_key, const gchar *text,
                             const gchar *event_id, gint64 created_at, gint64 expires_at,
                             gboolean apply_text, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && event_id != NULL, FALSE);
  if (text && !*text)
    text = NULL;
  if (!gh_store_begin_named(store, "drafts", error))
    return FALSE;
  if (apply_text) {
    gint64 id = 0;
    g_autoptr(GError) lookup = NULL;
    if (gh_store_find_conversation(store, GH_STORE_BACKEND_NIP17, room_key, &id, &lookup)) {
      if (!gh_store_set_draft(store, id, text, error))
        goto fail;
    } else if (!g_error_matches(lookup, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_propagate_error(error, g_steal_pointer(&lookup));
      goto fail;
    } else if (text) {
      /* A draft composed on another device is this account's own intent in
       * that room (as gh_store_conversations_set_draft() treats a local one). */
      if (!gh_store_ensure_conversation(store, GH_STORE_BACKEND_NIP17, room_key,
                                        GH_STORE_REQUEST_ACCEPTED, &id, error) ||
          !gh_store_set_draft(store, id, text, error))
        goto fail;
    }
  }
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO draft_sync_state(room_key, last_event_id, last_created_at, expires_at, cleared_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5) "
    "ON CONFLICT(room_key) DO UPDATE SET last_event_id = ?2, last_created_at = ?3, "
    "expires_at = MAX(expires_at, ?4), cleared_at = MAX(cleared_at, ?5)", error);
  if (!stmt)
    goto fail;
  gint64 cleared = apply_text && !text ? created_at : 0;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, event_id) ||
      sqlite3_bind_int64(stmt, 3, created_at) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 4, expires_at) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 5, cleared) != SQLITE_OK) {
    bind_failed(store, stmt, error);
    goto fail;
  }
  if (!run(store, stmt, error))
    goto fail;
  return gh_store_commit(store, error);
fail:
  gh_store_rollback(store);
  return FALSE;
}

/* ---- Publications ---------------------------------------------------------------------- */

gboolean
gh_store_drafts_save_publication(GhStore *store, const gchar *room_key, const gchar *d,
                                 gint64 local_revision, const gchar *event_json,
                                 const gchar *const *targets, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && d != NULL && event_json != NULL, FALSE);
  g_autofree gchar *targets_json = json_strv(targets);
  sqlite3_stmt *stmt = prepare(store,
    "INSERT OR REPLACE INTO draft_publications(room_key, d, local_revision, event_json, "
    "targets_json, outcomes_json, state) VALUES (?1, ?2, ?3, ?4, ?5, '{}', 1)", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, d) ||
      sqlite3_bind_int64(stmt, 3, local_revision) != SQLITE_OK ||
      !bind_text(stmt, 4, event_json) || !bind_text(stmt, 5, targets_json))
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

gboolean
gh_store_drafts_set_outcomes(GhStore *store, const gchar *room_key, const gchar *d,
                             const gchar *outcomes_json, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && d != NULL && outcomes_json != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "UPDATE draft_publications SET outcomes_json = ?3 WHERE room_key = ?1 AND d = ?2", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, d) || !bind_text(stmt, 3, outcomes_json))
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

gboolean
gh_store_drafts_delete_publication(GhStore *store, const gchar *room_key, const gchar *d,
                                   GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && d != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "DELETE FROM draft_publications WHERE room_key = ?1 AND d = ?2", error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, d))
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

gboolean
gh_store_drafts_accept(GhStore *store, const gchar *room_key, const gchar *d,
                       gint64 local_revision, const gchar *event_id, gint64 created_at,
                       gint64 expires_at, gboolean *out_current, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && d != NULL && event_id != NULL, FALSE);
  if (out_current)
    *out_current = FALSE;
  if (!gh_store_begin_named(store, "drafts", error))
    return FALSE;
  if (!gh_store_drafts_delete_publication(store, room_key, d, error))
    goto fail;
  sqlite3_stmt *stmt = prepare(store,
    "UPDATE draft_sync_state SET dirty = 0, last_event_id = ?3, last_created_at = ?4, "
    "expires_at = ?5 WHERE room_key = ?1 AND local_revision = ?2", error);
  if (!stmt)
    goto fail;
  if (!bind_text(stmt, 1, room_key) || sqlite3_bind_int64(stmt, 2, local_revision) != SQLITE_OK ||
      !bind_text(stmt, 3, event_id) || sqlite3_bind_int64(stmt, 4, created_at) != SQLITE_OK ||
      sqlite3_bind_int64(stmt, 5, expires_at) != SQLITE_OK) {
    bind_failed(store, stmt, error);
    goto fail;
  }
  if (!run(store, stmt, error))
    goto fail;
  gboolean current = sqlite3_changes(gh_store_get_db(store)) > 0;
  if (!gh_store_commit(store, error))
    return FALSE;
  if (out_current)
    *out_current = current;
  return TRUE;
fail:
  gh_store_rollback(store);
  return FALSE;
}

GPtrArray *
gh_store_drafts_list_publications(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT room_key, d, local_revision, event_json, targets_json, outcomes_json, state "
    "FROM draft_publications ORDER BY room_key, d", error);
  if (!stmt)
    return NULL;
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify) gh_store_draft_publication_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    GhStoreDraftPublication *row = g_new0(GhStoreDraftPublication, 1);
    row->room_key = g_strdup(column_text(stmt, 0));
    row->d = g_strdup(column_text(stmt, 1));
    row->local_revision = sqlite3_column_int64(stmt, 2);
    row->event_json = g_strdup(column_text(stmt, 3) ? column_text(stmt, 3) : "");
    row->targets = parse_strv(column_text(stmt, 4) ? column_text(stmt, 4) : "[]");
    row->outcomes_json = g_strdup(column_text(stmt, 5) ? column_text(stmt, 5) : "{}");
    row->state = sqlite3_column_int(stmt, 6);
    g_ptr_array_add(rows, row);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Listing draft publications", error);
    g_ptr_array_unref(rows);
    return NULL;
  }
  return rows;
}

gboolean
gh_store_drafts_delete_unsent(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return gh_store_exec(store, "DELETE FROM draft_publications WHERE state = 1", error);
}

/* ---- Conflict copies ------------------------------------------------------------------- */

gboolean
gh_store_drafts_add_conflict(GhStore *store, const gchar *room_key, const gchar *text,
                             const gchar *source_event_id, gint64 saved_at, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(room_key != NULL && text != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store,
    "INSERT INTO draft_conflicts(room_key, text, source_event_id, saved_at) VALUES (?1, ?2, ?3, ?4)",
    error);
  if (!stmt)
    return FALSE;
  if (!bind_text(stmt, 1, room_key) || !bind_text(stmt, 2, text) ||
      !bind_text(stmt, 3, source_event_id) || sqlite3_bind_int64(stmt, 4, saved_at) != SQLITE_OK)
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}

GPtrArray *
gh_store_drafts_list_conflicts(GhStore *store, const gchar *room_key, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = prepare(store,
    "SELECT id, room_key, text, source_event_id, saved_at FROM draft_conflicts "
    "WHERE ?1 IS NULL OR room_key = ?1 ORDER BY saved_at DESC, id DESC", error);
  if (!stmt)
    return NULL;
  if (!bind_text(stmt, 1, room_key)) {
    bind_failed(store, stmt, error);
    return NULL;
  }
  GPtrArray *rows = g_ptr_array_new_with_free_func((GDestroyNotify) gh_store_draft_conflict_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    GhStoreDraftConflict *row = g_new0(GhStoreDraftConflict, 1);
    row->id = sqlite3_column_int64(stmt, 0);
    row->room_key = g_strdup(column_text(stmt, 1));
    row->text = g_strdup(column_text(stmt, 2));
    row->source_event_id = g_strdup(column_text(stmt, 3));
    row->saved_at = sqlite3_column_int64(stmt, 4);
    g_ptr_array_add(rows, row);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Listing draft conflicts", error);
    g_ptr_array_unref(rows);
    return NULL;
  }
  return rows;
}

gboolean
gh_store_drafts_delete_conflict(GhStore *store, gint64 id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  sqlite3_stmt *stmt = prepare(store, "DELETE FROM draft_conflicts WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK)
    return bind_failed(store, stmt, error);
  return run(store, stmt, error);
}
