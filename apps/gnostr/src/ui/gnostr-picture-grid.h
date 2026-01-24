/**
 * gnostr-picture-grid.h - NIP-68 Picture Grid Widget
 *
 * A GTK4 widget for displaying NIP-68 pictures in a responsive grid layout.
 * Designed for Instagram-like picture feed experience.
 *
 * Features:
 * - Responsive grid layout (adjusts columns based on width)
 * - Infinite scroll with virtualization
 * - Full-size image overlay on click
 * - Smooth scroll animations
 * - Lazy loading of images
 * - Pull-to-refresh support
 * - Loading and empty states
 *
 * Signals:
 * - "picture-clicked" (event_id) - Emitted when a picture is clicked
 * - "author-clicked" (pubkey_hex) - Emitted when author info is clicked
 * - "load-more" - Emitted when scrolling near the end (for pagination)
 * - "refresh-requested" - Emitted on pull-to-refresh
 * - "like-clicked" (event_id) - Emitted when like button is clicked
 * - "zap-clicked" (event_id, pubkey_hex, lud16) - Emitted for zapping
 * - "hashtag-clicked" (tag) - Emitted when a hashtag is clicked
 */

#ifndef GNOSTR_PICTURE_GRID_H
#define GNOSTR_PICTURE_GRID_H

#include <gtk/gtk.h>
#include "../util/nip68_picture.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_PICTURE_GRID (gnostr_picture_grid_get_type())

G_DECLARE_FINAL_TYPE(GnostrPictureGrid, gnostr_picture_grid, GNOSTR, PICTURE_GRID, GtkWidget)

typedef struct _GnostrPictureGrid GnostrPictureGrid;

/**
 * GnostrPictureGridColumn:
 * Column configuration for the grid.
 */
typedef enum {
  GNOSTR_PICTURE_GRID_AUTO = 0,   /* Automatic based on width */
  GNOSTR_PICTURE_GRID_1_COL = 1,  /* Single column (mobile) */
  GNOSTR_PICTURE_GRID_2_COL = 2,  /* Two columns */
  GNOSTR_PICTURE_GRID_3_COL = 3,  /* Three columns (default) */
  GNOSTR_PICTURE_GRID_4_COL = 4,  /* Four columns */
  GNOSTR_PICTURE_GRID_5_COL = 5,  /* Five columns (wide screens) */
} GnostrPictureGridColumns;

/**
 * gnostr_picture_grid_new:
 *
 * Creates a new picture grid widget.
 *
 * Returns: (transfer full): A new #GnostrPictureGrid
 */
GnostrPictureGrid *gnostr_picture_grid_new(void);

/**
 * gnostr_picture_grid_clear:
 * @self: A #GnostrPictureGrid
 *
 * Removes all pictures from the grid.
 */
void gnostr_picture_grid_clear(GnostrPictureGrid *self);

/**
 * gnostr_picture_grid_add_picture:
 * @self: A #GnostrPictureGrid
 * @meta: The picture metadata to add
 *
 * Adds a picture to the grid.
 * The metadata is copied internally.
 */
void gnostr_picture_grid_add_picture(GnostrPictureGrid *self,
                                      const GnostrPictureMeta *meta);

/**
 * gnostr_picture_grid_add_pictures:
 * @self: A #GnostrPictureGrid
 * @pictures: (array length=count): Array of picture metadata
 * @count: Number of pictures in the array
 *
 * Adds multiple pictures to the grid at once.
 * More efficient than adding one by one.
 */
void gnostr_picture_grid_add_pictures(GnostrPictureGrid *self,
                                       const GnostrPictureMeta **pictures,
                                       size_t count);

/**
 * gnostr_picture_grid_remove_picture:
 * @self: A #GnostrPictureGrid
 * @event_id: Event ID of the picture to remove
 *
 * Removes a picture from the grid by event ID.
 *
 * Returns: TRUE if the picture was found and removed.
 */
gboolean gnostr_picture_grid_remove_picture(GnostrPictureGrid *self,
                                             const char *event_id);

/**
 * gnostr_picture_grid_update_picture:
 * @self: A #GnostrPictureGrid
 * @meta: Updated picture metadata
 *
 * Updates an existing picture in the grid.
 * The picture is matched by event_id.
 *
 * Returns: TRUE if the picture was found and updated.
 */
gboolean gnostr_picture_grid_update_picture(GnostrPictureGrid *self,
                                             const GnostrPictureMeta *meta);

/**
 * gnostr_picture_grid_set_author_info:
 * @self: A #GnostrPictureGrid
 * @pubkey: Author pubkey (hex)
 * @display_name: (nullable): Display name
 * @avatar_url: (nullable): Avatar URL
 * @nip05: (nullable): NIP-05 identifier
 * @lud16: (nullable): Lightning address
 *
 * Updates author info for all pictures by the given pubkey.
 * Call this when profile metadata is loaded.
 */
