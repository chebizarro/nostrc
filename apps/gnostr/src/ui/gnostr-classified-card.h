/*
 * gnostr-classified-card.h - NIP-99 Classified Listing Card Widget
 *
 * Displays a single kind 30402 classified listing with:
 * - Image carousel/gallery for listing images
 * - Title and summary display
 * - Price prominently displayed with currency
 * - Location badge
 * - Category tags
 * - Contact seller button
 * - Seller info with avatar and NIP-05
 */

#ifndef GNOSTR_CLASSIFIED_CARD_H
#define GNOSTR_CLASSIFIED_CARD_H

#include <gtk/gtk.h>
#include "../util/nip99_classifieds.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CLASSIFIED_CARD (gnostr_classified_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrClassifiedCard, gnostr_classified_card, GNOSTR, CLASSIFIED_CARD, GtkWidget)

/**
 * Signals:
 *
 * "contact-seller" (gchar* pubkey_hex, gchar* lud16, gpointer user_data)
 *   - Emitted when user clicks to contact the seller
 *   - lud16 may be NULL if seller has no lightning address
 *
 * "view-details" (gchar* event_id, gchar* naddr, gpointer user_data)
 *   - Emitted when user clicks to view full listing details
 *
 * "image-clicked" (gchar* image_url, guint image_index, gpointer user_data)
 *   - Emitted when user clicks on an image to view full-size
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when user clicks on seller avatar/name
 *
 * "category-clicked" (gchar* category, gpointer user_data)
 *   - Emitted when user clicks on a category tag to filter
 *
 * "share-listing" (gchar* nostr_uri, gpointer user_data)
 *   - Emitted when user shares the listing
 */

typedef struct _GnostrClassifiedCard GnostrClassifiedCard;

/**
 * gnostr_classified_card_new:
 *
 * Creates a new classified card widget.
 *
 * Returns: (transfer full): A new GnostrClassifiedCard widget.
 */
GnostrClassifiedCard *gnostr_classified_card_new(void);

/**
 * gnostr_classified_card_set_listing:
 * @self: The classified card widget.
 * @classified: The classified listing data to display.
 *
 * Sets all the listing data on the card. The widget copies the data internally.
 */
void gnostr_classified_card_set_listing(GnostrClassifiedCard *self,
                                         const GnostrClassified *classified);

/**
 * gnostr_classified_card_set_title:
 * @self: The classified card widget.
 * @title: The listing title.
 *
 * Sets the listing title.
 */
void gnostr_classified_card_set_title(GnostrClassifiedCard *self,
                                       const char *title);

/**
 * gnostr_classified_card_set_summary:
 * @self: The classified card widget.
 * @summary: Brief description of the listing.
 *
 * Sets the listing summary/description preview.
 */
void gnostr_classified_card_set_summary(GnostrClassifiedCard *self,
                                         const char *summary);

/**
 * gnostr_classified_card_set_price:
 * @self: The classified card widget.
 * @price: The price structure.
 *
 * Sets the listing price.
 */
void gnostr_classified_card_set_price(GnostrClassifiedCard *self,
                                       const GnostrClassifiedPrice *price);

/**
 * gnostr_classified_card_set_location:
 * @self: The classified card widget.
 * @location: Location string (e.g., "Berlin, Germany").
 *
 * Sets the listing location badge.
 */
void gnostr_classified_card_set_location(GnostrClassifiedCard *self,
                                          const char *location);

/**
 * gnostr_classified_card_set_images:
 * @self: The classified card widget.
 * @images: (element-type utf8): Array of image URLs.
 *
 * Sets the listing images for the carousel.
 */
void gnostr_classified_card_set_images(GnostrClassifiedCard *self,
                                        GPtrArray *images);

/**
 * gnostr_classified_card_set_categories:
 * @self: The classified card widget.
 * @categories: (element-type utf8): Array of category strings.
 *
 * Sets the category tags to display.
 */
void gnostr_classified_card_set_categories(GnostrClassifiedCard *self,
                                            GPtrArray *categories);

