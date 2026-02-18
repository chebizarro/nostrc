/**
 * NIP-69: Peer-to-Peer Order Events Implementation
 *
 * Parsing and building P2P order events for trading.
 */

#include "nip69_p2p.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

gboolean gnostr_p2p_is_order_kind(gint kind) {
  return kind == NIP69_KIND_ORDER;
}

GnostrP2pOrder *gnostr_p2p_order_new(void) {
  GnostrP2pOrder *order = g_new0(GnostrP2pOrder, 1);
  order->type = GNOSTR_P2P_ORDER_BUY;
  return order;
}

void gnostr_p2p_order_free(GnostrP2pOrder *order) {
  if (!order) return;

  g_free(order->order_id);
  g_strfreev(order->payment_methods);
  g_free(order->price_source);
  g_free(order->network);
  g_free(order->layer);
  g_free(order->pubkey);
  g_free(order->event_id);
  g_free(order);
}

const gchar *gnostr_p2p_order_type_to_string(GnostrP2pOrderType type) {
  switch (type) {
    case GNOSTR_P2P_ORDER_BUY:
      return "buy";
    case GNOSTR_P2P_ORDER_SELL:
      return "sell";
    default:
      return "buy";
  }
}

gboolean gnostr_p2p_order_type_from_string(const gchar *str,
                                            GnostrP2pOrderType *type) {
  if (!str || !type) return FALSE;

  if (g_strcmp0(str, "buy") == 0) {
    *type = GNOSTR_P2P_ORDER_BUY;
    return TRUE;
  } else if (g_strcmp0(str, "sell") == 0) {
    *type = GNOSTR_P2P_ORDER_SELL;
    return TRUE;
  }

  return FALSE;
}

gboolean gnostr_p2p_order_is_expired(const GnostrP2pOrder *order) {
  if (!order || order->expiration <= 0) return FALSE;
  gint64 now = (gint64)time(NULL);
  return now >= order->expiration;
}

gboolean gnostr_p2p_order_parse_tags(GnostrP2pOrder *order,
                                      const gchar *tags_json) {
  if (!order || !tags_json || !*tags_json) return FALSE;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_debug("NIP-69: Failed to parse tags JSON: %s",
            error ? error->message : "unknown");
    g_clear_error(&error);
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root)) {
    return FALSE;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  /* Collect payment methods in a GPtrArray */
  GPtrArray *pm_arr = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const gchar *tag_name = json_array_get_string_element(tag, 0);
    if (!tag_name) continue;

    if (g_strcmp0(tag_name, "d") == 0) {
      /* Order ID: ["d", "<order-id>"] */
      const gchar *order_id = json_array_get_string_element(tag, 1);
      if (order_id && *order_id) {
        g_free(order->order_id);
        order->order_id = g_strdup(order_id);
      }
    } else if (g_strcmp0(tag_name, "k") == 0) {
      /* Order type: ["k", "buy"|"sell"] */
      const gchar *type_str = json_array_get_string_element(tag, 1);
      if (type_str) {
        gnostr_p2p_order_type_from_string(type_str, &order->type);
      }
    } else if (g_strcmp0(tag_name, "fa") == 0) {
      /* Fiat amount: ["fa", "<amount>"] */
      const gchar *amount_str = json_array_get_string_element(tag, 1);
      if (amount_str) {
        order->fiat_amount = g_ascii_strtod(amount_str, NULL);
      }
    } else if (g_strcmp0(tag_name, "pm") == 0) {
      /* Payment method: ["pm", "<method>", ...] - can have multiple values */
      for (guint j = 1; j < tag_len; j++) {
        const gchar *method = json_array_get_string_element(tag, j);
        if (method && *method) {
          g_ptr_array_add(pm_arr, g_strdup(method));
        }
      }
    } else if (g_strcmp0(tag_name, "premium") == 0) {
      /* Premium: ["premium", "<percentage>"] */
      const gchar *premium_str = json_array_get_string_element(tag, 1);
      if (premium_str) {
        order->premium = g_ascii_strtod(premium_str, NULL);
      }
    } else if (g_strcmp0(tag_name, "source") == 0) {
      /* Price source: ["source", "<source>"] */
      const gchar *source = json_array_get_string_element(tag, 1);
      if (source && *source) {
        g_free(order->price_source);
        order->price_source = g_strdup(source);
      }
    } else if (g_strcmp0(tag_name, "network") == 0) {
      /* Bitcoin network: ["network", "mainnet"|"signet"|"liquid"] */
      const gchar *network = json_array_get_string_element(tag, 1);
      if (network && *network) {
        g_free(order->network);
        order->network = g_strdup(network);
      }
    } else if (g_strcmp0(tag_name, "layer") == 0) {
      /* Settlement layer: ["layer", "onchain"|"lightning"|"liquid"] */
      const gchar *layer = json_array_get_string_element(tag, 1);
      if (layer && *layer) {
        g_free(order->layer);
        order->layer = g_strdup(layer);
      }
    } else if (g_strcmp0(tag_name, "expiration") == 0) {
      /* Expiration: ["expiration", "<timestamp>"] */
      const gchar *exp_str = json_array_get_string_element(tag, 1);
      if (exp_str) {
        order->expiration = g_ascii_strtoll(exp_str, NULL, 10);
      }
    } else if (g_strcmp0(tag_name, "bond") == 0) {
      /* Bond percentage: ["bond", "<percentage>"] */
      const gchar *bond_str = json_array_get_string_element(tag, 1);
      if (bond_str) {
        order->bond_pct = g_ascii_strtod(bond_str, NULL);
      }
    } else if (g_strcmp0(tag_name, "rating") == 0 && tag_len >= 4) {
      /* Rating: ["rating", "<type>", "<positive>", "<total>"] */
      /* We skip the type field (index 1) and just use positive/total */
      const gchar *pos_str = json_array_get_string_element(tag, 2);
      const gchar *total_str = json_array_get_string_element(tag, 3);
      if (pos_str && total_str) {
        order->rating_positive = (gint)g_ascii_strtoll(pos_str, NULL, 10);
        order->rating_total = (gint)g_ascii_strtoll(total_str, NULL, 10);
      }
    }
  }

  /* Convert payment methods array to NULL-terminated string array */
  if (pm_arr->len > 0) {
    g_ptr_array_add(pm_arr, NULL);
    g_strfreev(order->payment_methods);
    order->payment_methods = (gchar **)g_ptr_array_free(pm_arr, FALSE);
    order->pm_count = order->payment_methods ?
                      g_strv_length(order->payment_methods) : 0;
  } else {
    g_ptr_array_free(pm_arr, TRUE);
  }


  /* Order ID is required */
  return order->order_id != NULL;
}

