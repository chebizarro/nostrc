/**
 * NIP-60: Cashu Wallet Utility Implementation
 *
 * Implements parsing and building of Cashu wallet events:
 * - Kind 17375: Token events (encrypted ecash proofs)
 * - Kind 7375: Wallet transaction history
 */

#define G_LOG_DOMAIN "nip60-cashu"

#include "nip60_cashu.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* ============== Kind Checking ============== */

gboolean
gnostr_nip60_is_token_kind(gint kind)
{
  return kind == NIP60_KIND_TOKEN;
}

gboolean
gnostr_nip60_is_history_kind(gint kind)
{
  return kind == NIP60_KIND_HISTORY;
}

/* ============== Token API ============== */

GnostrCashuToken *
gnostr_cashu_token_new(void)
{
  return g_new0(GnostrCashuToken, 1);
}

void
gnostr_cashu_token_free(GnostrCashuToken *token)
{
  if (!token) return;
  g_free(token->proofs_json);
  g_free(token->mint_url);
  g_free(token->unit);
  g_free(token->event_id);
  g_free(token->direction);
  g_free(token->counterparty);
  g_free(token->related_event_id);
  g_free(token->wallet_ref);
  g_free(token);
}

/**
 * Parse tags from a JSON object into a token structure.
 * Internal helper function.
 */
static void
parse_token_tags(JsonObject *root, GnostrCashuToken *token)
{
  JsonArray *tags = json_object_get_array_member(root, "tags");
  if (!tags) return;

  guint len = json_array_get_length(tags);
  for (guint i = 0; i < len; i++) {
    JsonArray *tag = json_array_get_array_element(tags, i);
    if (!tag || json_array_get_length(tag) < 2) continue;

    const gchar *tag_name = json_array_get_string_element(tag, 0);
    const gchar *tag_value = json_array_get_string_element(tag, 1);
    if (!tag_name || !tag_value) continue;

    if (g_strcmp0(tag_name, "a") == 0 && !token->wallet_ref) {
      token->wallet_ref = g_strdup(tag_value);
    } else if (g_strcmp0(tag_name, "e") == 0 && !token->related_event_id) {
      token->related_event_id = g_strdup(tag_value);
    } else if (g_strcmp0(tag_name, "direction") == 0) {
      g_free(token->direction);
      token->direction = g_strdup(tag_value);
    } else if (g_strcmp0(tag_name, "amount") == 0) {
      token->amount_msats = g_ascii_strtoll(tag_value, NULL, 10);
    } else if (g_strcmp0(tag_name, "unit") == 0) {
      g_free(token->unit);
      token->unit = g_strdup(tag_value);
    } else if (g_strcmp0(tag_name, "p") == 0 && !token->counterparty) {
      token->counterparty = g_strdup(tag_value);
    }
  }
}

