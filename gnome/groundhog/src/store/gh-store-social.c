#include "gh-store-social.h"

#include <jansson.h>
#include <sqlite3.h>
#include <string.h>
#include <nostr-kinds.h>

static gboolean
fail(GError **error, GhStoreError code, const gchar *message)
{
  g_set_error_literal(error, GH_STORE_ERROR, code, message);
  return FALSE;
}

static sqlite3_stmt *
prepare(GhStore *store, const gchar *sql, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  gint rc = sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    return stmt;
  sqlite3_finalize(stmt);
  gh_store_set_sqlite_error(store, rc, "Preparing social storage", error);
  return NULL;
}

static gboolean
run(GhStore *store, sqlite3_stmt *stmt, GError **error)
{
  gint rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE || gh_store_set_sqlite_error(store, rc, "Saving social storage", error);
}

static gboolean
writable(GhStore *store, GError **error)
{
  if (gh_store_is_read_only(store))
    return fail(error, GH_STORE_ERROR_CORRUPT, "Account storage is read-only");
  if (gh_store_get_transaction_depth(store))
    return fail(error, GH_STORE_ERROR_STATE, "Social edits require their own transaction");
  return TRUE;
}

void
gh_store_social_identity_free(GhStoreSocialIdentity *identity)
{
  g_free(identity->identifier);
  g_free(identity);
}

GPtrArray *
gh_store_social_list(GhStore *store, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT kind,identifier FROM social_documents ORDER BY kind,identifier", error);
  if (!stmt)
    return NULL;
  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_social_identity_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    GhStoreSocialIdentity *id = g_new0(GhStoreSocialIdentity, 1);
    id->kind = sqlite3_column_int(stmt, 0);
    id->identifier = g_strdup((const gchar *)sqlite3_column_text(stmt, 1));
    g_ptr_array_add(result, id);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Reading social documents", error);
    g_ptr_array_unref(result);
    return NULL;
  }
  return result;
}

gboolean
gh_store_social_get_preferences(GhStore *store, GhStoreSocialPreferences *out, GError **error)
{
  *out = (GhStoreSocialPreferences){0};
  sqlite3_stmt *stmt = prepare(store, "SELECT sync_enabled,blossom_imported FROM social_preferences WHERE id=1", error);
  if (!stmt)
    return FALSE;
  gint rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    out->sync_enabled = sqlite3_column_int(stmt, 0) != 0;
    out->blossom_imported = sqlite3_column_int(stmt, 1) != 0;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_ROW)
    return TRUE;
  return fail(error, GH_STORE_ERROR_CORRUPT, "Account social preferences are unavailable");
}