GnostrP2pOrder *gnostr_p2p_order_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_debug("NIP-69: Failed to parse order JSON: %s",
            error ? error->message : "unknown");
    g_clear_error(&error);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);

  /* Check kind */
  if (!json_object_has_member(obj, "kind")) {
    return NULL;
  }
  gint64 kind = json_object_get_int_member(obj, "kind");
  if (kind != NIP69_KIND_ORDER) {
    return NULL;
  }

  GnostrP2pOrder *order = gnostr_p2p_order_new();

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    order->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    order->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    order->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Parse tags */
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    guint n_tags = json_array_get_length(tags);

    /* Collect payment methods in a GPtrArray */
    GPtrArray *pm_arr = g_ptr_array_new();

    for (guint i = 0; i < n_tags; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

      JsonArray *tag = json_node_get_array(tag_node);
      guint tag_len = json_array_get_length(tag);
      if (tag_len < 2) continue;

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        /* Order ID: ["d", "<order-id>"] */
        const gchar *order_id = json_array_get_string_element(tag, 1);
        if (order_id && *order_id) {
          g_free(order->order_id);
          order->order_id = g_strdup(order_id);
        }
      } else if (g_strcmp0(tag_name, "k") == 0) {
        /* Order type: ["k", "buy"|"sell"] */
        const gchar *type_str = json_array_get_string_element(tag, 1);
        if (type_str) {
          gnostr_p2p_order_type_from_string(type_str, &order->type);
        }
      } else if (g_strcmp0(tag_name, "fa") == 0) {
        /* Fiat amount: ["fa", "<amount>"] */
        const gchar *amount_str = json_array_get_string_element(tag, 1);
        if (amount_str) {
          order->fiat_amount = g_ascii_strtod(amount_str, NULL);
        }
      } else if (g_strcmp0(tag_name, "pm") == 0) {
        /* Payment method: ["pm", "<method>", ...] - can have multiple values */
        for (guint j = 1; j < tag_len; j++) {
          const gchar *method = json_array_get_string_element(tag, j);
          if (method && *method) {
            g_ptr_array_add(pm_arr, g_strdup(method));
          }
        }
      } else if (g_strcmp0(tag_name, "premium") == 0) {
        /* Premium: ["premium", "<percentage>"] */
        const gchar *premium_str = json_array_get_string_element(tag, 1);
        if (premium_str) {
          order->premium = g_ascii_strtod(premium_str, NULL);
        }
      } else if (g_strcmp0(tag_name, "source") == 0) {
        /* Price source: ["source", "<source>"] */
        const gchar *source = json_array_get_string_element(tag, 1);
        if (source && *source) {
          g_free(order->price_source);
          order->price_source = g_strdup(source);
        }
      } else if (g_strcmp0(tag_name, "network") == 0) {
        /* Bitcoin network: ["network", "mainnet"|"signet"|"liquid"] */
        const gchar *network = json_array_get_string_element(tag, 1);
        if (network && *network) {
          g_free(order->network);
          order->network = g_strdup(network);
        }
      } else if (g_strcmp0(tag_name, "layer") == 0) {
        /* Settlement layer: ["layer", "onchain"|"lightning"|"liquid"] */
        const gchar *layer = json_array_get_string_element(tag, 1);
        if (layer && *layer) {
          g_free(order->layer);
          order->layer = g_strdup(layer);
        }
      } else if (g_strcmp0(tag_name, "expiration") == 0) {
        /* Expiration: ["expiration", "<timestamp>"] */
        const gchar *exp_str = json_array_get_string_element(tag, 1);
        if (exp_str) {
          order->expiration = g_ascii_strtoll(exp_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "bond") == 0) {
        /* Bond percentage: ["bond", "<percentage>"] */
        const gchar *bond_str = json_array_get_string_element(tag, 1);
        if (bond_str) {
          order->bond_pct = g_ascii_strtod(bond_str, NULL);
        }
      } else if (g_strcmp0(tag_name, "rating") == 0 && tag_len >= 4) {
        /* Rating: ["rating", "<type>", "<positive>", "<total>"] */
        const gchar *pos_str = json_array_get_string_element(tag, 2);
        const gchar *total_str = json_array_get_string_element(tag, 3);
        if (pos_str && total_str) {
          order->rating_positive = (gint)g_ascii_strtoll(pos_str, NULL, 10);
          order->rating_total = (gint)g_ascii_strtoll(total_str, NULL, 10);
        }
      }
    }

    /* Convert payment methods array to NULL-terminated string array */
    if (pm_arr->len > 0) {
      g_ptr_array_add(pm_arr, NULL);
      order->payment_methods = (gchar **)g_ptr_array_free(pm_arr, FALSE);
      order->pm_count = order->payment_methods ?
                        g_strv_length(order->payment_methods) : 0;
    } else {
      g_ptr_array_free(pm_arr, TRUE);
    }
  }


  /* Validate: must have order_id (d tag) */
  if (!order->order_id) {
    g_debug("NIP-69: Order missing 'd' tag (order_id)");
    gnostr_p2p_order_free(order);
    return NULL;
  }

  return order;
}

