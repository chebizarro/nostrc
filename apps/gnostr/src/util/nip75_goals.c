/**
 * NIP-75: Zap Goals Utility Implementation
 *
 * Parsing, building, and progress calculation for zap goal events.
 */

#include "nip75_goals.h"
#include "zap.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

gboolean gnostr_nip75_is_goal_kind(gint kind) {
  return kind == NIP75_KIND_ZAP_GOAL;
}

GnostrNip75Goal *gnostr_nip75_goal_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_debug("NIP-75: Failed to parse goal JSON: %s", error ? error->message : "unknown");
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
  if (kind != NIP75_KIND_ZAP_GOAL) {
    return NULL;
  }

  GnostrNip75Goal *goal = g_new0(GnostrNip75Goal, 1);

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    goal->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    goal->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get description from content */
  if (json_object_has_member(obj, "content")) {
    goal->description = g_strdup(json_object_get_string_member(obj, "content"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    goal->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Parse tags */
  GPtrArray *relays_arr = g_ptr_array_new();

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

      if (g_strcmp0(tag_name, "amount") == 0) {
        /* Target amount: ["amount", "millisats"] */
        const gchar *amount_str = json_array_get_string_element(tag, 1);
        if (amount_str) {
          goal->target_msat = g_ascii_strtoll(amount_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "relays") == 0) {
        /* Relays: ["relays", "wss://...", "wss://..."] */
        for (guint j = 1; j < tag_len; j++) {
          const gchar *relay = json_array_get_string_element(tag, j);
          if (relay && *relay) {
            g_ptr_array_add(relays_arr, g_strdup(relay));
          }
        }
      } else if (g_strcmp0(tag_name, "closed_at") == 0) {
        /* Deadline: ["closed_at", "timestamp"] */
        const gchar *ts_str = json_array_get_string_element(tag, 1);
        if (ts_str) {
          goal->closed_at = g_ascii_strtoll(ts_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "e") == 0) {
        /* Linked event: ["e", "event_id"] */
        const gchar *event_id = json_array_get_string_element(tag, 1);
        if (event_id && !goal->linked_event_id) {
          goal->linked_event_id = g_strdup(event_id);
        }
      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* Linked profile: ["p", "pubkey"] */
        const gchar *pubkey = json_array_get_string_element(tag, 1);
        if (pubkey && !goal->linked_pubkey) {
          goal->linked_pubkey = g_strdup(pubkey);
        }
      } else if (g_strcmp0(tag_name, "r") == 0) {
        /* External URL: ["r", "url"] */
        const gchar *url = json_array_get_string_element(tag, 1);
        if (url && !goal->external_url) {
          goal->external_url = g_strdup(url);
        }
      }
    }
  }

  /* Convert relays array to NULL-terminated string array */
  if (relays_arr->len > 0) {
    g_ptr_array_add(relays_arr, NULL);
    goal->relays = (gchar **)g_ptr_array_free(relays_arr, FALSE);
  } else {
    g_ptr_array_free(relays_arr, TRUE);
  }


  /* Validate: must have target amount */
  if (goal->target_msat <= 0) {
    g_debug("NIP-75: Goal missing valid amount tag");
    gnostr_nip75_goal_free(goal);
    return NULL;
  }

  return goal;
}

void gnostr_nip75_goal_free(GnostrNip75Goal *goal) {
  if (!goal) return;
  g_free(goal->event_id);
  g_free(goal->pubkey);
  g_free(goal->description);
  g_strfreev(goal->relays);
  g_free(goal->linked_event_id);
  g_free(goal->linked_pubkey);
  g_free(goal->external_url);
  g_free(goal);
}

GnostrNip75GoalProgress *gnostr_nip75_goal_progress_new(void) {
  return g_new0(GnostrNip75GoalProgress, 1);
}

gboolean gnostr_nip75_goal_is_expired(const GnostrNip75Goal *goal) {
  if (!goal || goal->closed_at <= 0) return FALSE;
  gint64 now = (gint64)time(NULL);
  return now >= goal->closed_at;
}

gboolean gnostr_nip75_goal_has_deadline(const GnostrNip75Goal *goal) {
  return goal && goal->closed_at > 0;
}

GnostrNip75GoalProgress *gnostr_nip75_calculate_progress(
    const GnostrNip75Goal *goal,
    const gchar **zap_receipts_json,
    gsize num_receipts) {

  GnostrNip75GoalProgress *progress = gnostr_nip75_goal_progress_new();

  if (!goal) return progress;

  /* Sum up all zap receipt amounts */
  gint64 total_msat = 0;
  guint count = 0;

  for (gsize i = 0; i < num_receipts; i++) {
    if (!zap_receipts_json[i]) continue;

    GnostrZapReceipt *receipt = gnostr_zap_parse_receipt(zap_receipts_json[i]);
    if (receipt) {
      /* Use the bolt11 amount from the receipt */
      if (receipt->amount_msat > 0) {
        total_msat += receipt->amount_msat;
        count++;
      }
      gnostr_zap_receipt_free(receipt);
    }
  }

  progress->total_received_msat = total_msat;
  progress->zap_count = count;

  /* Calculate percentage */
  if (goal->target_msat > 0) {
    progress->progress_percent = ((gdouble)total_msat / (gdouble)goal->target_msat) * 100.0;
    progress->is_complete = (total_msat >= goal->target_msat);
  }

  /* Check expiration */
  progress->is_expired = gnostr_nip75_goal_is_expired(goal);

  return progress;
}

gchar *gnostr_nip75_build_goal_event(const gchar *description,
                                      gint64 target_msat,
                                      const gchar * const *relays,
                                      gint64 closed_at,
                                      const gchar *linked_event_id,
                                      const gchar *linked_pubkey,
                                      const gchar *external_url) {
  if (target_msat <= 0) {
    g_warning("NIP-75: Cannot create goal with non-positive target");
    return NULL;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kind 9041 - zap goal */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, NIP75_KIND_ZAP_GOAL);

  /* Content - goal description */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, description ? description : "");

  /* Created at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* Amount tag - required */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "amount");
  gchar *amount_str = g_strdup_printf("%" G_GINT64_FORMAT, target_msat);
  json_builder_add_string_value(builder, amount_str);
  g_free(amount_str);
  json_builder_end_array(builder);

  /* Relays tag */
  if (relays && relays[0]) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "relays");
    for (gsize i = 0; relays[i]; i++) {
      json_builder_add_string_value(builder, relays[i]);
    }
    json_builder_end_array(builder);
  }

  /* Closed at tag - optional deadline */
  if (closed_at > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "closed_at");
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, closed_at);
    json_builder_add_string_value(builder, ts_str);
    g_free(ts_str);
    json_builder_end_array(builder);
  }

  /* Linked event - optional */
  if (linked_event_id && *linked_event_id) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "e");
    json_builder_add_string_value(builder, linked_event_id);
    json_builder_end_array(builder);
  }

  /* Linked profile - optional */
  if (linked_pubkey && *linked_pubkey) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "p");
    json_builder_add_string_value(builder, linked_pubkey);
    json_builder_end_array(builder);
  }

  /* External URL - optional */
  if (external_url && *external_url) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "r");
    json_builder_add_string_value(builder, external_url);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);  /* tags */
  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  json_generator_set_pretty(gen, FALSE);
  gchar *result = json_generator_to_data(gen, NULL);


  return result;
}

