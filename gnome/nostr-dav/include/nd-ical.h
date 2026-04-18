/* nd-ical.h - Lightweight ICS ↔ NIP-52 conversion
 *
 * SPDX-License-Identifier: MIT
 *
 * Parses iCalendar VEVENT components and converts them to/from
 * NIP-52 calendar event data. No libical dependency — handles the
 * subset of properties needed for the DAV bridge.
 */
#ifndef ND_ICAL_H
#define ND_ICAL_H

#include <glib.h>

G_BEGIN_DECLS

/* NIP-52 kind numbers */
#define ND_NIP52_KIND_DATE  31922
#define ND_NIP52_KIND_TIME  31923

/**
 * NdCalendarEvent:
 *
 * Represents a calendar event in both ICS and NIP-52 domains.
 */
typedef struct {
  gchar *uid;              /* ICS UID / NIP-52 "d" tag */
  gchar *summary;          /* ICS SUMMARY / NIP-52 "title" tag */
  gchar *description;      /* ICS DESCRIPTION / NIP-52 content */

  /* Time — mutually exclusive date vs timestamp representation */
  gboolean is_date_only;   /* TRUE → kind 31922, FALSE → kind 31923 */
  gchar *dtstart_date;     /* YYYY-MM-DD (date-only) */
  gchar *dtend_date;       /* YYYY-MM-DD (date-only) */
  gint64 dtstart_ts;       /* Unix timestamp (time-based) */
  gint64 dtend_ts;         /* Unix timestamp (time-based), 0 if absent */
  gchar *start_tzid;       /* TZID parameter on DTSTART (nullable) */
  gchar *end_tzid;         /* TZID parameter on DTEND (nullable) */

  /* Metadata */
  gchar *location;         /* ICS LOCATION / NIP-52 "location" tag */
  gchar **categories;      /* ICS CATEGORIES / NIP-52 "t" tags (NULL-terminated) */
  gchar *url;              /* ICS URL / NIP-52 "r" tag */
  gchar *geo;              /* ICS GEO lat;lon → NIP-52 "g" tag (geohash) */

  /* NIP-52 specific */
  gchar *pubkey;           /* Creator pubkey (hex, from NIP-52) */
  gint64 created_at;       /* Event created_at (from NIP-52) */
  gint   kind;             /* 31922 or 31923 */

  /* Has RRULE? (v1: reject if TRUE) */
  gboolean has_rrule;
} NdCalendarEvent;

/**
 * nd_calendar_event_free:
 * @event: (transfer full): event to free
 */
void nd_calendar_event_free(NdCalendarEvent *event);

/**
 * nd_ical_parse_vevent:
 * @ics_text: raw iCalendar text (VCALENDAR wrapper with VEVENT inside)
 * @error: (out) (optional): location for error
 *
 * Parses the first VEVENT from an ICS payload.
 *
 * Returns: (transfer full) (nullable): parsed event or NULL on error.
 */
NdCalendarEvent *nd_ical_parse_vevent(const gchar *ics_text,
                                      GError     **error);

/**
 * nd_ical_generate_vevent:
 * @event: the calendar event
 *
 * Generates a full VCALENDAR/VEVENT ICS string from an event.
 *
 * Returns: (transfer full): ICS text.
 */
gchar *nd_ical_generate_vevent(const NdCalendarEvent *event);

/**
 * nd_ical_event_to_nip52_json:
 * @event: the calendar event
 *
 * Builds unsigned NIP-52 event JSON from a parsed calendar event.
 * The JSON includes kind, content, created_at, and tags.
 *
 * Returns: (transfer full) (nullable): JSON string or NULL on error.
 */
gchar *nd_ical_event_to_nip52_json(const NdCalendarEvent *event);

/**
 * nd_ical_event_from_nip52_json:
 * @json_str: NIP-52 event JSON
 * @error: (out) (optional): location for error
 *
 * Parses a NIP-52 calendar event (kind 31922 or 31923) into an NdCalendarEvent.
 *
 * Returns: (transfer full) (nullable): parsed event or NULL on error.
 */
NdCalendarEvent *nd_ical_event_from_nip52_json(const gchar *json_str,
                                                GError     **error);

/**
 * nd_ical_compute_etag:
 * @event: the calendar event
 *
 * Computes a stable ETag for a calendar event.
 * Hash of (kind, pubkey, d-tag, created_at).
 *
 * Returns: (transfer full): ETag string (quoted, per HTTP spec).
 */
gchar *nd_ical_compute_etag(const NdCalendarEvent *event);

G_END_DECLS
#endif /* ND_ICAL_H */
