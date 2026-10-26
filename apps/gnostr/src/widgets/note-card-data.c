/* note-card-data.c — Ref-counted data bucket for NoteCardRow content fields.
 *
 * SPDX-License-Identifier: MIT
 * nostrc-ncr-lifecycle: NoteCardRow lifecycle hardening Phase 5
 */

#include "note-card-data.h"

NoteCardData *
note_card_data_new (void)
{
  NoteCardData *data = g_new0 (NoteCardData, 1);
  g_atomic_ref_count_init (&data->ref_count);
  return data;
}

NoteCardData *
note_card_data_ref (NoteCardData *data)
{
  g_return_val_if_fail (data != NULL, NULL);
  g_atomic_ref_count_inc (&data->ref_count);
  return data;
}

void
note_card_data_clear_strings (NoteCardData *data)
{
  if (!data) return;

  /* Core identity */
  g_clear_pointer (&data->id_hex, g_free);
  g_clear_pointer (&data->root_id, g_free);
  g_clear_pointer (&data->parent_id, g_free);
  g_clear_pointer (&data->pubkey_hex, g_free);
  g_clear_pointer (&data->parent_pubkey, g_free);

  /* Author info */
  g_clear_pointer (&data->avatar_url, g_free);
  g_clear_pointer (&data->nip05, g_free);
  g_clear_pointer (&data->author_lud16, g_free);

  /* Content */
  g_clear_pointer (&data->content_text, g_free);

  /* NIP-18 Repost */
  g_clear_pointer (&data->reposter_pubkey, g_free);
  g_clear_pointer (&data->reposter_display_name, g_free);

  /* NIP-57 Zap receipt */
  g_clear_pointer (&data->zap_sender_pubkey, g_free);
  g_clear_pointer (&data->zap_recipient_pubkey, g_free);
  g_clear_pointer (&data->zap_target_event_id, g_free);

  /* NIP-18 Quote */
  g_clear_pointer (&data->quoted_event_id, g_free);

  /* NIP-36 Sensitive content */
  g_clear_pointer (&data->content_warning_reason, g_free);

  /* NIP-23 Article */
  g_clear_pointer (&data->article_d_tag, g_free);
  g_clear_pointer (&data->article_title, g_free);
  g_clear_pointer (&data->article_image_url, g_free);

  /* NIP-71 Video */
  g_clear_pointer (&data->video_d_tag, g_free);
  g_clear_pointer (&data->video_url, g_free);
  g_clear_pointer (&data->video_thumb_url, g_free);
  g_clear_pointer (&data->video_title, g_free);

  /* NIP-48 Proxy */
  g_clear_pointer (&data->proxy_id, g_free);
  g_clear_pointer (&data->proxy_protocol, g_free);
}

void
note_card_data_unref (NoteCardData *data)
{
  g_return_if_fail (data != NULL);
  if (g_atomic_ref_count_dec (&data->ref_count)) {
    note_card_data_clear_strings (data);
    g_free (data);
  }
}
