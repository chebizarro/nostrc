#include "gh-store-public-notes.h"
#include <nostr-event.h>
#include <nostr-tag.h>
#include <sqlite3.h>
#include <string.h>

#define MAX_PUBLIC_EVENT (64u * 1024u)
#define MAX_PUBLIC_NOTES 1024

static gboolean
invalid(GError **error, const gchar *reason)
{
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, reason);
  return FALSE;
}

void
gh_public_note_free(GhPublicNote *note)
{
  if (!note) return;
  g_free(note->id);
  g_free(note->pubkey);
  g_free(note->dtag);
  g_free(note->content);
  g_free(note->preview);
  g_free(note->event_json);
  g_free(note);
}

GhPublicNote *
gh_public_note_from_signed_json(const gchar *json, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!json || strlen(json) > MAX_PUBLIC_EVENT || !g_utf8_validate(json, -1, NULL)) {
    invalid(error, "Public event is missing, too large, or not UTF-8");
    return NULL;
  }
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_signed(event, json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
      nostr_event_validate(event, NULL) != NOSTR_EVENT_VALIDATION_OK) {
    if (event) nostr_event_free(event);
    invalid(error, "Public event has an invalid ID or signature");
    return NULL;
  }
  gint kind = nostr_event_get_kind(event);
  /* Public note and addressable-note authoring only. This allowlist excludes
   * NIP-59 seals (13), private rumors/files (14/15), gift wraps (1059),
   * encrypted group routing (445), and future private transport kinds. */
  if (kind != 1 && (kind < 30000 || kind >= 40000)) {
    nostr_event_free(event);
    invalid(error, "This event kind is not a public note");
    return NULL;
  }
  GhPublicNote *note = g_new0(GhPublicNote, 1);
  note->id = nostr_event_get_id(event);
  note->pubkey = g_strdup(nostr_event_get_pubkey(event));
  note->kind = kind;
  note->created_at = nostr_event_get_created_at(event);
  note->content = g_strdup(nostr_event_get_content(event));
  g_autofree gchar *valid = g_utf8_make_valid(note->content ? note->content : "", -1);
  note->preview = g_utf8_substring(valid, 0, 180);
  g_strdelimit(note->preview, "\r\n\t", ' ');
  note->event_json = g_strdup(json);
  const NostrTags *tags = nostr_event_get_tags(event);
  for (gsize i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (nostr_tag_size(tag) >= 2 && g_strcmp0(nostr_tag_get(tag, 0), "d") == 0) {
      note->dtag = g_strdup(nostr_tag_get(tag, 1));
      break;
    }
  }
  nostr_event_free(event);
  return note;
}

gboolean
gh_store_public_note_put(GhStore *store, const GhPublicNote *note, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (!note || !note->event_json || gh_store_is_read_only(store))
    return invalid(error, "No writable store or public note");
  g_autoptr(GhPublicNote) verified = gh_public_note_from_signed_json(note->event_json, error);
  if (!verified) return FALSE;
  if (g_strcmp0(verified->id, note->id) != 0 ||
      g_strcmp0(verified->pubkey, note->pubkey) != 0 ||
      verified->kind != note->kind)
    return invalid(error, "Public-note fields do not match the signed event");
  if (!gh_store_begin(store, error)) return FALSE;
  sqlite3 *db = gh_store_get_db(store);
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db,
    "INSERT INTO public_notes(id,pubkey,kind,dtag,created_at,event_json,stored_at) "
    "VALUES(?,?,?,?,?,?,?) ON CONFLICT(id) DO UPDATE SET stored_at=excluded.stored_at",
    -1, &stmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(stmt, 1, verified->id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, verified->pubkey, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, verified->kind);
    if (verified->dtag) sqlite3_bind_text(stmt, 4, verified->dtag, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 4);
    sqlite3_bind_int64(stmt, 5, verified->created_at);
    sqlite3_bind_text(stmt, 6, verified->event_json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, g_get_real_time() / G_USEC_PER_SEC);
    rc = sqlite3_step(stmt);
  }
  if (stmt) sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    gh_store_set_sqlite_error(store, rc, "Save public note", error);
    gh_store_rollback(store);
    return FALSE;
  }
  if (!gh_store_exec(store,
    "DELETE FROM public_notes WHERE id IN (SELECT id FROM public_notes "
    "ORDER BY stored_at DESC, id LIMIT -1 OFFSET 1024)", error)) {
    gh_store_rollback(store);
    return FALSE;
  }
  return gh_store_commit(store, error);
}

GPtrArray *
gh_store_public_notes_load(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3 *db = gh_store_get_db(store);
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(db, "SELECT event_json FROM public_notes ORDER BY stored_at DESC, id",
                              -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    gh_store_set_sqlite_error(store, rc, "Load public notes", error);
    return NULL;
  }
  GPtrArray *notes = g_ptr_array_new_with_free_func((GDestroyNotify)gh_public_note_free);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    const gchar *json = (const gchar *)sqlite3_column_text(stmt, 0);
    GhPublicNote *note = gh_public_note_from_signed_json(json, error);
    if (!note) break;
    g_ptr_array_add(notes, note);
  }
  sqlite3_finalize(stmt);
  if (rc != SQLITE_DONE) {
    if (!error || !*error)
      gh_store_set_sqlite_error(store, rc, "Load public notes", error);
    g_ptr_array_unref(notes);
    return NULL;
  }
  return notes;
}
