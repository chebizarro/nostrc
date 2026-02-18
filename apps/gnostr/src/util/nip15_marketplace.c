/**
 * NIP-15: Nostr Marketplace Utility Implementation
 *
 * Parsing, building, and formatting for marketplace stalls and products.
 */

#include "nip15_marketplace.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <math.h>

/* ============== Shipping Zone ============== */

GnostrShippingZone *
gnostr_shipping_zone_new(void)
{
  GnostrShippingZone *zone = g_new0(GnostrShippingZone, 1);
  zone->regions = g_ptr_array_new_with_free_func(g_free);
  return zone;
}

void
gnostr_shipping_zone_free(GnostrShippingZone *zone)
{
  if (!zone) return;
  g_free(zone->zone_name);
  if (zone->regions) {
    g_ptr_array_unref(zone->regions);
  }
  g_free(zone);
}

void
gnostr_shipping_zone_add_region(GnostrShippingZone *zone, const gchar *region)
{
  if (!zone || !region || !*region) return;
  g_ptr_array_add(zone->regions, g_strdup(region));
}

/* ============== Stall ============== */

GnostrStall *
gnostr_stall_new(void)
{
  GnostrStall *stall = g_new0(GnostrStall, 1);
  stall->shipping_zones = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_shipping_zone_free);
  return stall;
}

void
gnostr_stall_free(GnostrStall *stall)
{
  if (!stall) return;
  g_free(stall->stall_id);
  g_free(stall->name);
  g_free(stall->description);
  g_free(stall->image);
  g_free(stall->currency);
  g_free(stall->pubkey);
  g_free(stall->event_id);
  if (stall->shipping_zones) {
    g_ptr_array_unref(stall->shipping_zones);
  }
  g_free(stall);
}

void
gnostr_stall_add_shipping_zone(GnostrStall *stall, GnostrShippingZone *zone)
{
  if (!stall || !zone) return;
  g_ptr_array_add(stall->shipping_zones, zone);
  stall->zone_count = stall->shipping_zones->len;
}

GnostrStall *
gnostr_stall_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_debug("NIP-15: Failed to parse stall JSON: %s", error ? error->message : "unknown");
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
  if (kind != NIP15_KIND_STALL) {
    return NULL;
  }

  GnostrStall *stall = gnostr_stall_new();

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    stall->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    stall->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    stall->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Parse tags */
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    guint n_tags = json_array_get_length(tags);

    for (guint i = 0; i < n_tags; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

      JsonArray *tag = json_node_get_array(tag_node);
      guint tag_len = json_array_get_length(tag);
      if (tag_len < 2) continue;

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        /* Stall ID: ["d", "<stall_id>"] */
        const gchar *stall_id = json_array_get_string_element(tag, 1);
        if (stall_id) {
          stall->stall_id = g_strdup(stall_id);
        }
      } else if (g_strcmp0(tag_name, "name") == 0) {
        /* Name: ["name", "<name>"] */
        const gchar *name = json_array_get_string_element(tag, 1);
        if (name) {
          stall->name = g_strdup(name);
        }
      } else if (g_strcmp0(tag_name, "description") == 0) {
        /* Description: ["description", "<desc>"] */
        const gchar *desc = json_array_get_string_element(tag, 1);
        if (desc) {
          stall->description = g_strdup(desc);
        }
      } else if (g_strcmp0(tag_name, "image") == 0) {
        /* Image: ["image", "<url>"] */
        const gchar *image = json_array_get_string_element(tag, 1);
        if (image) {
          stall->image = g_strdup(image);
        }
      } else if (g_strcmp0(tag_name, "currency") == 0) {
        /* Currency: ["currency", "<code>"] */
        const gchar *currency = json_array_get_string_element(tag, 1);
        if (currency) {
          stall->currency = g_strdup(currency);
        }
      } else if (g_strcmp0(tag_name, "shipping") == 0 && tag_len >= 3) {
        /* Shipping: ["shipping", "<zone>", "<cost>", "<region>", ...] */
        GnostrShippingZone *zone = gnostr_shipping_zone_new();

        const gchar *zone_name = json_array_get_string_element(tag, 1);
        if (zone_name) {
          zone->zone_name = g_strdup(zone_name);
        }

        const gchar *cost_str = json_array_get_string_element(tag, 2);
        if (cost_str) {
          zone->cost = g_ascii_strtod(cost_str, NULL);
        }

        /* Remaining elements are regions */
        for (guint j = 3; j < tag_len; j++) {
          const gchar *region = json_array_get_string_element(tag, j);
          if (region && *region) {
            gnostr_shipping_zone_add_region(zone, region);
          }
        }

        gnostr_stall_add_shipping_zone(stall, zone);
      }
    }
  }


  /* Validate: must have stall_id */
  if (!stall->stall_id || !*stall->stall_id) {
    g_debug("NIP-15: Stall missing 'd' tag identifier");
    gnostr_stall_free(stall);
    return NULL;
  }

  return stall;
}

