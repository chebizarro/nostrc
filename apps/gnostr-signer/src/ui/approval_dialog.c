#include <gtk/gtk.h>

// Callback signature for decision results
// decision: TRUE=Accept, FALSE=Reject; remember: reserved (FALSE for now)
typedef void (*GnostrApprovalCallback)(gboolean decision, gboolean remember, gpointer user_data);

typedef struct {
  GnostrApprovalCallback cb;
  gpointer user_data;
} ApprovalData;

static void on_response_cb(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  ApprovalData *data = (ApprovalData *)user_data;
  GtkAlertDialog *dlg = GTK_ALERT_DIALOG(source);
  GError *error = NULL;
  int resp = gtk_alert_dialog_choose_finish(dlg, res, &error);
  if (error) {
    g_warning("Alert dialog failed: %s", error->message);
    g_clear_error(&error);
  }
  gboolean remember = FALSE; /* TODO: add a persistent remember UI later */
  gboolean decision = (resp == 0); // 0: Accept, 1: Reject
  if (data->cb) data->cb(decision, remember, data->user_data);
  g_free(data);
}

// Non-blocking helper to present approval UI.
// app_name: requester, preview: short event content/summary
void gnostr_show_approval_dialog(GtkWindow *parent, const char *account_npub,
                                 const char *app_name, const char *preview,
                                 GnostrApprovalCallback cb, gpointer user_data) {
  GtkAlertDialog *dlg = gtk_alert_dialog_new("%s requests event signature", app_name ? app_name : "App");
  gtk_alert_dialog_set_buttons(dlg, (const char * const[]){"Accept", "Reject", NULL});
  
  // Detail text summarizing account/app/preview (simple for now)
  GString *detail = g_string_new(NULL);
  if (account_npub && *account_npub)
    g_string_append_printf(detail, "Account: %s\n", account_npub);
  if (app_name && *app_name)
    g_string_append_printf(detail, "App: %s\n", app_name);
  if (preview && *preview)
    g_string_append_printf(detail, "Preview: %s", preview);
  if (detail->len > 0)
    gtk_alert_dialog_set_detail(dlg, detail->str);
  g_string_free(detail, TRUE);

  ApprovalData *data = g_new0(ApprovalData, 1);
  data->cb = cb;
  data->user_data = user_data;

  gtk_alert_dialog_choose(dlg, parent, NULL, on_response_cb, data);
}

// header for other compilation units
void gnostr_show_approval_dialog(GtkWindow *parent, const char *account_npub,
                                 const char *app_name, const char *preview,
                                 GnostrApprovalCallback cb, gpointer user_data);
