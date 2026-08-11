#include "gn-concord-community-row.h"

#include <string.h>

struct _GnConcordCommunityRow {
  GtkBox parent_instance;
  GnConcordCommunityItem *item;
  GtkLabel *title;
  GtkLabel *detail;
};

G_DEFINE_TYPE(GnConcordCommunityRow, gn_concord_community_row, GTK_TYPE_BOX)

static void gn_concord_community_row_dispose(GObject *object) {
  GnConcordCommunityRow *self = GN_CONCORD_COMMUNITY_ROW(object);
  g_clear_object(&self->item);
  G_OBJECT_CLASS(gn_concord_community_row_parent_class)->dispose(object);
}

static void gn_concord_community_row_class_init(
    GnConcordCommunityRowClass *klass) {
  G_OBJECT_CLASS(klass)->dispose = gn_concord_community_row_dispose;
}

static void gn_concord_community_row_init(GnConcordCommunityRow *self) {
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
  self->detail = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->detail, 0);
  gtk_widget_add_css_class(GTK_WIDGET(self->detail), "dim-label");
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->title));
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->detail));
}

GnConcordCommunityRow *gn_concord_community_row_new(void) {
  return g_object_new(GN_TYPE_CONCORD_COMMUNITY_ROW, NULL);
}

void gn_concord_community_row_bind(GnConcordCommunityRow *self,
                                   GnConcordCommunityItem *item) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_ROW(self));
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_ITEM(item));
  g_set_object(&self->item, item);

  const char *name = gn_concord_community_item_get_name(item);
  const char *community_id =
    gn_concord_community_item_get_community_id(item);
  /* The name is a join-time preview from the bundle; the Control Plane fold
   * is always the authority (CORD-02 §6). Fall back to the id, which is the
   * only identity that self-certifies. */
  g_autofree gchar *fallback = community_id && strlen(community_id) > 16
    ? g_strdup_printf("%.16s…", community_id) : g_strdup(community_id);
  gtk_label_set_text(self->title, name && *name ? name : fallback);

  guint channels = gn_concord_community_item_get_channel_count(item);
  g_autofree gchar *detail = g_strdup_printf(
    "%u channel%s · epoch %" G_GUINT64_FORMAT "%s", channels,
    channels == 1 ? "" : "s",
    gn_concord_community_item_get_root_epoch(item),
    gn_concord_community_item_get_has_control_pk(item)
      ? "" : " · legacy control plane");
  gtk_label_set_text(self->detail, detail);
}

void gn_concord_community_row_unbind(GnConcordCommunityRow *self) {
  g_return_if_fail(GN_IS_CONCORD_COMMUNITY_ROW(self));
  g_clear_object(&self->item);
  gtk_label_set_text(self->title, NULL);
  gtk_label_set_text(self->detail, NULL);
}
