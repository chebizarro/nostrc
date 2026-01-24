/**
 * nip53_live.h - NIP-53 Live Activities utility for gnostr
 *
 * GTK-friendly wrapper for parsing and managing NIP-53 live activity events.
 * Live activities are kind 30311 events representing:
 * - Live streams and broadcasts
 * - Audio/video spaces
 * - Live events with participants
 *
 * Status values: planned, live, ended
 */

#ifndef GNOSTR_NIP53_LIVE_H
#define GNOSTR_NIP53_LIVE_H

#include <glib.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/**
 * Live Activity Status
 */
typedef enum {
  GNOSTR_LIVE_STATUS_UNKNOWN = 0,  /* Status not specified */
  GNOSTR_LIVE_STATUS_PLANNED,      /* Scheduled for future */
  GNOSTR_LIVE_STATUS_LIVE,         /* Currently live/active */
  GNOSTR_LIVE_STATUS_ENDED         /* Stream has ended */
} GnostrLiveStatus;

/**
 * Live Activity Participant
 * Represents a speaker, host, or participant in a live activity
 */
typedef struct {
  char *pubkey_hex;     /* 64-char hex pubkey */
  char *relay_hint;     /* Optional relay URL for this participant */
  char *role;           /* Role: "host", "speaker", "participant", etc. */
  char *display_name;   /* Cached display name (populated by resolver) */
  char *avatar_url;     /* Cached avatar URL (populated by resolver) */
} GnostrLiveParticipant;

/**
 * Live Activity Event
 * Parsed representation of a NIP-53 kind 30311 event
 */
typedef struct {
  /* Event metadata */
  char *event_id;           /* Event ID (hex) */
  char *pubkey;             /* Event author pubkey (hex) */
  char *d_tag;              /* Unique identifier (d tag) */
  gint64 created_at;        /* Event creation timestamp */

  /* Content fields from tags */
  char *title;              /* Event title */
  char *summary;            /* Event description/summary */
  char *image;              /* Cover/preview image URL */
  GnostrLiveStatus status;  /* Current status */

  /* Streaming info */
  char **streaming_urls;    /* NULL-terminated array of streaming URLs */
  char **recording_urls;    /* NULL-terminated array of recording URLs */

  /* Timing */
  gint64 starts_at;         /* Scheduled start time (0 if not set) */
  gint64 ends_at;           /* Scheduled/actual end time (0 if not set) */

  /* Participants */
  GnostrLiveParticipant **participants;  /* NULL-terminated array */
  gsize participant_count;

  /* Metadata */
  char **hashtags;          /* NULL-terminated array of hashtags (t tags) */
  char **relays;            /* NULL-terminated array of relay URLs */
  gint current_viewers;     /* Current viewer/participant count */
  gint total_viewers;       /* Total/peak viewer count */
} GnostrLiveActivity;

/**
 * gnostr_live_status_from_string:
 * @status_str: status string from event tag
 *
 * Parses a status string into enum value.
 *
 * Returns: The corresponding GnostrLiveStatus
 */
GnostrLiveStatus gnostr_live_status_from_string(const char *status_str);

/**
 * gnostr_live_status_to_string:
 * @status: status enum value
 *
 * Returns: Static string representation of the status
 */
const char *gnostr_live_status_to_string(GnostrLiveStatus status);

/**
 * gnostr_live_activity_parse:
 * @event_json: Raw JSON string of the kind 30311 event
 *
 * Parses a NIP-53 live activity event from JSON.
 *
 * Returns: (transfer full) (nullable): New #GnostrLiveActivity or NULL on error
 */
GnostrLiveActivity *gnostr_live_activity_parse(const char *event_json);

/**
 * gnostr_live_activity_parse_tags:
 * @tags_json: JSON array of tags from event
 * @pubkey: Event author pubkey (hex)
 * @event_id: Event ID (hex)
 * @created_at: Event creation timestamp
 *
 * Parses live activity from pre-extracted tags array.
 *
 * Returns: (transfer full) (nullable): New #GnostrLiveActivity or NULL on error
 */
GnostrLiveActivity *gnostr_live_activity_parse_tags(const char *tags_json,
                                                     const char *pubkey,
                                                     const char *event_id,
                                                     gint64 created_at);

/**
 * gnostr_live_activity_free:
 * @activity: Activity to free
 *
 * Frees a live activity and all its data.
 */
void gnostr_live_activity_free(GnostrLiveActivity *activity);

/**
 * gnostr_live_activity_copy:
 * @activity: Activity to copy
 *
 * Creates a deep copy of a live activity.
 *
 * Returns: (transfer full) (nullable): New copy or NULL
 */
GnostrLiveActivity *gnostr_live_activity_copy(const GnostrLiveActivity *activity);

/**
 * gnostr_live_participant_free:
 * @participant: Participant to free
 *
 * Frees a participant struct.
 */
void gnostr_live_participant_free(GnostrLiveParticipant *participant);

/**
 * gnostr_live_activity_get_host:
 * @activity: The live activity
 *
 * Gets the host participant (first with role "host").
 *
 * Returns: (transfer none) (nullable): Host participant or NULL
 */
GnostrLiveParticipant *gnostr_live_activity_get_host(const GnostrLiveActivity *activity);

/**
 * gnostr_live_activity_get_speakers:
 * @activity: The live activity
 * @out_count: (out): Number of speakers returned
 *
 * Gets all participants with role "speaker" or "host".
 *
 * Returns: (transfer container) (nullable): Array of speakers (free array only, not elements)
 */
GnostrLiveParticipant **gnostr_live_activity_get_speakers(const GnostrLiveActivity *activity,
                                                           gsize *out_count);

/**
 * gnostr_live_activity_get_primary_stream:
 * @activity: The live activity
 *
 * Gets the first/primary streaming URL.
 *
 * Returns: (transfer none) (nullable): Primary streaming URL or NULL
 */
const char *gnostr_live_activity_get_primary_stream(const GnostrLiveActivity *activity);

/**
 * gnostr_live_activity_is_active:
 * @activity: The live activity
 *
 * Checks if the activity is currently live.
 *
 * Returns: TRUE if status is LIVE
 */
gboolean gnostr_live_activity_is_active(const GnostrLiveActivity *activity);

/**
 * gnostr_live_activity_format_time_until:
 * @activity: The live activity
 *
 * Formats time until start for planned events (e.g., "in 2 hours", "Tomorrow at 3 PM").
 *
 * Returns: (transfer full) (nullable): Formatted string or NULL if not planned
 */
char *gnostr_live_activity_format_time_until(const GnostrLiveActivity *activity);

/**
 * gnostr_live_activity_format_duration:
 * @activity: The live activity
 *
 * Formats duration for live/ended events (e.g., "Live for 45 min", "Lasted 2 hours").
 *
 * Returns: (transfer full) (nullable): Formatted string or NULL
 */
char *gnostr_live_activity_format_duration(const GnostrLiveActivity *activity);

G_END_DECLS

#endif /* GNOSTR_NIP53_LIVE_H */
