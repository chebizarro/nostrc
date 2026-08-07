#ifndef GNOSTR_BUG_REPORT_DIALOG_H
#define GNOSTR_BUG_REPORT_DIALOG_H

#include "gnostr-main-window.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_BUG_REPORT_DIALOG (gnostr_bug_report_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrBugReportDialog, gnostr_bug_report_dialog,
                     GNOSTR, BUG_REPORT_DIALOG, AdwDialog)

GnostrBugReportDialog *gnostr_bug_report_dialog_new(GnostrMainWindow *parent);
void gnostr_bug_report_dialog_present(GnostrBugReportDialog *self,
                                      GtkWidget *parent);

/**
 * Discover gnostr macOS diagnostic reports, newest first.
 *
 * If @directory is NULL, the platform default is used. Linux currently
 * returns an empty array for the default. Passing a directory explicitly is
 * supported on every platform for tests.
 *
 * Returns: (transfer full) (element-type utf8): paths owned by the array.
 */
GPtrArray *gnostr_bug_report_discover_crash_logs(const char *directory);

G_END_DECLS

#endif /* GNOSTR_BUG_REPORT_DIALOG_H */
