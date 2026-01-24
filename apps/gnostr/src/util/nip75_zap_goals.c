/**
 * NIP-75: Zap Goals Utility Implementation
 *
 * Parsing, building, and progress calculation for zap goal events.
 */

#include "nip75_zap_goals.h"
#include "zap.h"
#include <jansson.h>
#include <string.h>
#include <time.h>

gboolean gnostr_nip75_is_zap_goal_kind(gint kind) {
  return kind == NIP75_KIND_ZAP_GOAL;
}

GnostrZapGoal *gnostr_zap_goal_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  json_error_t error;
  json_t *root = json_loads(json_str, 0, &error);
  if (!root) {
    g_debug("NIP-75: Failed to parse goal JSON: %s", error.text);
    return NULL;
  }

  if (!json_is_object(root)) {
    json_decref(root);
    return NULL;
  }

  /* Check kind */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || !json_is_integer(kind_val)) {
    json_decref(root);
    return NULL;
  }
  json_int_t kind = json_integer_value(kind_val);
  if (kind != NIP75_KIND_ZAP_GOAL) {
    json_decref(root);
    return NULL;
  }

  GnostrZapGoal *goal = g_new0(GnostrZapGoal, 1);

  /* Get event ID */
  json_t *id_val = json_object_get(root, "id");
  if (id_val && json_is_string(id_val)) {
    goal->event_id = g_strdup(json_string_value(id_val));
  }

  /* Get pubkey */
  json_t *pk_val = json_object_get(root, "pubkey");
  if (pk_val && json_is_string(pk_val)) {
    goal->pubkey = g_strdup(json_string_value(pk_val));
  }

  /* Get title from content */
  json_t *content_val = json_object_get(root, "content");
  if (content_val && json_is_string(content_val)) {
    goal->title = g_strdup(json_string_value(content_val));
  }

  /* Get created_at */
  json_t *created_val = json_object_get(root, "created_at");
  if (created_val && json_is_integer(created_val)) {
    goal->created_at = json_integer_value(created_val);
  }

  /* Parse tags */
  GPtrArray *relays_arr = g_ptr_array_new();

  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    size_t n_tags = json_array_size(tags);

    for (size_t i = 0; i < n_tags; i++) {
      json_t *tag = json_array_get(tags, i);
      if (!json_is_array(tag)) continue;

      size_t tag_len = json_array_size(tag);
      if (tag_len < 2) continue;

      json_t *tag_name_val = json_array_get(tag, 0);
      if (!tag_name_val || !json_is_string(tag_name_val)) continue;
      const char *tag_name = json_string_value(tag_name_val);

      if (g_strcmp0(tag_name, "amount") == 0) {
        /* Target amount: ["amount", "millisats"] */
        json_t *amount_val = json_array_get(tag, 1);
        if (amount_val && json_is_string(amount_val)) {
          const char *amount_str = json_string_value(amount_val);
          goal->target_msats = g_ascii_strtoll(amount_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "relays") == 0) {
        /* Relays: ["relays", "wss://...", "wss://..."] */
        for (size_t j = 1; j < tag_len; j++) {
          json_t *relay_val = json_array_get(tag, j);
          if (relay_val && json_is_string(relay_val)) {
            const char *relay = json_string_value(relay_val);
            if (relay && *relay) {
              g_ptr_array_add(relays_arr, g_strdup(relay));
            }
          }
        }
      } else if (g_strcmp0(tag_name, "closed_at") == 0) {
        /* Deadline: ["closed_at", "timestamp"] */
        json_t *ts_val = json_array_get(tag, 1);
        if (ts_val && json_is_string(ts_val)) {
          const char *ts_str = json_string_value(ts_val);
          goal->end_time = g_ascii_strtoll(ts_str, NULL, 10);
        }
      } else if (g_strcmp0(tag_name, "e") == 0) {
        /* Linked event: ["e", "event_id"] */
        json_t *eid_val = json_array_get(tag, 1);
        if (eid_val && json_is_string(eid_val) && !goal->linked_event_id) {
          goal->linked_event_id = g_strdup(json_string_value(eid_val));
        }
      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* Linked profile: ["p", "pubkey"] */
        json_t *lpk_val = json_array_get(tag, 1);
        if (lpk_val && json_is_string(lpk_val) && !goal->linked_pubkey) {
          goal->linked_pubkey = g_strdup(json_string_value(lpk_val));
        }
      } else if (g_strcmp0(tag_name, "r") == 0) {
        /* External URL: ["r", "url"] */
        json_t *url_val = json_array_get(tag, 1);
        if (url_val && json_is_string(url_val) && !goal->external_url) {
          goal->external_url = g_strdup(json_string_value(url_val));
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

  json_decref(root);

  /* Validate: must have target amount */
  if (goal->target_msats <= 0) {
    g_debug("NIP-75: Goal missing valid amount tag");
    gnostr_zap_goal_free(goal);
    return NULL;
  }

  return goal;
}

void gnostr_zap_goal_free(GnostrZapGoal *goal) {
  if (!goal) return;
  g_free(goal->title);
  g_free(goal->event_id);
  g_free(goal->pubkey);
  g_free(goal->lud16);
  g_strfreev(goal->relays);
  g_free(goal->linked_event_id);
  g_free(goal->linked_pubkey);
  g_free(goal->external_url);
  g_free(goal);
}

GnostrZapGoalProgress *gnostr_zap_goal_progress_new(void) {
  return g_new0(GnostrZapGoalProgress, 1);
}

GnostrZapGoalProgress *gnostr_zap_goal_calculate_progress(
    const GnostrZapGoal *goal,
    const gchar **zap_receipts_json,
    gsize num_receipts) {

  GnostrZapGoalProgress *progress = gnostr_zap_goal_progress_new();

  if (!goal) return progress;

  /* Sum up all zap receipt amounts */
  gint64 total_msats = 0;
  guint count = 0;

  for (gsize i = 0; i < num_receipts; i++) {
    if (!zap_receipts_json[i]) continue;

    GnostrZapReceipt *receipt = gnostr_zap_parse_receipt(zap_receipts_json[i]);
    if (receipt) {
      /* Use the bolt11 amount from the receipt */
      if (receipt->amount_msat > 0) {
        total_msats += receipt->amount_msat;
        count++;
      }
      gnostr_zap_receipt_free(receipt);
    }
  }

  progress->total_received_msats = total_msats;
  progress->zap_count = count;

  /* Calculate percentage */
  if (goal->target_msats > 0) {
    progress->progress_percent = ((gdouble)total_msats / (gdouble)goal->target_msats) * 100.0;
    progress->is_complete = (total_msats >= goal->target_msats);
  }

  /* Check expiration */
  progress->is_expired = gnostr_zap_goal_is_expired(goal);

  return progress;
}

void gnostr_zap_goal_update_current(GnostrZapGoal *goal,
                                     const GnostrZapGoalProgress *progress) {
  if (!goal || !progress) return;
  goal->current_msats = progress->total_received_msats;
}

gchar *gnostr_zap_goal_create_event(const gchar *title,
                                     gint64 target_msats,
                                     const gchar * const *relays,
                                     gint64 closed_at,
                                     const gchar *linked_event_id,
                                     const gchar *linked_pubkey,
                                     const gchar *external_url) {
  if (target_msats <= 0) {
    g_warning("NIP-75: Cannot create goal with non-positive target");
    return NULL;
  }

  json_t *event = json_object();

  /* Kind 9041 - zap goal */
  json_object_set_new(event, "kind", json_integer(NIP75_KIND_ZAP_GOAL));

  /* Content - goal title/description */
  json_object_set_new(event, "content", json_string(title ? title : ""));

  /* Created at */
  json_object_set_new(event, "created_at", json_integer((json_int_t)time(NULL)));

  /* Tags array */
  json_t *tags = json_array();

  /* Amount tag - required */
  json_t *amount_tag = json_array();
  json_array_append_new(amount_tag, json_string("amount"));
  gchar *amount_str = g_strdup_printf("%" G_GINT64_FORMAT, target_msats);
  json_array_append_new(amount_tag, json_string(amount_str));
  g_free(amount_str);
  json_array_append_new(tags, amount_tag);

  /* Relays tag */
  if (relays && relays[0]) {
    json_t *relays_tag = json_array();
    json_array_append_new(relays_tag, json_string("relays"));
    for (gsize i = 0; relays[i]; i++) {
      json_array_append_new(relays_tag, json_string(relays[i]));
    }
    json_array_append_new(tags, relays_tag);
  }

  /* Closed at tag - optional deadline */
  if (closed_at > 0) {
    json_t *closed_tag = json_array();
    json_array_append_new(closed_tag, json_string("closed_at"));
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, closed_at);
    json_array_append_new(closed_tag, json_string(ts_str));
    g_free(ts_str);
    json_array_append_new(tags, closed_tag);
  }

  /* Linked event - optional */
  if (linked_event_id && *linked_event_id) {
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(linked_event_id));
    json_array_append_new(tags, e_tag);
  }

  /* Linked profile - optional */
  if (linked_pubkey && *linked_pubkey) {
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(linked_pubkey));
    json_array_append_new(tags, p_tag);
  }

  /* External URL - optional */
  if (external_url && *external_url) {
    json_t *r_tag = json_array();
    json_array_append_new(r_tag, json_string("r"));
    json_array_append_new(r_tag, json_string(external_url));
    json_array_append_new(tags, r_tag);
  }

  json_object_set_new(event, "tags", tags);

  /* Serialize to string */
  gchar *result = json_dumps(event, JSON_COMPACT);
  json_decref(event);

  return result;
}

