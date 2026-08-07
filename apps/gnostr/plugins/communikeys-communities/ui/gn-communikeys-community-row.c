#include "gn-communikeys-community-row.h"
#include <string.h>
struct _GnCommunikeysCommunityRow {
  GtkBox parent_instance;
  GnCommunikeysCommunityItem *item;
  GtkLabel *title;
  GtkLabel *detail;
  GtkLabel *description;
};
G_DEFINE_TYPE(GnCommunikeysCommunityRow, gn_communikeys_community_row,
              GTK_TYPE_BOX)
static void gn_communikeys_community_row_dispose(GObject *object) {
  GnCommunikeysCommunityRow *self = GN_COMMUNIKEYS_COMMUNITY_ROW(object);
  g_clear_object(&self->item);
  G_OBJECT_CLASS(gn_communikeys_community_row_parent_class)->dispose(object);
}
static void gn_communikeys_community_row_class_init(
    GnCommunikeysCommunityRowClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = gn_communikeys_community_row_dispose;
}
static void gn_communikeys_community_row_init(
    GnCommunikeysCommunityRow *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self),
                                 GTK_ORIENTATION_VERTICAL);
  gtk_box_set_spacing(GTK_BOX(self), 3);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 10);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 10);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  self->title = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->title, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->title), "heading");
  self->description = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->description, 0);
  gtk_label_set_wrap(self->description, TRUE);
  gtk_label_set_ellipsize(self->description, PANGO_ELLIPSIZE_END);
  gtk_label_set_lines(self->description, 2);
  self->detail = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->detail, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->detail), "dim-label");
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->title));
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->description));
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->detail));
}
GnCommunikeysCommunityRow *gn_communikeys_community_row_new(void) {
  return g_object_new(GN_TYPE_COMMUNIKEYS_COMMUNITY_ROW, NULL);
}
void gn_communikeys_community_row_bind(
    GnCommunikeysCommunityRow *self, GnCommunikeysCommunityItem *item) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_ROW(self));
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_ITEM(item));
  g_set_object(&self->item, item);
  const char *pubkey = gn_communikeys_community_item_get_pubkey(item);
  g_autofree gchar *title = pubkey && strlen(pubkey) > 16
    ? g_strdup_printf("%.16s…", pubkey) : g_strdup(pubkey);
  gtk_label_set_text(self->title, title);
  gtk_label_set_text(self->description,
    gn_communikeys_community_item_get_description(item));
  gtk_widget_set_visible(GTK_WIDGET(self->description),
    gn_communikeys_community_item_get_description(item) != NULL);
  g_autofree gchar *detail = g_strdup_printf(
    "%u sections · %s",
    gn_communikeys_community_item_get_section_count(item),
    gn_communikeys_community_item_get_main_relay(item)
      ? gn_communikeys_community_item_get_main_relay(item)
      : "relay unavailable");
  gtk_label_set_text(self->detail, detail);
}
void gn_communikeys_community_row_unbind(GnCommunikeysCommunityRow *self) {
  g_return_if_fail(GN_IS_COMMUNIKEYS_COMMUNITY_ROW(self));
  g_clear_object(&self->item);
  gtk_label_set_text(self->title, NULL);
  gtk_label_set_text(self->description, NULL);
  gtk_label_set_text(self->detail, NULL);
}
