/*
 * nip99_classifieds.h - NIP-99 Classified Listings implementation for GNostr
 *
 * NIP-99 defines kind 30402 for classified listing events with:
 *   - ["d", "unique-id"] - Unique identifier for the listing
 *   - ["title", "Item title"] - Listing title
 *   - ["summary", "Brief description"] - Short summary
 *   - ["published_at", "timestamp"] - Publication timestamp
 *   - ["location", "City, Country"] - Location
 *   - ["price", "100", "USD"] - Price with currency
 *   - ["t", "category"] - Category/hashtag
 *   - ["image", "url"] - Image URL(s), can have multiple
 *   - Content: Full description in markdown
 */

#ifndef NIP99_CLASSIFIEDS_H
#define NIP99_CLASSIFIEDS_H

#include <glib-object.h>
#include <gio/gio.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

/* Nostr event kind for NIP-99 classified listings */
#define NIP99_KIND_CLASSIFIED_LISTING 30402

/**
 * GnostrClassifiedPrice:
 *
 * Represents a price with amount and currency.
 */
typedef struct _GnostrClassifiedPrice {
  gchar *amount;      /* Price amount as string (e.g., "100", "1.5") */
  gchar *currency;    /* Currency code (e.g., "USD", "EUR", "BTC", "sats") */
} GnostrClassifiedPrice;

/**
 * GnostrClassified:
 *
 * Represents a classified listing (kind 30402).
 * Contains all metadata for displaying a listing.
 */
typedef struct _GnostrClassified {
  /* Identifiers */
  gchar *event_id;         /* Event ID (hex, 64 chars) */
  gchar *d_tag;            /* "d" tag - unique identifier for this listing */
  gchar *pubkey;           /* Seller's public key (hex, 64 chars) */

  /* Content */
  gchar *title;            /* "title" tag - listing title */
  gchar *summary;          /* "summary" tag - brief description */
  gchar *description;      /* Event content - full description (markdown) */

  /* Price */
  GnostrClassifiedPrice *price;  /* Price with currency */

  /* Location */
  gchar *location;         /* "location" tag - City, Country */

  /* Categories/Tags */
  GPtrArray *categories;   /* Array of gchar* from "t" tags */

  /* Images */
  GPtrArray *images;       /* Array of gchar* image URLs from "image" tags */

  /* Timestamps */
  gint64 published_at;     /* "published_at" tag - publication timestamp */
  gint64 created_at;       /* Event created_at timestamp */

  /* Seller info (fetched separately) */
  gchar *seller_name;      /* Display name from profile */
  gchar *seller_avatar;    /* Avatar URL from profile */
  gchar *seller_nip05;     /* NIP-05 identifier */
  gchar *seller_lud16;     /* Lightning address for payments */
} GnostrClassified;

/* ============== Price API ============== */

/**
 * gnostr_classified_price_new:
 *
 * Creates a new empty price struct.
 *
 * Returns: (transfer full): A new price. Free with gnostr_classified_price_free().
 */
GnostrClassifiedPrice *gnostr_classified_price_new(void);

/**
 * gnostr_classified_price_free:
 * @price: Price to free.
 *
 * Frees a price struct and its contents.
 */
void gnostr_classified_price_free(GnostrClassifiedPrice *price);

/**
 * gnostr_classified_price_parse:
 * @amount: Price amount string.
 * @currency: Currency code string (optional, defaults to "USD").
 *
 * Creates a price from amount and currency strings.
 *
 * Returns: (transfer full): Parsed price or NULL on error.
 */
GnostrClassifiedPrice *gnostr_classified_price_parse(const gchar *amount,
                                                      const gchar *currency);

/**
 * gnostr_classified_price_format:
 * @price: The price to format.
 *
 * Formats a price for display (e.g., "100 USD", "0.001 BTC").
 *
 * Returns: (transfer full): Formatted price string. Caller frees with g_free().
 */
gchar *gnostr_classified_price_format(const GnostrClassifiedPrice *price);

/**
 * gnostr_classified_price_to_sats:
 * @price: The price to convert.
 *
 * Attempts to convert price to satoshis if it's BTC-denominated.
 * Returns -1 if not applicable or conversion fails.
 *
 * Returns: Price in satoshis, or -1 if not convertible.
 */
gint64 gnostr_classified_price_to_sats(const GnostrClassifiedPrice *price);

/* ============== Classified Listing API ============== */

/**
 * gnostr_classified_new:
 *
 * Creates a new empty classified listing.
 *
 * Returns: (transfer full): A new listing. Free with gnostr_classified_free().
 */
GnostrClassified *gnostr_classified_new(void);

/**
 * gnostr_classified_free:
 * @classified: Classified listing to free.
 *
 * Frees a classified listing and all its contents.
 */
void gnostr_classified_free(GnostrClassified *classified);

