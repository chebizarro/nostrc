/* Pending/failed NIP-18 reposts on a timeline row (nostrc-uoe2q): the same
 * GnNostrReferenceCard states for the legacy and snapshot bind paths, and
 * the fetch goes through the reference resolver rather than the legacy
 * request-embed path (nostrc-8xfib.10). */
#include "gnostr-timeline-repost.h"
#include <nostr-gtk-1.0/gn-nostr-reference-card.h>
#include <string.h>

void gnostr_timeline_repost_show_pending(NostrGtkNoteCardRow *row,
                                         const GnNostrRepostDescriptor *descriptor,
                                         GnNostrReferenceResolver *resolver,
                                         gboolean fetch) {
  g_return_if_fail(NOSTR_GTK_IS_NOTE_CARD_ROW(row));
  g_return_if_fail(descriptor && descriptor->target && !descriptor->quote);
  nostr_gtk_note_card_row_set_content(row, "");
  nostr_gtk_note_card_row_set_reference(row, descriptor, resolver);
  GnNostrReferenceCard *card = nostr_gtk_note_card_row_get_reference_card(row);
  if (fetch && card && gn_nostr_reference_card_get_state(card) == GN_NOSTR_REFERENCE_CARD_INERT)
    gn_nostr_reference_card_fetch(card);
}

void gnostr_timeline_repost_show_snapshot(NostrGtkNoteCardRow *row,
                                          GnostrTimelineItemViewModel *vm,
                                          const char *root_id,
                                          gint source_kind,
                                          GnNostrReferenceResolver *resolver) {
  g_return_if_fail(NOSTR_GTK_IS_NOTE_CARD_ROW(row));
  g_return_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(vm));
  const char *orig_id = gnostr_timeline_item_view_model_get_reposted_event_id(vm);
  if (!orig_id || !*orig_id)
    return;

  switch (gnostr_timeline_item_view_model_get_repost_state(vm)) {
    case GNOSTR_TIMELINE_PREVIEW_RESOLVED: {
      const char *orig_pubkey = gnostr_timeline_item_view_model_get_reposted_pubkey(vm);
      const char *orig_display = gnostr_timeline_item_view_model_get_reposted_display_name(vm);
      const char *orig_avatar = gnostr_timeline_item_view_model_get_reposted_avatar_url(vm);
      gint64 orig_created_at = gnostr_timeline_item_view_model_get_reposted_created_at(vm);
      g_autofree char *orig_fallback = NULL;
      if ((!orig_display || !*orig_display) && orig_pubkey && strlen(orig_pubkey) >= 8)
        orig_fallback = g_strdup_printf("%.8s...", orig_pubkey);
      nostr_gtk_note_card_row_set_author_name_only(row,
                                                   (orig_display && *orig_display) ? orig_display : orig_fallback,
                                                   NULL);
      if (orig_avatar && *orig_avatar)
        nostr_gtk_note_card_row_set_avatar(row, orig_avatar);
      nostr_gtk_note_card_row_set_precomputed_markup(row,
                                                     gnostr_timeline_item_view_model_get_reposted_content(vm),
                                                     gnostr_timeline_item_view_model_get_reposted_rendered_content(vm));
      g_autoptr(GDateTime) when = orig_created_at > 0
        ? g_date_time_new_from_unix_local(orig_created_at) : NULL;
      g_autofree gchar *orig_ts = when ? g_date_time_format(when, "%Y-%m-%d %H:%M") : NULL;
      nostr_gtk_note_card_row_set_timestamp(row, orig_created_at, orig_ts);
      if (orig_pubkey && *orig_pubkey)
        nostr_gtk_note_card_row_set_ids(row, orig_id, root_id, orig_pubkey);
      break;
    }
    case GNOSTR_TIMELINE_PREVIEW_MISSING: {
      g_autofree gchar *uri = gn_nostr_reference_build_event(orig_id, NULL, -1, NULL);
      g_autoptr(GnNostrReference) target = uri ? gn_nostr_reference_parse(uri) : NULL;
      if (target) {
        GnNostrRepostDescriptor missing = { .target = target, .source_kind = source_kind };
        gnostr_timeline_repost_show_pending(row, &missing, resolver, FALSE);
      }
      break;
    }
    case GNOSTR_TIMELINE_PREVIEW_ABSENT:
    default:
      break;
  }
}