GnostrCashuToken *
gnostr_cashu_token_parse(const gchar *event_json,
                          const gchar *decrypted_content)
{
  if (!event_json || !*event_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("cashu_token: failed to parse event JSON: %s", error->message);
    g_error_free(error);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_warning("cashu_token: invalid JSON structure");
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify kind */
  gint64 kind = json_object_get_int_member(root, "kind");
  if (kind != NIP60_KIND_TOKEN) {
    g_debug("cashu_token: wrong kind %ld, expected %d", (long)kind, NIP60_KIND_TOKEN);
    return NULL;
  }

  GnostrCashuToken *token = gnostr_cashu_token_new();

  /* Extract event ID */
  if (json_object_has_member(root, "id")) {
    token->event_id = g_strdup(json_object_get_string_member(root, "id"));
  }

  /* Extract created_at */
  if (json_object_has_member(root, "created_at")) {
    token->created_at = json_object_get_int_member(root, "created_at");
  }

  /* Parse tags */
  parse_token_tags(root, token);

  /* Parse decrypted content if provided */
  if (decrypted_content && *decrypted_content) {
    /* The decrypted content should contain Cashu token data */
    g_autoptr(JsonParser) content_parser = json_parser_new();

    if (json_parser_load_from_data(content_parser, decrypted_content, -1, NULL)) {
      JsonNode *content_node = json_parser_get_root(content_parser);
      if (content_node && JSON_NODE_HOLDS_OBJECT(content_node)) {
        JsonObject *content_obj = json_node_get_object(content_node);

        /* Extract proofs array */
        if (json_object_has_member(content_obj, "proofs")) {
          JsonArray *proofs = json_object_get_array_member(content_obj, "proofs");
          if (proofs) {
            g_autoptr(JsonGenerator) gen = json_generator_new();
            JsonNode *proofs_node = json_array_get_element(proofs, 0);
            /* Store the whole proofs array */
            JsonNode *array_node = json_node_new(JSON_NODE_ARRAY);
            json_node_set_array(array_node, json_array_ref(proofs));
            json_generator_set_root(gen, array_node);
            token->proofs_json = json_generator_to_data(gen, NULL);
            json_node_free(array_node);
          }
        }

        /* Extract mint URL */
        if (json_object_has_member(content_obj, "mint")) {
          token->mint_url = g_strdup(json_object_get_string_member(content_obj, "mint"));
        }

        /* Extract unit if present in content */
        if (json_object_has_member(content_obj, "unit") && !token->unit) {
          token->unit = g_strdup(json_object_get_string_member(content_obj, "unit"));
        }
      }
    } else {
      /* Content might be just the proofs JSON directly */
      token->proofs_json = g_strdup(decrypted_content);
    }
  }

  /* Set default unit if not specified */
  if (!token->unit) {
    token->unit = g_strdup(NIP60_UNIT_SAT);
  }

  g_debug("cashu_token: parsed token (amount=%ld %s, direction=%s)",
          (long)token->amount_msats,
          token->unit ? token->unit : "unknown",
          token->direction ? token->direction : "unknown");

  return token;
}

GnostrCashuToken *
gnostr_cashu_token_parse_tags(const gchar *event_json)
{
  /* Parse with NULL decrypted content - only extracts tags */
  return gnostr_cashu_token_parse(event_json, NULL);
}

/* ============== Transaction API ============== */

GnostrCashuTx *
gnostr_cashu_tx_new(void)
{
  return g_new0(GnostrCashuTx, 1);
}

void
gnostr_cashu_tx_free(GnostrCashuTx *tx)
{
  if (!tx) return;
  g_free(tx->event_id);
  g_free(tx->direction);
  g_free(tx->unit);
  g_free(tx->counterparty);
  g_free(tx->wallet_ref);
  g_free(tx->related_event_id);
  g_free(tx);
}

GnostrCashuTx *
gnostr_cashu_tx_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("cashu_tx: failed to parse event JSON: %s", error->message);
    g_error_free(error);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_warning("cashu_tx: invalid JSON structure");
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify kind */
  gint64 kind = json_object_get_int_member(root, "kind");
  if (kind != NIP60_KIND_HISTORY) {
    g_debug("cashu_tx: wrong kind %ld, expected %d", (long)kind, NIP60_KIND_HISTORY);
    return NULL;
  }

  GnostrCashuTx *tx = gnostr_cashu_tx_new();

  /* Extract event ID */
  if (json_object_has_member(root, "id")) {
    tx->event_id = g_strdup(json_object_get_string_member(root, "id"));
  }

  /* Extract created_at as timestamp */
  if (json_object_has_member(root, "created_at")) {
    tx->timestamp = json_object_get_int_member(root, "created_at");
  }

  /* Parse tags */
  JsonArray *tags = json_object_get_array_member(root, "tags");
  if (tags) {
    guint len = json_array_get_length(tags);
    for (guint i = 0; i < len; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2) continue;

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      const gchar *tag_value = json_array_get_string_element(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "a") == 0 && !tx->wallet_ref) {
        tx->wallet_ref = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "e") == 0 && !tx->related_event_id) {
        tx->related_event_id = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "direction") == 0) {
        g_free(tx->direction);
        tx->direction = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "amount") == 0) {
        tx->amount_msats = g_ascii_strtoll(tag_value, NULL, 10);
      } else if (g_strcmp0(tag_name, "unit") == 0) {
        g_free(tx->unit);
        tx->unit = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "p") == 0 && !tx->counterparty) {
        tx->counterparty = g_strdup(tag_value);
      }
    }
  }

  /* Set default unit if not specified */
  if (!tx->unit) {
    tx->unit = g_strdup(NIP60_UNIT_SAT);
  }

  g_debug("cashu_tx: parsed transaction (amount=%ld %s, direction=%s)",
          (long)tx->amount_msats,
          tx->unit ? tx->unit : "unknown",
          tx->direction ? tx->direction : "unknown");

  return tx;
}

