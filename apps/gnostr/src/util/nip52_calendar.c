/**
 * NIP-52: Calendar Events Utility Implementation
 *
 * Parsing, building, and formatting for calendar events.
 */

/* Required for strptime on POSIX systems */
#define _XOPEN_SOURCE 700

#include "nip52_calendar.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* Date format for date-based events: YYYY-MM-DD */
#define DATE_FORMAT "%Y-%m-%d"

/* Helper: validate hex string */
static gboolean is_valid_hex(const gchar *str, gsize expected_len) {
  if (!str) return FALSE;
  gsize len = strlen(str);
  if (expected_len > 0 && len != expected_len) return FALSE;
  for (gsize i = 0; i < len; i++) {
    gchar c = str[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Helper: parse date string YYYY-MM-DD to timestamp */
static gint64 parse_date_string(const gchar *date_str) {
  if (!date_str || strlen(date_str) < 10) return 0;

  struct tm tm = {0};
  if (strptime(date_str, DATE_FORMAT, &tm) == NULL) {
    return 0;
  }

  /* Set to midnight */
  tm.tm_hour = 0;
  tm.tm_min = 0;
  tm.tm_sec = 0;
  tm.tm_isdst = -1;  /* Let system determine DST */

  return (gint64)mktime(&tm);
}

gboolean gnostr_nip52_is_calendar_kind(gint kind) {
  return kind == NIP52_KIND_DATE_BASED_EVENT || kind == NIP52_KIND_TIME_BASED_EVENT;
}

gboolean gnostr_nip52_is_date_based(gint kind) {
  return kind == NIP52_KIND_DATE_BASED_EVENT;
}

gboolean gnostr_nip52_is_time_based(gint kind) {
  return kind == NIP52_KIND_TIME_BASED_EVENT;
}

GnostrNip52CalendarEvent *gnostr_nip52_calendar_event_parse(const gchar *json_str) {
  if (!json_str || !*json_str) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, json_str, -1, &error)) {
    g_debug("NIP-52: Failed to parse calendar event JSON: %s", error ? error->message : "unknown");
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
  if (!gnostr_nip52_is_calendar_kind((gint)kind)) {
    return NULL;
  }

  GnostrNip52CalendarEvent *event = g_new0(GnostrNip52CalendarEvent, 1);
  event->type = (GnostrCalendarEventType)kind;

  /* Get event ID */
  if (json_object_has_member(obj, "id")) {
    event->event_id = g_strdup(json_object_get_string_member(obj, "id"));
  }

  /* Get pubkey */
  if (json_object_has_member(obj, "pubkey")) {
    event->pubkey = g_strdup(json_object_get_string_member(obj, "pubkey"));
  }

  /* Get description from content */
  if (json_object_has_member(obj, "content")) {
    event->description = g_strdup(json_object_get_string_member(obj, "content"));
  }

  /* Get created_at */
  if (json_object_has_member(obj, "created_at")) {
    event->created_at = json_object_get_int_member(obj, "created_at");
  }

  /* Temporary arrays for accumulating multi-value tags */
  GPtrArray *locations_arr = g_ptr_array_new();
  GPtrArray *geohashes_arr = g_ptr_array_new();
  GPtrArray *participants_arr = g_ptr_array_new_with_free_func(NULL);
  GPtrArray *hashtags_arr = g_ptr_array_new();
  GPtrArray *references_arr = g_ptr_array_new();

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
      const gchar *tag_value = json_array_get_string_element(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        /* Unique identifier */
        g_free(event->d_tag);
        event->d_tag = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "title") == 0) {
        /* Event title */
        g_free(event->title);
        event->title = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "image") == 0) {
        /* Event image */
        g_free(event->image);
        event->image = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "start") == 0) {
        /* Start time/date */
        if (event->type == GNOSTR_CALENDAR_EVENT_TIME_BASED) {
          /* Time-based: Unix timestamp */
          event->start = g_ascii_strtoll(tag_value, NULL, 10);
        } else {
          /* Date-based: YYYY-MM-DD */
          event->start_date = g_strdup(tag_value);
          event->start = parse_date_string(tag_value);
        }

      } else if (g_strcmp0(tag_name, "end") == 0) {
        /* End time/date */
        if (event->type == GNOSTR_CALENDAR_EVENT_TIME_BASED) {
          /* Time-based: Unix timestamp */
          event->end = g_ascii_strtoll(tag_value, NULL, 10);
        } else {
          /* Date-based: YYYY-MM-DD */
          event->end_date = g_strdup(tag_value);
          event->end = parse_date_string(tag_value);
        }

      } else if (g_strcmp0(tag_name, "start_tzid") == 0) {
        /* Start timezone */
        g_free(event->start_tzid);
        event->start_tzid = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "end_tzid") == 0) {
        /* End timezone */
        g_free(event->end_tzid);
        event->end_tzid = g_strdup(tag_value);

      } else if (g_strcmp0(tag_name, "location") == 0) {
        /* Location */
        g_ptr_array_add(locations_arr, g_strdup(tag_value));

      } else if (g_strcmp0(tag_name, "g") == 0) {
        /* Geohash */
        g_ptr_array_add(geohashes_arr, g_strdup(tag_value));

      } else if (g_strcmp0(tag_name, "p") == 0) {
        /* Participant */
        if (is_valid_hex(tag_value, 64)) {
          GnostrNip52Participant *p = g_new0(GnostrNip52Participant, 1);
          p->pubkey = g_strdup(tag_value);

          if (tag_len > 2) {
            const gchar *relay = json_array_get_string_element(tag, 2);
            if (relay && *relay) {
              p->relay = g_strdup(relay);
            }
          }
          if (tag_len > 3) {
            const gchar *role = json_array_get_string_element(tag, 3);
            if (role && *role) {
              p->role = g_strdup(role);
            }
          }

          g_ptr_array_add(participants_arr, p);
        }

      } else if (g_strcmp0(tag_name, "t") == 0) {
        /* Hashtag */
        g_ptr_array_add(hashtags_arr, g_strdup(tag_value));

      } else if (g_strcmp0(tag_name, "r") == 0) {
        /* Reference URL */
        g_ptr_array_add(references_arr, g_strdup(tag_value));
      }
    }
  }

  /* Convert arrays to NULL-terminated string arrays */
  if (locations_arr->len > 0) {
    event->locations_count = locations_arr->len;
    g_ptr_array_add(locations_arr, NULL);
    event->locations = (gchar **)g_ptr_array_free(locations_arr, FALSE);
  } else {
    g_ptr_array_free(locations_arr, TRUE);
  }

  if (geohashes_arr->len > 0) {
    event->geohashes_count = geohashes_arr->len;
    g_ptr_array_add(geohashes_arr, NULL);
    event->geohashes = (gchar **)g_ptr_array_free(geohashes_arr, FALSE);
  } else {
    g_ptr_array_free(geohashes_arr, TRUE);
  }

  if (participants_arr->len > 0) {
    event->participants_count = participants_arr->len;
    event->participants = g_new0(GnostrNip52Participant, participants_arr->len);
    for (guint i = 0; i < participants_arr->len; i++) {
      GnostrNip52Participant *src = g_ptr_array_index(participants_arr, i);
      event->participants[i] = *src;
      g_free(src);
    }
    g_ptr_array_free(participants_arr, TRUE);
  } else {
    g_ptr_array_free(participants_arr, TRUE);
  }

  if (hashtags_arr->len > 0) {
    event->hashtags_count = hashtags_arr->len;
    g_ptr_array_add(hashtags_arr, NULL);
    event->hashtags = (gchar **)g_ptr_array_free(hashtags_arr, FALSE);
  } else {
    g_ptr_array_free(hashtags_arr, TRUE);
  }

  if (references_arr->len > 0) {
    event->references_count = references_arr->len;
    g_ptr_array_add(references_arr, NULL);
    event->references = (gchar **)g_ptr_array_free(references_arr, FALSE);
  } else {
    g_ptr_array_free(references_arr, TRUE);
  }


  /* Validate: must have start time and d-tag */
  if (event->start <= 0 || !event->d_tag || !*event->d_tag) {
    g_debug("NIP-52: Calendar event missing required start or d tag");
    gnostr_nip52_calendar_event_free(event);
    return NULL;
  }

  return event;
}

