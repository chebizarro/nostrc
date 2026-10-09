#include "gh-update-handoff.h"
#include <glib/gi18n.h>
#include <gio/gio.h>

#ifndef GH_PACKAGE_CHANNEL
#define GH_PACKAGE_CHANNEL "source"
#endif

const gchar *gh_update_handoff_package_channel(void)
{
  return GH_PACKAGE_CHANNEL;
}

const gchar *gh_update_handoff_instructions(const gchar *channel)
{
  if (g_str_equal(channel, "flatpak"))
    return _("Groundhog is updated through Flatpak. Open your software center or run flatpak update.");
  if (g_str_equal(channel, "deb"))
    return _("Groundhog is updated by your Debian package manager. Use your software center or apt to install updates.");
  if (g_str_equal(channel, "rpm"))
    return _("Groundhog is updated by your RPM package manager. Use your software center or dnf to install updates.");
  if (g_str_equal(channel, "arch"))
    return _("Groundhog is updated by your Arch package manager. Use your software center or pacman to install updates.");
  if (g_str_equal(channel, "nix"))
    return _("Groundhog is updated by your Nix configuration or profile. Update it through the same source that installed it.");
  return _("This build does not identify an update channel. Obtain a newer build from your original distributor.");
}

static void on_open_done(GObject *source, GAsyncResult *result, gpointer data)
{
  AdwDialog *dialog = ADW_DIALOG(data);
  g_autoptr(GError) error = NULL;
  (void)source;
  gboolean opened = g_app_info_launch_default_for_uri_finish(result, &error);
  if (gtk_widget_get_root(GTK_WIDGET(dialog))) {
    if (!opened) {
      GtkLabel *label = g_object_get_data(G_OBJECT(dialog), "update-error-label");
      gtk_label_set_text(label, _("Software could not be opened. Use the instructions above."));
      gtk_widget_set_visible(GTK_WIDGET(label), TRUE);
    } else {
      adw_dialog_close(dialog);
    }
  }
  g_object_unref(dialog);
}

static void on_open_clicked(GtkButton *button, AdwDialog *dialog)
{
  (void)button;
  g_app_info_launch_default_for_uri_async("appstream:org.nostr.Groundhog", NULL, NULL,
                                           on_open_done, g_object_ref(dialog));
}

static void on_close_clicked(GtkButton *button, AdwDialog *dialog)
{
  (void)button;
  adw_dialog_close(dialog);
}

void gh_update_handoff_present(GtkWindow *parent)
{
  g_return_if_fail(GTK_IS_WINDOW(parent));
  const gchar *channel = gh_update_handoff_package_channel();
  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Update Groundhog"));
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  GtkWidget *message = gtk_label_new(gh_update_handoff_instructions(channel));
  gtk_label_set_wrap(GTK_LABEL(message), TRUE);
  gtk_label_set_xalign(GTK_LABEL(message), 0);
  gtk_box_append(GTK_BOX(box), message);
  GtkWidget *error = gtk_label_new(NULL);
  gtk_label_set_wrap(GTK_LABEL(error), TRUE);
  gtk_label_set_xalign(GTK_LABEL(error), 0);
  gtk_widget_set_visible(error, FALSE);
  gtk_box_append(GTK_BOX(box), error);
  g_object_set_data(G_OBJECT(dialog), "update-error-label", error);
  GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_halign(buttons, GTK_ALIGN_END);
  g_autoptr(GAppInfo) handler = g_app_info_get_default_for_uri_scheme("appstream");
  if (handler && !g_str_equal(channel, "source") && !g_str_equal(channel, "unknown")) {
    GtkWidget *open = gtk_button_new_with_mnemonic(_("_Open Software"));
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_clicked), dialog);
    gtk_box_append(GTK_BOX(buttons), open);
  }
  GtkWidget *close = gtk_button_new_with_mnemonic(_("_Close"));
  g_signal_connect(close, "clicked", G_CALLBACK(on_close_clicked), dialog);
  gtk_box_append(GTK_BOX(buttons), close);
  gtk_box_append(GTK_BOX(box), buttons);
  adw_dialog_set_child(dialog, box);
  adw_dialog_present(dialog, GTK_WIDGET(parent));
}
