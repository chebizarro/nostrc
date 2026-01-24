/**
 * NIP-43 Relay Access Metadata - Implementation
 *
 * Parses relay access requirements from NIP-11 relay information documents.
 * Handles limitation object and fees structure.
 */

#include "nip43_access.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* Period constants in seconds */
#define SECONDS_PER_HOUR   3600
#define SECONDS_PER_DAY    86400
#define SECONDS_PER_WEEK   604800
#define SECONDS_PER_MONTH  2592000   /* 30 days */
#define SECONDS_PER_YEAR   31536000  /* 365 days */

/* ============== Memory Management ============== */

void gnostr_relay_fee_free(GnostrRelayFee *fee) {
  if (!fee) return;
  g_free(fee->unit);
  g_free(fee);
}

static void gnostr_relay_fee_array_free(GnostrRelayFee *fees, gsize count) {
  if (!fees) return;
  for (gsize i = 0; i < count; i++) {
    g_free(fees[i].unit);
  }
  g_free(fees);
}

void gnostr_relay_fees_free(GnostrRelayFees *fees) {
  if (!fees) return;
  gnostr_relay_fee_array_free(fees->admission, fees->admission_count);
  gnostr_relay_fee_array_free(fees->subscription, fees->subscription_count);
  gnostr_relay_fee_array_free(fees->publication, fees->publication_count);
  g_free(fees);
}

void gnostr_relay_access_free(GnostrRelayAccess *access) {
  if (!access) return;
  g_free(access->payments_url);
  gnostr_relay_fees_free(access->fees);
  g_free(access);
}

/* ============== JSON Parsing Helpers ============== */

/* Helper to get optional string from JSON object */
static gchar *json_object_get_string_or_null(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return NULL;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return NULL;
  const gchar *val = json_node_get_string(node);
  return val ? g_strdup(val) : NULL;
}

/* Helper to get optional int64 from JSON object */
static gint64 json_object_get_int64_or_zero(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return 0;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return 0;
  return json_node_get_int(node);
}

/* Helper to get optional bool from JSON object */
static gboolean json_object_get_bool_or_false(JsonObject *obj, const gchar *member) {
  if (!json_object_has_member(obj, member)) return FALSE;
  JsonNode *node = json_object_get_member(obj, member);
  if (!node || JSON_NODE_TYPE(node) != JSON_NODE_VALUE) return FALSE;
  return json_node_get_boolean(node);
}

/**
 * Parse a single fee object from JSON:
 * {"amount": 1000000, "unit": "msats", "period": 2592000}
 */
static gboolean parse_single_fee(JsonObject *fee_obj, GnostrRelayFee *out_fee) {
  if (!fee_obj || !out_fee) return FALSE;

  out_fee->amount = json_object_get_int64_or_zero(fee_obj, "amount");
  out_fee->unit = json_object_get_string_or_null(fee_obj, "unit");
  out_fee->period = json_object_get_int64_or_zero(fee_obj, "period");

  /* Default unit to msats if not specified */
  if (!out_fee->unit) {
    out_fee->unit = g_strdup("msats");
  }

  return TRUE;
}

/**
 * Parse a fee array (e.g., admission, subscription, publication)
 */
static GnostrRelayFee *parse_fee_array(JsonArray *arr, gsize *out_count) {
  if (!arr || !out_count) {
    if (out_count) *out_count = 0;
    return NULL;
  }

  gsize len = json_array_get_length(arr);
  if (len == 0) {
    *out_count = 0;
    return NULL;
  }

  GnostrRelayFee *fees = g_new0(GnostrRelayFee, len);
  gsize actual = 0;

  for (gsize i = 0; i < len; i++) {
    JsonNode *node = json_array_get_element(arr, i);
    if (!node || JSON_NODE_TYPE(node) != JSON_NODE_OBJECT) continue;

    JsonObject *fee_obj = json_node_get_object(node);
    if (parse_single_fee(fee_obj, &fees[actual])) {
      actual++;
    }
  }

  *out_count = actual;

  if (actual == 0) {
    g_free(fees);
    return NULL;
  }

  return fees;
}

