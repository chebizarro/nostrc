/* sheet-event-details.h - Event Details View dialog
 *
 * Displays full information about a signed Nostr event:
 * - Event Type (kind number + name)
 * - Date/Time
 * - Public Key (truncated + copy button)
 * - Event ID (truncated + copy button)
 * - Signature (truncated + copy button)
 * - Content (expandable)
 * - Tags (list view)
 */
#ifndef APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_EVENT_DETAILS_H
#define APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_EVENT_DETAILS_H

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SHEET_EVENT_DETAILS (sheet_event_details_get_type())
G_DECLARE_FINAL_TYPE(SheetEventDetails, sheet_event_details, SHEET, EVENT_DETAILS, AdwDialog)

/* Create a new event details dialog */
SheetEventDetails *sheet_event_details_new(void);

/* Set the event data to display */
void sheet_event_details_set_event(SheetEventDetails *self,
                                   gint kind,
                                   gint64 created_at,
                                   const gchar *pubkey,
                                   const gchar *event_id,
                                   const gchar *signature,
                                   const gchar *content,
                                   const gchar *tags_json);

/* Set event from raw JSON */
void sheet_event_details_set_event_json(SheetEventDetails *self,
                                        const gchar *event_json);

G_END_DECLS
#endif /* APPS_GNOSTR_SIGNER_UI_SHEETS_SHEET_EVENT_DETAILS_H */
