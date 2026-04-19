#ifndef NIPS_NIP99_NOSTR_NIP99_NIP99_H
#define NIPS_NIP99_NOSTR_NIP99_NIP99_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nostr-event.h"
#include "nostr-tag.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * NIP-99: Classified Listings
 *
 * Kind 30402 for published listings, kind 30403 for drafts.
 * Addressable events with "d" tag identifier.
 *
 * Required tags: d, title, summary, published_at, location, price
 * Optional tags: image (multiple), t (categories)
 *
 * Price tag format: ["price", "amount", "currency", "frequency?"]
 *   currency: 3-letter ISO 4217 or "BTC"/"sats"
 *   frequency: "hour", "day", "week", "month", "year" (optional)
 */

#define NOSTR_NIP99_KIND_LISTING       30402
#define NOSTR_NIP99_KIND_DRAFT_LISTING 30403

/**
 * Price with borrowed pointers from price tag elements.
 */
typedef struct {
    const char *amount;    /**< Price amount string (borrowed) */
    const char *currency;  /**< Currency code, e.g. "USD" (borrowed) */
    const char *frequency; /**< Payment frequency (borrowed, nullable) */
} NostrNip99Price;

/**
 * Parsed classified listing with borrowed pointers.
 * Valid while the source event is alive and unmodified.
 */
typedef struct {
    const char *identifier;  /**< "d" tag value (borrowed) */
    const char *title;       /**< "title" tag value (borrowed, nullable) */
    const char *summary;     /**< "summary" tag value (borrowed, nullable) */
    const char *location;    /**< "location" tag value (borrowed, nullable) */
    int64_t published_at;    /**< "published_at" parsed timestamp, 0 if absent */
    NostrNip99Price price;   /**< Price details (borrowed) */
} NostrNip99Listing;

/**
 * A single image entry from an "image" tag.
 */
typedef struct {
    const char *url;        /**< Image URL (borrowed) */
    const char *dimensions; /**< Dimensions string, e.g. "800x600" (borrowed, nullable) */
} NostrNip99Image;

/**
 * nostr_nip99_parse:
 * @ev: (in): event to inspect
 * @out: (out): parsed listing metadata
 *
 * Parses listing metadata from event tags.
 *
 * Returns: 0 on success, -EINVAL on bad input, -ENOENT if no "d" tag
 */
int nostr_nip99_parse(const NostrEvent *ev, NostrNip99Listing *out);

/**
 * nostr_nip99_validate:
 * @ev: (in): event to validate
 *
 * Checks that the event has kind 30402/30403 and all required tags:
 * d, title, summary, published_at, location, price.
 *
 * Returns: true if valid classified listing event
 */
bool nostr_nip99_validate(const NostrEvent *ev);

/**
 * nostr_nip99_is_listing:
 * @ev: (in): event to check
 *
 * Returns: true if kind is 30402 or 30403
 */
bool nostr_nip99_is_listing(const NostrEvent *ev);

/**
 * nostr_nip99_get_images:
 * @ev: (in): event to inspect
 * @images: (out caller-allocates): array to fill
 * @max: maximum images to store
 * @out_count: (out): number of images stored
 *
 * Extracts image entries from "image" tags.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip99_get_images(const NostrEvent *ev, NostrNip99Image *images,
                            size_t max, size_t *out_count);

/**
 * nostr_nip99_count_images:
 * @ev: (in): event to inspect
 *
 * Returns: number of "image" tags
 */
size_t nostr_nip99_count_images(const NostrEvent *ev);

/**
 * nostr_nip99_get_categories:
 * @ev: (in): event to inspect
 * @tags: (out caller-allocates): array of borrowed category pointers
 * @max: maximum categories to store
 * @out_count: (out): number stored
 *
 * Extracts category values from "t" tags.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip99_get_categories(const NostrEvent *ev, const char **tags,
                                size_t max, size_t *out_count);

/**
 * nostr_nip99_create_listing:
 * @ev: (inout): event to populate
 * @listing: (in): listing metadata
 *
 * Sets kind to 30402 and adds all listing tags.
 * Content (full description) should be set separately.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip99_create_listing(NostrEvent *ev, const NostrNip99Listing *listing);

/**
 * nostr_nip99_add_image:
 * @ev: (inout): event to modify
 * @url: (in): image URL
 * @dimensions: (in) (nullable): dimensions string, e.g. "800x600"
 *
 * Appends an "image" tag.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip99_add_image(NostrEvent *ev, const char *url,
                           const char *dimensions);

/**
 * nostr_nip99_add_category:
 * @ev: (inout): event to modify
 * @category: (in): category/hashtag value
 *
 * Appends a "t" tag.
 *
 * Returns: 0 on success, negative errno on error
 */
int nostr_nip99_add_category(NostrEvent *ev, const char *category);

#ifdef __cplusplus
}
#endif
#endif /* NIPS_NIP99_NOSTR_NIP99_NIP99_H */
