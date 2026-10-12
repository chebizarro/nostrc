/* Pending/failed NIP-18 reposts on a timeline row (nostrc-uoe2q): the same
 * GnNostrReferenceCard states for the legacy and snapshot bind paths, and
 * the fetch goes through the reference resolver rather than the legacy
 * request-embed path (nostrc-8xfib.10). */
#include "gnostr-timeline-repost.h"
#include <nostr-gtk-1.0/gn-nostr-reference-card.h>

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
