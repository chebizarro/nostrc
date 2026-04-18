/* nd-ical.c - Lightweight ICS ↔ NIP-52 conversion
 *
 * SPDX-License-Identifier: MIT
 */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#endif

#include "nd-ical.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

#define ND_ICAL_ERROR (nd_ical_error_quark())
G_DEFINE_QUARK(nd-ical-error-quark, nd_ical_error)

enum {
  ND_ICAL_ERROR_PARSE = 1,
  ND_ICAL_ERROR_UNSUPPORTED,
  ND_ICAL_ERROR_MISSING_FIELD
};

/* ---- Free ---- */

void
nd_calendar_event_free(NdCalendarEvent *event)
{
  if (event == NULL) return;
  g_free(event->uid);
  g_free(event->summary);
  g_free(event->description);
  g_free(event->dtstart_date);
  g_free(event->dtend_date);
  g_free(event->start_tzid);
  g_free(event->end_tzid);
  g_free(event->location);
  g_strfreev(event->categories);
  g_free(event->url);
  g_free(event->geo);
  g_free(event->pubkey);
  g_free(event);
}

/* ---- ICS line unfolding ---- */

/**
 * Unfold ICS content lines (RFC 5545 §3.1): CRLF followed by a space
 * or tab is a continuation of the previous line.
 */
static gchar *
ics_unfold(const gchar *text)
{
  GString *out = g_string_new(NULL);
  const gchar *p = text;

  while (*p) {
    if (p[0] == '\r' && p[1] == '\n' && (p[2] == ' ' || p[2] == '\t')) {
      p += 3; /* skip CRLF + WSP */
    } else if (p[0] == '\n' && (p[1] == ' ' || p[1] == '\t')) {
      p += 2; /* LF-only folding */
    } else {
      g_string_append_c(out, *p);
      p++;
    }
  }

  return g_string_free(out, FALSE);
}

/**
 * Extract the TZID parameter from a property line like:
 *   DTSTART;TZID=America/New_York:20260115T150000
 * Returns the TZID value or NULL.
 */
static gchar *
extract_tzid(const gchar *line)
{
  const gchar *tzid = strstr(line, "TZID=");
  if (tzid == NULL) return NULL;

  tzid += 5; /* skip "TZID=" */
  const gchar *end = strchr(tzid, ':');
  if (end == NULL) end = strchr(tzid, ';');
  if (end == NULL) return g_strdup(tzid);

  return g_strndup(tzid, (gsize)(end - tzid));
}

/**
 * Extract the value part after the first ':' from a property line.
 * In ICS format, the first ':' after the property name/params is the
 * value separator. Using strchr (not strrchr) so URLs with colons work.
 */
static gchar *
extract_value(const gchar *line)
{
  const gchar *colon = strchr(line, ':');
  if (colon == NULL) return g_strdup("");
  return g_strdup(colon + 1);
}

/**
 * Check if a DTSTART value is date-only (YYYYMMDD, 8 chars, no 'T').
 */
static gboolean
is_date_only_value(const gchar *value)
{
  if (value == NULL) return FALSE;
  gsize len = strlen(value);
  return (len == 8 && strchr(value, 'T') == NULL);
}

/**
 * Parse ICS date value (YYYYMMDD) to ISO date string (YYYY-MM-DD).
 */
static gchar *
ics_date_to_iso(const gchar *ics_date)
{
  if (ics_date == NULL || strlen(ics_date) < 8) return NULL;
  return g_strdup_printf("%.4s-%.2s-%.2s",
                         ics_date, ics_date + 4, ics_date + 6);
}

/**
 * Parse ICS datetime value (YYYYMMDDTHHMMSS[Z]) to Unix timestamp.
 */