void gnostr_nip52_participant_free(GnostrNip52Participant *participant) {
  if (!participant) return;
  g_free(participant->pubkey);
  g_free(participant->relay);
  g_free(participant->role);
}

void gnostr_nip52_calendar_event_free(GnostrNip52CalendarEvent *event) {
  if (!event) return;

  g_free(event->event_id);
  g_free(event->pubkey);
  g_free(event->d_tag);
  g_free(event->title);
  g_free(event->description);
  g_free(event->image);
  g_free(event->start_date);
  g_free(event->end_date);
  g_free(event->start_tzid);
  g_free(event->end_tzid);

  g_strfreev(event->locations);
  g_strfreev(event->geohashes);
  g_strfreev(event->hashtags);
  g_strfreev(event->references);

  if (event->participants) {
    for (gsize i = 0; i < event->participants_count; i++) {
      gnostr_nip52_participant_free(&event->participants[i]);
    }
    g_free(event->participants);
  }

  g_free(event);
}

gboolean gnostr_nip52_event_is_upcoming(const GnostrNip52CalendarEvent *event) {
  if (!event || event->start <= 0) return FALSE;
  gint64 now = (gint64)time(NULL);
  return event->start > now;
}

gboolean gnostr_nip52_event_is_ongoing(const GnostrNip52CalendarEvent *event) {
  if (!event || event->start <= 0) return FALSE;
  gint64 now = (gint64)time(NULL);

  if (now < event->start) return FALSE;

  /* If no end time, consider event ongoing for 24 hours after start */
  gint64 end = (event->end > 0) ? event->end : (event->start + 86400);
  return now <= end;
}

