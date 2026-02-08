/**
 * nip53_live.c - NIP-53 Live Activities implementation
 *
 * Parses kind 30311 live activity events per NIP-53 specification.
 */

#include "nip53_live.h"
#include <string.h>
#include "nostr_json.h"
#include <json.h>
#include <nostr-event.h>
#include <time.h>

/* Helper to check if string is valid 64-char hex */
static gboolean is_valid_hex_pubkey(const char *str) {
  if (!str || strlen(str) != 64) return FALSE;
  for (size_t i = 0; i < 64; i++) {
    char c = str[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return FALSE;
    }
  }
  return TRUE;
}

GnostrLiveStatus gnostr_live_status_from_string(const char *status_str) {
  if (!status_str) return GNOSTR_LIVE_STATUS_UNKNOWN;

  if (g_ascii_strcasecmp(status_str, "live") == 0) {
    return GNOSTR_LIVE_STATUS_LIVE;
  } else if (g_ascii_strcasecmp(status_str, "planned") == 0) {
    return GNOSTR_LIVE_STATUS_PLANNED;
  } else if (g_ascii_strcasecmp(status_str, "ended") == 0) {
    return GNOSTR_LIVE_STATUS_ENDED;
  }

  return GNOSTR_LIVE_STATUS_UNKNOWN;
}

const char *gnostr_live_status_to_string(GnostrLiveStatus status) {
  switch (status) {
    case GNOSTR_LIVE_STATUS_LIVE:    return "live";
    case GNOSTR_LIVE_STATUS_PLANNED: return "planned";
    case GNOSTR_LIVE_STATUS_ENDED:   return "ended";
    default:                         return "unknown";
  }
}

void gnostr_live_participant_free(GnostrLiveParticipant *participant) {
  if (!participant) return;
  g_free(participant->pubkey_hex);
  g_free(participant->relay_hint);
  g_free(participant->role);
  g_free(participant->display_name);
  g_free(participant->avatar_url);
  g_free(participant);
}

static GnostrLiveParticipant *participant_copy(const GnostrLiveParticipant *src) {
  if (!src) return NULL;
  GnostrLiveParticipant *dst = g_new0(GnostrLiveParticipant, 1);
  dst->pubkey_hex = g_strdup(src->pubkey_hex);
  dst->relay_hint = g_strdup(src->relay_hint);
  dst->role = g_strdup(src->role);
  dst->display_name = g_strdup(src->display_name);
  dst->avatar_url = g_strdup(src->avatar_url);
  return dst;
}

void gnostr_live_activity_free(GnostrLiveActivity *activity) {
  if (!activity) return;

  g_free(activity->event_id);
  g_free(activity->pubkey);
  g_free(activity->d_tag);
  g_free(activity->title);
  g_free(activity->summary);
  g_free(activity->image);

  if (activity->streaming_urls) {
    g_strfreev(activity->streaming_urls);
  }
  if (activity->recording_urls) {
    g_strfreev(activity->recording_urls);
  }

  if (activity->participants) {
    for (gsize i = 0; activity->participants[i]; i++) {
      gnostr_live_participant_free(activity->participants[i]);
    }
    g_free(activity->participants);
  }

  if (activity->hashtags) {
    g_strfreev(activity->hashtags);
  }
  if (activity->relays) {
    g_strfreev(activity->relays);
  }

  g_free(activity);
}