gchar *
gnostr_stall_get_naddr(const GnostrStall *stall)
{
  if (!stall || !stall->pubkey || !stall->stall_id) return NULL;
  return g_strdup_printf("%d:%s:%s",
                         NIP15_KIND_STALL,
                         stall->pubkey,
                         stall->stall_id);
}

gchar *
gnostr_stall_build_tags(const GnostrStall *stall)
{
  if (!stall || !stall->stall_id) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* d tag - stall ID */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "d");
  json_builder_add_string_value(builder, stall->stall_id);
  json_builder_end_array(builder);

  /* name tag */
  if (stall->name && *stall->name) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "name");
    json_builder_add_string_value(builder, stall->name);
    json_builder_end_array(builder);
  }

  /* description tag */
  if (stall->description && *stall->description) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "description");
    json_builder_add_string_value(builder, stall->description);
    json_builder_end_array(builder);
  }

  /* image tag */
  if (stall->image && *stall->image) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "image");
    json_builder_add_string_value(builder, stall->image);
    json_builder_end_array(builder);
  }

  /* currency tag */
  if (stall->currency && *stall->currency) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "currency");
    json_builder_add_string_value(builder, stall->currency);
    json_builder_end_array(builder);
  }

  /* shipping tags */
  if (stall->shipping_zones) {
    for (guint i = 0; i < stall->shipping_zones->len; i++) {
      GnostrShippingZone *zone = g_ptr_array_index(stall->shipping_zones, i);
      if (!zone) continue;

      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "shipping");

      /* Zone name */
      json_builder_add_string_value(builder, zone->zone_name ? zone->zone_name : "");

      /* Cost */
      gchar *cost_str = g_strdup_printf("%.2f", zone->cost);
      json_builder_add_string_value(builder, cost_str);
      g_free(cost_str);

      /* Regions */
      if (zone->regions) {
        for (guint j = 0; j < zone->regions->len; j++) {
          const gchar *region = g_ptr_array_index(zone->regions, j);
          if (region && *region) {
            json_builder_add_string_value(builder, region);
          }
        }
      }

      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

/* ============== Product Spec ============== */

GnostrProductSpec *
gnostr_product_spec_new(const gchar *key, const gchar *value)
{
  GnostrProductSpec *spec = g_new0(GnostrProductSpec, 1);
  spec->key = g_strdup(key);
  spec->value = g_strdup(value);
  return spec;
}

void
gnostr_product_spec_free(GnostrProductSpec *spec)
{
  if (!spec) return;
  g_free(spec->key);
  g_free(spec->value);
  g_free(spec);
}

/* ============== Product ============== */

GnostrProduct *
gnostr_product_new(void)
{
  GnostrProduct *product = g_new0(GnostrProduct, 1);
  product->images = g_ptr_array_new_with_free_func(g_free);
  product->specs = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_product_spec_free);
  product->categories = g_ptr_array_new_with_free_func(g_free);
  product->quantity = -1;  /* -1 means unlimited/unspecified */
  return product;
}

