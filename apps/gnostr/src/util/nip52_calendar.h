/**
 * NIP-52: Calendar Events Utility
 *
 * Calendar events for date-based (kind 31922) and time-based (kind 31923) events.
 * These are parameterized replaceable events using the "d" tag as identifier.
 *
 * Event Structure:
 * - kind: 31922 (date-based) or 31923 (time-based)
 * - content: event description
 * - tags:
 *   - ["d", "<identifier>"] - required unique identifier
 *   - ["title", "<title>"] - event title
 *   - ["start", "<timestamp|date>"] - required start time/date
 *   - ["end", "<timestamp|date>"] - optional end time/date
 *   - ["location", "<location>"] - optional location (multiple allowed)
 *   - ["g", "<geohash>"] - optional geohash (multiple allowed)
 *   - ["p", "<pubkey>", "<relay>", "<role>"] - participants (multiple allowed)
 *   - ["t", "<hashtag>"] - optional hashtags (multiple allowed)
 *   - ["r", "<url>"] - optional references (multiple allowed)
 *   - ["image", "<url>"] - optional event image
 *   - ["start_tzid", "<timezone>"] - optional timezone for start (time-based)
 *   - ["end_tzid", "<timezone>"] - optional timezone for end (time-based)
 */

#ifndef GNOSTR_NIP52_CALENDAR_H
#define GNOSTR_NIP52_CALENDAR_H

#include <glib.h>

G_BEGIN_DECLS

/* Kind numbers for calendar events */
#define NIP52_KIND_DATE_BASED_EVENT 31922
#define NIP52_KIND_TIME_BASED_EVENT 31923

/* Calendar event types */
typedef enum {
  GNOSTR_CALENDAR_EVENT_DATE_BASED = 31922,
  GNOSTR_CALENDAR_EVENT_TIME_BASED = 31923
} GnostrCalendarEventType;

/**
 * Parsed participant data from "p" tags
 */
typedef struct {
  gchar *pubkey;       /* Participant public key (hex) */
  gchar *relay;        /* Suggested relay URL (optional) */
  gchar *role;         /* Role in event: "host", "speaker", "attendee", etc. (optional) */
} GnostrNip52Participant;

/**
 * Parsed calendar event data
 */
typedef struct {
  gchar *event_id;           /* Event ID (hex) */
  gchar *pubkey;             /* Creator public key (hex) */
  GnostrCalendarEventType type;  /* Date-based or time-based */
  gchar *d_tag;              /* Unique identifier ("d" tag) */
  gchar *title;              /* Event title */
  gchar *description;        /* Event description (from content) */
  gchar *image;              /* Event image URL (optional) */

  /* Time information */
  gint64 start;              /* Start time (unix timestamp for time-based, date midnight for date-based) */
  gint64 end;                /* End time (unix timestamp, 0 if not specified) */
  gchar *start_date;         /* Original start date string for date-based (YYYY-MM-DD) */
  gchar *end_date;           /* Original end date string for date-based (YYYY-MM-DD) */
  gchar *start_tzid;         /* Timezone ID for start (optional) */
  gchar *end_tzid;           /* Timezone ID for end (optional) */

  /* Location information */
  gchar **locations;         /* Location strings (NULL-terminated) */
  gsize locations_count;     /* Number of locations */
  gchar **geohashes;         /* Geohash strings (NULL-terminated) */
  gsize geohashes_count;     /* Number of geohashes */

  /* Participants */
  GnostrNip52Participant *participants;  /* Array of participants */
  gsize participants_count;              /* Number of participants */

  /* Metadata */
  gchar **hashtags;          /* Hashtag strings (NULL-terminated) */
  gsize hashtags_count;      /* Number of hashtags */
  gchar **references;        /* Reference URLs (NULL-terminated) */
  gsize references_count;    /* Number of references */

  gint64 created_at;         /* Event creation timestamp */
} GnostrNip52CalendarEvent;

/**
 * gnostr_nip52_is_calendar_kind:
 * @kind: Event kind
 *
 * Check if an event kind is a calendar event (kind 31922 or 31923).
 *
 * Returns: TRUE if kind is a calendar event kind
 */
gboolean gnostr_nip52_is_calendar_kind(gint kind);

/**
 * gnostr_nip52_is_date_based:
 * @kind: Event kind
 *
 * Check if calendar event is date-based (kind 31922).
 *
 * Returns: TRUE if kind is date-based
 */
gboolean gnostr_nip52_is_date_based(gint kind);

/**
 * gnostr_nip52_is_time_based:
 * @kind: Event kind
 *
 * Check if calendar event is time-based (kind 31923).
 *
 * Returns: TRUE if kind is time-based
 */
