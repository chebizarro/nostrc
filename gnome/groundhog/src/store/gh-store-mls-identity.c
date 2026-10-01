#include "gh-store-mls-identity.h"

#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

/* Value: "1 <check> <checked_at> <added_by or ->", version first. */
#define RECORD_VERSION 1

static gboolean
hex_ok(const gchar *value, gsize min, gsize max)
{
  gsize n = value ? strlen(value) : 0;
  if (n < min || n > max || n % 2)
    return FALSE;
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isdigit(value[i]) && (value[i] < 'a' || value[i] > 'f'))
      return FALSE;
  return TRUE;
}

static gboolean
invalid(GError **error, const gchar *what)
{
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Encrypted group: %s", what);
  return FALSE;
}

/* GH_STORE_MLS_MEMBER_PREFIX + H(group) + "/": every device row of a group. */
static gchar *
group_prefix(const gchar *group_hex, GError **error)
{
  if (!hex_ok(group_hex, 2, 512)) {
    invalid(error, "not a group id");
    return NULL;
  }
  g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, group_hex, -1);
  return g_strconcat(GH_STORE_MLS_MEMBER_PREFIX, digest, "/", NULL);
}

static gchar *
record_key(const gchar *group_hex, const gchar *account, const gchar *signature_key,
           GError **error)
{
  if (!hex_ok(account, 64, 64) || !hex_ok(signature_key, 64, 64)) {
    invalid(error, "not an account and device key");
    return NULL;
  }
  g_autofree gchar *prefix = group_prefix(group_hex, error);
  if (!prefix)
    return NULL;
  g_autofree gchar *joined = g_strjoin(":", group_hex, account, signature_key, NULL);
  g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, joined, -1);
  return g_strconcat(prefix, digest, NULL);
}

static gchar *
refused_key(const gchar *group_hex, GError **error)
{
  if (!hex_ok(group_hex, 2, 512)) {
    invalid(error, "not a group id");
    return NULL;
  }
  g_autofree gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, group_hex, -1);
  return g_strconcat(GH_STORE_MLS_REFUSED_PREFIX, digest, NULL);
}

/* Refuses to run inside a transaction SQLite already abandoned (see
 * gh-store-conversations.c). */
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
  gh_store_set_sqlite_error(store, rc, "Preparing an encrypted-group member statement", error);
  return NULL;
}

void
gh_store_mls_member_clear(GhStoreMlsMember *member)
{
  if (!member)
    return;
  g_free(member->added_by);
  memset(member, 0, sizeof *member);
}

/* A record this module wrote; anything else reads as none. */
static gboolean
parse_record(const gchar *value, GhStoreMlsMember *out)
{
  g_auto(GStrv) parts = value ? g_strsplit(value, " ", -1) : NULL;
  if (!parts || g_strv_length(parts) != 4 || g_strcmp0(parts[0], "1") != 0)
    return FALSE;
  gchar *end = NULL;
  gint64 check = g_ascii_strtoll(parts[1], &end, 10);
  if (*end || check < GH_STORE_MLS_MEMBER_UNCHECKED || check > GH_STORE_MLS_MEMBER_NOT_FOUND)
    return FALSE;
  gint64 at = g_ascii_strtoll(parts[2], &end, 10);
  if (*end || at < 0)
    return FALSE;
  gboolean none = g_str_equal(parts[3], "-");
  if (!none && !hex_ok(parts[3], 64, 64))
    return FALSE;
  out->check = (GhStoreMlsMemberCheck)check;
  out->checked_at = at;
  out->added_by = none ? NULL : g_strdup(parts[3]);
  return TRUE;
}

gboolean
gh_store_mls_member_load(GhStore *store, const gchar *group_hex, const gchar *account,
                         const gchar *signature_key, GhStoreMlsMember *out, gboolean *found,
                         GError **error)
{
  g_return_val_if_fail(store != NULL && out != NULL && found != NULL, FALSE);
  memset(out, 0, sizeof *out);
  *found = FALSE;
  g_autofree gchar *key = record_key(group_hex, account, signature_key, error);
  if (!key)
    return FALSE;
  sqlite3_stmt *stmt = prepare(store, "SELECT value FROM meta WHERE key = ?1", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const gchar *value = (const gchar *)sqlite3_column_text(stmt, 0);
    *found = parse_record(value, out);
    rc = SQLITE_DONE;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Reading an encrypted-group member", error);
}

gboolean
gh_store_mls_member_save(GhStore *store, const gchar *group_hex, const gchar *account,
                         const gchar *signature_key, const GhStoreMlsMember *member,
                         GError **error)
{
  g_return_val_if_fail(store != NULL && member != NULL, FALSE);
  if (member->check > GH_STORE_MLS_MEMBER_NOT_FOUND || member->checked_at < 0 ||
      (member->added_by && !hex_ok(member->added_by, 64, 64))) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "Encrypted-group member: not a valid record");
    return FALSE;
  }
  if (gh_store_is_read_only(store)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT, "The store is read-only");
    return FALSE;
  }
  g_autofree gchar *key = record_key(group_hex, account, signature_key, error);
  if (!key)
    return FALSE;
  g_autofree gchar *value = g_strdup_printf("%d %d %" G_GINT64_FORMAT " %s", RECORD_VERSION,
                                            (gint)member->check, member->checked_at,
                                            member->added_by ? member->added_by : "-");
  sqlite3_stmt *stmt = prepare(store,
                               "INSERT INTO meta (key, value) VALUES (?1, ?2) "
                               "ON CONFLICT (key) DO UPDATE SET value = excluded.value",
                               error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Saving an encrypted-group member", error);
}