GnostrLiveActivity *gnostr_live_activity_copy(const GnostrLiveActivity *activity) {
  if (!activity) return NULL;

  GnostrLiveActivity *copy = g_new0(GnostrLiveActivity, 1);

  copy->event_id = g_strdup(activity->event_id);
  copy->pubkey = g_strdup(activity->pubkey);
  copy->d_tag = g_strdup(activity->d_tag);
  copy->created_at = activity->created_at;

  copy->title = g_strdup(activity->title);
  copy->summary = g_strdup(activity->summary);
  copy->image = g_strdup(activity->image);
  copy->status = activity->status;

  if (activity->streaming_urls) {
    copy->streaming_urls = g_strdupv(activity->streaming_urls);
  }
  if (activity->recording_urls) {
    copy->recording_urls = g_strdupv(activity->recording_urls);
  }

  copy->starts_at = activity->starts_at;
  copy->ends_at = activity->ends_at;

  if (activity->participants && activity->participant_count > 0) {
    copy->participants = g_new0(GnostrLiveParticipant *, activity->participant_count + 1);
    for (gsize i = 0; i < activity->participant_count; i++) {
      copy->participants[i] = participant_copy(activity->participants[i]);
    }
    copy->participant_count = activity->participant_count;
  }

  if (activity->hashtags) {
    copy->hashtags = g_strdupv(activity->hashtags);
  }
  if (activity->relays) {
    copy->relays = g_strdupv(activity->relays);
  }

  copy->current_viewers = activity->current_viewers;
  copy->total_viewers = activity->total_viewers;

  return copy;
}

GnostrLiveActivity *gnostr_live_activity_parse(const char *event_json) {
  if (!event_json) return NULL;

  /* Deserialize to NostrEvent using the facade */
  NostrEvent event = {0};
  if (nostr_event_deserialize(&event, event_json) != 0) {
    g_debug("nip53: failed to parse event JSON");
    return NULL;
  }

  /* Check kind - must be 30311 for live activity */
  if (event.kind != 30311) {
    g_debug("nip53: event is not kind 30311 (got %d)", event.kind);
    /* Free internal event fields (stack-allocated event) */
    free(event.id);
    free(event.pubkey);
    free(event.content);
    free(event.sig);
    if (event.tags) nostr_tags_free(event.tags);
    return NULL;
  }

  /* Create activity struct */
  GnostrLiveActivity *activity = g_new0(GnostrLiveActivity, 1);
  activity->event_id = g_strdup(event.id);
  activity->pubkey = g_strdup(event.pubkey);
  activity->created_at = event.created_at;

  /* Dynamic arrays for collecting items */
  GPtrArray *streaming_arr = g_ptr_array_new();
  GPtrArray *recording_arr = g_ptr_array_new();
  GPtrArray *participants_arr = g_ptr_array_new();
  GPtrArray *hashtags_arr = g_ptr_array_new();
  GPtrArray *relays_arr = g_ptr_array_new();

  /* Parse tags using NostrTags API */
  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "d") == 0) {
        g_free(activity->d_tag);
        activity->d_tag = g_strdup(tag_value);
      } else if (strcmp(tag_name, "title") == 0) {
        g_free(activity->title);
        activity->title = g_strdup(tag_value);
      } else if (strcmp(tag_name, "summary") == 0) {
        g_free(activity->summary);
        activity->summary = g_strdup(tag_value);
      } else if (strcmp(tag_name, "image") == 0) {
        g_free(activity->image);
        activity->image = g_strdup(tag_value);
      } else if (strcmp(tag_name, "status") == 0) {
        activity->status = gnostr_live_status_from_string(tag_value);
      } else if (strcmp(tag_name, "starts") == 0 || strcmp(tag_name, "start") == 0) {
        activity->starts_at = g_ascii_strtoll(tag_value, NULL, 10);
      } else if (strcmp(tag_name, "ends") == 0 || strcmp(tag_name, "end") == 0) {
        activity->ends_at = g_ascii_strtoll(tag_value, NULL, 10);
      } else if (strcmp(tag_name, "streaming") == 0) {
        g_ptr_array_add(streaming_arr, g_strdup(tag_value));
      } else if (strcmp(tag_name, "recording") == 0) {
        g_ptr_array_add(recording_arr, g_strdup(tag_value));
      } else if (strcmp(tag_name, "p") == 0 && is_valid_hex_pubkey(tag_value)) {
        GnostrLiveParticipant *p = g_new0(GnostrLiveParticipant, 1);
        p->pubkey_hex = g_strdup(tag_value);
        if (tag_len > 2) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            p->relay_hint = g_strdup(relay);
          }
        }
        if (tag_len > 3) {
          const char *role = nostr_tag_get(tag, 3);
          if (role && *role) {
            p->role = g_strdup(role);
          }
        }
        g_ptr_array_add(participants_arr, p);
      } else if (strcmp(tag_name, "t") == 0) {
        g_ptr_array_add(hashtags_arr, g_strdup(tag_value));
      } else if (strcmp(tag_name, "relay") == 0 || strcmp(tag_name, "r") == 0) {
        g_ptr_array_add(relays_arr, g_strdup(tag_value));
      } else if (strcmp(tag_name, "current_participants") == 0) {
        activity->current_viewers = (gint)g_ascii_strtoll(tag_value, NULL, 10);
      } else if (strcmp(tag_name, "total_participants") == 0) {
        activity->total_viewers = (gint)g_ascii_strtoll(tag_value, NULL, 10);
      }
    }
  }

  /* Convert dynamic arrays to NULL-terminated arrays */
  if (streaming_arr->len > 0) {
    g_ptr_array_add(streaming_arr, NULL);
    activity->streaming_urls = (char **)g_ptr_array_free(streaming_arr, FALSE);
  } else {
    g_ptr_array_free(streaming_arr, TRUE);
  }

  if (recording_arr->len > 0) {
    g_ptr_array_add(recording_arr, NULL);
    activity->recording_urls = (char **)g_ptr_array_free(recording_arr, FALSE);
  } else {
    g_ptr_array_free(recording_arr, TRUE);
  }

  if (participants_arr->len > 0) {
    activity->participant_count = participants_arr->len;
    g_ptr_array_add(participants_arr, NULL);
    activity->participants = (GnostrLiveParticipant **)g_ptr_array_free(participants_arr, FALSE);
  } else {
    g_ptr_array_free(participants_arr, TRUE);
  }

  if (hashtags_arr->len > 0) {
    g_ptr_array_add(hashtags_arr, NULL);
    activity->hashtags = (char **)g_ptr_array_free(hashtags_arr, FALSE);
  } else {
    g_ptr_array_free(hashtags_arr, TRUE);
  }

  if (relays_arr->len > 0) {
    g_ptr_array_add(relays_arr, NULL);
    activity->relays = (char **)g_ptr_array_free(relays_arr, FALSE);
  } else {
    g_ptr_array_free(relays_arr, TRUE);
  }

  /* Free internal event fields (stack-allocated event) */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);

  g_debug("nip53: parsed live activity '%s' (status=%s, %zu participants)",
          activity->title ? activity->title : "(untitled)",
          gnostr_live_status_to_string(activity->status),
          activity->participant_count);

  return activity;
}