void
gnostr_product_free(GnostrProduct *product)
{
  if (!product) return;
  g_free(product->product_id);
  g_free(product->stall_id);
  g_free(product->stall_event_id);
  g_free(product->stall_relay);
  g_free(product->name);
  g_free(product->description);
  g_free(product->currency);
  g_free(product->pubkey);
  g_free(product->event_id);
  if (product->images) {
    g_ptr_array_unref(product->images);
  }
  if (product->specs) {
    g_ptr_array_unref(product->specs);
  }
  if (product->categories) {
    g_ptr_array_unref(product->categories);
  }
  g_free(product);
}

void
gnostr_product_add_image(GnostrProduct *product, const gchar *image_url)
{
  if (!product || !image_url || !*image_url) return;
  g_ptr_array_add(product->images, g_strdup(image_url));
  product->image_count = product->images->len;
}

void
gnostr_product_add_spec(GnostrProduct *product, const gchar *key, const gchar *value)
{
  if (!product || !key || !*key) return;
  GnostrProductSpec *spec = gnostr_product_spec_new(key, value);
  g_ptr_array_add(product->specs, spec);
  product->spec_count = product->specs->len;
}

void
gnostr_product_add_category(GnostrProduct *product, const gchar *category)
{
  if (!product || !category || !*category) return;
  g_ptr_array_add(product->categories, g_strdup(category));
}

GnostrProduct *
gnostr_product_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_debug("NIP-15: Failed to parse product JSON: %s", error ? error->message : "unknown");
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
  if (kind != NIP15_KIND_PRODUCT) {
    return NULL;
  }

  GnostrProduct *product = gnostr_product_new();

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    product->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    product->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    product->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Parse tags */
  if (json_object_has_member(obj, "tags")) {
    JsonArray *tags = json_object_get_array_member(obj, "tags");
    guint n_tags = json_array_get_length(tags);

    for (guint i = 0; i < n_tags; i++) {
      JsonNode *tag_node = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

      JsonArray *tag = json_node_get_array(tag_node);
      guint tag_len = json_array_get_length(tag);
      if (tag_len < 2) continue;

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      if (!tag_name) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        /* Product ID: ["d", "<product_id>"] */
        const gchar *product_id = json_array_get_string_element(tag, 1);
        if (product_id) {
          product->product_id = g_strdup(product_id);
        }
      } else if (g_strcmp0(tag_name, "stall") == 0) {
        /* Stall reference: ["stall", "<stall_id>", "<stall_event_id>", "<relay>"] */
        const gchar *stall_id = json_array_get_string_element(tag, 1);
        if (stall_id) {
          product->stall_id = g_strdup(stall_id);
        }
        if (tag_len >= 3) {
          const gchar *stall_event_id = json_array_get_string_element(tag, 2);
          if (stall_event_id && *stall_event_id) {
            product->stall_event_id = g_strdup(stall_event_id);
          }
        }
        if (tag_len >= 4) {
          const gchar *relay = json_array_get_string_element(tag, 3);
          if (relay && *relay) {
            product->stall_relay = g_strdup(relay);
          }
        }
      } else if (g_strcmp0(tag_name, "name") == 0) {
        /* Name: ["name", "<name>"] */
        const gchar *name = json_array_get_string_element(tag, 1);
        if (name) {
          product->name = g_strdup(name);
        }
      } else if (g_strcmp0(tag_name, "description") == 0) {
        /* Description: ["description", "<desc>"] */
        const gchar *desc = json_array_get_string_element(tag, 1);
        if (desc) {
          product->description = g_strdup(desc);
        }
      } else if (g_strcmp0(tag_name, "images") == 0) {
        /* Images: ["images", "<url1>", "<url2>", ...] */
        for (guint j = 1; j < tag_len; j++) {
          const gchar *url = json_array_get_string_element(tag, j);
          if (url && *url) {
            gnostr_product_add_image(product, url);
          }
        }
      } else if (g_strcmp0(tag_name, "price") == 0 && tag_len >= 2) {
        /* Price: ["price", "<amount>", "<currency>"] */
        const gchar *amount_str = json_array_get_string_element(tag, 1);
        if (amount_str) {
          product->price = g_ascii_strtod(amount_str, NULL);
        }
        if (tag_len >= 3) {
          const gchar *currency = json_array_get_string_element(tag, 2);
          if (currency && *currency) {
            product->currency = g_strdup(currency);
          }
        }
      } else if (g_strcmp0(tag_name, "quantity") == 0) {
        /* Quantity: ["quantity", "<num>"] */
        const gchar *qty_str = json_array_get_string_element(tag, 1);
        if (qty_str) {
          product->quantity = (gint)g_ascii_strtoll(qty_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "specs") == 0 && tag_len >= 3) {
        /* Specs: ["specs", "<key1>", "<value1>", "<key2>", "<value2>", ...] */
        for (guint j = 1; j + 1 < tag_len; j += 2) {
          const gchar *key = json_array_get_string_element(tag, j);
          const gchar *value = json_array_get_string_element(tag, j + 1);
          if (key && *key) {
            gnostr_product_add_spec(product, key, value ? value : "");
          }
        }
      } else if (g_strcmp0(tag_name, "t") == 0) {
        /* Category: ["t", "<category>"] */
        const gchar *category = json_array_get_string_element(tag, 1);
        if (category && *category) {
          gnostr_product_add_category(product, category);
        }
      }
    }
  }


  /* Validate: must have product_id */
  if (!product->product_id || !*product->product_id) {
    g_debug("NIP-15: Product missing 'd' tag identifier");
    gnostr_product_free(product);
    return NULL;
  }

  return product;
}