gboolean gnostr_zap_goal_is_expired(const GnostrZapGoal *goal) {
  if (!goal || goal->end_time <= 0) return FALSE;
  gint64 now = (gint64)time(NULL);
  return now >= goal->end_time;
}

gboolean gnostr_zap_goal_has_deadline(const GnostrZapGoal *goal) {
  return goal && goal->end_time > 0;
}

gboolean gnostr_zap_goal_is_complete(const GnostrZapGoal *goal) {
  if (!goal || goal->target_msats <= 0) return FALSE;
  return goal->current_msats >= goal->target_msats;
}

gchar *gnostr_zap_goal_format_target(gint64 target_msats) {
  gint64 sats = target_msats / 1000;

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

gchar *gnostr_zap_goal_format_progress(gint64 current_msats, gint64 target_msats) {
  gchar *current_str = gnostr_zap_goal_format_target(current_msats);
  gchar *target_str = gnostr_zap_goal_format_target(target_msats);

  /* Remove " sats" suffix from current to avoid "X sats / Y sats" */
  gchar *current_num = g_strdup(current_str);
  gchar *sats_pos = g_strrstr(current_num, " sats");
  if (sats_pos) {
    *sats_pos = '\0';
  }
  /* Also handle BTC suffix */
  gchar *btc_pos = g_strrstr(current_num, " BTC");
  if (btc_pos) {
    *btc_pos = '\0';
  }

  gchar *result = g_strdup_printf("%s / %s", current_num, target_str);

  g_free(current_str);
  g_free(target_str);
  g_free(current_num);

  return result;
}

gchar *gnostr_zap_goal_format_time_remaining(gint64 end_time) {
  if (end_time <= 0) return NULL;

  gint64 now = (gint64)time(NULL);
  gint64 remaining = end_time - now;

  if (remaining <= 0) {
    return g_strdup("Ended");
  }

  /* Convert to human-readable format */
  if (remaining < 60) {
    return g_strdup_printf("%d second%s", (int)remaining, remaining == 1 ? "" : "s");
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

gdouble gnostr_zap_goal_get_progress_percent(const GnostrZapGoal *goal) {
  if (!goal || goal->target_msats <= 0) return 0.0;
  return ((gdouble)goal->current_msats / (gdouble)goal->target_msats) * 100.0;
}
