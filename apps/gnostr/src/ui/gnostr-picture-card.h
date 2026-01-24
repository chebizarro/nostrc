/**
 * gnostr-picture-card.h - NIP-68 Picture Card Widget
 *
 * A GTK4 widget for displaying NIP-68 picture posts in a card format.
 * Designed for use in picture-first feeds (Instagram-like experience).
 *
 * Features:
 * - Clickable image thumbnail
 * - Caption display with truncation
 * - Author avatar and name
 * - Like/zap/repost counts
 * - Content warning overlay support
 * - Multi-image gallery indicator
 *
 * Signals:
 * - "image-clicked" - Emitted when the image is clicked (for full-size view)
 * - "author-clicked" (pubkey_hex) - Emitted when author info is clicked
 * - "like-clicked" - Emitted when like button is clicked
 * - "zap-clicked" - Emitted when zap button is clicked
 * - "reply-clicked" - Emitted when reply button is clicked
 * - "repost-clicked" - Emitted when repost button is clicked
 * - "share-clicked" - Emitted when share button is clicked
 * - "hashtag-clicked" (tag) - Emitted when a hashtag is clicked
 */

#ifndef GNOSTR_PICTURE_CARD_H
#define GNOSTR_PICTURE_CARD_H

#include <gtk/gtk.h>
#include "../util/nip68_picture.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_PICTURE_CARD (gnostr_picture_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrPictureCard, gnostr_picture_card, GNOSTR, PICTURE_CARD, GtkWidget)

typedef struct _GnostrPictureCard GnostrPictureCard;

/**
 * gnostr_picture_card_new:
 *
 * Creates a new picture card widget.
 *
 * Returns: (transfer full): A new #GnostrPictureCard
 */
GnostrPictureCard *gnostr_picture_card_new(void);

/**
 * gnostr_picture_card_set_picture:
 * @self: A #GnostrPictureCard
 * @meta: The picture metadata to display
 *
 * Sets the picture data to display in this card.
 * The metadata is copied internally.
 */
void gnostr_picture_card_set_picture(GnostrPictureCard *self,
                                      const GnostrPictureMeta *meta);

/**
 * gnostr_picture_card_get_picture:
 * @self: A #GnostrPictureCard
 *
 * Gets the current picture metadata.
 *
 * Returns: (transfer none) (nullable): The current picture metadata or NULL
 */
const GnostrPictureMeta *gnostr_picture_card_get_picture(GnostrPictureCard *self);

/**
 * gnostr_picture_card_set_author:
 * @self: A #GnostrPictureCard
 * @display_name: (nullable): Author display name
 * @handle: (nullable): Author handle/username
 * @avatar_url: (nullable): Avatar image URL
 * @nip05: (nullable): NIP-05 identifier for verification
 *
 * Sets the author information to display.
 */
void gnostr_picture_card_set_author(GnostrPictureCard *self,
                                     const char *display_name,
                                     const char *handle,
                                     const char *avatar_url,
                                     const char *nip05);

/**
 * gnostr_picture_card_set_author_lud16:
 * @self: A #GnostrPictureCard
 * @lud16: (nullable): Lightning address for zapping
 *
 * Sets the author's lightning address (enables zap button).
 */
void gnostr_picture_card_set_author_lud16(GnostrPictureCard *self,
                                           const char *lud16);

/**
 * gnostr_picture_card_set_reaction_counts:
 * @self: A #GnostrPictureCard
 * @likes: Number of likes
 * @zaps: Number of zaps
 * @zap_sats: Total zap amount in sats
 * @reposts: Number of reposts
 * @replies: Number of replies
 *
 * Updates the reaction counts displayed on the card.
 */
void gnostr_picture_card_set_reaction_counts(GnostrPictureCard *self,
                                              int likes,
                                              int zaps,
                                              gint64 zap_sats,
                                              int reposts,
                                              int replies);

/**
 * gnostr_picture_card_set_user_reaction:
 * @self: A #GnostrPictureCard
 * @liked: Whether the current user has liked this
 * @reposted: Whether the current user has reposted this
 *
 * Sets the user's reaction state (affects button appearance).
 */
void gnostr_picture_card_set_user_reaction(GnostrPictureCard *self,
                                            gboolean liked,
                                            gboolean reposted);

/**
 * gnostr_picture_card_set_logged_in:
 * @self: A #GnostrPictureCard
 * @logged_in: Whether a user is logged in
 *
 * Sets the login state (affects button sensitivity).
 */
void gnostr_picture_card_set_logged_in(GnostrPictureCard *self,
                                        gboolean logged_in);

/**
 * gnostr_picture_card_set_loading:
 * @self: A #GnostrPictureCard
 * @loading: Whether to show loading state
 *
 * Shows or hides the loading spinner for the image.
 */
void gnostr_picture_card_set_loading(GnostrPictureCard *self,
                                      gboolean loading);

/**
 * gnostr_picture_card_reveal_content:
 * @self: A #GnostrPictureCard
 *
 * Reveals content-warning protected content.
 * Call this when the user clicks to reveal sensitive content.
 */
void gnostr_picture_card_reveal_content(GnostrPictureCard *self);

/**
 * gnostr_picture_card_set_compact:
 * @self: A #GnostrPictureCard
 * @compact: Whether to use compact layout
 *
 * Enables compact mode (less padding, smaller text).
 */
void gnostr_picture_card_set_compact(GnostrPictureCard *self,
                                      gboolean compact);

/**
 * gnostr_picture_card_get_event_id:
 * @self: A #GnostrPictureCard
 *
 * Gets the event ID of the displayed picture.
 *
 * Returns: (transfer none) (nullable): Event ID hex string or NULL
 */
const char *gnostr_picture_card_get_event_id(GnostrPictureCard *self);

/**
 * gnostr_picture_card_get_pubkey:
 * @self: A #GnostrPictureCard
 *
 * Gets the author pubkey of the displayed picture.
 *
 * Returns: (transfer none) (nullable): Pubkey hex string or NULL
 */
const char *gnostr_picture_card_get_pubkey(GnostrPictureCard *self);

/**
 * gnostr_picture_card_get_image_urls:
 * @self: A #GnostrPictureCard
 * @count: (out) (optional): Number of URLs returned
 *
 * Gets all image URLs for gallery navigation.
 *
 * Returns: (transfer full) (array zero-terminated=1): NULL-terminated array
 *          of image URLs. Free with g_strfreev().
 */
char **gnostr_picture_card_get_image_urls(GnostrPictureCard *self,
                                           size_t *count);

G_END_DECLS

#endif /* GNOSTR_PICTURE_CARD_H */
