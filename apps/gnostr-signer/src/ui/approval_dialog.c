#include <gtk/gtk.h>
#include "../accounts_store.h"

// Callback signature for decision results
// decision: TRUE=Accept, FALSE=Reject; remember: TRUE to persist policy; selected_account may be NULL
typedef void (*GnostrApprovalCallback)(gboolean decision, gboolean remember, const char *selected_account, gpointer user_data);

typedef struct {
  GnostrApprovalCallback cb;
  gpointer user_data;
  GtkCheckButton *remember;
  GtkWindow *win;
  GtkComboBoxText *acct_combo;
} ApprovalData;

static void do_finish(ApprovalData *data, gboolean decision) {
  gboolean remember = FALSE;
  if (data->remember) remember = gtk_check_button_get_active(data->remember);
  const char *selected = NULL;
  if (data->acct_combo) selected = gtk_combo_box_text_get_active_text(data->acct_combo);
  if (data->cb) data->cb(decision, remember, selected, data->user_data);
  if (data->win) gtk_window_destroy(data->win);
  g_free(data);
}

static void on_accept_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  do_finish((ApprovalData*)user_data, TRUE);
}

static void on_reject_clicked(GtkButton *btn, gpointer user_data) {
  (void)btn;
  do_finish((ApprovalData*)user_data, FALSE);
}

// Non-blocking helper to present approval UI.
// app_name: requester, preview: short event content/summary
void gnostr_show_approval_dialog(GtkWindow *parent, const char *account_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as,
                                 GnostrApprovalCallback cb, gpointer user_data) {
  GtkWindow *win = GTK_WINDOW(gtk_window_new());
  gtk_window_set_transient_for(win, parent);
  gtk_window_set_modal(win, TRUE);
  gtk_window_set_title(win, "Approval Required");
  gtk_window_set_default_size(win, 420, 200);

  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(root, 16);
  gtk_widget_set_margin_bottom(root, 16);
  gtk_widget_set_margin_start(root, 16);
  gtk_widget_set_margin_end(root, 16);
  gtk_window_set_child(win, root);

  gchar *title = g_strdup_printf("%s requests event signature", app_name ? app_name : "App");
  GtkWidget *title_lbl = gtk_label_new(title);
  g_free(title);
  gtk_widget_add_css_class(title_lbl, "title-2");
  gtk_box_append(GTK_BOX(root), title_lbl);

  // Summary
  GString *detail = g_string_new(NULL);
  if (account_npub && *account_npub)
    g_string_append_printf(detail, "Account: %s\n", account_npub);
  if (app_name && *app_name)
    g_string_append_printf(detail, "App: %s\n", app_name);
  if (preview && *preview)
    g_string_append_printf(detail, "Preview: %s", preview);
  GtkWidget *detail_lbl = gtk_label_new(detail->len ? detail->str : "");
  gtk_label_set_wrap(GTK_LABEL(detail_lbl), TRUE);
  g_string_free(detail, TRUE);
  gtk_box_append(GTK_BOX(root), detail_lbl);

  // Account selector (optional)
  GtkWidget *acct_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *acct_lbl = gtk_label_new("Account:");
  gtk_widget_set_halign(acct_lbl, GTK_ALIGN_START);
  GtkWidget *acct_combo = gtk_combo_box_text_new();
  if (as) {
    GPtrArray *items = accounts_store_list(as);
    if (items) {
      for (guint i = 0; i < items->len; i++) {
        AccountEntry *e = g_ptr_array_index(items, i);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(acct_combo), e->id);
        if (account_npub && g_strcmp0(account_npub, e->id) == 0) {
          gtk_combo_box_set_active(GTK_COMBO_BOX(acct_combo), i);
        }
        g_free(e->id); g_free(e->label); g_free(e);
      }
      g_ptr_array_free(items, TRUE);
    }
  }
  if (gtk_combo_box_get_active(GTK_COMBO_BOX(acct_combo)) < 0) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(acct_combo), 0);
  }
  gtk_box_append(GTK_BOX(acct_row), acct_lbl);
  gtk_box_append(GTK_BOX(acct_row), acct_combo);
  gtk_box_append(GTK_BOX(root), acct_row);

  // Remember
  GtkWidget *remember = gtk_check_button_new_with_label("Remember this decision");
  gtk_box_append(GTK_BOX(root), remember);

  // Buttons
  GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btns, GTK_ALIGN_END);
  GtkWidget *accept = gtk_button_new_with_label("Accept");
  GtkWidget *reject = gtk_button_new_with_label("Reject");
  gtk_box_append(GTK_BOX(btns), reject);
  gtk_box_append(GTK_BOX(btns), accept);
  gtk_box_append(GTK_BOX(root), btns);

  ApprovalData *data = g_new0(ApprovalData, 1);
  data->cb = cb;
  data->user_data = user_data;
  data->remember = GTK_CHECK_BUTTON(remember);
  data->win = win;
  data->acct_combo = GTK_COMBO_BOX_TEXT(acct_combo);

  g_signal_connect(accept, "clicked", G_CALLBACK(on_accept_clicked), data);
  g_signal_connect(reject, "clicked", G_CALLBACK(on_reject_clicked), data);

  gtk_window_present(win);
}

// header for other compilation units
void gnostr_show_approval_dialog(GtkWindow *parent, const char *account_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as,
                                 GnostrApprovalCallback cb, gpointer user_data);