gboolean gnostr_nip52_event_is_past(const GnostrNip52CalendarEvent *event) {
  if (!event || event->start <= 0) return FALSE;
  gint64 now = (gint64)time(NULL);

  /* If no end time, consider event past 24 hours after start */
  gint64 end = (event->end > 0) ? event->end : (event->start + 86400);
  return now > end;
}

gchar *gnostr_nip52_format_date(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup("Unknown date");

  GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
  if (!dt) return g_strdup("Unknown date");

  gchar *result = g_date_time_format(dt, "%B %e, %Y");
  g_date_time_unref(dt);
  return result;
}

gchar *gnostr_nip52_format_time(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup("Unknown time");

  GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
  if (!dt) return g_strdup("Unknown time");

  gchar *result = g_date_time_format(dt, "%l:%M %p");
  g_date_time_unref(dt);

  /* Trim leading space if present */
  if (result && result[0] == ' ') {
    gchar *trimmed = g_strdup(result + 1);
    g_free(result);
    return trimmed;
  }
  return result;
}

gchar *gnostr_nip52_format_datetime(gint64 timestamp) {
  if (timestamp <= 0) return g_strdup("Unknown");

  GDateTime *dt = g_date_time_new_from_unix_local(timestamp);
  if (!dt) return g_strdup("Unknown");

  gchar *result = g_date_time_format(dt, "%b %e, %Y at %l:%M %p");
  g_date_time_unref(dt);
  return result;
}

gchar *gnostr_nip52_format_date_range(const GnostrNip52CalendarEvent *event) {
  if (!event || event->start <= 0) return g_strdup("Unknown");

  GDateTime *start_dt = g_date_time_new_from_unix_local(event->start);
  if (!start_dt) return g_strdup("Unknown");

  gchar *result = NULL;

  if (event->type == GNOSTR_CALENDAR_EVENT_DATE_BASED) {
    /* Date-based event */
    if (event->end > 0 && event->end != event->start) {
      GDateTime *end_dt = g_date_time_new_from_unix_local(event->end);
      if (end_dt) {
        /* Check if same month */
        if (g_date_time_get_month(start_dt) == g_date_time_get_month(end_dt) &&
            g_date_time_get_year(start_dt) == g_date_time_get_year(end_dt)) {
          gchar *start_str = g_date_time_format(start_dt, "%B %e");
          gchar *end_str = g_date_time_format(end_dt, "%e, %Y");
          result = g_strdup_printf("%s-%s", start_str, end_str);
          g_free(start_str);
          g_free(end_str);
        } else {
          gchar *start_str = g_date_time_format(start_dt, "%B %e, %Y");
          gchar *end_str = g_date_time_format(end_dt, "%B %e, %Y");
          result = g_strdup_printf("%s - %s", start_str, end_str);
          g_free(start_str);
          g_free(end_str);
        }
        g_date_time_unref(end_dt);
      }
    }

    if (!result) {
      result = g_date_time_format(start_dt, "%B %e, %Y");
    }

  } else {
    /* Time-based event */
    if (event->end > 0) {
      GDateTime *end_dt = g_date_time_new_from_unix_local(event->end);
      if (end_dt) {
        /* Check if same day */
        if (g_date_time_get_day_of_year(start_dt) == g_date_time_get_day_of_year(end_dt) &&
            g_date_time_get_year(start_dt) == g_date_time_get_year(end_dt)) {
          gchar *date_str = g_date_time_format(start_dt, "%b %e, %Y");
          gchar *start_time = g_date_time_format(start_dt, "%l:%M %p");
          gchar *end_time = g_date_time_format(end_dt, "%l:%M %p");
          result = g_strdup_printf("%s at %s - %s", date_str, start_time, end_time);
          g_free(date_str);
          g_free(start_time);
          g_free(end_time);
        } else {
          gchar *start_str = g_date_time_format(start_dt, "%b %e at %l:%M %p");
          gchar *end_str = g_date_time_format(end_dt, "%b %e at %l:%M %p");
          result = g_strdup_printf("%s - %s", start_str, end_str);
          g_free(start_str);
          g_free(end_str);
        }
        g_date_time_unref(end_dt);
      }
    }

    if (!result) {
      result = g_date_time_format(start_dt, "%b %e, %Y at %l:%M %p");
    }
  }

  g_date_time_unref(start_dt);
  return result;
}

