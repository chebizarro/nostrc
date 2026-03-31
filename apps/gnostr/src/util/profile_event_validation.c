#include "profile_event_validation.h"

#include <nostr-event.h>
#include <json.h>
#include <nostr-gobject-1.0/nostr_json.h>

#include <stdlib.h>

static gboolean
set_reason(gchar **out_reason, const gchar *reason)
{
  if (out_reason)
    *out_reason = g_strdup(reason);
  return FALSE;
}

static gboolean
hex_string_is_valid(const gchar *hex, gsize expected_len)
{
  if (!hex || strlen(hex) != expected_len)
    return FALSE;

  for (gsize i = 0; i < expected_len; i++) {
    if (!g_ascii_isxdigit(hex[i]))
      return FALSE;
  }

  return TRUE;
}

gboolean
gnostr_profile_event_extract_for_apply(const gchar *event_json,
                                       gchar      **out_pubkey_hex,
                                       gchar      **out_content_json,
                                       gint64      *out_created_at,
                                       gchar      **out_reason)
{
  g_return_val_if_fail(out_pubkey_hex != NULL, FALSE);
  g_return_val_if_fail(out_content_json != NULL, FALSE);

  *out_pubkey_hex = NULL;
  *out_content_json = NULL;
  if (out_created_at)
    *out_created_at = 0;
  if (out_reason)
    *out_reason = NULL;

  if (!event_json || !*event_json)
    return set_reason(out_reason, "empty event json");

  if (!gnostr_json_has_key(event_json, "tags"))
    return set_reason(out_reason, "missing tags field");

  g_autofree gchar *raw_id = gnostr_json_get_string(event_json, "id", NULL);
  if (!hex_string_is_valid(raw_id, 64))
    return set_reason(out_reason, "missing or invalid id");

  NostrEvent *evt = nostr_event_new();
  if (!evt)
    return set_reason(out_reason, "deserialize failed");

  if (nostr_event_deserialize(evt, event_json) != 0) {
    nostr_event_free(evt);
    return set_reason(out_reason, "deserialize failed");
  }

  if (nostr_event_get_kind(evt) != 0) {
    nostr_event_free(evt);
    return set_reason(out_reason, "unexpected kind (expected 0)");
  }

  char *saved_id = evt->id;
  evt->id = NULL;
  char *canonical_id = nostr_event_get_id(evt);
  evt->id = saved_id;

  if (!hex_string_is_valid(canonical_id, 64) ||
      g_ascii_strcasecmp(raw_id, canonical_id) != 0) {
    free(canonical_id);
    nostr_event_free(evt);
    return set_reason(out_reason, "canonical id mismatch");
  }

  free(canonical_id);

  const gchar *pubkey_hex = nostr_event_get_pubkey(evt);
  if (!hex_string_is_valid(pubkey_hex, 64)) {
    nostr_event_free(evt);
    return set_reason(out_reason, "missing or invalid pubkey");
  }

  if (!nostr_event_check_signature(evt)) {
    nostr_event_free(evt);
    return set_reason(out_reason, "invalid signature");
  }

  const gchar *content_json = nostr_event_get_content(evt);
  if (!content_json) {
    nostr_event_free(evt);
    return set_reason(out_reason, "missing content");
  }

  *out_pubkey_hex = g_ascii_strdown(pubkey_hex, -1);
  *out_content_json = g_strdup(content_json);
  if (out_created_at)
    *out_created_at = nostr_event_get_created_at(evt);
  nostr_event_free(evt);
  return TRUE;
}