gboolean gnostr_nip52_is_time_based(gint kind);

/**
 * gnostr_nip52_calendar_event_parse:
 * @json_str: JSON string of the event
 *
 * Parse a calendar event from JSON.
 *
 * Returns: (transfer full) (nullable): Parsed event or NULL on error.
 *          Free with gnostr_nip52_calendar_event_free().
 */
GnostrNip52CalendarEvent *gnostr_nip52_calendar_event_parse(const gchar *json_str);

/**
 * gnostr_nip52_calendar_event_free:
 * @event: Event to free
 *
 * Free a parsed calendar event.
 */
void gnostr_nip52_calendar_event_free(GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_participant_free:
 * @participant: Participant to free (frees strings but not the struct itself)
 *
 * Free participant data.
 */
void gnostr_nip52_participant_free(GnostrNip52Participant *participant);

/**
 * gnostr_nip52_event_is_upcoming:
 * @event: Calendar event
 *
 * Check if the event is upcoming (hasn't started yet).
 *
 * Returns: TRUE if event start time is in the future
 */
gboolean gnostr_nip52_event_is_upcoming(const GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_event_is_ongoing:
 * @event: Calendar event
 *
 * Check if the event is currently ongoing.
 *
 * Returns: TRUE if current time is between start and end
 */
gboolean gnostr_nip52_event_is_ongoing(const GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_event_is_past:
 * @event: Calendar event
 *
 * Check if the event has ended.
 *
 * Returns: TRUE if event end time has passed
 */
gboolean gnostr_nip52_event_is_past(const GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_format_date:
 * @timestamp: Unix timestamp
 *
 * Format a date for display (e.g., "January 15, 2026").
 *
 * Returns: (transfer full): Formatted date string
 */
gchar *gnostr_nip52_format_date(gint64 timestamp);

/**
 * gnostr_nip52_format_time:
 * @timestamp: Unix timestamp
 *
 * Format a time for display (e.g., "3:00 PM").
 *
 * Returns: (transfer full): Formatted time string
 */
gchar *gnostr_nip52_format_time(gint64 timestamp);

/**
 * gnostr_nip52_format_datetime:
 * @timestamp: Unix timestamp
 *
 * Format a date and time for display (e.g., "Jan 15, 2026 at 3:00 PM").
 *
 * Returns: (transfer full): Formatted datetime string
 */
gchar *gnostr_nip52_format_datetime(gint64 timestamp);

/**
 * gnostr_nip52_format_date_range:
 * @event: Calendar event
 *
 * Format the event's date/time range for display.
 * For date-based: "January 15, 2026" or "January 15-17, 2026"
 * For time-based: "Jan 15, 2026 at 3:00 PM - 5:00 PM"
 *
 * Returns: (transfer full): Formatted range string
 */
gchar *gnostr_nip52_format_date_range(const GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_format_time_until:
 * @event: Calendar event
 *
 * Format time remaining until event starts (e.g., "in 3 days", "in 2 hours").
 *
 * Returns: (transfer full) (nullable): Formatted string or NULL if event started
 */
gchar *gnostr_nip52_format_time_until(const GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_get_primary_location:
 * @event: Calendar event
 *
 * Get the first/primary location from the event.
 *
 * Returns: (transfer none) (nullable): Primary location string or NULL
 */
const gchar *gnostr_nip52_get_primary_location(const GnostrNip52CalendarEvent *event);

/**
 * gnostr_nip52_build_a_tag:
 * @kind: Event kind (31922 or 31923)
 * @pubkey_hex: Creator's public key
 * @d_tag: The "d" tag identifier
 *
 * Build an "a" tag value for referencing this calendar event.
 * Format: "kind:pubkey:d-tag"
 *
 * Returns: (transfer full): "a" tag value string
 */
gchar *gnostr_nip52_build_a_tag(gint kind, const gchar *pubkey_hex, const gchar *d_tag);

/**
 * gnostr_nip52_build_calendar_event:
 * @type: Event type (date-based or time-based)
 * @title: Event title
 * @description: Event description (content)
 * @start: Start timestamp
 * @end: End timestamp (0 for no end time)
 * @location: Primary location (optional)
 * @image: Event image URL (optional)
 *
 * Build an unsigned calendar event JSON.
 * The event must be signed before publishing.
 *
 * Returns: (transfer full) (nullable): JSON string of the unsigned event
 */
gchar *gnostr_nip52_build_calendar_event(GnostrCalendarEventType type,
                                          const gchar *title,
                                          const gchar *description,
                                          gint64 start,
                                          gint64 end,
                                          const gchar *location,
                                          const gchar *image);

G_END_DECLS

#endif /* GNOSTR_NIP52_CALENDAR_H */
