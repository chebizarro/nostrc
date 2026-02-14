/**
 * NIP-75: Zap Goals Utility Implementation
 *
 * Parsing, building, and progress calculation for zap goal events.
 */

#include "nip75_zap_goals.h"
#include "zap.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include <string.h>
#include <time.h>

gboolean gnostr_nip75_is_zap_goal_kind(gint kind) {
  return kind == NIP75_KIND_ZAP_GOAL;
}

/* Callback context for parsing zap goal tags */
typedef struct {
  GnostrZapGoal *goal;
  GPtrArray *relays_arr;
} ZapGoalParseCtx;

static gboolean
zap_goal_tag_callback(gsize index, const gchar *element_json, gpointer user_data)
{
  (void)index;
  ZapGoalParseCtx *ctx = user_data;

  char *tag_name = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }

  if (g_strcmp0(tag_name, "amount") == 0) {
    char *amount_str = NULL;
    amount_str = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (amount_str) {
      ctx->goal->target_msats = g_ascii_strtoll(amount_str, NULL, 10);
      free(amount_str);
    }
  } else if (g_strcmp0(tag_name, "relays") == 0) {
    /* Get all relay URLs starting from index 1 */
    size_t arr_len = 0;
    if ((arr_len = gnostr_json_get_array_length(element_json, NULL, NULL)) >= 0) {
      for (size_t j = 1; j < arr_len; j++) {
        char *relay = NULL;
        if ((relay = gnostr_json_get_array_string(element_json, NULL, j, NULL)) != NULL && relay && *relay) {
          g_ptr_array_add(ctx->relays_arr, g_strdup(relay));
          free(relay);
        }
      }
    }
  } else if (g_strcmp0(tag_name, "closed_at") == 0) {
    char *ts_str = NULL;
    ts_str = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (ts_str) {
      ctx->goal->end_time = g_ascii_strtoll(ts_str, NULL, 10);
      free(ts_str);
    }
  } else if (g_strcmp0(tag_name, "e") == 0 && !ctx->goal->linked_event_id) {
    char *event_id = NULL;
    event_id = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (event_id) {
      ctx->goal->linked_event_id = g_strdup(event_id);
      free(event_id);
    }
  } else if (g_strcmp0(tag_name, "p") == 0 && !ctx->goal->linked_pubkey) {
    char *pubkey = NULL;
    pubkey = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (pubkey) {
      ctx->goal->linked_pubkey = g_strdup(pubkey);
      free(pubkey);
    }
  } else if (g_strcmp0(tag_name, "r") == 0 && !ctx->goal->external_url) {
    char *url = NULL;
    url = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
    if (url) {
      ctx->goal->external_url = g_strdup(url);
      free(url);
    }
  }

  free(tag_name);
  return TRUE;
}

GnostrZapGoal *gnostr_zap_goal_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  if (!gnostr_json_is_valid(json_str)) {
    g_debug("NIP-75: Failed to parse goal JSON");
    return NULL;
  }

  if (!gnostr_json_is_object_str(json_str)) {
    return NULL;
  }

  /* Check kind */
  int kind = gnostr_json_get_int(json_str, "kind", NULL);
  if (kind != NIP75_KIND_ZAP_GOAL) {
    return NULL;
  }

  GnostrZapGoal *goal = g_new0(GnostrZapGoal, 1);

  /* Get event ID */
  char *event_id = NULL;
  event_id = gnostr_json_get_string(json_str, "id", NULL);
  if (event_id) {
    goal->event_id = g_strdup(event_id);
    free(event_id);
  }

  /* Get pubkey */
  char *pubkey = NULL;
  pubkey = gnostr_json_get_string(json_str, "pubkey", NULL);
  if (pubkey) {
    goal->pubkey = g_strdup(pubkey);
    free(pubkey);
  }

  /* Get title from content */
  char *content = NULL;
  content = gnostr_json_get_string(json_str, "content", NULL);
  if (content) {
    goal->title = g_strdup(content);
    free(content);
  }

  /* Get created_at */
  int64_t created_at = 0;
  if ((created_at = gnostr_json_get_int64(json_str, "created_at", NULL), TRUE)) {
    goal->created_at = created_at;
  }

  /* Parse tags */
  GPtrArray *relays_arr = g_ptr_array_new();
  ZapGoalParseCtx ctx = { .goal = goal, .relays_arr = relays_arr };
  gnostr_json_array_foreach(json_str, "tags", zap_goal_tag_callback, &ctx);

  /* Convert relays array to NULL-terminated string array */
  if (relays_arr->len > 0) {
    g_ptr_array_add(relays_arr, NULL);
    goal->relays = (gchar **)g_ptr_array_free(relays_arr, FALSE);
  } else {
    g_ptr_array_free(relays_arr, TRUE);
  }

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

  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* Kind 9041 - zap goal */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NIP75_KIND_ZAP_GOAL);

  /* Content - goal title/description */
  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, title ? title : "");

  /* Created at */
  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  /* Tags array */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* Amount tag - required */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "amount");
  gchar *amount_str = g_strdup_printf("%" G_GINT64_FORMAT, target_msats);
  gnostr_json_builder_add_string(builder, amount_str);
  g_free(amount_str);
  gnostr_json_builder_end_array(builder);

  /* Relays tag */
  if (relays && relays[0]) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "relays");
    for (gsize i = 0; relays[i]; i++) {
      gnostr_json_builder_add_string(builder, relays[i]);
    }
    gnostr_json_builder_end_array(builder);
  }

  /* Closed at tag - optional deadline */
  if (closed_at > 0) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "closed_at");
    gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, closed_at);
    gnostr_json_builder_add_string(builder, ts_str);
    g_free(ts_str);
    gnostr_json_builder_end_array(builder);
  }

  /* Linked event - optional */
  if (linked_event_id && *linked_event_id) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, linked_event_id);
    gnostr_json_builder_end_array(builder);
  }

  /* Linked profile - optional */
  if (linked_pubkey && *linked_pubkey) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, linked_pubkey);
    gnostr_json_builder_end_array(builder);
  }

  /* External URL - optional */
  if (external_url && *external_url) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "r");
    gnostr_json_builder_add_string(builder, external_url);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);  /* tags */
  gnostr_json_builder_end_object(builder);

  char *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

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