/* ============== Parsing Functions ============== */

GnostrRelayFees *gnostr_relay_fees_parse(const gchar *fees_json) {
  if (!fees_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *err = NULL;

  if (!json_parser_load_from_data(parser, fees_json, -1, &err)) {
    g_warning("nip43: fees JSON parse error: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  GnostrRelayFees *fees = g_new0(GnostrRelayFees, 1);

  /* Parse admission fees */
  if (json_object_has_member(obj, "admission")) {
    JsonArray *admission_arr = json_object_get_array_member(obj, "admission");
    fees->admission = parse_fee_array(admission_arr, &fees->admission_count);
  }

  /* Parse subscription fees */
  if (json_object_has_member(obj, "subscription")) {
    JsonArray *subscription_arr = json_object_get_array_member(obj, "subscription");
    fees->subscription = parse_fee_array(subscription_arr, &fees->subscription_count);
  }

  /* Parse publication fees */
  if (json_object_has_member(obj, "publication")) {
    JsonArray *publication_arr = json_object_get_array_member(obj, "publication");
    fees->publication = parse_fee_array(publication_arr, &fees->publication_count);
  }

  g_object_unref(parser);
  return fees;
}

GnostrRelayAccess *gnostr_relay_access_parse_info_object(gpointer root_object) {
  JsonObject *obj = (JsonObject *)root_object;
  if (!obj) return NULL;

  GnostrRelayAccess *access = g_new0(GnostrRelayAccess, 1);

  /* Parse limitation object */
  if (json_object_has_member(obj, "limitation")) {
    JsonObject *lim = json_object_get_object_member(obj, "limitation");
    if (lim) {
      access->auth_required = json_object_get_bool_or_false(lim, "auth_required");
      access->payment_required = json_object_get_bool_or_false(lim, "payment_required");
      access->restricted_writes = json_object_get_bool_or_false(lim, "restricted_writes");
    }
  }

  /* Parse payments_url */
  access->payments_url = json_object_get_string_or_null(obj, "payments_url");

  /* Parse fees object */
  if (json_object_has_member(obj, "fees")) {
    JsonObject *fees_obj = json_object_get_object_member(obj, "fees");
    if (fees_obj) {
      access->fees = g_new0(GnostrRelayFees, 1);

      /* Parse admission fees */
      if (json_object_has_member(fees_obj, "admission")) {
        JsonArray *admission_arr = json_object_get_array_member(fees_obj, "admission");
        access->fees->admission = parse_fee_array(admission_arr, &access->fees->admission_count);
      }

      /* Parse subscription fees */
      if (json_object_has_member(fees_obj, "subscription")) {
        JsonArray *subscription_arr = json_object_get_array_member(fees_obj, "subscription");
        access->fees->subscription = parse_fee_array(subscription_arr, &access->fees->subscription_count);
      }

      /* Parse publication fees */
      if (json_object_has_member(fees_obj, "publication")) {
        JsonArray *publication_arr = json_object_get_array_member(fees_obj, "publication");
        access->fees->publication = parse_fee_array(publication_arr, &access->fees->publication_count);
      }

      /* If no fees were parsed, free the empty struct */
      if (access->fees->admission_count == 0 &&
          access->fees->subscription_count == 0 &&
          access->fees->publication_count == 0) {
        gnostr_relay_fees_free(access->fees);
        access->fees = NULL;
      }
    }
  }

  return access;
}

GnostrRelayAccess *gnostr_relay_access_parse_info(const gchar *info_json) {
  if (!info_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *err = NULL;

  if (!json_parser_load_from_data(parser, info_json, -1, &err)) {
    g_warning("nip43: info JSON parse error: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  GnostrRelayAccess *access = gnostr_relay_access_parse_info_object(obj);

  g_object_unref(parser);
  return access;
}

/* ============== Helper Functions ============== */

gboolean gnostr_relay_access_requires_payment(const GnostrRelayAccess *access) {
  if (!access) return FALSE;

  /* Explicit payment_required flag */
  if (access->payment_required) return TRUE;

  /* Check if any fees are specified */
  if (access->fees) {
    if (access->fees->admission_count > 0) return TRUE;
    if (access->fees->subscription_count > 0) return TRUE;
    if (access->fees->publication_count > 0) return TRUE;
  }

  return FALSE;
}

gboolean gnostr_relay_access_has_admission_fee(const GnostrRelayAccess *access) {
  if (!access || !access->fees) return FALSE;
  return access->fees->admission_count > 0;
}

gboolean gnostr_relay_access_has_subscription_fee(const GnostrRelayAccess *access) {
  if (!access || !access->fees) return FALSE;
  return access->fees->subscription_count > 0;
}

gboolean gnostr_relay_access_has_publication_fee(const GnostrRelayAccess *access) {
  if (!access || !access->fees) return FALSE;
  return access->fees->publication_count > 0;
}

/* ============== Unit Conversion ============== */

gint64 gnostr_relay_fee_to_msats(gint64 amount, const gchar *unit) {
  if (!unit) return amount; /* Assume msats if no unit */

  if (g_ascii_strcasecmp(unit, "msats") == 0 ||
      g_ascii_strcasecmp(unit, "msat") == 0 ||
      g_ascii_strcasecmp(unit, "millisats") == 0 ||
      g_ascii_strcasecmp(unit, "millisatoshis") == 0) {
    return amount;
  }

  if (g_ascii_strcasecmp(unit, "sats") == 0 ||
      g_ascii_strcasecmp(unit, "sat") == 0 ||
      g_ascii_strcasecmp(unit, "satoshis") == 0) {
    return amount * 1000;
  }

  if (g_ascii_strcasecmp(unit, "btc") == 0 ||
      g_ascii_strcasecmp(unit, "bitcoin") == 0) {
    return amount * 100000000000LL; /* 1 BTC = 100,000,000 sats = 100,000,000,000 msats */
  }

  /* Unknown unit */
  g_warning("nip43: unknown fee unit '%s', assuming msats", unit);
  return amount;
}

gint64 gnostr_relay_access_get_min_admission_msats(const GnostrRelayAccess *access) {
  if (!access || !access->fees || access->fees->admission_count == 0) {
    return 0;
  }

  gint64 min_msats = G_MAXINT64;

  for (gsize i = 0; i < access->fees->admission_count; i++) {
    GnostrRelayFee *fee = &access->fees->admission[i];
    gint64 msats = gnostr_relay_fee_to_msats(fee->amount, fee->unit);
    if (msats < min_msats) {
      min_msats = msats;
    }
  }

  return (min_msats == G_MAXINT64) ? 0 : min_msats;
}

gint64 gnostr_relay_access_get_min_subscription_msats(const GnostrRelayAccess *access,
                                                        gint64 *period_out) {
  if (!access || !access->fees || access->fees->subscription_count == 0) {
    if (period_out) *period_out = 0;
    return 0;
  }

  gint64 min_msats = G_MAXINT64;
  gint64 min_period = 0;

  for (gsize i = 0; i < access->fees->subscription_count; i++) {
    GnostrRelayFee *fee = &access->fees->subscription[i];
    gint64 msats = gnostr_relay_fee_to_msats(fee->amount, fee->unit);
    if (msats < min_msats) {
      min_msats = msats;
      min_period = fee->period;
    }
  }

  if (period_out) *period_out = min_period;
  return (min_msats == G_MAXINT64) ? 0 : min_msats;
}

/* ============== Formatting Helpers ============== */

const gchar *gnostr_relay_fee_period_to_string(gint64 period_seconds) {
  if (period_seconds <= 0) {
    return "one-time";
  }

  /* Approximate period matching */
  if (period_seconds <= SECONDS_PER_HOUR) {
    return "hour";
  } else if (period_seconds <= SECONDS_PER_DAY) {
    return "day";
  } else if (period_seconds <= SECONDS_PER_WEEK) {
    return "week";
  } else if (period_seconds <= SECONDS_PER_MONTH) {
    return "month";
  } else {
    return "year";
  }
}

gchar *gnostr_relay_fee_format(const GnostrRelayFee *fee) {
  if (!fee) return g_strdup("(unknown)");

  gint64 msats = gnostr_relay_fee_to_msats(fee->amount, fee->unit);

  /* Format in sats if amount is large enough */
  if (msats >= 1000) {
    gint64 sats = msats / 1000;
    gint64 remainder = msats % 1000;

    if (fee->period > 0) {
      const gchar *period_str = gnostr_relay_fee_period_to_string(fee->period);
      if (remainder > 0) {
        return g_strdup_printf("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT " sats/%s",
                               sats, remainder, period_str);
      } else {
        return g_strdup_printf("%" G_GINT64_FORMAT " sats/%s", sats, period_str);
      }
    } else {
      if (remainder > 0) {
        return g_strdup_printf("%" G_GINT64_FORMAT ".%03" G_GINT64_FORMAT " sats",
                               sats, remainder);
      } else {
        return g_strdup_printf("%" G_GINT64_FORMAT " sats", sats);
      }
    }
  } else {
    /* Show in msats for small amounts */
    if (fee->period > 0) {
      const gchar *period_str = gnostr_relay_fee_period_to_string(fee->period);
      return g_strdup_printf("%" G_GINT64_FORMAT " msats/%s", msats, period_str);
    } else {
      return g_strdup_printf("%" G_GINT64_FORMAT " msats", msats);
    }
  }
}

gchar *gnostr_relay_fees_format_summary(const GnostrRelayFees *fees) {
  if (!fees) return g_strdup("(no fees)");

  GString *str = g_string_new(NULL);
  gboolean first = TRUE;

  /* Format admission fees */
  if (fees->admission_count > 0) {
    gchar *fee_str = gnostr_relay_fee_format(&fees->admission[0]);
    g_string_append_printf(str, "Admission: %s", fee_str);
    g_free(fee_str);
    first = FALSE;
  }

  /* Format subscription fees */
  if (fees->subscription_count > 0) {
    if (!first) g_string_append(str, ", ");
    gchar *fee_str = gnostr_relay_fee_format(&fees->subscription[0]);
    g_string_append_printf(str, "Subscription: %s", fee_str);
    g_free(fee_str);
    first = FALSE;
  }

  /* Format publication fees */
  if (fees->publication_count > 0) {
    if (!first) g_string_append(str, ", ");
    gchar *fee_str = gnostr_relay_fee_format(&fees->publication[0]);
    g_string_append_printf(str, "Per event: %s", fee_str);
    g_free(fee_str);
    first = FALSE;
  }

  if (first) {
    g_string_free(str, TRUE);
    return g_strdup("(no fees)");
  }

  return g_string_free(str, FALSE);
}

gchar *gnostr_relay_access_format_requirements(const GnostrRelayAccess *access) {
  if (!access) return g_strdup("(unknown)");

  GString *str = g_string_new(NULL);
  gboolean has_any = FALSE;

  if (access->auth_required) {
    g_string_append(str, "Authentication required");
    has_any = TRUE;
  }

  if (access->payment_required || gnostr_relay_access_requires_payment(access)) {
    if (has_any) g_string_append(str, "\n");
    g_string_append(str, "Payment required");
    has_any = TRUE;

    /* Add fee details */
    if (access->fees) {
      gchar *fees_str = gnostr_relay_fees_format_summary(access->fees);
      if (g_strcmp0(fees_str, "(no fees)") != 0) {
        g_string_append_printf(str, " (%s)", fees_str);
      }
      g_free(fees_str);
    }

    /* Add payments URL if available */
    if (access->payments_url) {
      g_string_append_printf(str, "\nPayment page: %s", access->payments_url);
    }
  }

  if (access->restricted_writes) {
    if (has_any) g_string_append(str, "\n");
    g_string_append(str, "Writes restricted");
    has_any = TRUE;
  }

  if (!has_any) {
    g_string_free(str, TRUE);
    return g_strdup("Open access");
  }

  return g_string_free(str, FALSE);
}
