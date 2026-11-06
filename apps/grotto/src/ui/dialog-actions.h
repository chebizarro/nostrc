#pragma once

#include <adwaita.h>
#include <glib/gi18n.h>

/* Action sheets share one dismissal and affirmative placement. */
static inline GtkWidget *grotto_dialog_header(AdwDialog *dialog, GtkWidget *affirmative) {
  GtkWidget *header = adw_header_bar_new();
  adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(header), FALSE);
  adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(header), FALSE);
  GtkWidget *cancel = gtk_button_new_with_mnemonic(_("_Cancel"));
  g_signal_connect_swapped(cancel, "clicked", G_CALLBACK(adw_dialog_close), dialog);
  adw_header_bar_pack_start(ADW_HEADER_BAR(header), cancel);
  gtk_widget_add_css_class(affirmative, "suggested-action");
  adw_header_bar_pack_end(ADW_HEADER_BAR(header), affirmative);
  adw_dialog_set_default_widget(dialog, affirmative);
  return header;
}
