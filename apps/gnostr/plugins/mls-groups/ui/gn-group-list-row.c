/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-group-list-row.c - Group list row widget
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-group-list-row.h"

struct _GnGroupListRow
{
  GtkBox parent_instance;

  /* Widgets */
  GtkImage *icon;
  GtkLabel *name_label;
  GtkLabel *detail_label;
  GtkLabel *badge_label;

  /* Bound data */
  MarmotGobjectGroup *group;   /* strong ref, nullable */
};

G_DEFINE_TYPE(GnGroupListRow, gn_group_list_row, GTK_TYPE_BOX)

static void
gn_group_list_row_dispose(GObject *object)
{
  GnGroupListRow *self = GN_GROUP_LIST_ROW(object);
  g_clear_object(&self->group);
  G_OBJECT_CLASS(gn_group_list_row_parent_class)->dispose(object);
}

static void
gn_group_list_row_class_init(GnGroupListRowClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose = gn_group_list_row_dispose;
}

static void
gn_group_list_row_init(GnGroupListRow *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 12);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 8);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 8);

  /* Group icon */
  self->icon = GTK_IMAGE(gtk_image_new_from_icon_name("system-users-symbolic"));
  gtk_image_set_pixel_size(self->icon, 32);
  gtk_widget_set_valign(GTK_WIDGET(self->icon), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->icon));

  /* Text box (name + detail) */
  GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

  self->name_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->name_label, 0);
  gtk_label_set_ellipsize(self->name_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->name_label), "heading");
  gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(self->name_label));

  self->detail_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->detail_label, 0);
  gtk_label_set_ellipsize(self->detail_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->detail_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->detail_label), "caption");
  gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(self->detail_label));

  gtk_box_append(GTK_BOX(self), text_box);

  /* Unread badge (hidden by default) */
  self->badge_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_set_valign(GTK_WIDGET(self->badge_label), GTK_ALIGN_CENTER);
  gtk_widget_add_css_class(GTK_WIDGET(self->badge_label), "badge");
  gtk_widget_set_visible(GTK_WIDGET(self->badge_label), FALSE);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->badge_label));
}

/* ── Public API ──────────────────────────────────────────────────── */

GnGroupListRow *
gn_group_list_row_new(void)
{
  return g_object_new(GN_TYPE_GROUP_LIST_ROW, NULL);
}

void
gn_group_list_row_bind(GnGroupListRow     *self,
                        MarmotGobjectGroup *group)
{
  g_return_if_fail(GN_IS_GROUP_LIST_ROW(self));
  g_return_if_fail(MARMOT_GOBJECT_IS_GROUP(group));

  g_set_object(&self->group, group);

  const gchar *name = marmot_gobject_group_get_name(group);
  const gchar *desc = marmot_gobject_group_get_description(group);
  guint epoch = marmot_gobject_group_get_epoch(group);

  gtk_label_set_text(self->name_label,
                     (name && *name) ? name : "Unnamed Group");

  if (desc && *desc)
    {
      gtk_label_set_text(self->detail_label, desc);
    }
  else
    {
      g_autofree gchar *detail = g_strdup_printf("Epoch %u", epoch);
      gtk_label_set_text(self->detail_label, detail);
    }

  /* TODO: Show unread badge based on local read state */
}

void
gn_group_list_row_unbind(GnGroupListRow *self)
{
  g_return_if_fail(GN_IS_GROUP_LIST_ROW(self));

  g_clear_object(&self->group);
  gtk_label_set_text(self->name_label, NULL);
  gtk_label_set_text(self->detail_label, NULL);
  gtk_widget_set_visible(GTK_WIDGET(self->badge_label), FALSE);
}