static gint64
ics_datetime_to_timestamp(const gchar *ics_dt)
{
  if (ics_dt == NULL || strlen(ics_dt) < 15) return 0;

  struct tm tm = {0};
  /* Parse YYYYMMDDTHHMMSS */
  char buf[5];

  strncpy(buf, ics_dt, 4); buf[4] = '\0';
  tm.tm_year = atoi(buf) - 1900;

  strncpy(buf, ics_dt + 4, 2); buf[2] = '\0';
  tm.tm_mon = atoi(buf) - 1;

  strncpy(buf, ics_dt + 6, 2); buf[2] = '\0';
  tm.tm_mday = atoi(buf);

  /* Skip 'T' at position 8 */
  strncpy(buf, ics_dt + 9, 2); buf[2] = '\0';
  tm.tm_hour = atoi(buf);

  strncpy(buf, ics_dt + 11, 2); buf[2] = '\0';
  tm.tm_min = atoi(buf);

  strncpy(buf, ics_dt + 13, 2); buf[2] = '\0';
  tm.tm_sec = atoi(buf);

  gboolean utc = (strlen(ics_dt) >= 16 && ics_dt[15] == 'Z');

  if (utc) {
    return (gint64)timegm(&tm);
  } else {
    tm.tm_isdst = -1;
    return (gint64)mktime(&tm);
  }
}

/**
 * Convert ISO date (YYYY-MM-DD) to ICS date (YYYYMMDD).
 */
static gchar *
iso_date_to_ics(const gchar *iso_date)
{
  if (iso_date == NULL || strlen(iso_date) < 10) return NULL;
  /* Remove dashes */
  return g_strdup_printf("%.4s%.2s%.2s",
                         iso_date, iso_date + 5, iso_date + 8);
}

/**
 * Convert Unix timestamp to ICS datetime (YYYYMMDDTHHMMSSZ).
 */
static gchar *
timestamp_to_ics_utc(gint64 ts)
{
  time_t t = (time_t)ts;
  struct tm tm;
  gmtime_r(&t, &tm);
  return g_strdup_printf("%04d%02d%02dT%02d%02d%02dZ",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                         tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* ---- ICS parser ---- */

NdCalendarEvent *
nd_ical_parse_vevent(const gchar *ics_text, GError **error)
{
  if (ics_text == NULL || *ics_text == '\0') {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_PARSE,
                        "Empty ICS text");
    return NULL;
  }

  g_autofree gchar *unfolded = ics_unfold(ics_text);

  /* Find VEVENT boundaries */
  const gchar *vevent_start = strstr(unfolded, "BEGIN:VEVENT");
  if (vevent_start == NULL) {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_PARSE,
                        "No VEVENT found in ICS data");
    return NULL;
  }
  const gchar *vevent_end = strstr(vevent_start, "END:VEVENT");
  if (vevent_end == NULL) {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_PARSE,
                        "Unterminated VEVENT");
    return NULL;
  }

  NdCalendarEvent *event = g_new0(NdCalendarEvent, 1);
  GPtrArray *categories = g_ptr_array_new();

  /* Parse line by line within VEVENT */
  g_autofree gchar *vevent_text = g_strndup(vevent_start,
                                             (gsize)(vevent_end - vevent_start));
  g_auto(GStrv) lines = g_strsplit(vevent_text, "\n", -1);

  for (gsize i = 0; lines[i] != NULL; i++) {
    gchar *line = g_strstrip(lines[i]);
    /* Strip trailing CR */
    gsize len = strlen(line);
    if (len > 0 && line[len - 1] == '\r')
      line[len - 1] = '\0';

    if (g_str_has_prefix(line, "UID:")) {
      g_free(event->uid);
      event->uid = extract_value(line);
    } else if (g_str_has_prefix(line, "SUMMARY")) {
      g_free(event->summary);
      event->summary = extract_value(line);
    } else if (g_str_has_prefix(line, "DESCRIPTION")) {
      g_free(event->description);
      event->description = extract_value(line);
    } else if (g_str_has_prefix(line, "DTSTART")) {
      g_autofree gchar *val = extract_value(line);
      event->start_tzid = extract_tzid(line);

      if (is_date_only_value(val) ||
          strstr(line, "VALUE=DATE") != NULL) {
        event->is_date_only = TRUE;
        event->dtstart_date = ics_date_to_iso(val);
      } else {
        event->is_date_only = FALSE;
        event->dtstart_ts = ics_datetime_to_timestamp(val);
      }
    } else if (g_str_has_prefix(line, "DTEND")) {
      g_autofree gchar *val = extract_value(line);
      event->end_tzid = extract_tzid(line);

      if (is_date_only_value(val) ||
          strstr(line, "VALUE=DATE") != NULL) {
        event->dtend_date = ics_date_to_iso(val);
      } else {
        event->dtend_ts = ics_datetime_to_timestamp(val);
      }
    } else if (g_str_has_prefix(line, "LOCATION")) {
      g_free(event->location);
      event->location = extract_value(line);
    } else if (g_str_has_prefix(line, "CATEGORIES")) {
      g_autofree gchar *val = extract_value(line);
      g_auto(GStrv) cats = g_strsplit(val, ",", -1);
      for (gsize j = 0; cats[j]; j++) {
        gchar *cat = g_strstrip(cats[j]);
        if (*cat) g_ptr_array_add(categories, g_strdup(cat));
      }
    } else if (g_str_has_prefix(line, "URL")) {
      g_free(event->url);
      event->url = extract_value(line);
    } else if (g_str_has_prefix(line, "GEO")) {
      g_free(event->geo);
      event->geo = extract_value(line);
    } else if (g_str_has_prefix(line, "RRULE") ||
               g_str_has_prefix(line, "RDATE") ||
               g_str_has_prefix(line, "EXDATE")) {
      event->has_rrule = TRUE;
    }
  }

  /* Finalize categories */
  if (categories->len > 0) {
    g_ptr_array_add(categories, NULL);
    event->categories = (gchar **)g_ptr_array_free(categories, FALSE);
  } else {
    g_ptr_array_unref(categories);
  }

  /* Validate required fields */
  if (event->uid == NULL || event->uid[0] == '\0') {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_MISSING_FIELD,
                        "VEVENT missing required UID property");
    nd_calendar_event_free(event);
    return NULL;
  }

  if (event->has_rrule) {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_UNSUPPORTED,
                        "Recurring events (RRULE/RDATE/EXDATE) are not supported by NIP-52");
    nd_calendar_event_free(event);
    return NULL;
  }

  /* Set kind */
  event->kind = event->is_date_only ? ND_NIP52_KIND_DATE : ND_NIP52_KIND_TIME;

  return event;
}

