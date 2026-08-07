#include "dm_gift_wrap_validation.h"

#include "nostr-event.h"
#include "json.h"
#include "nostr-kinds.h"

static gboolean
set_reason(gchar **out_reason, const gchar *reason)
{
  if (out_reason)
    *out_reason = g_strdup(reason);
  return FALSE;
}

gboolean
gnostr_dm_gift_wrap_parse_for_processing(const gchar *gift_wrap_json,
                                         NostrEvent **out_gift_wrap,
                                         gchar      **out_reason)
{
  g_return_val_if_fail(out_gift_wrap != NULL, FALSE);

  *out_gift_wrap = NULL;
  if (out_reason)
    *out_reason = NULL;

  if (!gift_wrap_json || !*gift_wrap_json)
    return set_reason(out_reason, "empty gift wrap json");

  NostrEvent *gift_wrap = nostr_event_new();
  if (!gift_wrap)
    return set_reason(out_reason, "failed to allocate event");

  NostrEventValidationStatus parse_status =
      nostr_event_deserialize_signed(gift_wrap, gift_wrap_json, NULL);
  if (parse_status != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(gift_wrap);
    return set_reason(out_reason, "deserialize failed");
  }

  if (nostr_event_get_kind(gift_wrap) != NOSTR_KIND_GIFT_WRAP) {
    gint kind = nostr_event_get_kind(gift_wrap);
    nostr_event_free(gift_wrap);
    if (out_reason)
      *out_reason = g_strdup_printf("unexpected kind %d", kind);
    return FALSE;
  }

  NostrEventValidationStatus validation_status =
      nostr_event_validate(gift_wrap, NULL);
  if (validation_status != NOSTR_EVENT_VALIDATION_OK) {
    nostr_event_free(gift_wrap);
    if (validation_status == NOSTR_EVENT_VALIDATION_CANONICAL_ID_MISMATCH)
      return set_reason(out_reason, "canonical id mismatch");
    return set_reason(out_reason, "invalid signature");
  }

  *out_gift_wrap = gift_wrap;
  return TRUE;
}