GStrv
gh_store_social_dup_servers(GhStore *store, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT url FROM social_servers ORDER BY position,url", error);
  if (!stmt) return NULL;
  g_autoptr(GStrvBuilder) builder = g_strv_builder_new();
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    g_strv_builder_add(builder, (const gchar *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Reading account attachment servers", error);
    return NULL;
  }
  return g_strv_builder_end(builder);
}

static gboolean
import_generation_valid(gpointer data, const gchar *account, guint64 generation)
{
  return generation == 1 && !g_strcmp0(account, gh_store_get_account_pubkey(data));
}
static gchar *import_load(gpointer data, gint kind, const gchar *identifier, GError **error)
{
  return gh_store_social_load(data, kind, identifier, error);
}

static gboolean
import_commit(gpointer data, GNostrListDocument *snapshot, const gchar *state, GError **error)
{
  return gh_store_social_commit(data, snapshot, state, error);
}

gboolean
gh_store_social_import_blossom_legacy(GhStore *store, const gchar *const *urls, GError **error)
{
  GhStoreSocialPreferences prefs;
  if (!gh_store_social_get_preferences(store, &prefs, error)) return FALSE;
  if (prefs.blossom_imported) return TRUE;
  if (!writable(store, error)) return FALSE;
  sqlite3_stmt *stmt = prepare(store,
    "SELECT EXISTS(SELECT 1 FROM social_documents WHERE kind=?1) OR EXISTS(SELECT 1 FROM social_servers)", error);
  if (!stmt) return FALSE;
  sqlite3_bind_int(stmt, 1, NOSTR_KIND_BLOSSOM_SERVER_LIST);
  gint rc = sqlite3_step(stmt);
  gboolean existing = rc == SQLITE_ROW && sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW)
    return gh_store_set_sqlite_error(store, rc, "Checking legacy server import", error);
  if (existing) {
    if (!gh_store_begin_named(store, "blossom-import", error)) return FALSE;
    if (!gh_store_exec(store, "UPDATE social_preferences SET blossom_imported=1 WHERE id=1", error)) {
      gh_store_rollback(store);
      return FALSE;
    }
    return gh_store_commit(store, error);
  }
  /* Use the shared engine to produce its replay state, never manufacture JSON
   * that pretends to be an engine snapshot. No signer or transport is installed. */
  const GNostrListAdapter adapter = { .generation_valid = import_generation_valid,
    .load = import_load, .commit_snapshot = import_commit };
  g_autoptr(GNostrListService) service = gnostr_list_service_new(
    gh_store_get_account_pubkey(store), 1, &adapter, store, NULL);
  const GNostrListReadPlanEntry plan = { .kind = NOSTR_KIND_BLOSSOM_SERVER_LIST,
    .identifier = "", .publish_policy = GNOSTR_LIST_EXPLICIT_ONLY };
  if (!gnostr_list_service_start(service, &plan, 1, error)) {
    gnostr_list_service_close(service);
    return FALSE;
  }
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  for (guint i = 0; urls && urls[i]; i++) {
    const gchar *fields[] = { "server", urls[i] };
    GNostrListOperation *op = gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, FALSE, fields, 2, 0, 0);
    if (!op) {
      gnostr_list_service_close(service);
      return fail(error, GH_STORE_ERROR_INVALID, "Invalid legacy attachment server");
    }
    g_ptr_array_add(ops, op);
  }
  g_autoptr(GNostrListDocument) saved = gnostr_list_service_apply(service,
    NOSTR_KIND_BLOSSOM_SERVER_LIST, "", 0, ops, error);
  /* A queued engine wake must not retain a usable borrowed store. */
  gnostr_list_service_close(service);
  return saved != NULL;
}

gboolean
gh_store_social_set_sync_enabled(GhStore *store, gboolean enabled, GError **error)
{
  if (!writable(store, error) || !gh_store_begin_named(store, "social-preferences", error))
    return FALSE;
  sqlite3_stmt *stmt = prepare(store, "UPDATE social_preferences SET sync_enabled=?1 WHERE id=1", error);
  if (!stmt)
    goto rollback;
  sqlite3_bind_int(stmt, 1, !!enabled);
  if (!run(store, stmt, error))
    goto rollback;
  if (!enabled && !gh_store_exec(store, "DELETE FROM social_publications", error))
    goto rollback;
  return gh_store_commit(store, error);
rollback:
  gh_store_rollback(store);
  return FALSE;
}

gchar *
gh_store_social_load(GhStore *store, gint kind, const gchar *identifier, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT decoded_json FROM social_documents WHERE kind=?1 AND identifier=?2", error);
  if (!stmt)
    return NULL;
  sqlite3_bind_int(stmt, 1, kind);
  sqlite3_bind_text(stmt, 2, identifier, -1, SQLITE_STATIC);
  gint rc = sqlite3_step(stmt);
  gchar *result = rc == SQLITE_ROW ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    gh_store_set_sqlite_error(store, rc, "Loading social state", error);
  return result;
}

static gchar *
normalized(const gchar *name, const gchar *value)
{
  if (!strcmp(name, "p") || !strcmp(name, "e"))
    return g_ascii_strdown(value, -1);
  if (!strcmp(name, "word") || !strcmp(name, "t")) {
    if (!strcmp(name, "t") && *value == '#')
      value++;
    g_autofree gchar *fold = g_utf8_casefold(value, -1);
    return g_utf8_normalize(fold, -1, G_NORMALIZE_NFC);
  }
  return g_strdup(value);
}

