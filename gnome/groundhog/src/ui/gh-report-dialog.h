#ifndef GH_REPORT_DIALOG_H
#define GH_REPORT_DIALOG_H
#include <adwaita.h>
#include "gh-report-service.h"
#include "gh-message.h"
G_BEGIN_DECLS
#define GH_TYPE_REPORT_DIALOG (gh_report_dialog_get_type())
G_DECLARE_FINAL_TYPE(GhReportDialog, gh_report_dialog, GH, REPORT_DIALOG, AdwDialog)
typedef gboolean (*GhReportPublicRoomFunc)(gpointer data, const gchar *room_id);
/* Account action carries only (account,target). Public action is offered only
 * after the live room predicate confirms received public metadata, and is
 * checked again on activation and submission. */
void gh_report_dialog_install(GtkApplication *app, GhReportService *service, GhSocialLists *lists,
                              GhReportPublicRoomFunc public_room, gpointer data);
void gh_report_dialog_uninstall(GtkApplication *app);
gboolean gh_report_dialog_message_is_public(GtkWidget *widget, GhMessage *message);
GhReportDialog *gh_report_dialog_new(GhReportService *service, GhSocialLists *lists,
    const gchar *target, const gchar *public_json, const gchar *room_id,
    GhReportPublicRoomFunc public_room, gpointer data);
G_END_DECLS
#endif
