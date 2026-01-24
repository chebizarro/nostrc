/**
 * @file gnostr-emoji-content.h
 * @brief NIP-30 Custom Emoji-aware content widget
 *
 * A widget that displays text content with inline custom emoji images.
 * Uses GtkFlowBox internally to mix GtkLabel text segments with GtkPicture
 * emoji images for true inline rendering.
 */

#ifndef GNOSTR_EMOJI_CONTENT_H
#define GNOSTR_EMOJI_CONTENT_H

#include <gtk/gtk.h>
#include "../util/custom_emoji.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_EMOJI_CONTENT (gnostr_emoji_content_get_type())

G_DECLARE_FINAL_TYPE(GnostrEmojiContent, gnostr_emoji_content, GNOSTR, EMOJI_CONTENT, GtkWidget)

/**
 * gnostr_emoji_content_new:
 *
 * Creates a new emoji-aware content widget.
 *
 * Returns: (transfer full): A new GnostrEmojiContent widget
 */
GnostrEmojiContent *gnostr_emoji_content_new(void);

/**
 * gnostr_emoji_content_set_content:
 * @self: the emoji content widget
 * @content: the text content (may contain :shortcode: patterns)
 * @emoji_list: (nullable): list of custom emojis to render (or NULL for plain text)
 *
 * Sets the content, replacing :shortcode: patterns with emoji images from the list.
 */
void gnostr_emoji_content_set_content(GnostrEmojiContent *self,
                                       const char *content,
                                       GnostrEmojiList *emoji_list);

/**
 * gnostr_emoji_content_set_wrap:
 * @self: the emoji content widget
 * @wrap: whether to wrap text
 *
 * Sets whether the content should wrap.
 */
void gnostr_emoji_content_set_wrap(GnostrEmojiContent *self, gboolean wrap);

/**
 * gnostr_emoji_content_set_selectable:
 * @self: the emoji content widget
 * @selectable: whether text should be selectable
 *
 * Sets whether the text content is selectable.
 */
void gnostr_emoji_content_set_selectable(GnostrEmojiContent *self, gboolean selectable);

/**
 * gnostr_emoji_content_get_text:
 * @self: the emoji content widget
 *
 * Gets the plain text content (without emoji replacements).
 *
 * Returns: (transfer none) (nullable): The plain text content
 */
const char *gnostr_emoji_content_get_text(GnostrEmojiContent *self);

G_END_DECLS

#endif /* GNOSTR_EMOJI_CONTENT_H */