static GBytes *
entry_key(const gchar *name, const gchar *value)
{
  g_autofree gchar *norm = normalized(name, value);
  gsize a = strlen(name), b = strlen(norm);
  gchar *bytes = g_malloc(a + 1 + b);
  memcpy(bytes, name, a + 1);
  memcpy(bytes + a + 1, norm, b);
  return g_bytes_new_take(bytes, a + 1 + b);
}

static void
bind_key(sqlite3_stmt *stmt, gint column, GBytes *key)
{
  gsize size;
  gconstpointer data = g_bytes_get_data(key, &size);
  sqlite3_bind_blob(stmt, column, data, (gint)size, SQLITE_TRANSIENT);
}

static gboolean
indexes(GhStore *store, GNostrListDocument *snapshot, GError **error)
{
  gint kind = gnostr_list_document_get_kind(snapshot);
  const gchar *sql = kind == NOSTR_KIND_FOLLOW_LIST ? "DELETE FROM social_follows" :
    kind == NOSTR_KIND_MUTE_LIST ? "DELETE FROM social_mutes" :
    kind == NOSTR_KIND_BLOSSOM_SERVER_LIST ? "DELETE FROM social_servers" : NULL;
  if (!sql)
    return TRUE;
  if (!gh_store_exec(store, sql, error))
    return FALSE;
  const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
  for (guint p = 0; p < 2; p++) {
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      const gchar *name = nostr_nip51_tag_field(tag, 0), *value = nostr_nip51_tag_field(tag, 1);
      if (!value || nostr_nip51_validate_item(nostr_nip51_schema_lookup(kind), tag) != NOSTR_LIST_ITEM_VALID)
        continue;
      sqlite3_stmt *stmt = NULL;
      if (kind == NOSTR_KIND_FOLLOW_LIST && !strcmp(name, "p")) {
        stmt = prepare(store, "INSERT OR IGNORE INTO social_follows(pubkey,relay_hint,petname,position) VALUES(?1,?2,?3,?4)", error);
        if (!stmt)
          return FALSE;
        g_autofree gchar *key = normalized(name, value);
        sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, nostr_nip51_tag_field(tag, 2), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, nostr_nip51_tag_field(tag, 3), -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, i);
      } else if (kind == NOSTR_KIND_MUTE_LIST &&
                 (!strcmp(name, "p") || !strcmp(name, "e") || !strcmp(name, "word") || !strcmp(name, "t"))) {
        /* Iterate public first: a duplicate private entry always wins. */
        stmt = prepare(store, "INSERT INTO social_mutes(entry_key,tag_name,value,position,is_private) VALUES(?1,?2,?3,?4,?5) ON CONFLICT(entry_key) DO UPDATE SET position=excluded.position,is_private=excluded.is_private", error);
        if (!stmt)
          return FALSE;
        g_autoptr(GBytes) key = entry_key(name, value);
        bind_key(stmt, 1, key);
        sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
        g_autofree gchar *norm = normalized(name, value);
        sqlite3_bind_text(stmt, 3, norm, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, i);
        sqlite3_bind_int(stmt, 5, p);
      } else if (kind == NOSTR_KIND_BLOSSOM_SERVER_LIST && !strcmp(name, "server")) {
        stmt = prepare(store, "INSERT OR IGNORE INTO social_servers(url,position) VALUES(?1,?2)", error);
        if (!stmt)
          return FALSE;
        sqlite3_bind_text(stmt, 1, value, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, i);
      }
      if (stmt && !run(store, stmt, error))
        return FALSE;
    }
  }
  return TRUE;
}