/* ---- ICS generator ---- */

gchar *
nd_ical_generate_vevent(const NdCalendarEvent *event)
{
  g_return_val_if_fail(event != NULL, NULL);

  GString *ics = g_string_new(
    "BEGIN:VCALENDAR\r\n"
    "VERSION:2.0\r\n"
    "PRODID:-//nostr-dav//NONSGML v1.0//EN\r\n"
    "BEGIN:VEVENT\r\n");

  /* UID */
  g_string_append_printf(ics, "UID:%s\r\n", event->uid ? event->uid : "");

  /* SUMMARY */
  if (event->summary)
    g_string_append_printf(ics, "SUMMARY:%s\r\n", event->summary);

  /* DESCRIPTION */
  if (event->description && event->description[0])
    g_string_append_printf(ics, "DESCRIPTION:%s\r\n", event->description);

  /* DTSTART / DTEND */
  if (event->is_date_only) {
    if (event->dtstart_date) {
      g_autofree gchar *ics_date = iso_date_to_ics(event->dtstart_date);
      if (ics_date)
        g_string_append_printf(ics, "DTSTART;VALUE=DATE:%s\r\n", ics_date);
    }
    if (event->dtend_date) {
      g_autofree gchar *ics_date = iso_date_to_ics(event->dtend_date);
      if (ics_date)
        g_string_append_printf(ics, "DTEND;VALUE=DATE:%s\r\n", ics_date);
    }
  } else {
    if (event->dtstart_ts > 0) {
      if (event->start_tzid) {
        g_autofree gchar *dt = timestamp_to_ics_utc(event->dtstart_ts);
        g_string_append_printf(ics, "DTSTART;TZID=%s:%s\r\n",
                               event->start_tzid, dt);
      } else {
        g_autofree gchar *dt = timestamp_to_ics_utc(event->dtstart_ts);
        g_string_append_printf(ics, "DTSTART:%s\r\n", dt);
      }
    }
    if (event->dtend_ts > 0) {
      if (event->end_tzid) {
        g_autofree gchar *dt = timestamp_to_ics_utc(event->dtend_ts);
        g_string_append_printf(ics, "DTEND;TZID=%s:%s\r\n",
                               event->end_tzid, dt);
      } else {
        g_autofree gchar *dt = timestamp_to_ics_utc(event->dtend_ts);
        g_string_append_printf(ics, "DTEND:%s\r\n", dt);
      }
    }
  }

  /* LOCATION */
  if (event->location)
    g_string_append_printf(ics, "LOCATION:%s\r\n", event->location);

  /* CATEGORIES */
  if (event->categories) {
    g_autofree gchar *cats = g_strjoinv(",", event->categories);
    g_string_append_printf(ics, "CATEGORIES:%s\r\n", cats);
  }

  /* URL */
  if (event->url)
    g_string_append_printf(ics, "URL:%s\r\n", event->url);

  /* GEO */
  if (event->geo)
    g_string_append_printf(ics, "GEO:%s\r\n", event->geo);

  /* DTSTAMP (required by RFC 5545) */
  g_autofree gchar *dtstamp = timestamp_to_ics_utc(
    event->created_at > 0 ? event->created_at : (gint64)time(NULL));
  g_string_append_printf(ics, "DTSTAMP:%s\r\n", dtstamp);

  g_string_append(ics,
    "END:VEVENT\r\n"
    "END:VCALENDAR\r\n");

  return g_string_free(ics, FALSE);
}