gchar *gnostr_p2p_build_order_tags(const gchar *order_id,
                                    GnostrP2pOrderType type,
                                    gdouble fiat_amount,
                                    const gchar * const *payment_methods,
                                    gdouble premium,
                                    const gchar *price_source,
                                    const gchar *network,
                                    const gchar *layer,
                                    gint64 expiration,
                                    gdouble bond_pct) {
  if (!order_id || !*order_id) {
    g_warning("NIP-69: Cannot build order tags without order_id");
    return NULL;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* d tag - order identifier (required) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "d");
  json_builder_add_string_value(builder, order_id);
  json_builder_end_array(builder);

  /* k tag - order type (required) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "k");
  json_builder_add_string_value(builder, gnostr_p2p_order_type_to_string(type));
  json_builder_end_array(builder);

  /* fa tag - fiat amount */
  if (fiat_amount > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "fa");
    gchar *amount_str = g_strdup_printf("%.2f", fiat_amount);
    json_builder_add_string_value(builder, amount_str);
    g_free(amount_str);
    json_builder_end_array(builder);
  }

  /* pm tags - payment methods (each as separate tag, or combined) */
  if (payment_methods) {
    for (gsize i = 0; payment_methods[i]; i++) {
      if (payment_methods[i][0]) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "pm");
        json_builder_add_string_value(builder, payment_methods[i]);
        json_builder_end_array(builder);
      }
    }
  }

  /* premium tag */
  if (premium != 0.0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "premium");
    gchar *premium_str = g_strdup_printf("%.2f", premium);
    json_builder_add_string_value(builder, premium_str);
    g_free(premium_str);
    json_builder_end_array(builder);
  }

  /* source tag - price feed source */
  if (price_source && *price_source) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "source");
    json_builder_add_string_value(builder, price_source);
    json_builder_end_array(builder);
  }

  /* network tag - Bitcoin network */
  if (network && *network) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "network");
    json_builder_add_string_value(builder, network);
    json_builder_end_array(builder);
  }

  /* layer tag - settlement layer */
  if (layer && *layer) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "layer");
    json_builder_add_string_value(builder, layer);
    json_builder_end_array(builder);
  }

  /* expiration tag */
  if (expiration > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "expiration");
    gchar *exp_str = g_strdup_printf("%" G_GINT64_FORMAT, expiration);
    json_builder_add_string_value(builder, exp_str);
    g_free(exp_str);
    json_builder_end_array(builder);
  }

  /* bond tag - bond percentage */
  if (bond_pct > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "bond");
    gchar *bond_str = g_strdup_printf("%.2f", bond_pct);
    json_builder_add_string_value(builder, bond_str);
    g_free(bond_str);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}
