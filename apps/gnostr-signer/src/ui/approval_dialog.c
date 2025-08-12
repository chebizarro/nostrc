#include <gtk/gtk.h>
#include "../accounts_store.h"

// Callback signature for decision results
// decision: TRUE=Accept, FALSE=Reject; remember: TRUE to persist policy; selected_identity may be NULL; ttl_seconds: 0=Forever or no TTL
typedef void (*GnostrApprovalCallback)(gboolean decision, gboolean remember, const char *selected_identity, guint64 ttl_seconds, gpointer user_data);

typedef struct {
  GnostrApprovalCallback cb;
  gpointer user_data;
  GtkCheckButton *always;
  GtkWindow *win;
  GtkDropDown *ident_dropdown;
  GtkStringList *ident_model; /* model backing the dropdown */
  GtkDropDown *ttl_dropdown;
} ApprovalData;

static void on_always_toggled(GtkCheckButton *btn, gpointer user_data) {
  GtkWidget *ttl_dd = GTK_WIDGET(user_data);
  gboolean active = gtk_check_button_get_active(btn);
  gtk_widget_set_sensitive(ttl_dd, active);
}

static void do_finish(ApprovalData *data, gboolean decision) {
  gboolean remember = FALSE;
  if (data->always) remember = gtk_check_button_get_active(data->always);
  guint64 ttl_seconds = 0; /* 0 = Forever */
  if (remember && data->ttl_dropdown) {
    guint idx = gtk_drop_down_get_selected(data->ttl_dropdown);
    switch ((int)idx) {
      case 0: ttl_seconds = 600; break;   /* 10m */
      case 1: ttl_seconds = 3600; break;  /* 1h */
      case 2: ttl_seconds = 86400; break; /* 24h */
      case 3: default: ttl_seconds = 0; break; /* Forever */
    }
  }
  const char *selected = NULL;
  if (data->ident_dropdown && data->ident_model) {
    guint idx = gtk_drop_down_get_selected(data->ident_dropdown);
    guint n = g_list_model_get_n_items(G_LIST_MODEL(data->ident_model));
    if (n > 0) {
      if ((int)idx < 0 || idx >= n) idx = 0; /* fallback to first */
      selected = gtk_string_list_get_string(data->ident_model, idx);
    }
  }
  if (data->cb) data->cb(decision, remember, selected, ttl_seconds, data->user_data);
  if (data->win) gtk_window_destroy(data->win);
  g_free(data);
}

static gboolean on_close_request(GtkWindow *win, gpointer user_data) {
  (void)win;
  /* Treat window close as Reject to ensure callbacks and cleanup happen */
  do_finish((ApprovalData*)user_data, FALSE);
  return TRUE; /* we handled it */
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
void gnostr_show_approval_dialog(GtkWindow *parent, const char *identity_npub,
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
  if (identity_npub && *identity_npub)
    g_string_append_printf(detail, "Identity: %s\n", identity_npub);
  if (app_name && *app_name)
    g_string_append_printf(detail, "App: %s\n", app_name);
  if (preview && *preview)
    g_string_append_printf(detail, "Preview: %s", preview);
  GtkWidget *detail_lbl = gtk_label_new(detail->len ? detail->str : "");
  gtk_label_set_wrap(GTK_LABEL(detail_lbl), TRUE);
  g_string_free(detail, TRUE);
  gtk_box_append(GTK_BOX(root), detail_lbl);

  // Identity selector (optional)
  GtkWidget *ident_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *ident_lbl = gtk_label_new("Identity:");
  gtk_widget_set_halign(ident_lbl, GTK_ALIGN_START);
  GtkStringList *ident_model = gtk_string_list_new(NULL);
  guint selected_idx = (guint)-1;
  if (as) {
    GPtrArray *items = accounts_store_list(as);
    if (items) {
      for (guint i = 0; i < items->len; i++) {
        AccountEntry *e = g_ptr_array_index(items, i);
        gtk_string_list_append(ident_model, e->id);
        if (selected_idx == (guint)-1 && identity_npub && g_strcmp0(identity_npub, e->id) == 0) {
          selected_idx = i;
        }
        g_free(e->id); g_free(e->label); g_free(e);
      }
      g_ptr_array_free(items, TRUE);
    }
  }
  // If no accounts, but an identity was provided, include it so selection isn't empty
  if (g_list_model_get_n_items(G_LIST_MODEL(ident_model)) == 0 && identity_npub && *identity_npub) {
    gtk_string_list_append(ident_model, identity_npub);
    selected_idx = 0;
  }
  GtkWidget *ident_dropdown = gtk_drop_down_new(G_LIST_MODEL(ident_model), NULL);
  if ((int)selected_idx < 0) selected_idx = 0;
  gtk_drop_down_set_selected(GTK_DROP_DOWN(ident_dropdown), selected_idx);
  gtk_box_append(GTK_BOX(ident_row), ident_lbl);
  gtk_box_append(GTK_BOX(ident_row), ident_dropdown);
  gtk_box_append(GTK_BOX(root), ident_row);

  // Always approve + TTL
  GtkWidget *ttl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *always = gtk_check_button_new_with_label("Always approve");
  gtk_box_append(GTK_BOX(ttl_row), always);
  GtkStringList *ttl_model = gtk_string_list_new(NULL);
  gtk_string_list_append(ttl_model, "10 minutes");
  gtk_string_list_append(ttl_model, "1 hour");
  gtk_string_list_append(ttl_model, "24 hours");
  gtk_string_list_append(ttl_model, "Forever");
  GtkWidget *ttl_dd = gtk_drop_down_new(G_LIST_MODEL(ttl_model), NULL);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(ttl_dd), 0); /* default 10m */
  gtk_widget_set_sensitive(ttl_dd, FALSE);
  // toggle enable when 'always' checked
  g_signal_connect(always, "toggled", G_CALLBACK(on_always_toggled), ttl_dd);
  gtk_box_append(GTK_BOX(ttl_row), ttl_dd);
  gtk_box_append(GTK_BOX(root), ttl_row);

  // Buttons
  GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btns, GTK_ALIGN_END);
  GtkWidget *accept = gtk_button_new_with_label("Accept");
  GtkWidget *reject = gtk_button_new_with_label("Reject");
  gtk_box_append(GTK_BOX(btns), reject);
  gtk_box_append(GTK_BOX(btns), accept);
  gtk_box_append(GTK_BOX(root), btns);
  // Disable Accept if we have no identity options at all
  if (g_list_model_get_n_items(G_LIST_MODEL(ident_model)) == 0) {
    gtk_widget_set_sensitive(accept, FALSE);
  }

  ApprovalData *data = g_new0(ApprovalData, 1);
  data->cb = cb;
  data->user_data = user_data;
  data->always = GTK_CHECK_BUTTON(always);
  data->win = win;
  data->ident_dropdown = GTK_DROP_DOWN(ident_dropdown);
  data->ident_model = ident_model;
  data->ttl_dropdown = GTK_DROP_DOWN(ttl_dd);

  g_signal_connect(accept, "clicked", G_CALLBACK(on_accept_clicked), data);
  g_signal_connect(reject, "clicked", G_CALLBACK(on_reject_clicked), data);
  g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), data);

  gtk_window_present(win);
}

// header for other compilation units
void gnostr_show_approval_dialog(GtkWindow *parent, const char *identity_npub,
                                 const char *app_name, const char *preview,
                                 AccountsStore *as,
                                 GnostrApprovalCallback cb, gpointer user_data);
