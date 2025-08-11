#include <gtk/gtk.h>

GtkWidget *gnostr_settings_page_new(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  GtkWidget *title = gtk_label_new("Settings");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *desc = gtk_label_new("Account management, key display, and advanced options will appear here.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_box_append(GTK_BOX(box), desc);

  return box;
}

GtkWidget *gnostr_settings_page_new(void);
