#include <gtk/gtk.h>

GtkWidget *gnostr_home_page_new(void) {
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 16);
  gtk_widget_set_margin_bottom(box, 16);
  gtk_widget_set_margin_start(box, 16);
  gtk_widget_set_margin_end(box, 16);

  /* Set accessibility for the home page container (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(box),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, "Home page",
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Main dashboard showing pending signing requests and status",
                                 -1);

  GtkWidget *title = gtk_label_new("Home");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *desc = gtk_label_new("Pending requests will appear here. Approvals UI will be shown as dialogs.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_box_append(GTK_BOX(box), desc);

  /* Set accessibility for the description (nostrc-qfdg) */
  gtk_accessible_update_property(GTK_ACCESSIBLE(desc),
                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION, "Information about how signing requests are handled",
                                 -1);

  return box;
}

/* public header would normally be in a .h; to keep build simple, declare here */
GtkWidget *gnostr_home_page_new(void);
