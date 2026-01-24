/*
 * gnostr-calendar-event-card.h - NIP-52 Calendar Event Card Widget
 *
 * Displays kind 31922 (date-based) and kind 31923 (time-based) calendar events with:
 * - Title from "title" tag
 * - Date/time range from "start" and "end" tags
 * - Location from "location" tags
 * - Organizer info (avatar, name) from profile lookup
 * - Participant list from "p" tags with profile links
 * - Event image from "image" tag (optional)
 * - Status indicator (upcoming, ongoing, past)
 * - Timezone display for time-based events
 */

#ifndef GNOSTR_CALENDAR_EVENT_CARD_H
#define GNOSTR_CALENDAR_EVENT_CARD_H

#include <gtk/gtk.h>
#include "../util/nip52_calendar.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_CALENDAR_EVENT_CARD (gnostr_calendar_event_card_get_type())

G_DECLARE_FINAL_TYPE(GnostrCalendarEventCard, gnostr_calendar_event_card, GNOSTR, CALENDAR_EVENT_CARD, GtkWidget)

/* Signals:
 * "open-profile" (gchar* pubkey_hex, gpointer user_data) - open user profile
 * "open-event" (gchar* event_id_hex, gpointer user_data) - open full event view
 * "open-url" (gchar* url, gpointer user_data) - open external URL/location
 * "open-map" (gchar* geohash, gpointer user_data) - open location on map
 * "rsvp-requested" (gchar* event_id, gchar* d_tag, gchar* pubkey_hex, gpointer user_data)
 * "share-event" (gchar* nostr_uri, gpointer user_data)
 */

typedef struct _GnostrCalendarEventCard GnostrCalendarEventCard;

/**
 * gnostr_calendar_event_card_new:
 *
 * Creates a new calendar event card widget.
 *
 * Returns: (transfer full): New calendar event card widget.
 */
GnostrCalendarEventCard *gnostr_calendar_event_card_new(void);

/**
 * gnostr_calendar_event_card_set_event:
 * @self: Calendar event card widget
 * @event: Parsed NIP-52 calendar event
 *
 * Set the calendar event data. Copies the relevant fields.
 */
void gnostr_calendar_event_card_set_event(GnostrCalendarEventCard *self,
                                           const GnostrNip52CalendarEvent *event);

/**
 * gnostr_calendar_event_card_set_organizer:
 * @self: Calendar event card widget
 * @display_name: Organizer's display name
 * @handle: Organizer's handle/username
 * @avatar_url: Organizer's avatar URL
 * @pubkey_hex: Organizer's public key in hex format
 *
 * Set the event organizer's profile information.
 */
void gnostr_calendar_event_card_set_organizer(GnostrCalendarEventCard *self,
                                               const char *display_name,
                                               const char *handle,
                                               const char *avatar_url,
                                               const char *pubkey_hex);

/**
 * gnostr_calendar_event_card_set_nip05:
 * @self: Calendar event card widget
 * @nip05: NIP-05 identifier
 * @pubkey_hex: Organizer's public key for verification
 *
 * Set NIP-05 verification for the organizer.
 */
void gnostr_calendar_event_card_set_nip05(GnostrCalendarEventCard *self,
                                           const char *nip05,
                                           const char *pubkey_hex);

/**
 * gnostr_calendar_event_card_add_participant:
 * @self: Calendar event card widget
 * @display_name: Participant's display name
 * @avatar_url: Participant's avatar URL
 * @pubkey_hex: Participant's public key
 * @role: Participant's role (optional, e.g., "speaker", "host")
 *
 * Add a participant to the event card with profile info.
 * Call after set_event for each participant.
 */
void gnostr_calendar_event_card_add_participant(GnostrCalendarEventCard *self,
                                                 const char *display_name,
                                                 const char *avatar_url,
                                                 const char *pubkey_hex,
                                                 const char *role);

/**
 * gnostr_calendar_event_card_set_logged_in:
 * @self: Calendar event card widget
 * @logged_in: Whether user is logged in
 *
 * Set login state. Affects RSVP button sensitivity.
 */
void gnostr_calendar_event_card_set_logged_in(GnostrCalendarEventCard *self,
                                               gboolean logged_in);

/**
 * gnostr_calendar_event_card_set_rsvp_status:
 * @self: Calendar event card widget
 * @has_rsvp: Whether user has RSVP'd to this event
 *
 * Set the user's RSVP status for this event.
 */
void gnostr_calendar_event_card_set_rsvp_status(GnostrCalendarEventCard *self,
                                                 gboolean has_rsvp);

/**
 * gnostr_calendar_event_card_get_event_id:
 * @self: Calendar event card widget
 *
 * Get the event ID.
 *
 * Returns: (transfer none) (nullable): Event ID or NULL.
 */
const char *gnostr_calendar_event_card_get_event_id(GnostrCalendarEventCard *self);

/**
 * gnostr_calendar_event_card_get_d_tag:
 * @self: Calendar event card widget
 *
 * Get the d-tag identifier.
 *
 * Returns: (transfer none) (nullable): D-tag or NULL.
 */
const char *gnostr_calendar_event_card_get_d_tag(GnostrCalendarEventCard *self);

/**
 * gnostr_calendar_event_card_get_a_tag:
 * @self: Calendar event card widget
 *
 * Get the NIP-33 "a" tag reference (kind:pubkey:d-tag).
 *
 * Returns: (transfer full) (nullable): "a" tag value or NULL.
 */
char *gnostr_calendar_event_card_get_a_tag(GnostrCalendarEventCard *self);

/**
 * gnostr_calendar_event_card_get_event_type:
 * @self: Calendar event card widget
 *
 * Get the event type (date-based or time-based).
 *
 * Returns: Event type enum value.
 */
GnostrCalendarEventType gnostr_calendar_event_card_get_event_type(GnostrCalendarEventCard *self);

/**
 * gnostr_calendar_event_card_is_date_based:
 * @self: Calendar event card widget
 *
 * Check if this is a date-based event (kind 31922).
 *
 * Returns: TRUE if date-based, FALSE if time-based.
 */
gboolean gnostr_calendar_event_card_is_date_based(GnostrCalendarEventCard *self);

G_END_DECLS

#endif /* GNOSTR_CALENDAR_EVENT_CARD_H */