GnostrLiveActivity *gnostr_live_activity_parse_tags(const char *tags_json,
                                                     const char *pubkey,
                                                     const char *event_id,
                                                     gint64 created_at) {
  if (!tags_json) return NULL;

  /* Wrap in a fake event structure for unified parsing */
  char *event_json = g_strdup_printf(
    "{\"kind\":30311,\"id\":\"%s\",\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ",\"tags\":%s}",
    event_id ? event_id : "",
    pubkey ? pubkey : "",
    created_at,
    tags_json
  );

  GnostrLiveActivity *activity = gnostr_live_activity_parse(event_json);
  g_free(event_json);

  return activity;
}

GnostrLiveParticipant *gnostr_live_activity_get_host(const GnostrLiveActivity *activity) {
  if (!activity || !activity->participants) return NULL;

  for (gsize i = 0; i < activity->participant_count; i++) {
    GnostrLiveParticipant *p = activity->participants[i];
    if (p && p->role && g_ascii_strcasecmp(p->role, "host") == 0) {
      return p;
    }
  }

  /* If no explicit host, return first participant (event author) */
  if (activity->participant_count > 0) {
    return activity->participants[0];
  }

  return NULL;
}

GnostrLiveParticipant **gnostr_live_activity_get_speakers(const GnostrLiveActivity *activity,
                                                           gsize *out_count) {
  if (out_count) *out_count = 0;
  if (!activity || !activity->participants) return NULL;

  GPtrArray *speakers = g_ptr_array_new();

  for (gsize i = 0; i < activity->participant_count; i++) {
    GnostrLiveParticipant *p = activity->participants[i];
    if (p && p->role) {
      if (g_ascii_strcasecmp(p->role, "host") == 0 ||
          g_ascii_strcasecmp(p->role, "speaker") == 0) {
        g_ptr_array_add(speakers, p);
      }
    }
  }

  if (speakers->len == 0) {
    g_ptr_array_free(speakers, TRUE);
    return NULL;
  }

  if (out_count) *out_count = speakers->len;
  g_ptr_array_add(speakers, NULL);
  return (GnostrLiveParticipant **)g_ptr_array_free(speakers, FALSE);
}

