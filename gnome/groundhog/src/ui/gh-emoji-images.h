#ifndef GH_EMOJI_IMAGES_H
#define GH_EMOJI_IMAGES_H
#include <adwaita.h>
#include "gh-web-content.h"
#include "gh-emoji.h"
G_BEGIN_DECLS
#define GH_TYPE_EMOJI_IMAGES (gh_emoji_images_get_type())
G_DECLARE_FINAL_TYPE(GhEmojiImages, gh_emoji_images, GH, EMOJI_IMAGES, GtkBox)
/* NULL text: picker; otherwise inline message text. Copies tags, performs no I/O.
 * picked(shortcode, tags), changed() when inline text replaces the fallback. */
GtkWidget *gh_emoji_images_new(GPtrArray *tags, const gchar *text, GSettings *settings,
                               GhWebContent *web);
gboolean gh_emoji_images_has_artwork(GhEmojiImages *self);
void gh_emoji_images_revoke(GhEmojiImages *self);
G_END_DECLS
#endif
