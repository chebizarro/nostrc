#include <gtk/gtk.h>

GtkWidget *gnostr_permissions_page_new(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  GtkWidget *title = gtk_label_new("Permissions");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *reset = gtk_button_new_with_label("Reset permissions");
  gtk_box_append(GTK_BOX(box), reset);

  GtkWidget *list = gtk_list_box_new();
  gtk_box_append(GTK_BOX(box), list);

  GtkWidget *row = gtk_label_new("com.example.app.demo");
  gtk_list_box_append(GTK_LIST_BOX(list), row);

  return box;
}

GtkWidget *gnostr_permissions_page_new(void);
