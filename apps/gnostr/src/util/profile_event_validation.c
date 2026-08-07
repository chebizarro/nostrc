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

  NostrEvent *evt = nostr_event_new();
  if (!evt)
    return set_reason(out_reason, "deserialize failed");

  NostrEventValidationStatus parse_status =
      nostr_event_deserialize_signed(evt, event_json, NULL);
  if (parse_status != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(evt);
    return set_reason(out_reason, "deserialize failed");
  }

  if (nostr_event_get_kind(evt) != 0) {
    nostr_event_free(evt);
    return set_reason(out_reason, "unexpected kind (expected 0)");
  }

  NostrEventValidationStatus validation_status =
      nostr_event_validate(evt, NULL);
  if (validation_status != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(evt);
    if (validation_status == NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH)
      return set_reason(out_reason, "canonical id mismatch");
    if (validation_status == NOSTR_EVENT_VALIDATION_BAD_ID)
      return set_reason(out_reason, "missing or invalid id");
    if (validation_status == NOSTR_EVENT_VALIDATION_BAD_PUBKEY)
      return set_reason(out_reason, "missing or invalid pubkey");
    return set_reason(out_reason, "invalid signature");
  }

  const gchar *pubkey_hex = nostr_event_get_pubkey(evt);

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
