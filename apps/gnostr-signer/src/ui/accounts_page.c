#include <gtk/gtk.h>

static void on_toggle_visibility(GtkButton *btn, gpointer user_data) {
  GtkEntry *entry = GTK_ENTRY(user_data);
  gboolean vis = gtk_entry_get_visibility(entry);
  gtk_entry_set_visibility(entry, !vis);

  /* Update accessibility for screen readers (nostrc-qfdg) */
  const char *label = vis ? "Show key" : "Hide key";
  const char *desc = vis ? "Key is now hidden. Click to reveal." : "Key is now visible. Click to hide.";
  gtk_accessible_update_property(GTK_ACCESSIBLE(btn),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, desc,
                                 -1);
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry),
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                 vis ? "Key input is hidden" : "Key input is visible",
                                 -1);
}

static void on_add_identity(GtkButton *btn, gpointer user_data) {
  (void)btn; GtkWindow *win = GTK_WINDOW(user_data);
  GtkAlertDialog *dlg = gtk_alert_dialog_new("Add identity not implemented here. Use Settings → Import Key.");
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

  /* Set accessibility for the container (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(box),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Add new identity page",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Enter a private key or public identity to add a new account",
                                 -1);

  GtkWidget *title = gtk_label_new("Add New Identity");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "nsec1... (private key) or npub1... (public identity)");
  gtk_widget_set_focusable(entry, TRUE);
  gtk_widget_set_hexpand(entry, TRUE);
  gtk_box_append(GTK_BOX(row), entry);

  /* Set accessibility for key entry (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(entry),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Nostr key entry",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Enter an nsec1 private key or npub1 public identity",
                                 -1);

  GtkWidget *eye = gtk_button_new_from_icon_name("view-reveal-symbolic");
  gtk_widget_set_focusable(eye, TRUE);
  g_signal_connect(eye, "clicked", G_CALLBACK(on_toggle_visibility), entry);
  gtk_box_append(GTK_BOX(row), eye);

  /* Set accessibility for visibility toggle button (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(eye),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Show key",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Toggle visibility of the entered key",
                                 -1);

  gtk_box_append(GTK_BOX(box), row);

  GtkWidget *add = gtk_button_new_with_label("Add Identity");
  gtk_widget_set_focusable(add, TRUE);
  g_signal_connect(add, "clicked", G_CALLBACK(on_add_identity), parent);
  gtk_box_append(GTK_BOX(box), add);

  /* Set accessibility for add button (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(add),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Add identity",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Add the entered key as a new identity",
                                 -1);

  GtkWidget *gen = gtk_link_button_new_with_label("", "Generate a new key");
  gtk_widget_set_focusable(gen, TRUE);
  g_signal_connect(gen, "activate-link", G_CALLBACK(on_generate), parent);
  gtk_box_append(GTK_BOX(box), gen);

  /* Set accessibility for generate link (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(gen),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Generate new key",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Create a new random Nostr private key",
                                 -1);

  return box;
}

GtkWidget *gnostr_accounts_page_new(GtkWindow *parent);