/* ============== Tag Building ============== */

/**
 * Helper to create a tag array (GPtrArray of strings).
 */
static GPtrArray *
create_tag(const gchar *name, const gchar *value)
{
  GPtrArray *tag = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(tag, g_strdup(name));
  g_ptr_array_add(tag, g_strdup(value));
  return tag;
}

GPtrArray *
gnostr_cashu_build_token_tags(const gchar *wallet_ref,
                               const gchar *direction,
                               gint64 amount_msats,
                               const gchar *unit,
                               const gchar *counterparty,
                               const gchar *related_event_id)
{
  g_return_val_if_fail(wallet_ref != NULL, NULL);
  g_return_val_if_fail(direction != NULL, NULL);
  g_return_val_if_fail(unit != NULL, NULL);

  GPtrArray *tags = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);

  /* Required: wallet reference */
  g_ptr_array_add(tags, create_tag("a", wallet_ref));

  /* Required: direction */
  g_ptr_array_add(tags, create_tag("direction", direction));

  /* Required: amount */
  g_autofree gchar *amount_str = g_strdup_printf("%ld", (long)amount_msats);
  g_ptr_array_add(tags, create_tag("amount", amount_str));

  /* Required: unit */
  g_ptr_array_add(tags, create_tag("unit", unit));

  /* Optional: counterparty */
  if (counterparty && *counterparty) {
    g_ptr_array_add(tags, create_tag("p", counterparty));
  }

  /* Optional: related event */
  if (related_event_id && *related_event_id) {
    g_ptr_array_add(tags, create_tag("e", related_event_id));
  }

  return tags;
}

GPtrArray *
gnostr_cashu_build_history_tags(const gchar *wallet_ref,
                                 const gchar *direction,
                                 gint64 amount_msats,
                                 const gchar *unit,
                                 const gchar *counterparty,
                                 const gchar *related_event_id)
{
  /* History tags use the same structure as token tags */
  return gnostr_cashu_build_token_tags(wallet_ref, direction, amount_msats,
                                        unit, counterparty, related_event_id);
}

/* ============== Utility Functions ============== */

gchar *
gnostr_cashu_format_amount(gint64 amount_msats, const gchar *unit)
{
  if (!unit) unit = NIP60_UNIT_SAT;

  if (g_strcmp0(unit, NIP60_UNIT_SAT) == 0) {
    /* Convert msats to sats */
    gint64 sats = amount_msats / 1000;
    if (sats >= 1000000) {
      return g_strdup_printf("%.2fM sats", (gdouble)sats / 1000000.0);
    } else if (sats >= 1000) {
      return g_strdup_printf("%.1fK sats", (gdouble)sats / 1000.0);
    } else {
      return g_strdup_printf("%ld sats", (long)sats);
    }
  } else if (g_strcmp0(unit, NIP60_UNIT_USD) == 0) {
    /* Amount is in cents (1/100 USD) */
    gdouble dollars = (gdouble)amount_msats / 100.0;
    return g_strdup_printf("$%.2f", dollars);
  } else if (g_strcmp0(unit, NIP60_UNIT_EUR) == 0) {
    /* Amount is in cents (1/100 EUR) */
    gdouble euros = (gdouble)amount_msats / 100.0;
    return g_strdup_printf("\u20AC%.2f", euros);  /* Euro sign */
  } else {
    /* Unknown unit - just show raw amount */
    return g_strdup_printf("%ld %s", (long)amount_msats, unit);
  }
}

