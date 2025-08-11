#include <gtk/gtk.h>

static void on_toggle_visibility(GtkButton *btn, gpointer user_data) {
  GtkEntry *entry = GTK_ENTRY(user_data);
  gboolean vis = gtk_entry_get_visibility(entry);
  gtk_entry_set_visibility(entry, !vis);
}

static void on_login(GtkButton *btn, gpointer user_data) {
  (void)btn; GtkWindow *win = GTK_WINDOW(user_data);
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Login not implemented yet (nsec or npub)");
  gtk_alert_dialog_show(dlg, win);
  g_object_unref(dlg);
}

static void on_generate(GtkButton *btn, gpointer user_data) {
  (void)btn; GtkWindow *win = GTK_WINDOW(user_data);
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Key generation not implemented yet");
  gtk_alert_dialog_show(dlg, win);
  g_object_unref(dlg);
}

GtkWidget *gnostr_accounts_page_new(GtkWindow *parent) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  GtkWidget *title = gtk_label_new("Add New Account");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nsec1... or npub1...");
  gtk_box_append(GTK_BOX(row), entry);
  GtkWidget *eye = gtk_button_new_from_icon_name("view-reveal-symbolic");
  g_signal_connect(eye, "clicked", G_CALLBACK(on_toggle_visibility), entry);
  gtk_box_append(GTK_BOX(row), eye);
  gtk_box_append(GTK_BOX(box), row);

  GtkWidget *login = gtk_button_new_with_label("Login");
  g_signal_connect(login, "clicked", G_CALLBACK(on_login), parent);
  gtk_box_append(GTK_BOX(box), login);

  GtkWidget *gen = gtk_link_button_new_with_label("", "Generate a new key");
  g_signal_connect(gen, "activate-link", G_CALLBACK(on_generate), parent);
  gtk_box_append(GTK_BOX(box), gen);

  return box;
}

GtkWidget *gnostr_accounts_page_new(GtkWindow *parent);
