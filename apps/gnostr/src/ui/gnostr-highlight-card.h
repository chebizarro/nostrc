/*
 * gnostr-highlight-card.h - NIP-84 Highlight Card Widget
 *
 * Displays a kind 9802 highlight event with the highlighted text,
 * context, source link, and author attribution.
 */

#ifndef GNOSTR_HIGHLIGHT_CARD_H
#define GNOSTR_HIGHLIGHT_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_HIGHLIGHT_CARD (gnostr_highlight_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrHighlightCard, gnostr_highlight_card, GNOSTR, HIGHLIGHT_CARD, GtkWidget)

/*
 * Signals:
 * "open-source" (gchar* source_ref, gpointer user_data)
 *    - Emitted when user clicks the source link
 *    - source_ref is event_id (for notes), a-tag (for articles), or URL
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *    - Emitted when user clicks the highlighter's profile
 *
 * "open-author-profile" (gchar* pubkey_hex, gpointer user_data)
 *    - Emitted when user clicks the original content author's profile
 */

typedef struct _GnostrHighlightCard GnostrHighlightCard;

/*
 * gnostr_highlight_card_new:
 *
 * Creates a new highlight card widget.
 *
 * Returns: (transfer full): A new #GnostrHighlightCard
 */
GnostrHighlightCard *gnostr_highlight_card_new(void);

/*
 * gnostr_highlight_card_set_highlight:
 * @self: The highlight card
 * @event_id: Highlight event ID (hex)
 * @highlighted_text: The highlighted text content
 * @context: (nullable): Surrounding context text
 * @comment: (nullable): User's annotation/comment
 * @created_at: Timestamp of the highlight
 *
 * Sets the main content of the highlight card.
 */
void gnostr_highlight_card_set_highlight(GnostrHighlightCard *self,
                                          const char *event_id,
                                          const char *highlighted_text,
                                          const char *context,
                                          const char *comment,
                                          gint64 created_at);

/*
 * gnostr_highlight_card_set_source_note:
 * @self: The highlight card
 * @source_event_id: Source note's event ID
 * @relay_hint: (nullable): Relay URL hint
 *
 * Sets the source as a kind 1 text note.
 */
void gnostr_highlight_card_set_source_note(GnostrHighlightCard *self,
                                            const char *source_event_id,
                                            const char *relay_hint);

/*
 * gnostr_highlight_card_set_source_article:
 * @self: The highlight card
 * @a_tag: Article's a-tag value (kind:pubkey:d-tag)
 * @relay_hint: (nullable): Relay URL hint
 *
 * Sets the source as a NIP-23 article.
 */
void gnostr_highlight_card_set_source_article(GnostrHighlightCard *self,
                                               const char *a_tag,
                                               const char *relay_hint);

/*
 * gnostr_highlight_card_set_source_url:
 * @self: The highlight card
 * @url: External URL
 *
 * Sets the source as an external URL.
 */
void gnostr_highlight_card_set_source_url(GnostrHighlightCard *self,
                                           const char *url);

/*
 * gnostr_highlight_card_set_highlighter:
 * @self: The highlight card
 * @pubkey_hex: Highlighter's public key (hex)
 * @display_name: (nullable): Display name
 * @avatar_url: (nullable): Avatar image URL
 *
 * Sets the person who created the highlight.
 */
void gnostr_highlight_card_set_highlighter(GnostrHighlightCard *self,
                                            const char *pubkey_hex,
                                            const char *display_name,
                                            const char *avatar_url);

/*
 * gnostr_highlight_card_set_author:
 * @self: The highlight card
 * @pubkey_hex: Original content author's public key (hex)
 * @display_name: (nullable): Display name
 *
 * Sets the original content author (from "p" tag).
 */
void gnostr_highlight_card_set_author(GnostrHighlightCard *self,
                                       const char *pubkey_hex,
                                       const char *display_name);

/*
 * gnostr_highlight_card_get_event_id:
 * @self: The highlight card
 *
 * Gets the highlight event ID.
 *
 * Returns: (transfer none) (nullable): The event ID
 */
const char *gnostr_highlight_card_get_event_id(GnostrHighlightCard *self);

/*
 * gnostr_highlight_card_get_highlighter_pubkey:
 * @self: The highlight card
 *
 * Gets the highlighter's public key.
 *
 * Returns: (transfer none) (nullable): The pubkey hex
 */
const char *gnostr_highlight_card_get_highlighter_pubkey(GnostrHighlightCard *self);

G_END_DECLS

#endif /* GNOSTR_HIGHLIGHT_CARD_H */