/**
 * gnostr_classified_parse:
 * @event_json: JSON string of a kind 30402 event.
 *
 * Parses a classified listing from event JSON.
 *
 * Returns: (transfer full): Parsed listing or NULL on error.
 */
GnostrClassified *gnostr_classified_parse(const gchar *event_json);

/**
 * gnostr_classified_get_naddr:
 * @classified: Classified listing.
 *
 * Builds the NIP-33 address tag value for this listing.
 * Format: "30402:<pubkey>:<d_tag>"
 *
 * Returns: (transfer full): Address string (caller frees with g_free).
 */
gchar *gnostr_classified_get_naddr(const GnostrClassified *classified);

/**
 * gnostr_classified_get_primary_image:
 * @classified: Classified listing.
 *
 * Gets the first/primary image URL for display.
 *
 * Returns: (transfer none): First image URL or NULL if no images.
 */
const gchar *gnostr_classified_get_primary_image(const GnostrClassified *classified);

/**
 * gnostr_classified_get_category_string:
 * @classified: Classified listing.
 *
 * Joins all categories into a comma-separated string.
 *
 * Returns: (transfer full): Category string. Caller frees with g_free().
 */
gchar *gnostr_classified_get_category_string(const GnostrClassified *classified);

/* ============== Event Creation API ============== */

/**
 * gnostr_classified_create_event_json:
 * @classified: The classified listing data.
 *
 * Creates an unsigned event JSON for a classified listing.
 * The caller must sign the event before publishing.
 *
 * Returns: (transfer full): JSON string of the unsigned event. Caller frees with g_free().
 */
gchar *gnostr_classified_create_event_json(const GnostrClassified *classified);

/* ============== Async Fetch API ============== */

/**
 * GnostrClassifiedFetchCallback:
 * @classifieds: (element-type GnostrClassified): Array of fetched listings.
 * @user_data: User data passed to the fetch function.
 *
 * Callback for classified fetch operations.
 */
typedef void (*GnostrClassifiedFetchCallback)(GPtrArray *classifieds,
                                               gpointer user_data);

/**
 * gnostr_fetch_classifieds_async:
 * @filter_category: (nullable): Filter by category tag, or NULL for all.
 * @filter_location: (nullable): Filter by location, or NULL for all.
 * @limit: Maximum number of listings to fetch (0 for default).
 * @cancellable: (nullable): Cancellable for the operation.
 * @callback: Callback when listings are fetched.
 * @user_data: User data for callback.
 *
 * Fetches classified listings matching the optional filters.
 */
void gnostr_fetch_classifieds_async(const gchar *filter_category,
                                     const gchar *filter_location,
                                     guint limit,
                                     GCancellable *cancellable,
                                     GnostrClassifiedFetchCallback callback,
                                     gpointer user_data);

/**
 * gnostr_fetch_user_classifieds_async:
 * @pubkey_hex: User's public key in hex (64 chars).
 * @cancellable: (nullable): Cancellable for the operation.
 * @callback: Callback when listings are fetched.
 * @user_data: User data for callback.
 *
 * Fetches all classified listings from a specific seller.
 */
void gnostr_fetch_user_classifieds_async(const gchar *pubkey_hex,
                                          GCancellable *cancellable,
                                          GnostrClassifiedFetchCallback callback,
                                          gpointer user_data);

/**
 * GnostrClassifiedSingleCallback:
 * @classified: The fetched listing, or NULL on error.
 * @user_data: User data passed to the fetch function.
 *
 * Callback for single classified fetch operations.
 */
typedef void (*GnostrClassifiedSingleCallback)(GnostrClassified *classified,
                                                gpointer user_data);

/**
 * gnostr_fetch_classified_by_naddr_async:
 * @naddr: NIP-33 address in format "30402:<pubkey>:<d_tag>".
 * @cancellable: (nullable): Cancellable for the operation.
 * @callback: Callback when listing is fetched.
 * @user_data: User data for callback.
 *
 * Fetches a single classified listing by its addressable reference.
 */
void gnostr_fetch_classified_by_naddr_async(const gchar *naddr,
                                             GCancellable *cancellable,
                                             GnostrClassifiedSingleCallback callback,
                                             gpointer user_data);

/* ============== Image Cache ============== */

/**
 * gnostr_classified_prefetch_images:
 * @classified: The classified listing.
 *
 * Prefetches all images for this listing into the cache.
 */
void gnostr_classified_prefetch_images(const GnostrClassified *classified);

/**
 * gnostr_classified_get_cached_image:
 * @url: Image URL.
 *
 * Attempts to load a classified image from cache.
 *
 * Returns: (transfer full): Cached texture or NULL if not cached.
 */
GdkTexture *gnostr_classified_get_cached_image(const gchar *url);

G_END_DECLS

#endif /* NIP99_CLASSIFIEDS_H */
