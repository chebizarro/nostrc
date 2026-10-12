/* A NIP-18 kind 6/16 repost whose original is not available locally
 * (nostrc-uoe2q, nostrc-8xfib.10). */
#ifndef GNOSTR_TIMELINE_REPOST_H
#define GNOSTR_TIMELINE_REPOST_H

#include <nostr-gtk-1.0/nostr-note-card-row.h>
#include <nostr-gtk-1.0/gn-nostr-reference-resolver.h>

G_BEGIN_DECLS

/* Shows @descriptor's repost card on @row in place of the note body, which
 * is cleared: the repost's own content is never displayed. With @fetch the
 * card asks @resolver's relay fetch at once (Finding on relays…, then the
 * original or "unavailable"); without it the card offers Find on Relays. */
void gnostr_timeline_repost_show_pending(NostrGtkNoteCardRow *row,
                                         const GnNostrRepostDescriptor *descriptor,
                                         GnNostrReferenceResolver *resolver,
                                         gboolean fetch);

G_END_DECLS
#endif