static gboolean
edits(GhStore *store, GNostrListDocument *snapshot, json_t *state, GError **error)
{
  gint kind = gnostr_list_document_get_kind(snapshot);
  const gchar *identifier = gnostr_list_document_get_identifier(snapshot);
  sqlite3_stmt *stmt = prepare(store, "DELETE FROM social_edits WHERE kind=?1 AND identifier=?2", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_int(stmt, 1, kind);
  sqlite3_bind_text(stmt, 2, identifier, -1, SQLITE_STATIC);
  if (!run(store, stmt, error))
    return FALSE;
  json_t *operations = json_object_get(state, "operations"), *op;
  size_t i;
  json_array_foreach(operations, i, op) {
    g_autofree gchar *wire = json_dumps(op, JSON_COMPACT);
    stmt = prepare(store, "INSERT INTO social_edits(kind,identifier,base_event_id,operation_json) VALUES(?1,?2,?3,?4)", error);
    if (!stmt)
      return FALSE;
    sqlite3_bind_int(stmt, 1, kind);
    sqlite3_bind_text(stmt, 2, identifier, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, gnostr_list_document_get_event_id(snapshot), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, wire, -1, SQLITE_STATIC);
    if (!run(store, stmt, error))
      return FALSE;
    if (json_integer_value(json_object_get(op, "type")) == GNOSTR_LIST_EDIT_REMOVE) {
      json_t *tag = json_object_get(op, "tag");
      const gchar *name = json_string_value(json_array_get(tag, 0));
      const gchar *value = json_string_value(json_array_get(tag, 1));
      if (!name || !value)
        continue;
      g_autoptr(GBytes) key = entry_key(name, value);
      stmt = prepare(store, "INSERT OR IGNORE INTO social_removals(kind,identifier,entry_key,removed_at,local_revision) VALUES(?1,?2,?3,?4,?5)", error);
      if (!stmt)
        return FALSE;
      sqlite3_bind_int(stmt, 1, kind);
      sqlite3_bind_text(stmt, 2, identifier, -1, SQLITE_STATIC);
      bind_key(stmt, 3, key);
      sqlite3_bind_int64(stmt, 4, gh_clock_get_unix(gh_store_get_clock(store)));
      sqlite3_bind_int64(stmt, 5, gnostr_list_document_get_local_revision(snapshot));
      if (!run(store, stmt, error))
        return FALSE;
    }
  }
  return TRUE;
}

gboolean
gh_store_social_commit(GhStore *store, GNostrListDocument *snapshot, const gchar *wire, GError **error)
{
  if (!writable(store, error))
    return FALSE;
  if (!wire || strlen(wire) > GH_STORE_SOCIAL_MAX_BYTES)
    return fail(error, GH_STORE_ERROR_FULL, "Account list storage limit reached");
  if (g_strcmp0(gh_store_get_account_pubkey(store), gnostr_list_document_get_author(snapshot)))
    return fail(error, GH_STORE_ERROR_FOREIGN, "Social state belongs to a different account");
  json_t *state = json_loads(wire, JSON_REJECT_DUPLICATES, NULL);
  gboolean valid = json_is_object(state) && json_integer_value(json_object_get(state, "version")) == 1 &&
    !g_strcmp0(json_string_value(json_object_get(state, "account")), gh_store_get_account_pubkey(store)) &&
    json_is_array(json_object_get(state, "operations"));
  if (!valid) {
    json_decref(state);
    return fail(error, GH_STORE_ERROR_INVALID, "Invalid social replay state");
  }
  if (!gh_store_begin_named(store, "social", error)) {
    json_decref(state);
    return FALSE;
  }
  sqlite3_stmt *stmt = prepare(store, "INSERT INTO social_documents(kind,identifier,event_id,created_at,event_json,decoded_json,private_state,local_revision,dirty) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9) ON CONFLICT(kind,identifier) DO UPDATE SET event_id=excluded.event_id,created_at=excluded.created_at,event_json=excluded.event_json,decoded_json=excluded.decoded_json,private_state=excluded.private_state,local_revision=excluded.local_revision,dirty=excluded.dirty", error);
  if (!stmt)
    goto rollback;
  sqlite3_bind_int(stmt, 1, gnostr_list_document_get_kind(snapshot));
  sqlite3_bind_text(stmt, 2, gnostr_list_document_get_identifier(snapshot), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 3, gnostr_list_document_get_event_id(snapshot), -1, SQLITE_STATIC);
  sqlite3_bind_int64(stmt, 4, gnostr_list_document_get_created_at(snapshot));
  sqlite3_bind_text(stmt, 5, gnostr_list_document_get_source_json(snapshot), -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 6, wire, -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 7, gnostr_list_document_get_private_state(snapshot));
  sqlite3_bind_int64(stmt, 8, gnostr_list_document_get_local_revision(snapshot));
  sqlite3_bind_int(stmt, 9, gnostr_list_document_get_dirty(snapshot));
  if (!run(store, stmt, error) || !edits(store, snapshot, state, error) || !indexes(store, snapshot, error))
    goto rollback;
  stmt = prepare(store,
    "SELECT (SELECT COALESCE(SUM(length(CAST(identifier AS BLOB))+COALESCE(length(CAST(event_json AS BLOB)),0)+COALESCE(length(CAST(decoded_json AS BLOB)),0)),0) FROM social_documents) +"
    " (SELECT COALESCE(SUM(length(CAST(operation_json AS BLOB))),0) FROM social_edits)", error);
  if (!stmt)
    goto rollback;
  gint rc = sqlite3_step(stmt);
  gint64 bytes = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : G_MAXINT64;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW) {
    gh_store_set_sqlite_error(store, rc, "Checking social storage bound", error);
    goto rollback;
  }
  if (bytes > GH_STORE_SOCIAL_MAX_BYTES) {
    fail(error, GH_STORE_ERROR_FULL, "Account list storage limit reached");
    goto rollback;
  }
  if (gnostr_list_document_get_kind(snapshot) == NOSTR_KIND_BLOSSOM_SERVER_LIST &&
      !gh_store_exec(store, "UPDATE social_preferences SET blossom_imported=1 WHERE id=1", error))
    goto rollback;
  json_decref(state);
  return gh_store_commit(store, error);
rollback:
  json_decref(state);
  gh_store_rollback(store);
  return FALSE;
}