/* ---- NIP-52 JSON conversion ---- */

gchar *
nd_ical_event_to_nip52_json(const NdCalendarEvent *event)
{
  g_return_val_if_fail(event != NULL, NULL);
  g_return_val_if_fail(event->uid != NULL, NULL);

  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);

  /* kind */
  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, event->kind);

  /* content */
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, event->description ? event->description : "");

  /* created_at */
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, (gint64)time(NULL));

  /* tags */
  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);

  /* d tag (UID) */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "d");
  json_builder_add_string_value(b, event->uid);
  json_builder_end_array(b);

  /* title tag */
  if (event->summary) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "title");
    json_builder_add_string_value(b, event->summary);
    json_builder_end_array(b);
  }

  /* start tag */
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "start");
  if (event->is_date_only && event->dtstart_date) {
    json_builder_add_string_value(b, event->dtstart_date);
  } else {
    g_autofree gchar *ts = g_strdup_printf("%" G_GINT64_FORMAT,
                                           event->dtstart_ts);
    json_builder_add_string_value(b, ts);
  }
  json_builder_end_array(b);

  /* end tag */
  if (event->is_date_only && event->dtend_date) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "end");
    json_builder_add_string_value(b, event->dtend_date);
    json_builder_end_array(b);
  } else if (!event->is_date_only && event->dtend_ts > 0) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "end");
    g_autofree gchar *ts = g_strdup_printf("%" G_GINT64_FORMAT,
                                           event->dtend_ts);
    json_builder_add_string_value(b, ts);
    json_builder_end_array(b);
  }

  /* start_tzid */
  if (event->start_tzid) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "start_tzid");
    json_builder_add_string_value(b, event->start_tzid);
    json_builder_end_array(b);
  }

  /* end_tzid */
  if (event->end_tzid) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "end_tzid");
    json_builder_add_string_value(b, event->end_tzid);
    json_builder_end_array(b);
  }

  /* location */
  if (event->location) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "location");
    json_builder_add_string_value(b, event->location);
    json_builder_end_array(b);
  }

  /* t tags (categories) */
  if (event->categories) {
    for (gsize i = 0; event->categories[i]; i++) {
      json_builder_begin_array(b);
      json_builder_add_string_value(b, "t");
      json_builder_add_string_value(b, event->categories[i]);
      json_builder_end_array(b);
    }
  }

  /* r tag (URL) */
  if (event->url) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "r");
    json_builder_add_string_value(b, event->url);
    json_builder_end_array(b);
  }

  /* g tag (geo) */
  if (event->geo) {
    json_builder_begin_array(b);
    json_builder_add_string_value(b, "g");
    json_builder_add_string_value(b, event->geo);
    json_builder_end_array(b);
  }

  json_builder_end_array(b); /* tags */
  json_builder_end_object(b);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, json_builder_get_root(b));
  json_generator_set_pretty(gen, FALSE);
  return json_generator_to_data(gen, NULL);
}

