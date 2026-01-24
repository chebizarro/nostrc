/**
 * GnostrReportDialog - NIP-56 Report Dialog
 *
 * Dialog for reporting content/users per NIP-56.
 * Creates kind 1984 events with appropriate tags.
 */

#ifndef GNOSTR_REPORT_DIALOG_H
#define GNOSTR_REPORT_DIALOG_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_REPORT_DIALOG (gnostr_report_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrReportDialog, gnostr_report_dialog, GNOSTR, REPORT_DIALOG, GtkWindow)

/**
 * Signals:
 * "report-sent" - Emitted when report is successfully sent
 *   void handler(GnostrReportDialog *self, gchar *event_id, gchar *report_type, gpointer user_data)
 * "report-failed" - Emitted when report fails
 *   void handler(GnostrReportDialog *self, gchar *error_message, gpointer user_data)
 */

/**
 * NIP-56 Report types
 */
typedef enum {
  GNOSTR_REPORT_TYPE_NUDITY,
  GNOSTR_REPORT_TYPE_MALWARE,
  GNOSTR_REPORT_TYPE_PROFANITY,
  GNOSTR_REPORT_TYPE_ILLEGAL,
  GNOSTR_REPORT_TYPE_SPAM,
  GNOSTR_REPORT_TYPE_IMPERSONATION,
  GNOSTR_REPORT_TYPE_OTHER
} GnostrReportType;

/**
 * gnostr_report_dialog_new:
 * @parent: (nullable): Parent window
 *
 * Create a new report dialog.
 *
 * Returns: (transfer full): New report dialog
 */
GnostrReportDialog *gnostr_report_dialog_new(GtkWindow *parent);

/**
 * gnostr_report_dialog_set_target:
 * @self: the report dialog
 * @event_id_hex: (nullable): Event ID being reported (hex)
 * @pubkey_hex: Pubkey of user being reported (hex)
 *
 * Set the report target. At minimum pubkey_hex must be provided.
 * If event_id_hex is provided, both the event and user are reported.
 */
void gnostr_report_dialog_set_target(GnostrReportDialog *self,
                                      const gchar *event_id_hex,
                                      const gchar *pubkey_hex);

/**
 * gnostr_report_type_to_string:
 * @type: Report type enum
 *
 * Get the NIP-56 string representation of a report type.
 *
 * Returns: String like "nudity", "spam", etc.
 */
const gchar *gnostr_report_type_to_string(GnostrReportType type);

G_END_DECLS

#endif /* GNOSTR_REPORT_DIALOG_H */