const char *gnostr_live_activity_get_primary_stream(const GnostrLiveActivity *activity) {
  if (!activity || !activity->streaming_urls) return NULL;
  return activity->streaming_urls[0];
}

gboolean gnostr_live_activity_is_active(const GnostrLiveActivity *activity) {
  if (!activity) return FALSE;
  return activity->status == GNOSTR_LIVE_STATUS_LIVE;
}

char *gnostr_live_activity_format_time_until(const GnostrLiveActivity *activity) {
  if (!activity || activity->status != GNOSTR_LIVE_STATUS_PLANNED) return NULL;
  if (activity->starts_at <= 0) return NULL;

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gint64 diff = activity->starts_at - now;

  if (diff <= 0) {
    return g_strdup("Starting soon");
  } else if (diff < 60) {
    return g_strdup("In less than a minute");
  } else if (diff < 3600) {
    gint minutes = (gint)(diff / 60);
    return g_strdup_printf("In %d minute%s", minutes, minutes == 1 ? "" : "s");
  } else if (diff < 86400) {
    gint hours = (gint)(diff / 3600);
    return g_strdup_printf("In %d hour%s", hours, hours == 1 ? "" : "s");
  } else {
    gint days = (gint)(diff / 86400);
    return g_strdup_printf("In %d day%s", days, days == 1 ? "" : "s");
  }
}

char *gnostr_live_activity_format_duration(const GnostrLiveActivity *activity) {
  if (!activity) return NULL;

  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gint64 start_time = activity->starts_at > 0 ? activity->starts_at : activity->created_at;
  gint64 end_time = activity->ends_at > 0 ? activity->ends_at : now;

  if (activity->status == GNOSTR_LIVE_STATUS_LIVE) {
    /* Currently live - show duration so far */
    gint64 diff = now - start_time;
    if (diff < 60) {
      return g_strdup("Live now");
    } else if (diff < 3600) {
      gint minutes = (gint)(diff / 60);
      return g_strdup_printf("Live for %d min", minutes);
    } else {
      gint hours = (gint)(diff / 3600);
      gint minutes = (gint)((diff % 3600) / 60);
      if (minutes > 0) {
        return g_strdup_printf("Live for %dh %dm", hours, minutes);
      }
      return g_strdup_printf("Live for %d hour%s", hours, hours == 1 ? "" : "s");
    }
  } else if (activity->status == GNOSTR_LIVE_STATUS_ENDED) {
    /* Ended - show total duration */
    gint64 diff = end_time - start_time;
    if (diff < 60) {
      return g_strdup("Lasted less than a minute");
    } else if (diff < 3600) {
      gint minutes = (gint)(diff / 60);
      return g_strdup_printf("Lasted %d min", minutes);
    } else {
      gint hours = (gint)(diff / 3600);
      gint minutes = (gint)((diff % 3600) / 60);
      if (minutes > 0) {
        return g_strdup_printf("Lasted %dh %dm", hours, minutes);
      }
      return g_strdup_printf("Lasted %d hour%s", hours, hours == 1 ? "" : "s");
    }
  }

  return NULL;
}
