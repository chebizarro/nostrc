/* GnNostrReferenceCard: the one NIP-18 repost / quote / NIP-21 note-reference
 * card for gnostr and Groundhog (nostrc-8xfib.6). Ported from the card row's
 * quote embed (nostr-note-card-row.c set_quote_info) and gnostr's timeline
 * factory resolution, with Groundhog's inert, consent-first behaviour:
 *
 *  - binding only consults the resolver's local lookup; it never fetches;
 *  - "Find on Relays…" is offered only when the resolver can fetch, and
 *    fetching happens only on that click or an explicit
 *    gn_nostr_reference_card_fetch() by the application;
 *  - anything shown is a verified event that matches the reference;
 *  - text only (no images), translated, with an accessible label. */
#ifndef GN_NOSTR_REFERENCE_CARD_H
#define GN_NOSTR_REFERENCE_CARD_H

#include <gtk/gtk.h>
#include <nostr-gtk-1.0/gn-nostr-reference-resolver.h>

G_BEGIN_DECLS

typedef enum {
  GN_NOSTR_REFERENCE_CARD_ROLE_MENTION, /* a nostr: note/address in the text */
  GN_NOSTR_REFERENCE_CARD_ROLE_QUOTE,   /* kind 1 q tag */
  GN_NOSTR_REFERENCE_CARD_ROLE_REPOST   /* kind 6 / 16 */
} GnNostrReferenceCardRole;

typedef enum {
  GN_NOSTR_REFERENCE_CARD_EMPTY,
  GN_NOSTR_REFERENCE_CARD_INERT,       /* not available locally */
  GN_NOSTR_REFERENCE_CARD_FETCHING,
  GN_NOSTR_REFERENCE_CARD_RESOLVED,
  GN_NOSTR_REFERENCE_CARD_UNAVAILABLE  /* fetched, nothing matching arrived */
} GnNostrReferenceCardState;

#define GN_TYPE_NOSTR_REFERENCE_CARD (gn_nostr_reference_card_get_type())
G_DECLARE_FINAL_TYPE(GnNostrReferenceCard, gn_nostr_reference_card, GN, NOSTR_REFERENCE_CARD, GtkBox)

GtkWidget *gn_nostr_reference_card_new(void);

/* The resolver used for local lookup, author names and fetching (may be NULL:
 * the card then stays inert unless an embedded original verified). */
void gn_nostr_reference_card_set_resolver(GnNostrReferenceCard *self,
                                          GnNostrReferenceResolver *resolver);
/* Points the card at @target (copied). @verified_json is an embedded original
 * (a kind-6 content), shown only if it verifies and matches. NULL clears. */
void gn_nostr_reference_card_set_reference(GnNostrReferenceCard *self,
                                           const GnNostrReference *target,
                                           GnNostrReferenceCardRole role,
                                           const gchar *verified_json);
/* set_reference() from a parsed repost/quote descriptor. */
void gn_nostr_reference_card_set_descriptor(GnNostrReferenceCard *self,
                                            const GnNostrRepostDescriptor *descriptor);
/* An application-side cached preview (e.g. gnostr's timeline view model):
 * shows @author_name/@content as RESOLVED without a lookup. */
void gn_nostr_reference_card_set_preview(GnNostrReferenceCard *self,
                                         const gchar *author_name,
                                         const gchar *content);
/* Explicit fetch through the resolver. FALSE when nothing was started
 * (no reference, already resolved/fetching, or the resolver cannot fetch). */
gboolean gn_nostr_reference_card_fetch(GnNostrReferenceCard *self);

GnNostrReferenceCardState gn_nostr_reference_card_get_state(GnNostrReferenceCard *self);
const GnNostrReference *gn_nostr_reference_card_get_reference(GnNostrReferenceCard *self);
/* The verified note, when RESOLVED from an event (not a preview). */
const GnNostrResolvedNote *gn_nostr_reference_card_get_note(GnNostrReferenceCard *self);

/* The translated one-line description of an unresolved reference, e.g.
 * "Quoted Nostr note: 0123456789ab". */
gchar *gn_nostr_reference_dup_summary(const GnNostrReference *reference,
                                      GnNostrReferenceCardRole role);

G_END_DECLS
#endif
