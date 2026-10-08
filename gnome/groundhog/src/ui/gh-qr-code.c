#include "gh-qr-code.h"

#include <glib/gi18n.h>
#include <qrencode.h>
#include <string.h>

GdkTexture *
gh_qr_code_texture_new(const gchar *text)
{
  g_return_val_if_fail(text != NULL, NULL);
  QRcode *qr = QRcode_encodeString(text, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
  if (!qr)
    return NULL;
  const int border = 4, scale = 4;
  const int size = (qr->width + border * 2) * scale;
  guchar *pixels = g_malloc((gsize)size * size * 3);
  memset(pixels, 255, (gsize)size * size * 3);
  for (int y = 0; y < qr->width; y++)
    for (int x = 0; x < qr->width; x++)
      if (qr->data[y * qr->width + x] & 1)
        for (int sy = 0; sy < scale; sy++)
          for (int sx = 0; sx < scale; sx++) {
            gsize offset = ((gsize)(y + border) * scale + sy) * size * 3 +
                           ((gsize)(x + border) * scale + sx) * 3;
            memset(pixels + offset, 0, 3);
          }
  QRcode_free(qr);
  g_autoptr(GBytes) bytes = g_bytes_new_take(pixels, (gsize)size * size * 3);
  return gdk_memory_texture_new(size, size, GDK_MEMORY_R8G8B8, bytes, size * 3);
}

void
gh_npub_qr_dialog_present(GtkWidget *parent, const gchar *npub)
{
  g_return_if_fail(GTK_IS_WIDGET(parent));
  g_return_if_fail(npub != NULL && g_str_has_prefix(npub, "npub1"));
  g_autofree gchar *uri = g_strconcat("nostr:", npub, NULL);
  g_autoptr(GdkTexture) texture = gh_qr_code_texture_new(uri);
  if (!texture)
    return;

  AdwDialog *dialog = adw_dialog_new();
  adw_dialog_set_title(dialog, _("Show QR Code"));
  adw_dialog_set_content_width(dialog, 360);
  adw_dialog_set_content_height(dialog, 360);
  AdwToolbarView *toolbar = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
  adw_toolbar_view_add_top_bar(toolbar, adw_header_bar_new());
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(texture));
  gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
  gtk_accessible_update_property(GTK_ACCESSIBLE(picture), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                 _("QR code for this npub"), -1);
  gtk_widget_set_size_request(picture, 240, 240);
  gtk_box_append(GTK_BOX(box), picture);
  GtkWidget *label = gtk_label_new(npub);
  gtk_label_set_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_selectable(GTK_LABEL(label), TRUE);
  gtk_widget_add_css_class(label, "monospace");
  gtk_box_append(GTK_BOX(box), label);
  adw_toolbar_view_set_content(toolbar, box);
  adw_dialog_set_child(dialog, GTK_WIDGET(toolbar));
  adw_dialog_present(dialog, parent);
}
