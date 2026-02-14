/* events-page.h - Recent Events page showing signed event history
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef APPS_GNOSTR_SIGNER_UI_EVENTS_PAGE_H
#define APPS_GNOSTR_SIGNER_UI_EVENTS_PAGE_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_EVENTS_PAGE (events_page_get_type())

G_DECLARE_FINAL_TYPE(EventsPage, events_page, EVENTS, PAGE, AdwBin)

/**
 * EventItem:
 *
 * Data model for a signed event in the history list.
 */
#define TYPE_EVENT_ITEM (event_item_get_type())

G_DECLARE_FINAL_TYPE(EventItem, event_item, EVENT, ITEM, GObject)

/**
 * event_item_new:
 * @event_id: the event ID (hex string)
 * @event_kind: the event kind number
 * @timestamp: Unix timestamp when the event was signed
 *
 * Creates a new EventItem for the events list.
 *
 * Returns: (transfer full): a new #EventItem
 */
EventItem *event_item_new(const char *event_id,
                          guint32     event_kind,
                          gint64      timestamp);

/**
 * event_item_get_event_id:
 * @self: an #EventItem
 *
 * Returns: (transfer none): the event ID
 */
const char *event_item_get_event_id(EventItem *self);

/**
 * event_item_get_event_kind:
 * @self: an #EventItem
 *
 * Returns: the event kind number
 */
guint32 event_item_get_event_kind(EventItem *self);

/**
 * event_item_get_timestamp:
 * @self: an #EventItem
 *
 * Returns: the Unix timestamp
 */
gint64 event_item_get_timestamp(EventItem *self);

/**
 * events_page_new:
 *
 * Creates a new EventsPage widget.
 *
 * Returns: (transfer full): a new #EventsPage
 */
EventsPage *events_page_new(void);

/**
 * events_page_add_event:
 * @self: an #EventsPage
 * @event_id: the event ID (hex string)
 * @event_kind: the event kind number
 * @timestamp: Unix timestamp when the event was signed
 *
 * Adds a signed event to the history list.
 */
void events_page_add_event(EventsPage *self,
                           const char *event_id,
                           guint32     event_kind,
                           gint64      timestamp);

/**
 * events_page_clear:
 * @self: an #EventsPage
 *
 * Clears all events from the history list.
 */
void events_page_clear(EventsPage *self);

/**
 * events_page_get_event_store:
 * @self: an #EventsPage
 *
 * Returns: (transfer none): the underlying #GListStore of events
 */
GListStore *events_page_get_event_store(EventsPage *self);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_EVENTS_PAGE_H */