gboolean
gnostr_cashu_validate_direction(const gchar *direction)
{
  if (!direction) return FALSE;
  return g_strcmp0(direction, NIP60_DIRECTION_IN) == 0 ||
         g_strcmp0(direction, NIP60_DIRECTION_OUT) == 0;
}

gboolean
gnostr_cashu_validate_unit(const gchar *unit)
{
  if (!unit) return FALSE;
  /* Known units per NIP-60 */
  return g_strcmp0(unit, NIP60_UNIT_SAT) == 0 ||
         g_strcmp0(unit, NIP60_UNIT_USD) == 0 ||
         g_strcmp0(unit, NIP60_UNIT_EUR) == 0;
}

gchar *
gnostr_cashu_get_mint_from_proofs(const gchar *proofs_json)
{
  if (!proofs_json || !*proofs_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, proofs_json, -1, NULL)) {
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node) return NULL;

  /* Proofs can be in various formats - try to find mint URL */
  if (JSON_NODE_HOLDS_OBJECT(root_node)) {
    JsonObject *obj = json_node_get_object(root_node);
    if (json_object_has_member(obj, "mint")) {
      return g_strdup(json_object_get_string_member(obj, "mint"));
    }
  } else if (JSON_NODE_HOLDS_ARRAY(root_node)) {
    /* Array of proofs - check first element */
    JsonArray *arr = json_node_get_array(root_node);
    if (json_array_get_length(arr) > 0) {
      JsonNode *first = json_array_get_element(arr, 0);
      if (JSON_NODE_HOLDS_OBJECT(first)) {
        JsonObject *obj = json_node_get_object(first);
        if (json_object_has_member(obj, "mint")) {
          return g_strdup(json_object_get_string_member(obj, "mint"));
        }
      }
    }
  }

  return NULL;
}

gint64
gnostr_cashu_calculate_proofs_amount(const gchar *proofs_json)
{
  if (!proofs_json || !*proofs_json) return 0;

  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, proofs_json, -1, NULL)) {
    return 0;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node) return 0;

  gint64 total = 0;

  if (JSON_NODE_HOLDS_ARRAY(root_node)) {
    JsonArray *arr = json_node_get_array(root_node);
    guint len = json_array_get_length(arr);

    for (guint i = 0; i < len; i++) {
      JsonNode *elem = json_array_get_element(arr, i);
      if (JSON_NODE_HOLDS_OBJECT(elem)) {
        JsonObject *proof = json_node_get_object(elem);
        if (json_object_has_member(proof, "amount")) {
          total += json_object_get_int_member(proof, "amount");
        }
      }
    }
  } else if (JSON_NODE_HOLDS_OBJECT(root_node)) {
    /* Single proof object */
    JsonObject *proof = json_node_get_object(root_node);
    if (json_object_has_member(proof, "amount")) {
      total = json_object_get_int_member(proof, "amount");
    }
    /* Check for nested proofs array */
    if (json_object_has_member(proof, "proofs")) {
      JsonArray *proofs = json_object_get_array_member(proof, "proofs");
      guint len = json_array_get_length(proofs);
      for (guint i = 0; i < len; i++) {
        JsonNode *elem = json_array_get_element(proofs, i);
        if (JSON_NODE_HOLDS_OBJECT(elem)) {
          JsonObject *p = json_node_get_object(elem);
          if (json_object_has_member(p, "amount")) {
            total += json_object_get_int_member(p, "amount");
          }
        }
      }
    }
  }

  return total;
}
