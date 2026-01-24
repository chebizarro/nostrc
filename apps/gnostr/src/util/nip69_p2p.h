/**
 * NIP-69: Peer-to-Peer Order Events
 *
 * Protocol for peer-to-peer order matching (trading).
 *
 * Order Event (kind 38383 - replaceable):
 * - content: order description (optional)
 * - tags:
 *   - ["d", "<order-id>"] - unique order identifier
 *   - ["k", "buy"|"sell"] - order type
 *   - ["fa", "<fiat-amount>"] - fiat amount
 *   - ["pm", "<method>", ...] - payment methods (repeatable)
 *   - ["premium", "<percentage>"] - premium over market price
 *   - ["source", "<price-source>"] - price feed source
 *   - ["network", "mainnet"|"signet"|"liquid"] - Bitcoin network
 *   - ["layer", "onchain"|"lightning"|"liquid"] - settlement layer
 *   - ["expiration", "<timestamp>"] - order expiration
 *   - ["bond", "<percentage>"] - required bond percentage
 *   - ["rating", "<type>", "<positive>", "<total>"] - user rating
 */

#ifndef GNOSTR_NIP69_P2P_H
#define GNOSTR_NIP69_P2P_H

#include <glib.h>

G_BEGIN_DECLS

/* NIP-69 Order event kind (replaceable) */
#define NIP69_KIND_ORDER 38383

/**
 * GnostrP2pOrderType:
 * @GNOSTR_P2P_ORDER_BUY: Buy order (buying Bitcoin)
 * @GNOSTR_P2P_ORDER_SELL: Sell order (selling Bitcoin)
 *
 * Type of peer-to-peer order.
 */
typedef enum {
  GNOSTR_P2P_ORDER_BUY,
  GNOSTR_P2P_ORDER_SELL
} GnostrP2pOrderType;

/**
 * GnostrP2pOrder:
 * @order_id: Unique order identifier ("d" tag)
 * @type: Order type (buy/sell)
 * @fiat_amount: Fiat amount for the order
 * @payment_methods: Array of payment method strings
 * @pm_count: Number of payment methods
 * @premium: Premium percentage over market price
 * @price_source: Price feed source URL/name
 * @network: Bitcoin network (mainnet, signet, liquid)
 * @layer: Settlement layer (onchain, lightning, liquid)
 * @expiration: Order expiration timestamp (0 = no expiration)
 * @bond_pct: Required bond percentage
 * @rating_positive: Positive rating count
 * @rating_total: Total rating count
 * @pubkey: Order creator's pubkey (hex)
 * @event_id: Order event ID (hex)
 * @created_at: Event creation timestamp
 *
 * Parsed peer-to-peer order data from a kind 38383 event.
 */
typedef struct {
  gchar *order_id;           /* "d" tag - unique order identifier */
  GnostrP2pOrderType type;   /* "k" tag - buy or sell */
  gdouble fiat_amount;       /* "fa" tag - fiat amount */
  gchar **payment_methods;   /* "pm" tags - payment methods (NULL-terminated) */
  gsize pm_count;            /* Number of payment methods */
  gdouble premium;           /* "premium" tag - percentage over market */
  gchar *price_source;       /* "source" tag - price feed source */
  gchar *network;            /* "network" tag - mainnet/signet/liquid */
  gchar *layer;              /* "layer" tag - onchain/lightning/liquid */
  gint64 expiration;         /* "expiration" tag - unix timestamp */
  gdouble bond_pct;          /* "bond" tag - bond percentage */
  gint rating_positive;      /* "rating" tag - positive count */
  gint rating_total;         /* "rating" tag - total count */
  gchar *pubkey;             /* Event author pubkey (hex) */
  gchar *event_id;           /* Event ID (hex) */
  gint64 created_at;         /* Event creation timestamp */
} GnostrP2pOrder;

/**
 * gnostr_p2p_order_new:
 *
 * Creates a new empty P2P order structure.
 *
 * Returns: (transfer full): A new order structure.
 *          Free with gnostr_p2p_order_free().
 */
GnostrP2pOrder *gnostr_p2p_order_new(void);

/**
 * gnostr_p2p_order_free:
 * @order: Order to free
 *
 * Frees a P2P order and all its contents.
 */
void gnostr_p2p_order_free(GnostrP2pOrder *order);

/**
 * gnostr_p2p_order_parse_tags:
 * @order: Order structure to populate
 * @tags_json: JSON array string containing event tags
 *
 * Parses NIP-69 tags from a JSON array and populates the order structure.
 * Handles all order-specific tags: d, k, fa, pm, premium, source,
 * network, layer, expiration, bond, rating.
 *
 * Returns: TRUE if essential tags were parsed successfully
 */
gboolean gnostr_p2p_order_parse_tags(GnostrP2pOrder *order,
                                      const gchar *tags_json);

/**
 * gnostr_p2p_order_parse:
 * @json_str: JSON string of a kind 38383 event
 *
 * Parses a complete P2P order event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed order or NULL on error.
 *          Free with gnostr_p2p_order_free().
 */
GnostrP2pOrder *gnostr_p2p_order_parse(const gchar *json_str);

/**
 * gnostr_p2p_build_order_tags:
 * @order_id: Unique order identifier
 * @type: Order type (buy/sell)
 * @fiat_amount: Fiat amount
 * @payment_methods: NULL-terminated array of payment method strings
 * @premium: Premium percentage (0 for no premium)
 * @price_source: Price source (optional, can be NULL)
 * @network: Bitcoin network (optional, can be NULL)
 * @layer: Settlement layer (optional, can be NULL)
 * @expiration: Expiration timestamp (0 for no expiration)
 * @bond_pct: Bond percentage (0 for no bond requirement)
 *
 * Builds a JSON tags array for a NIP-69 order event.
 *
 * Returns: (transfer full) (nullable): JSON array string of tags.
 *          Caller must free with g_free().
 */
gchar *gnostr_p2p_build_order_tags(const gchar *order_id,
                                    GnostrP2pOrderType type,
                                    gdouble fiat_amount,
                                    const gchar * const *payment_methods,
                                    gdouble premium,
                                    const gchar *price_source,
                                    const gchar *network,
                                    const gchar *layer,
                                    gint64 expiration,
                                    gdouble bond_pct);

/**
 * gnostr_p2p_is_order_kind:
 * @kind: Event kind
 *
 * Check if an event kind is a P2P order (kind 38383).
 *
 * Returns: TRUE if kind is 38383
 */
gboolean gnostr_p2p_is_order_kind(gint kind);

/**
 * gnostr_p2p_order_is_expired:
 * @order: P2P order
 *
 * Check if the order has passed its expiration.
 *
 * Returns: TRUE if expiration has passed, FALSE otherwise
 */
gboolean gnostr_p2p_order_is_expired(const GnostrP2pOrder *order);

/**
 * gnostr_p2p_order_type_to_string:
 * @type: Order type
 *
 * Convert order type to string representation.
 *
 * Returns: "buy" or "sell" string (static, do not free)
 */
const gchar *gnostr_p2p_order_type_to_string(GnostrP2pOrderType type);

/**
 * gnostr_p2p_order_type_from_string:
 * @str: String representation ("buy" or "sell")
 * @type: (out): Output order type
 *
 * Parse order type from string.
 *
 * Returns: TRUE if parsed successfully
 */
gboolean gnostr_p2p_order_type_from_string(const gchar *str,
                                            GnostrP2pOrderType *type);

G_END_DECLS

#endif /* GNOSTR_NIP69_P2P_H */