gchar *gnostr_nip75_format_target(gint64 target_msat) {
  gint64 sats = target_msat / 1000;

  if (sats >= 100000000) {
    /* 100M+ sats = show in BTC */
    return g_strdup_printf("%.2f BTC", sats / 100000000.0);
  } else if (sats >= 1000000) {
    /* 1M+ sats */
    gdouble val = sats / 1000000.0;
    if (val == (gint64)val) {
      return g_strdup_printf("%.0fM sats", val);
    }
    return g_strdup_printf("%.1fM sats", val);
  } else if (sats >= 10000) {
    /* 10K+ sats */
    gdouble val = sats / 1000.0;
    if (val == (gint64)val) {
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

gchar *gnostr_nip75_format_progress(gint64 received_msat, gint64 target_msat) {
  gchar *received_str = gnostr_nip75_format_target(received_msat);
  gchar *target_str = gnostr_nip75_format_target(target_msat);

  /* Remove " sats" suffix from received to avoid "X sats / Y sats" */
  gchar *received_num = g_strdup(received_str);
  gchar *sats_pos = g_strrstr(received_num, " sats");
  if (sats_pos) {
    *sats_pos = '\0';
  }

  gchar *result = g_strdup_printf("%s / %s", received_num, target_str);

  g_free(received_str);
  g_free(target_str);
  g_free(received_num);

  return result;
}

gchar *gnostr_nip75_format_time_remaining(gint64 closed_at) {
  if (closed_at <= 0) return NULL;

  gint64 now = (gint64)time(NULL);
  gint64 remaining = closed_at - now;

  if (remaining <= 0) {
    return g_strdup("Ended");
  }

  /* Convert to human-readable format */
  if (remaining < 60) {
    return g_strdup_printf("%d seconds", (int)remaining);
  } else if (remaining < 3600) {
    int minutes = (int)(remaining / 60);
    return g_strdup_printf("%d minute%s", minutes, minutes == 1 ? "" : "s");
  } else if (remaining < 86400) {
    int hours = (int)(remaining / 3600);
    return g_strdup_printf("%d hour%s", hours, hours == 1 ? "" : "s");
  } else if (remaining < 604800) {
    int days = (int)(remaining / 86400);
    return g_strdup_printf("%d day%s", days, days == 1 ? "" : "s");
  } else if (remaining < 2592000) {
    int weeks = (int)(remaining / 604800);
    return g_strdup_printf("%d week%s", weeks, weeks == 1 ? "" : "s");
  } else {
    int months = (int)(remaining / 2592000);
    return g_strdup_printf("%d month%s", months, months == 1 ? "" : "s");
  }
}