gchar *gnostr_nip52_format_time_until(const GnostrNip52CalendarEvent *event) {
  if (!event || event->start <= 0) return NULL;

  gint64 now = (gint64)time(NULL);
  gint64 remaining = event->start - now;

  if (remaining <= 0) return NULL;

  /* Format time remaining */
  if (remaining < 60) {
    return g_strdup("in less than a minute");
  } else if (remaining < 3600) {
    gint minutes = (gint)(remaining / 60);
    return g_strdup_printf("in %d minute%s", minutes, minutes == 1 ? "" : "s");
  } else if (remaining < 86400) {
    gint hours = (gint)(remaining / 3600);
    return g_strdup_printf("in %d hour%s", hours, hours == 1 ? "" : "s");
  } else if (remaining < 604800) {
    gint days = (gint)(remaining / 86400);
    return g_strdup_printf("in %d day%s", days, days == 1 ? "" : "s");
  } else if (remaining < 2592000) {
    gint weeks = (gint)(remaining / 604800);
    return g_strdup_printf("in %d week%s", weeks, weeks == 1 ? "" : "s");
  } else {
    gint months = (gint)(remaining / 2592000);
    return g_strdup_printf("in %d month%s", months, months == 1 ? "" : "s");
  }
}

const gchar *gnostr_nip52_get_primary_location(const GnostrNip52CalendarEvent *event) {
  if (!event || !event->locations || event->locations_count == 0) {
    return NULL;
  }
  return event->locations[0];
}

gchar *gnostr_nip52_build_a_tag(gint kind, const gchar *pubkey_hex, const gchar *d_tag) {
  if (!pubkey_hex || !d_tag) return NULL;
  return g_strdup_printf("%d:%s:%s", kind, pubkey_hex, d_tag);
}

gchar *gnostr_nip52_build_calendar_event(GnostrCalendarEventType type,
                                          const gchar *title,
                                          const gchar *description,
                                          gint64 start,
                                          gint64 end,
                                          const gchar *location,
                                          const gchar *image) {
  if (!title || !*title || start <= 0) {
    g_warning("NIP-52: Cannot create calendar event without title or start time");
    return NULL;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Kind */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, (gint64)type);

  /* Content - event description */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, description ? description : "");

  /* Created at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* d tag - unique identifier (use UUID or timestamp-based) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "d");
  gchar *d_tag = g_strdup_printf("%s-%ld", title, (long)time(NULL));
  /* Sanitize d_tag: lowercase, replace spaces with dashes */
  gchar *sanitized = g_utf8_strdown(d_tag, -1);
  g_free(d_tag);
  for (gchar *p = sanitized; *p; p++) {
    if (*p == ' ') *p = '-';
    if (!g_ascii_isalnum(*p) && *p != '-') *p = '-';
  }
  json_builder_add_string_value(builder, sanitized);
  g_free(sanitized);
  json_builder_end_array(builder);

  /* title tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "title");
  json_builder_add_string_value(builder, title);
  json_builder_end_array(builder);

  /* start tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "start");
  if (type == GNOSTR_CALENDAR_EVENT_TIME_BASED) {
    gchar *start_str = g_strdup_printf("%" G_GINT64_FORMAT, start);
    json_builder_add_string_value(builder, start_str);
    g_free(start_str);
  } else {
    GDateTime *dt = g_date_time_new_from_unix_local(start);
    if (dt) {
      gchar *date_str = g_date_time_format(dt, DATE_FORMAT);
      json_builder_add_string_value(builder, date_str);
      g_free(date_str);
      g_date_time_unref(dt);
    } else {
      gchar *start_str = g_strdup_printf("%" G_GINT64_FORMAT, start);
      json_builder_add_string_value(builder, start_str);
      g_free(start_str);
    }
  }
  json_builder_end_array(builder);

  /* end tag (optional) */
  if (end > 0) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "end");
    if (type == GNOSTR_CALENDAR_EVENT_TIME_BASED) {
      gchar *end_str = g_strdup_printf("%" G_GINT64_FORMAT, end);
      json_builder_add_string_value(builder, end_str);
      g_free(end_str);
    } else {
      GDateTime *dt = g_date_time_new_from_unix_local(end);
      if (dt) {
        gchar *date_str = g_date_time_format(dt, DATE_FORMAT);
        json_builder_add_string_value(builder, date_str);
        g_free(date_str);
        g_date_time_unref(dt);
      } else {
        gchar *end_str = g_strdup_printf("%" G_GINT64_FORMAT, end);
        json_builder_add_string_value(builder, end_str);
        g_free(end_str);
      }
    }
    json_builder_end_array(builder);
  }

  /* location tag (optional) */
  if (location && *location) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "location");
    json_builder_add_string_value(builder, location);
    json_builder_end_array(builder);
  }

  /* image tag (optional) */
  if (image && *image) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "image");
    json_builder_add_string_value(builder, image);
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