/* ---- social_event_cache (schema v14; badges, nostrc-9lwl7.13) ---- */

gboolean
gh_store_social_cache_put(GhStore *store, const gchar *event_id, gint kind, const gchar *author,
                          const gchar *identifier, const gchar *event_json, gint64 now,
                          GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
    "INSERT OR REPLACE INTO social_event_cache(event_id,kind,author,identifier,event_json,stored_at)"
    " VALUES(?,?,?,?,?,?)", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, kind);
  sqlite3_bind_text(stmt, 3, author, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 4, identifier ? identifier : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 5, event_json, -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 6, now);
  return run(store, stmt, error);
}

gchar *
gh_store_social_cache_get(GhStore *store, const gchar *event_id, GError **error)
{
  sqlite3_stmt *stmt = prepare(store, "SELECT event_json FROM social_event_cache WHERE event_id=?", error);
  if (!stmt)
    return NULL;
  sqlite3_bind_text(stmt, 1, event_id, -1, SQLITE_TRANSIENT);
  gint rc = sqlite3_step(stmt);
  gchar *json = rc == SQLITE_ROW ? g_strdup((const gchar *)sqlite3_column_text(stmt, 0)) : NULL;
  sqlite3_finalize(stmt);
  if (rc != SQLITE_ROW && rc != SQLITE_DONE)
    gh_store_set_sqlite_error(store, rc, "Reading the social event cache", error);
  return json;
}

GPtrArray *
gh_store_social_cache_find(GhStore *store, gint kind, const gchar *author, const gchar *identifier,
                           GError **error)
{
  sqlite3_stmt *stmt = prepare(store,
    "SELECT event_json FROM social_event_cache WHERE kind=?1 AND (?2 IS NULL OR author=?2)"
    " AND (?3 IS NULL OR identifier=?3)"
    " ORDER BY stored_at DESC, event_id ASC", error);
  if (!stmt)
    return NULL;
  sqlite3_bind_int(stmt, 1, kind);
  if (author)
    sqlite3_bind_text(stmt, 2, author, -1, SQLITE_TRANSIENT);
  if (identifier)
    sqlite3_bind_text(stmt, 3, identifier, -1, SQLITE_TRANSIENT);
  GPtrArray *rows = g_ptr_array_new_with_free_func(g_free);
  gint rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
    g_ptr_array_add(rows, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Reading the social event cache", error);
    g_ptr_array_unref(rows);
    return NULL;
  }
  return rows;
}
