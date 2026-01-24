/*
 * gnostr-torrent-card.h - NIP-35 Torrent Card Widget
 *
 * GTK4 widget for displaying NIP-35 kind 2003 torrent events.
 * Shows torrent title, file list, size, trackers, and action buttons.
 *
 * Features:
 * - Title and description display
 * - File list with sizes
 * - Total size indicator
 * - Category/hashtag pills
 * - External reference links (IMDB, TMDB, etc.)
 * - Copy magnet link action
 * - Author info with NIP-05 verification
 */

#ifndef GNOSTR_TORRENT_CARD_H
#define GNOSTR_TORRENT_CARD_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TORRENT_CARD (gnostr_torrent_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrTorrentCard, gnostr_torrent_card, GNOSTR, TORRENT_CARD, GtkWidget)

/*
 * Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data) - view author profile
 * "open-torrent" (gchar* event_id_hex, gpointer user_data) - view torrent details
 * "open-url" (gchar* url, gpointer user_data) - open external URL
 * "copy-magnet" (gchar* magnet_uri, gpointer user_data) - magnet copied to clipboard
 * "open-magnet" (gchar* magnet_uri, gpointer user_data) - open magnet in torrent client
 * "zap-requested" (gchar* event_id, gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 * "bookmark-toggled" (gchar* event_id, gboolean is_bookmarked, gpointer user_data)
 */

typedef struct _GnostrTorrentCard GnostrTorrentCard;

/**
 * gnostr_torrent_card_new:
 *
 * Creates a new torrent card widget.
 *
 * Returns: (transfer full): A new GnostrTorrentCard.
 */
GnostrTorrentCard *gnostr_torrent_card_new(void);

/**
 * gnostr_torrent_card_set_torrent:
 * @self: The torrent card.
 * @event_id: Event ID (hex).
 * @title: Torrent title.
 * @description: Torrent description (may be NULL).
 * @infohash: BitTorrent infohash (40 hex chars).
 * @created_at: Event creation timestamp.
 *
 * Sets the basic torrent information.
 */
void gnostr_torrent_card_set_torrent(GnostrTorrentCard *self,
                                      const char *event_id,
                                      const char *title,
                                      const char *description,
                                      const char *infohash,
                                      gint64 created_at);

/**
 * gnostr_torrent_card_set_author:
 * @self: The torrent card.
 * @display_name: Author display name.
 * @handle: Author handle/username.
 * @avatar_url: Avatar image URL (may be NULL).
 * @pubkey_hex: Author's public key (hex).
 *
 * Sets the author information for the torrent.
 */
void gnostr_torrent_card_set_author(GnostrTorrentCard *self,
                                     const char *display_name,
                                     const char *handle,
                                     const char *avatar_url,
                                     const char *pubkey_hex);

/**
 * gnostr_torrent_card_add_file:
 * @self: The torrent card.
 * @path: File path within torrent.
 * @size: File size in bytes (-1 if unknown).
 *
 * Adds a file to the file list.
 */
void gnostr_torrent_card_add_file(GnostrTorrentCard *self,
                                   const char *path,
                                   gint64 size);

/**
 * gnostr_torrent_card_set_total_size:
 * @self: The torrent card.
 * @total_size: Total size in bytes.
 *
 * Sets the total torrent size (overrides calculated value).
 */
void gnostr_torrent_card_set_total_size(GnostrTorrentCard *self,
                                         gint64 total_size);

/**
 * gnostr_torrent_card_add_tracker:
 * @self: The torrent card.
 * @tracker_url: Tracker URL.
 *
 * Adds a tracker to the tracker list.
 */
void gnostr_torrent_card_add_tracker(GnostrTorrentCard *self,
                                      const char *tracker_url);

/**
 * gnostr_torrent_card_add_category:
 * @self: The torrent card.
 * @category: Category/hashtag (without # prefix).
 *
 * Adds a category pill.
 */
void gnostr_torrent_card_add_category(GnostrTorrentCard *self,
                                       const char *category);

/**
 * gnostr_torrent_card_add_reference:
 * @self: The torrent card.
 * @prefix: Reference type (imdb, tmdb, etc.).
 * @value: Reference value.
 *
 * Adds an external reference link.
 */
void gnostr_torrent_card_add_reference(GnostrTorrentCard *self,
                                        const char *prefix,
                                        const char *value);

/**
 * gnostr_torrent_card_set_nip05:
 * @self: The torrent card.
 * @nip05: NIP-05 identifier.
 * @pubkey_hex: Expected public key.
 *
 * Initiates NIP-05 verification for the author.
 */
void gnostr_torrent_card_set_nip05(GnostrTorrentCard *self,
                                    const char *nip05,
                                    const char *pubkey_hex);

/**
 * gnostr_torrent_card_set_author_lud16:
 * @self: The torrent card.
 * @lud16: Lightning address.
 *
 * Sets author's lightning address for zapping.
 */
void gnostr_torrent_card_set_author_lud16(GnostrTorrentCard *self,
                                           const char *lud16);

/**
 * gnostr_torrent_card_set_bookmarked:
 * @self: The torrent card.
 * @is_bookmarked: Whether torrent is bookmarked.
 *
 * Updates the bookmark button state.
 */
void gnostr_torrent_card_set_bookmarked(GnostrTorrentCard *self,
                                         gboolean is_bookmarked);

/**
 * gnostr_torrent_card_set_logged_in:
 * @self: The torrent card.
 * @logged_in: Whether user is logged in.
 *
 * Updates button sensitivity based on login state.
 */
void gnostr_torrent_card_set_logged_in(GnostrTorrentCard *self,
                                        gboolean logged_in);

/**
 * gnostr_torrent_card_get_magnet:
 * @self: The torrent card.
 *
 * Generates and returns the magnet URI for this torrent.
 *
 * Returns: (transfer full) (nullable): Magnet URI or NULL.
 */
gchar *gnostr_torrent_card_get_magnet(GnostrTorrentCard *self);

/**
 * gnostr_torrent_card_get_infohash:
 * @self: The torrent card.
 *
 * Returns the infohash.
 *
 * Returns: (transfer none): Infohash string or NULL.
 */
const char *gnostr_torrent_card_get_infohash(GnostrTorrentCard *self);

/**
 * gnostr_torrent_card_get_event_id:
 * @self: The torrent card.
 *
 * Returns the event ID.
 *
 * Returns: (transfer none): Event ID string or NULL.
 */
const char *gnostr_torrent_card_get_event_id(GnostrTorrentCard *self);

G_END_DECLS

#endif /* GNOSTR_TORRENT_CARD_H */
