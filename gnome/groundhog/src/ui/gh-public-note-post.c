#include "gh-public-note-post.h"
#include "gh-relay-scope.h"
#include <nostr-event.h>
#include <string.h>
#include <stdlib.h>

static gboolean
invalid(GError **error, const gchar *reason)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, reason);
  return FALSE;
}

void
gh_public_post_snapshot_free(GhPublicPostSnapshot *snapshot)
{
  if (!snapshot) return;
  g_free(snapshot->unsigned_json);
  g_free(snapshot->account);
  g_strfreev(snapshot->write_relays);
  g_free(snapshot);
}

static gboolean
hex64(const gchar *text)
{
  if (!text || strlen(text) != 64) return FALSE;
  for (guint i = 0; i < 64; i++) if (!g_ascii_isxdigit(text[i])) return FALSE;
  return TRUE;
}

static gboolean
reference_matches(const GnNostrReference *reference, const GhPublicNote *note)
{
  if (!reference || !note || !reference->id) return FALSE;
  if (reference->type == GN_NOSTR_REFERENCE_EVENT)
    return g_ascii_strcasecmp(reference->id, note->id) == 0;
  return reference->type == GN_NOSTR_REFERENCE_ADDRESS && reference->author && note->dtag &&
    reference->kind == note->kind &&
    g_ascii_strcasecmp(reference->author, note->pubkey) == 0 &&
    g_strcmp0(reference->id, note->dtag) == 0;
}

gboolean
gh_public_post_snapshot_matches_signed(const GhPublicPostSnapshot *snapshot,
    const gchar *signed_json, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
  if (!snapshot || !snapshot->unsigned_json || !signed_json)
    return invalid(error, "The reviewed or signed public event is missing");
  NostrEvent *unsigned_event = nostr_event_new();
  NostrEvent *signed_event = nostr_event_new();
  gboolean okay = FALSE;
  if (unsigned_event && signed_event &&
      nostr_event_deserialize_unsigned(unsigned_event, snapshot->unsigned_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_deserialize_signed(signed_event, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_validate(signed_event, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    char *expected_id = nostr_event_get_id(unsigned_event);
    char *actual_id = nostr_event_get_id(signed_event);
    okay = expected_id && actual_id && g_strcmp0(expected_id, actual_id) == 0 &&
      g_ascii_strcasecmp(nostr_event_get_pubkey(signed_event), snapshot->account) == 0;
    free(expected_id);
    free(actual_id);
  }
  if (unsigned_event) nostr_event_free(unsigned_event);
  if (signed_event) nostr_event_free(signed_event);
  if (!okay) return invalid(error, "The signer returned a different or invalid public event");
  return TRUE;
}

GhPublicPostSnapshot *
gh_public_post_snapshot_new(const GhPublicNote *original,
    const GnNostrReference *reference, gboolean quote, const gchar *comment,
    const gchar *account_pubkey, gint64 created_at,
    const gchar *const *write_relays, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!reference_matches(reference, original) || !hex64(account_pubkey) || created_at <= 0) {
    invalid(error, "A verified public note and active account are required");
    return NULL;
  }
  g_autoptr(GhPublicNote) checked = gh_public_note_from_signed_json(original->event_json, error);
  if (!checked || g_strcmp0(checked->id, original->id) != 0) return NULL;
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; write_relays && write_relays[i]; i++) {
    const gchar *url = write_relays[i];
    if (!gh_relay_url_validate(url, NULL)) continue;
    if (!g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL)) {
      if (urls->len == 16) break;
      g_ptr_array_add(urls, g_strdup(url));
    }
  }
  if (!urls->len) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "No NIP-65 write relays are available for this account");
    return NULL;
  }
  if (quote && (!comment || !*comment || strlen(comment) > 8192 ||
                !g_utf8_validate(comment, -1, NULL))) {
    invalid(error, "Write a quote of at most 8,192 UTF-8 bytes");
    return NULL;
  }
  /* A relay hint came from a private chat, not the signed public event.
   * Do not leak it in a public e/a/q tag or quoted nevent. */
  GnNostrReference target = { .type = quote ? GN_NOSTR_REFERENCE_EVENT : reference->type,
    .uri = NULL, .id = !quote && reference->type == GN_NOSTR_REFERENCE_ADDRESS
      ? checked->dtag : checked->id,
    .author = checked->pubkey, .kind = checked->kind,
    .relay_hints = NULL, .author_authenticated = TRUE };
  g_autofree gchar *template = quote
    ? gn_nostr_build_quote_template(&target, comment)
    : gn_nostr_build_repost_template(&target, checked->event_json);
  if (!template) {
    invalid(error, "Could not build a repost or quote template");
    return NULL;
  }
  NostrEvent *event = nostr_event_new();
  if (!event || nostr_event_deserialize_compact(event, template, NULL) != 1) {
    if (event) nostr_event_free(event);
    invalid(error, "The public-post template is invalid");
    return NULL;
  }
  nostr_event_set_pubkey(event, account_pubkey);
  nostr_event_set_created_at(event, created_at);
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  if (!json) {
    invalid(error, "Could not serialize the public post");
    return NULL;
  }
  GhPublicPostSnapshot *snapshot = g_new0(GhPublicPostSnapshot, 1);
  snapshot->unsigned_json = json;
  snapshot->account = g_ascii_strdown(account_pubkey, -1);
  g_ptr_array_add(urls, NULL);
  snapshot->write_relays = (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
  return snapshot;
}