NdCalendarEvent *
nd_ical_event_from_nip52_json(const gchar *json_str, GError **error)
{
  if (json_str == NULL || *json_str == '\0') {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_PARSE,
                        "Empty JSON");
    return NULL;
  }

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *parse_err = NULL;
  if (!json_parser_load_from_data(parser, json_str, -1, &parse_err)) {
    g_propagate_error(error, parse_err);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_PARSE,
                        "JSON root is not an object");
    return NULL;
  }

  JsonObject *obj = json_node_get_object(root);
  gint64 kind = json_object_get_int_member_with_default(obj, "kind", 0);

  if (kind != ND_NIP52_KIND_DATE && kind != ND_NIP52_KIND_TIME) {
    g_set_error(error, ND_ICAL_ERROR, ND_ICAL_ERROR_PARSE,
                "Not a NIP-52 calendar event (kind %" G_GINT64_FORMAT ")", kind);
    return NULL;
  }

  NdCalendarEvent *event = g_new0(NdCalendarEvent, 1);
  event->kind = (gint)kind;
  event->is_date_only = (kind == ND_NIP52_KIND_DATE);
  event->description = g_strdup(
    json_object_get_string_member_with_default(obj, "content", ""));
  event->created_at = json_object_get_int_member_with_default(
    obj, "created_at", 0);
  event->pubkey = g_strdup(
    json_object_get_string_member_with_default(obj, "pubkey", ""));

  GPtrArray *cats = g_ptr_array_new();

  /* Parse tags */
  JsonArray *tags = json_object_get_array_member(obj, "tags");
  if (tags) {
    guint n = json_array_get_length(tags);
    for (guint i = 0; i < n; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2) continue;

      const gchar *key = json_array_get_string_element(tag, 0);
      const gchar *val = json_array_get_string_element(tag, 1);
      if (!key || !val) continue;

      if (g_str_equal(key, "d")) {
        g_free(event->uid);
        event->uid = g_strdup(val);
      } else if (g_str_equal(key, "title")) {
        g_free(event->summary);
        event->summary = g_strdup(val);
      } else if (g_str_equal(key, "start")) {
        if (event->is_date_only) {
          g_free(event->dtstart_date);
          event->dtstart_date = g_strdup(val);
        } else {
          event->dtstart_ts = g_ascii_strtoll(val, NULL, 10);
        }
      } else if (g_str_equal(key, "end")) {
        if (event->is_date_only) {
          g_free(event->dtend_date);
          event->dtend_date = g_strdup(val);
        } else {
          event->dtend_ts = g_ascii_strtoll(val, NULL, 10);
        }
      } else if (g_str_equal(key, "start_tzid")) {
        g_free(event->start_tzid);
        event->start_tzid = g_strdup(val);
      } else if (g_str_equal(key, "end_tzid")) {
        g_free(event->end_tzid);
        event->end_tzid = g_strdup(val);
      } else if (g_str_equal(key, "location")) {
        g_free(event->location);
        event->location = g_strdup(val);
      } else if (g_str_equal(key, "t")) {
        g_ptr_array_add(cats, g_strdup(val));
      } else if (g_str_equal(key, "r")) {
        g_free(event->url);
        event->url = g_strdup(val);
      } else if (g_str_equal(key, "g")) {
        g_free(event->geo);
        event->geo = g_strdup(val);
      }
    }
  }

  /* Finalize categories */
  if (cats->len > 0) {
    g_ptr_array_add(cats, NULL);
    event->categories = (gchar **)g_ptr_array_free(cats, FALSE);
  } else {
    g_ptr_array_unref(cats);
  }

  if (event->uid == NULL) {
    g_set_error_literal(error, ND_ICAL_ERROR, ND_ICAL_ERROR_MISSING_FIELD,
                        "NIP-52 event missing 'd' tag");
    nd_calendar_event_free(event);
    return NULL;
  }

  return event;
}

/* ---- ETag ---- */

gchar *
nd_ical_compute_etag(const NdCalendarEvent *event)
{
  g_return_val_if_fail(event != NULL, NULL);

  g_autofree gchar *input = g_strdup_printf("%d:%s:%s:%" G_GINT64_FORMAT,
    event->kind,
    event->pubkey ? event->pubkey : "",
    event->uid ? event->uid : "",
    event->created_at);

  g_autoptr(GChecksum) checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *)input, strlen(input));
  const gchar *hash = g_checksum_get_string(checksum);

  /* Quoted ETag: first 16 hex chars */
  return g_strdup_printf("\"%.*s\"", 16, hash);
}