/**
 * gnostr_classified_card_set_seller:
 * @self: The classified card widget.
 * @display_name: Seller's display name.
 * @avatar_url: Seller's avatar URL.
 * @pubkey_hex: Seller's public key (hex, 64 chars).
 *
 * Sets the seller information.
 */
void gnostr_classified_card_set_seller(GnostrClassifiedCard *self,
                                        const char *display_name,
                                        const char *avatar_url,
                                        const char *pubkey_hex);

/**
 * gnostr_classified_card_set_seller_nip05:
 * @self: The classified card widget.
 * @nip05: NIP-05 identifier.
 * @pubkey_hex: Seller's public key for verification.
 *
 * Sets and verifies the seller's NIP-05 identifier.
 */
void gnostr_classified_card_set_seller_nip05(GnostrClassifiedCard *self,
                                              const char *nip05,
                                              const char *pubkey_hex);

/**
 * gnostr_classified_card_set_seller_lud16:
 * @self: The classified card widget.
 * @lud16: Lightning address for payments.
 *
 * Sets the seller's lightning address for the contact button.
 */
void gnostr_classified_card_set_seller_lud16(GnostrClassifiedCard *self,
                                              const char *lud16);

/**
 * gnostr_classified_card_set_event_id:
 * @self: The classified card widget.
 * @event_id: Event ID (hex, 64 chars).
 * @d_tag: The d-tag identifier.
 *
 * Sets the event identifiers for navigation and sharing.
 */
void gnostr_classified_card_set_event_id(GnostrClassifiedCard *self,
                                          const char *event_id,
                                          const char *d_tag);

/**
 * gnostr_classified_card_set_published_at:
 * @self: The classified card widget.
 * @published_at: Publication timestamp.
 *
 * Sets the publication date to display.
 */
void gnostr_classified_card_set_published_at(GnostrClassifiedCard *self,
                                              gint64 published_at);

/**
 * gnostr_classified_card_set_compact:
 * @self: The classified card widget.
 * @compact: TRUE for compact/grid mode, FALSE for full card.
 *
 * Sets whether to show the card in compact mode (for grid views).
 */
void gnostr_classified_card_set_compact(GnostrClassifiedCard *self,
                                         gboolean compact);

/**
 * gnostr_classified_card_set_logged_in:
 * @self: The classified card widget.
 * @logged_in: TRUE if user is logged in.
 *
 * Sets the login state (affects button sensitivity).
 */
void gnostr_classified_card_set_logged_in(GnostrClassifiedCard *self,
                                           gboolean logged_in);

/**
 * gnostr_classified_card_get_event_id:
 * @self: The classified card widget.
 *
 * Gets the event ID of the listing.
 *
 * Returns: (transfer none): The event ID or NULL.
 */
const char *gnostr_classified_card_get_event_id(GnostrClassifiedCard *self);

/**
 * gnostr_classified_card_get_d_tag:
 * @self: The classified card widget.
 *
 * Gets the d-tag identifier of the listing.
 *
 * Returns: (transfer none): The d-tag or NULL.
 */
const char *gnostr_classified_card_get_d_tag(GnostrClassifiedCard *self);

/**
 * gnostr_classified_card_get_seller_pubkey:
 * @self: The classified card widget.
 *
 * Gets the seller's public key.
 *
 * Returns: (transfer none): The pubkey hex or NULL.
 */
const char *gnostr_classified_card_get_seller_pubkey(GnostrClassifiedCard *self);

/**
 * gnostr_classified_card_next_image:
 * @self: The classified card widget.
 *
 * Shows the next image in the carousel.
 */
void gnostr_classified_card_next_image(GnostrClassifiedCard *self);

/**
 * gnostr_classified_card_prev_image:
 * @self: The classified card widget.
 *
 * Shows the previous image in the carousel.
 */
void gnostr_classified_card_prev_image(GnostrClassifiedCard *self);

G_END_DECLS

#endif /* GNOSTR_CLASSIFIED_CARD_H */
