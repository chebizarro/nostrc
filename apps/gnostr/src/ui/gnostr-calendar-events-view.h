/*
 * gnostr-calendar-events-view.h - NIP-52 Calendar Events View
 *
 * Displays a filterable list of NIP-52 calendar events (kinds 31922/31923)
 * sourced from NDB via GnNostrEventModel. Provides filter pills
 * (All/Upcoming/Now/Past) and sorts events by start time.
 */

#ifndef GNOSTR_CALENDAR_EVENTS_VIEW_H
#define GNOSTR_CALENDAR_EVENTS_VIEW_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_CALENDAR_EVENTS_VIEW (gnostr_calendar_events_view_get_type())

G_DECLARE_FINAL_TYPE(GnostrCalendarEventsView, gnostr_calendar_events_view, GNOSTR, CALENDAR_EVENTS_VIEW, GtkWidget)

/**
 * GnostrCalendarEventsFilter:
 * @GNOSTR_CALENDAR_FILTER_ALL: Show all events
 * @GNOSTR_CALENDAR_FILTER_UPCOMING: Show only upcoming events
 * @GNOSTR_CALENDAR_FILTER_ONGOING: Show currently ongoing events
 * @GNOSTR_CALENDAR_FILTER_PAST: Show past events
 *
 * Filter modes for the calendar events view.
 */
typedef enum {
  GNOSTR_CALENDAR_FILTER_ALL,
  GNOSTR_CALENDAR_FILTER_UPCOMING,
  GNOSTR_CALENDAR_FILTER_ONGOING,
  GNOSTR_CALENDAR_FILTER_PAST
} GnostrCalendarEventsFilter;

/**
 * Signals:
 *
 * "open-profile" (gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when the user clicks an organizer/participant profile
 *
 * "open-event" (gchar* event_id_hex, gpointer user_data)
 *   - Emitted when the user clicks to expand an event
 *
 * "rsvp-requested" (gchar* event_id, gchar* d_tag, gchar* pubkey_hex, gpointer user_data)
 *   - Emitted when the user clicks the RSVP button on a card
 */

typedef struct _GnostrCalendarEventsView GnostrCalendarEventsView;

/**
 * gnostr_calendar_events_view_new:
 *
 * Creates a new calendar events view widget.
 *
 * Returns: (transfer full): A new GnostrCalendarEventsView widget.
 */
GnostrCalendarEventsView *gnostr_calendar_events_view_new(void);

/**
 * gnostr_calendar_events_view_set_filter:
 * @self: The calendar events view widget.
 * @filter: The filter mode to apply.
 *
 * Sets the active filter for the events list.
 */
void gnostr_calendar_events_view_set_filter(GnostrCalendarEventsView *self,
                                             GnostrCalendarEventsFilter filter);

/**
 * gnostr_calendar_events_view_get_filter:
 * @self: The calendar events view widget.
 *
 * Gets the active filter mode.
 *
 * Returns: Current filter mode.
 */
GnostrCalendarEventsFilter gnostr_calendar_events_view_get_filter(GnostrCalendarEventsView *self);

/**
 * gnostr_calendar_events_view_set_logged_in:
 * @self: The calendar events view widget.
 * @logged_in: Whether the user is logged in.
 *
 * Sets the login state. Affects RSVP button sensitivity on cards.
 */
void gnostr_calendar_events_view_set_logged_in(GnostrCalendarEventsView *self,
                                                gboolean logged_in);

/**
 * gnostr_calendar_events_view_get_event_count:
 * @self: The calendar events view widget.
 *
 * Returns the number of visible (filtered) events.
 *
 * Returns: Visible event count.
 */
guint gnostr_calendar_events_view_get_event_count(GnostrCalendarEventsView *self);

G_END_DECLS

#endif /* GNOSTR_CALENDAR_EVENTS_VIEW_H */
