/**
 * NIP-15: Nostr Marketplace Utility
 *
 * NIP-15 defines a protocol for decentralized marketplaces on Nostr.
 *
 * Event Kinds:
 * - Kind 30018: Stall/merchant profile (parameterized replaceable)
 * - Kind 30017: Product/listing (parameterized replaceable)
 *
 * Stall (30018) tags:
 * - ["d", "<stall-id>"] - unique stall identifier
 * - ["name", "<name>"] - stall name
 * - ["description", "<desc>"] - stall description
 * - ["image", "<url>"] - stall image
 * - ["currency", "<code>"] - default currency (sat, USD, EUR)
 * - ["shipping", "<zone>", "<cost>", "<region>"] - shipping options
 *
 * Product (30017) tags:
 * - ["d", "<product-id>"] - unique product identifier
 * - ["stall", "<stall-id>", "<stall-event-id>", "<relay>"] - reference to stall
 * - ["name", "<name>"] - product name
 * - ["description", "<desc>"] - product description
 * - ["images", "<url1>", "<url2>", ...] - product images
 * - ["price", "<amount>", "<currency>"] - price
 * - ["quantity", "<num>"] - available quantity
 * - ["specs", "<key1>", "<value1>", "<key2>", "<value2>", ...] - specifications
 * - ["t", "<category>"] - category tags
 */

#ifndef NIP15_MARKETPLACE_H
#define NIP15_MARKETPLACE_H

#include <glib.h>

G_BEGIN_DECLS

/* Nostr event kinds for NIP-15 marketplace */
#define NIP15_KIND_PRODUCT 30017
#define NIP15_KIND_STALL   30018

/**
 * GnostrShippingZone:
 *
 * Represents a shipping zone with cost and regions.
 */
typedef struct _GnostrShippingZone {
  gchar *zone_name;    /* Zone identifier (e.g., "domestic", "international") */
  gdouble cost;        /* Shipping cost */
  GPtrArray *regions;  /* Array of gchar* region codes/names */
} GnostrShippingZone;

/**
 * GnostrStall:
 *
 * Represents a merchant stall (kind 30018).
 * Contains stall metadata including shipping options.
 */
typedef struct _GnostrStall {
  gchar *stall_id;         /* "d" tag value - unique stall identifier */
  gchar *name;             /* "name" tag - stall display name */
  gchar *description;      /* "description" tag - stall description */
  gchar *image;            /* "image" tag - stall image URL */
  gchar *currency;         /* "currency" tag - default currency (sat, USD, EUR) */
  GPtrArray *shipping_zones; /* Array of GnostrShippingZone* */
  guint zone_count;        /* Number of shipping zones */
  gchar *pubkey;           /* Event author - merchant's pubkey (hex) */
  gchar *event_id;         /* Event ID of the stall event */
  gint64 created_at;       /* Creation timestamp */
} GnostrStall;

/**
 * GnostrProductSpec:
 *
 * Key-value specification for a product.
 */
typedef struct _GnostrProductSpec {
  gchar *key;    /* Specification name (e.g., "Color", "Size") */
  gchar *value;  /* Specification value (e.g., "Red", "Large") */
} GnostrProductSpec;

/**
 * GnostrProduct:
 *
 * Represents a product listing (kind 30017).
 * Contains product metadata and references to stall.
 */
typedef struct _GnostrProduct {
  gchar *product_id;       /* "d" tag value - unique product identifier */
  gchar *stall_id;         /* Stall ID from "stall" tag */
  gchar *stall_event_id;   /* Stall event ID from "stall" tag (optional) */
  gchar *stall_relay;      /* Stall relay hint from "stall" tag (optional) */
  gchar *name;             /* "name" tag - product name */
  gchar *description;      /* "description" tag - product description */
  GPtrArray *images;       /* Array of gchar* image URLs from "images" tag */
  guint image_count;       /* Number of images */
  gdouble price;           /* "price" tag - price amount */
  gchar *currency;         /* "price" tag - price currency */
  gint quantity;           /* "quantity" tag - available quantity (-1 = unlimited) */
  GPtrArray *specs;        /* Array of GnostrProductSpec* */
  guint spec_count;        /* Number of specifications */
  GPtrArray *categories;   /* Array of gchar* category tags ("t" tags) */
  gchar *pubkey;           /* Event author - merchant's pubkey (hex) */
  gchar *event_id;         /* Event ID of the product event */
  gint64 created_at;       /* Creation timestamp */
} GnostrProduct;

/* ============== Shipping Zone API ============== */

/**
 * gnostr_shipping_zone_new:
 *
 * Creates a new empty shipping zone.
 *
 * Returns: (transfer full): A new shipping zone. Free with gnostr_shipping_zone_free().
 */
GnostrShippingZone *gnostr_shipping_zone_new(void);

/**
 * gnostr_shipping_zone_free:
 * @zone: Shipping zone to free.
 *
 * Frees a shipping zone and all its contents.
 */
void gnostr_shipping_zone_free(GnostrShippingZone *zone);

/**
 * gnostr_shipping_zone_add_region:
 * @zone: Shipping zone.
 * @region: Region code/name to add.
 *
 * Adds a region to the shipping zone.
 */
void gnostr_shipping_zone_add_region(GnostrShippingZone *zone, const gchar *region);

/* ============== Stall API ============== */

/**
 * gnostr_stall_new:
 *
 * Creates a new empty stall.
 *
 * Returns: (transfer full): A new stall. Free with gnostr_stall_free().
 */
GnostrStall *gnostr_stall_new(void);

/**
 * gnostr_stall_free:
 * @stall: Stall to free.
 *
 * Frees a stall and all its contents.
 */
void gnostr_stall_free(GnostrStall *stall);