/* Runs `sql` with ?1 (and ?2 when given) bound. */
static gboolean
run(GhStore *store, const gchar *sql, const gchar *one, const gchar *two, const gchar *what,
    GError **error)
{
  if (gh_store_is_read_only(store)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT, "The store is read-only");
    return FALSE;
  }
  sqlite3_stmt *stmt = prepare(store, sql, error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, one, -1, SQLITE_STATIC);
  if (two)
    sqlite3_bind_text(stmt, 2, two, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, what, error);
}

gboolean
gh_store_mls_member_delete(GhStore *store, const gchar *group_hex, const gchar *account,
                           const gchar *signature_key, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *key = record_key(group_hex, account, signature_key, error);
  return key && run(store, "DELETE FROM meta WHERE key = ?1", key, NULL,
                    "Forgetting an encrypted-group member", error);
}

gboolean
gh_store_mls_member_forget_group(GhStore *store, const gchar *group_hex, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *prefix = group_prefix(group_hex, error);
  if (!prefix)
    return FALSE;
  /* Every key with the prefix sorts before the prefix with its last byte
   * ('/') raised by one ('0'). */
  g_autofree gchar *end = g_strdup(prefix);
  end[strlen(end) - 1] = '0';
  g_autofree gchar *refused = refused_key(group_hex, error);
  return refused &&
         run(store, "DELETE FROM meta WHERE key >= ?1 AND key < ?2", prefix, end,
             "Forgetting an encrypted group's members", error) &&
         run(store, "DELETE FROM meta WHERE key = ?1", refused, NULL,
             "Forgetting an encrypted group's refused change", error);
}

gboolean
gh_store_mls_refused_save(GhStore *store, const gchar *group_hex, const gchar *json,
                          guint cause, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *key = refused_key(group_hex, error);
  if (!key)
    return FALSE;
  if (!json)
    return run(store, "DELETE FROM meta WHERE key = ?1", key, NULL,
               "Forgetting an encrypted group's refused change", error);
  if (!g_utf8_validate(json, -1, NULL) || strlen(json) > 256 * 1024 || cause > 2)
    return invalid(error, "not a refused change");
  /* "1 <0|1|2> <json>": version, the cause (2 since W24b slice H). */
  g_autofree gchar *value = g_strdup_printf("1 %u %s", cause, json);
  return run(store,
             "INSERT INTO meta (key, value) VALUES (?1, ?2) "
             "ON CONFLICT (key) DO UPDATE SET value = excluded.value",
             key, value, "Saving an encrypted group's refused change", error);
}

gboolean
gh_store_mls_refused_load(GhStore *store, const gchar *group_hex, gchar **json,
                          guint *cause, GError **error)
{
  g_return_val_if_fail(store != NULL && json != NULL, FALSE);
  *json = NULL;
  if (cause)
    *cause = 0;
  g_autofree gchar *key = refused_key(group_hex, error);
  if (!key)
    return FALSE;
  sqlite3_stmt *stmt = prepare(store, "SELECT value FROM meta WHERE key = ?1", error);
  if (!stmt)
    return FALSE;
  sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    const gchar *value = (const gchar *)sqlite3_column_text(stmt, 0);
    /* Only a record this module wrote. */
    if (value && (g_str_has_prefix(value, "1 0 ") || g_str_has_prefix(value, "1 1 ") ||
                  g_str_has_prefix(value, "1 2 ")) &&
        value[4]) {
      *json = g_strdup(value + 4);
      if (cause)
        *cause = (guint)(value[2] - '0');
    }
    rc = SQLITE_DONE;
  }
  sqlite3_finalize(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Reading an encrypted group's refused change",
                                   error);
}