gchar *
gnostr_product_get_naddr(const GnostrProduct *product)
{
  if (!product || !product->pubkey || !product->product_id) return NULL;
  return g_strdup_printf("%d:%s:%s",
                         NIP15_KIND_PRODUCT,
                         product->pubkey,
                         product->product_id);
}

gchar *
gnostr_product_build_tags(const GnostrProduct *product)
{
  if (!product || !product->product_id) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* d tag - product ID */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "d");
  json_builder_add_string_value(builder, product->product_id);
  json_builder_end_array(builder);

  /* stall tag */
  if (product->stall_id && *product->stall_id) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "stall");
    json_builder_add_string_value(builder, product->stall_id);
    if (product->stall_event_id && *product->stall_event_id) {
      json_builder_add_string_value(builder, product->stall_event_id);
      if (product->stall_relay && *product->stall_relay) {
        json_builder_add_string_value(builder, product->stall_relay);
      }
    }
    json_builder_end_array(builder);
  }

  /* name tag */
  if (product->name && *product->name) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "name");
    json_builder_add_string_value(builder, product->name);
    json_builder_end_array(builder);
  }

  /* description tag */
  if (product->description && *product->description) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "description");
    json_builder_add_string_value(builder, product->description);
    json_builder_end_array(builder);
  }

  /* images tag */
  if (product->images && product->images->len > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "images");
    for (guint i = 0; i < product->images->len; i++) {
      const gchar *url = g_ptr_array_index(product->images, i);
      if (url && *url) {
        json_builder_add_string_value(builder, url);
      }
    }
    json_builder_end_array(builder);
  }

  /* price tag */
  if (product->price > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "price");
    gchar *price_str = g_strdup_printf("%.8g", product->price);
    json_builder_add_string_value(builder, price_str);
    g_free(price_str);
    if (product->currency && *product->currency) {
      json_builder_add_string_value(builder, product->currency);
    }
    json_builder_end_array(builder);
  }

  /* quantity tag */
  if (product->quantity >= 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "quantity");
    gchar *qty_str = g_strdup_printf("%d", product->quantity);
    json_builder_add_string_value(builder, qty_str);
    g_free(qty_str);
    json_builder_end_array(builder);
  }

  /* specs tag */
  if (product->specs && product->specs->len > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "specs");
    for (guint i = 0; i < product->specs->len; i++) {
      GnostrProductSpec *spec = g_ptr_array_index(product->specs, i);
      if (spec && spec->key && *spec->key) {
        json_builder_add_string_value(builder, spec->key);
        json_builder_add_string_value(builder, spec->value ? spec->value : "");
      }
    }
    json_builder_end_array(builder);
  }

  /* category t tags */
  if (product->categories) {
    for (guint i = 0; i < product->categories->len; i++) {
      const gchar *category = g_ptr_array_index(product->categories, i);
      if (category && *category) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "t");
        json_builder_add_string_value(builder, category);
        json_builder_end_array(builder);
      }
    }
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