void gnostr_picture_grid_set_author_info(GnostrPictureGrid *self,
                                          const char *pubkey,
                                          const char *display_name,
                                          const char *avatar_url,
                                          const char *nip05,
                                          const char *lud16);

/**
 * gnostr_picture_grid_set_reaction_counts:
 * @self: A #GnostrPictureGrid
 * @event_id: Event ID
 * @likes: Number of likes
 * @zaps: Number of zaps
 * @zap_sats: Total zap amount in sats
 * @reposts: Number of reposts
 * @replies: Number of replies
 *
 * Updates reaction counts for a specific picture.
 */
void gnostr_picture_grid_set_reaction_counts(GnostrPictureGrid *self,
                                              const char *event_id,
                                              int likes,
                                              int zaps,
                                              gint64 zap_sats,
                                              int reposts,
                                              int replies);

/**
 * gnostr_picture_grid_set_columns:
 * @self: A #GnostrPictureGrid
 * @columns: Column configuration
 *
 * Sets the number of columns. Use GNOSTR_PICTURE_GRID_AUTO for
 * responsive behavior based on widget width.
 */
void gnostr_picture_grid_set_columns(GnostrPictureGrid *self,
                                      GnostrPictureGridColumns columns);

/**
 * gnostr_picture_grid_set_spacing:
 * @self: A #GnostrPictureGrid
 * @spacing: Spacing between items in pixels
 *
 * Sets the spacing between grid items.
 */
void gnostr_picture_grid_set_spacing(GnostrPictureGrid *self,
                                      guint spacing);

/**
 * gnostr_picture_grid_set_loading:
 * @self: A #GnostrPictureGrid
 * @loading: Whether to show loading state
 *
 * Shows or hides the loading indicator.
 */
void gnostr_picture_grid_set_loading(GnostrPictureGrid *self,
                                      gboolean loading);

/**
 * gnostr_picture_grid_set_loading_more:
 * @self: A #GnostrPictureGrid
 * @loading: Whether to show loading-more indicator
 *
 * Shows or hides the "loading more" indicator at the bottom.
 */
void gnostr_picture_grid_set_loading_more(GnostrPictureGrid *self,
                                           gboolean loading);

/**
 * gnostr_picture_grid_set_empty_message:
 * @self: A #GnostrPictureGrid
 * @message: (nullable): Message to show when grid is empty
 *
 * Sets the message displayed when there are no pictures.
 */
void gnostr_picture_grid_set_empty_message(GnostrPictureGrid *self,
                                            const char *message);

/**
 * gnostr_picture_grid_set_logged_in:
 * @self: A #GnostrPictureGrid
 * @logged_in: Whether a user is logged in
 *
 * Sets the login state (affects action button sensitivity).
 */
void gnostr_picture_grid_set_logged_in(GnostrPictureGrid *self,
                                        gboolean logged_in);

/**
 * gnostr_picture_grid_show_overlay:
 * @self: A #GnostrPictureGrid
 * @event_id: Event ID of the picture to show
 *
 * Shows the full-size image overlay for a specific picture.
 */
void gnostr_picture_grid_show_overlay(GnostrPictureGrid *self,
                                       const char *event_id);

/**
 * gnostr_picture_grid_hide_overlay:
 * @self: A #GnostrPictureGrid
 *
 * Hides the full-size image overlay.
 */
void gnostr_picture_grid_hide_overlay(GnostrPictureGrid *self);

/**
 * gnostr_picture_grid_scroll_to_top:
 * @self: A #GnostrPictureGrid
 *
 * Scrolls the grid to the top.
 */
void gnostr_picture_grid_scroll_to_top(GnostrPictureGrid *self);

/**
 * gnostr_picture_grid_get_picture_count:
 * @self: A #GnostrPictureGrid
 *
 * Gets the number of pictures in the grid.
 *
 * Returns: Number of pictures.
 */
guint gnostr_picture_grid_get_picture_count(GnostrPictureGrid *self);

/**
 * gnostr_picture_grid_find_picture:
 * @self: A #GnostrPictureGrid
 * @event_id: Event ID to find
 *
 * Finds a picture by event ID.
 *
 * Returns: (transfer none) (nullable): The picture metadata or NULL.
 */
const GnostrPictureMeta *gnostr_picture_grid_find_picture(GnostrPictureGrid *self,
                                                           const char *event_id);

/**
 * gnostr_picture_grid_set_compact:
 * @self: A #GnostrPictureGrid
 * @compact: Whether to use compact layout
 *
 * Enables compact mode (smaller cards, no captions).
 */
void gnostr_picture_grid_set_compact(GnostrPictureGrid *self,
                                      gboolean compact);

G_END_DECLS

#endif /* GNOSTR_PICTURE_GRID_H */