/**
 * gnostr_stall_parse:
 * @event_json: JSON string of a kind 30018 event.
 *
 * Parses a stall from event JSON.
 *
 * Returns: (transfer full) (nullable): Parsed stall or NULL on error.
 */
GnostrStall *gnostr_stall_parse(const gchar *event_json);

/**
 * gnostr_stall_add_shipping_zone:
 * @stall: Stall to modify.
 * @zone: (transfer full): Shipping zone to add (ownership transferred).
 *
 * Adds a shipping zone to the stall.
 */
void gnostr_stall_add_shipping_zone(GnostrStall *stall, GnostrShippingZone *zone);

/**
 * gnostr_stall_get_naddr:
 * @stall: Stall.
 *
 * Builds the NIP-33 address tag value for this stall.
 * Format: "30018:<pubkey>:<stall_id>"
 *
 * Returns: (transfer full) (nullable): Address string (caller frees with g_free).
 */
gchar *gnostr_stall_get_naddr(const GnostrStall *stall);

/**
 * gnostr_stall_build_tags:
 * @stall: Stall to build tags for.
 *
 * Builds the tags array for a stall event.
 * Returns a JSON array string suitable for event construction.
 *
 * Returns: (transfer full) (nullable): JSON array string of tags.
 */
gchar *gnostr_stall_build_tags(const GnostrStall *stall);

/* ============== Product Spec API ============== */

/**
 * gnostr_product_spec_new:
 * @key: Specification key.
 * @value: Specification value.
 *
 * Creates a new product specification.
 *
 * Returns: (transfer full): A new spec. Free with gnostr_product_spec_free().
 */
GnostrProductSpec *gnostr_product_spec_new(const gchar *key, const gchar *value);

/**
 * gnostr_product_spec_free:
 * @spec: Specification to free.
 *
 * Frees a product specification.
 */
void gnostr_product_spec_free(GnostrProductSpec *spec);

/* ============== Product API ============== */

/**
 * gnostr_product_new:
 *
 * Creates a new empty product.
 *
 * Returns: (transfer full): A new product. Free with gnostr_product_free().
 */
GnostrProduct *gnostr_product_new(void);

/**
 * gnostr_product_free:
 * @product: Product to free.
 *
 * Frees a product and all its contents.
 */
void gnostr_product_free(GnostrProduct *product);

/**
 * gnostr_product_parse:
 * @event_json: JSON string of a kind 30017 event.
 *
 * Parses a product from event JSON.
 *
 * Returns: (transfer full) (nullable): Parsed product or NULL on error.
 */
GnostrProduct *gnostr_product_parse(const gchar *event_json);

/**
 * gnostr_product_add_image:
 * @product: Product to modify.
 * @image_url: Image URL to add.
 *
 * Adds an image URL to the product.
 */
void gnostr_product_add_image(GnostrProduct *product, const gchar *image_url);

/**
 * gnostr_product_add_spec:
 * @product: Product to modify.
 * @key: Specification key.
 * @value: Specification value.
 *
 * Adds a specification to the product.
 */
void gnostr_product_add_spec(GnostrProduct *product, const gchar *key, const gchar *value);

/**
 * gnostr_product_add_category:
 * @product: Product to modify.
 * @category: Category tag to add.
 *
 * Adds a category tag to the product.
 */
void gnostr_product_add_category(GnostrProduct *product, const gchar *category);

/**
 * gnostr_product_get_naddr:
 * @product: Product.
 *
 * Builds the NIP-33 address tag value for this product.
 * Format: "30017:<pubkey>:<product_id>"
 *
 * Returns: (transfer full) (nullable): Address string (caller frees with g_free).
 */
gchar *gnostr_product_get_naddr(const GnostrProduct *product);

/**
 * gnostr_product_build_tags:
 * @product: Product to build tags for.
 *
 * Builds the tags array for a product event.
 * Returns a JSON array string suitable for event construction.
 *
 * Returns: (transfer full) (nullable): JSON array string of tags.
 */
gchar *gnostr_product_build_tags(const GnostrProduct *product);

/* ============== Price Formatting Helpers ============== */

/**
 * gnostr_marketplace_format_price:
 * @price: Price amount.
 * @currency: Currency code (sat, USD, EUR, etc.).
 *
 * Formats a price for display with appropriate currency symbol.
 *
 * Returns: (transfer full): Formatted price string.
 */
gchar *gnostr_marketplace_format_price(gdouble price, const gchar *currency);

/**
 * gnostr_marketplace_format_price_sats:
 * @sats: Price in satoshis.
 *
 * Formats a satoshi price with appropriate formatting (K, M suffixes).
 *
 * Returns: (transfer full): Formatted price string (e.g., "100K sats").
 */
gchar *gnostr_marketplace_format_price_sats(gint64 sats);

/**
 * gnostr_marketplace_format_quantity:
 * @quantity: Available quantity (-1 for unlimited).
 *
 * Formats quantity for display.
 *
 * Returns: (transfer full): Formatted quantity string.
 */
gchar *gnostr_marketplace_format_quantity(gint quantity);

/**
 * gnostr_marketplace_is_stall_kind:
 * @kind: Event kind.
 *
 * Check if an event kind is a stall (kind 30018).
 *
 * Returns: TRUE if kind is 30018
 */
gboolean gnostr_marketplace_is_stall_kind(gint kind);

/**
 * gnostr_marketplace_is_product_kind:
 * @kind: Event kind.
 *
 * Check if an event kind is a product (kind 30017).
 *
 * Returns: TRUE if kind is 30017
 */
gboolean gnostr_marketplace_is_product_kind(gint kind);

G_END_DECLS

#endif /* NIP15_MARKETPLACE_H */
