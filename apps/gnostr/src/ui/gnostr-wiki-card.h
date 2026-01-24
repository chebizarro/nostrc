/*
 * gnostr-wiki-card.h - NIP-54 Wiki Article Card Widget
 *
 * Displays kind 30818 wiki article events with:
 * - Title from "title" tag
 * - Summary from "summary" tag
 * - Author avatar/name from profile lookup
 * - Last updated timestamp
 * - Markdown content rendered to Pango markup
 * - Related articles as clickable links
 * - Topic tags
 * - Table of contents for navigation
 */

#ifndef GNOSTR_WIKI_CARD_H
#define GNOSTR_WIKI_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_WIKI_CARD (gnostr_wiki_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrWikiCard, gnostr_wiki_card, GNOSTR, WIKI_CARD, GtkWidget)

/*
 * Signals:
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *    - Emitted when user clicks the author's profile
 *
 * "open-article" (gchar* event_id_hex, gpointer user_data)
 *    - Emitted when user clicks to view full article
 *
 * "open-related" (gchar* a_tag, gpointer user_data)
 *    - Emitted when user clicks a related article link
 *    - a_tag format: "30818:pubkey:d-tag"
 *
 * "open-url" (gchar* url, gpointer user_data)
 *    - Emitted when user clicks an external URL in content
 *
 * "zap-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 *    - Emitted when user wants to zap the article author
 *
 * "bookmark-toggled" (gchar* event_id, gboolean is_bookmarked, gpointer user_data)
 *    - Emitted when bookmark state changes
 *
 * "share-article" (gchar* nostr_uri, gpointer user_data)
 *    - Emitted when user wants to share the article
 *
 * "topic-clicked" (gchar* topic, gpointer user_data)
 *    - Emitted when user clicks a topic tag
 */

typedef struct _GnostrWikiCard GnostrWikiCard;

/*
 * gnostr_wiki_card_new:
 *
 * Creates a new wiki card widget.
 *
 * Returns: (transfer full): A new #GnostrWikiCard
 */
GnostrWikiCard *gnostr_wiki_card_new(void);

/*
 * gnostr_wiki_card_set_article:
 * @self: The wiki card
 * @event_id: Event ID (hex)
 * @d_tag: Article slug/identifier
 * @title: Display title
 * @summary: (nullable): Short summary
 * @published_at: Publication timestamp
 * @created_at: Event creation timestamp (for last updated)
 *
 * Sets the article metadata (from event tags).
 */
void gnostr_wiki_card_set_article(GnostrWikiCard *self,
                                   const char *event_id,
                                   const char *d_tag,
                                   const char *title,
                                   const char *summary,
                                   gint64 published_at,
                                   gint64 created_at);

/*
 * gnostr_wiki_card_set_author:
 * @self: The wiki card
 * @display_name: (nullable): Author's display name
 * @handle: (nullable): Author's handle (name/npub)
 * @avatar_url: (nullable): Avatar image URL
 * @pubkey_hex: Author's public key (hex)
 *
 * Sets the author information.
 */
void gnostr_wiki_card_set_author(GnostrWikiCard *self,
                                  const char *display_name,
                                  const char *handle,
                                  const char *avatar_url,
                                  const char *pubkey_hex);

/*
 * gnostr_wiki_card_set_content:
 * @self: The wiki card
 * @markdown_content: Full article content in Markdown
 *
 * Sets the full markdown content. This is used for:
 * - Reading time estimation
 * - Table of contents extraction
 * - Full article rendering when expanded
 */
void gnostr_wiki_card_set_content(GnostrWikiCard *self,
                                   const char *markdown_content);

/*
 * gnostr_wiki_card_set_related_articles:
 * @self: The wiki card
 * @a_tags: NULL-terminated array of "a" tag values
 * @count: Number of related articles
 *
 * Sets the related article links. Each a_tag should be
 * in format "30818:pubkey:d-tag".
 */
void gnostr_wiki_card_set_related_articles(GnostrWikiCard *self,
                                            const char **a_tags,
                                            gsize count);

/*
 * gnostr_wiki_card_set_topics:
 * @self: The wiki card
 * @topics: NULL-terminated array of topic strings
 * @count: Number of topics
 *
 * Sets the topic/category tags.
 */
void gnostr_wiki_card_set_topics(GnostrWikiCard *self,
                                  const char **topics,
                                  gsize count);

/*
 * gnostr_wiki_card_set_nip05:
 * @self: The wiki card
 * @nip05: NIP-05 identifier
 * @pubkey_hex: Author's public key for verification
 *
 * Sets and verifies NIP-05 identifier.
 */
void gnostr_wiki_card_set_nip05(GnostrWikiCard *self,
                                 const char *nip05,
                                 const char *pubkey_hex);

/*
 * gnostr_wiki_card_set_author_lud16:
 * @self: The wiki card
 * @lud16: Lightning address for zapping
 *
 * Sets author's lightning address for zapping.
 */
void gnostr_wiki_card_set_author_lud16(GnostrWikiCard *self,
                                        const char *lud16);

/*
 * gnostr_wiki_card_set_bookmarked:
 * @self: The wiki card
 * @is_bookmarked: Whether article is bookmarked
 *
 * Sets the bookmark state.
 */
void gnostr_wiki_card_set_bookmarked(GnostrWikiCard *self,
                                      gboolean is_bookmarked);

/*
 * gnostr_wiki_card_set_logged_in:
 * @self: The wiki card
 * @logged_in: Whether user is logged in
 *
 * Sets login state (affects button sensitivity).
 */
void gnostr_wiki_card_set_logged_in(GnostrWikiCard *self,
                                     gboolean logged_in);

/*
 * gnostr_wiki_card_set_expanded:
 * @self: The wiki card
 * @expanded: Whether to show full content
 *
 * Expands or collapses the card to show/hide full content.
 */
void gnostr_wiki_card_set_expanded(GnostrWikiCard *self,
                                    gboolean expanded);

/*
 * gnostr_wiki_card_get_expanded:
 * @self: The wiki card
 *
 * Returns: Whether the card is currently expanded.
 */
gboolean gnostr_wiki_card_get_expanded(GnostrWikiCard *self);

/*
 * gnostr_wiki_card_get_d_tag:
 * @self: The wiki card
 *
 * Gets the d-tag identifier for this article.
 *
 * Returns: (transfer none) (nullable): The d-tag or NULL.
 */
const char *gnostr_wiki_card_get_d_tag(GnostrWikiCard *self);

/*
 * gnostr_wiki_card_get_a_tag:
 * @self: The wiki card
 *
 * Gets the article's NIP-33 "a" tag reference (kind:pubkey:d-tag).
 *
 * Returns: (transfer full) (nullable): "a" tag value string.
 */
char *gnostr_wiki_card_get_a_tag(GnostrWikiCard *self);

/*
 * gnostr_wiki_card_get_event_id:
 * @self: The wiki card
 *
 * Gets the event ID for this article.
 *
 * Returns: (transfer none) (nullable): The event ID or NULL.
 */
const char *gnostr_wiki_card_get_event_id(GnostrWikiCard *self);

/*
 * gnostr_wiki_card_get_pubkey:
 * @self: The wiki card
 *
 * Gets the author's public key.
 *
 * Returns: (transfer none) (nullable): The pubkey hex or NULL.
 */
const char *gnostr_wiki_card_get_pubkey(GnostrWikiCard *self);

G_END_DECLS

#endif /* GNOSTR_WIKI_CARD_H */