/* ============== Price Formatting Helpers ============== */

gchar *
gnostr_marketplace_format_price(gdouble price, const gchar *currency)
{
  if (!currency || !*currency) {
    return g_strdup_printf("%.2f", price);
  }

  /* Handle common currencies */
  if (g_ascii_strcasecmp(currency, "sat") == 0 ||
      g_ascii_strcasecmp(currency, "sats") == 0) {
    return gnostr_marketplace_format_price_sats((gint64)price);
  } else if (g_ascii_strcasecmp(currency, "USD") == 0) {
    return g_strdup_printf("$%.2f", price);
  } else if (g_ascii_strcasecmp(currency, "EUR") == 0) {
    return g_strdup_printf("%.2f EUR", price);
  } else if (g_ascii_strcasecmp(currency, "GBP") == 0) {
    return g_strdup_printf("%.2f GBP", price);
  } else if (g_ascii_strcasecmp(currency, "BTC") == 0) {
    return g_strdup_printf("%.8f BTC", price);
  } else {
    /* Generic format for unknown currencies */
    return g_strdup_printf("%.2f %s", price, currency);
  }
}

gchar *
gnostr_marketplace_format_price_sats(gint64 sats)
{
  if (sats >= 100000000) {
    /* 100M+ sats = show in BTC */
    return g_strdup_printf("%.2f BTC", sats / 100000000.0);
  } else if (sats >= 1000000) {
    /* 1M+ sats */
    gdouble val = sats / 1000000.0;
    if (fabs(val - (gint64)val) < 0.001) {
      return g_strdup_printf("%.0fM sats", val);
    }
    return g_strdup_printf("%.1fM sats", val);
  } else if (sats >= 10000) {
    /* 10K+ sats */
    gdouble val = sats / 1000.0;
    if (fabs(val - (gint64)val) < 0.001) {
      return g_strdup_printf("%.0fK sats", val);
    }
    return g_strdup_printf("%.1fK sats", val);
  } else if (sats >= 1000) {
    /* 1K+ sats - with thousands separator */
    return g_strdup_printf("%'" G_GINT64_FORMAT " sats", sats);
  } else {
    return g_strdup_printf("%" G_GINT64_FORMAT " sats", sats);
  }
}

gchar *
gnostr_marketplace_format_quantity(gint quantity)
{
  if (quantity < 0) {
    return g_strdup("In stock");
  } else if (quantity == 0) {
    return g_strdup("Out of stock");
  } else if (quantity == 1) {
    return g_strdup("1 available");
  } else {
    return g_strdup_printf("%d available", quantity);
  }
}

gboolean
gnostr_marketplace_is_stall_kind(gint kind)
{
  return kind == NIP15_KIND_STALL;
}

gboolean
gnostr_marketplace_is_product_kind(gint kind)
{
  return kind == NIP15_KIND_PRODUCT;
}
