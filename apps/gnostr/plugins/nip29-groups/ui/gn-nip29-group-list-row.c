/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-list-row.c - Row widget for a NIP-29 group in the list
 */

#include "gn-nip29-group-list-row.h"

struct _GnNip29GroupListRow
{
  GtkBox parent_instance;

  GtkImage *icon;
  GtkLabel *name_label;
  GtkLabel *detail_label;
  GtkBox   *badge_box;

  GnNip29GroupItem *item;
};

G_DEFINE_TYPE(GnNip29GroupListRow, gn_nip29_group_list_row, GTK_TYPE_BOX)

static void
gn_nip29_group_list_row_dispose(GObject *object)
{
  GnNip29GroupListRow *self = GN_NIP29_GROUP_LIST_ROW(object);
  g_clear_object(&self->item);
  G_OBJECT_CLASS(gn_nip29_group_list_row_parent_class)->dispose(object);
}

static void
gn_nip29_group_list_row_class_init(GnNip29GroupListRowClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_group_list_row_dispose;
}

static void
gn_nip29_group_list_row_init(GnNip29GroupListRow *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 10);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 10);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 10);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);

  /* Group icon */
  self->icon = GTK_IMAGE(gtk_image_new_from_icon_name("system-users-symbolic"));
  gtk_image_set_pixel_size(self->icon, 32);
  gtk_widget_set_valign(GTK_WIDGET(self->icon), GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->icon));

  /* Vertical box with name + detail + badges */
  GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);

  self->name_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->name_label, 0);
  gtk_label_set_ellipsize(self->name_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->name_label), "heading");
  gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(self->name_label));

  /* Detail line: relay + member info */
  self->detail_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->detail_label, 0);
  gtk_label_set_ellipsize(self->detail_label, PANGO_ELLIPSIZE_END);
  gtk_widget_add_css_class(GTK_WIDGET(self->detail_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->detail_label), "caption");
  gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(self->detail_label));

  /* Metadata badge row */
  self->badge_box = GTK_BOX(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4));
  gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(self->badge_box));

  gtk_box_append(GTK_BOX(self), text_box);
}

/* ── Helpers ─────────────────────────────────────────────────────── */

static void
add_badge(GtkBox *box, const char *text, const char *css_class)
{
  GtkWidget *label = gtk_label_new(text);
  gtk_widget_add_css_class(label, "caption");
  if (css_class != NULL)
    gtk_widget_add_css_class(label, css_class);
  gtk_box_append(box, label);
}

static void
clear_badge_box(GtkBox *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
    gtk_box_remove(box, child);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnNip29GroupListRow *
gn_nip29_group_list_row_new(void)
{
  return g_object_new(GN_TYPE_NIP29_GROUP_LIST_ROW, NULL);
}

void
gn_nip29_group_list_row_bind(GnNip29GroupListRow *self,
                              GnNip29GroupItem    *item)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_LIST_ROW(self));
  g_return_if_fail(GN_IS_NIP29_GROUP_ITEM(item));

  g_set_object(&self->item, item);

  /* Name */
  const char *display = gn_nip29_group_item_get_display_name(item);
  gtk_label_set_text(self->name_label, display ? display : "Unknown Group");

  /* Detail: relay + members */
  const char *relay = gn_nip29_group_item_get_relay_url(item);
  gboolean members_loaded = gn_nip29_group_item_get_members_loaded(item);

  g_autofree gchar *detail = NULL;
  if (members_loaded)
    {
      guint mc = gn_nip29_group_item_get_member_count(item);
      gboolean partial = gn_nip29_group_item_get_members_may_be_partial(item);
      detail = g_strdup_printf("%s · %u member%s%s",
                               relay ? relay : "relay",
                               mc, mc == 1 ? "" : "s",
                               partial ? "+" : "");
    }
  else
    {
      detail = g_strdup_printf("%s · members unknown",
                               relay ? relay : "relay");
    }
  gtk_label_set_text(self->detail_label, detail);

  /* Badges */
  clear_badge_box(self->badge_box);

  if (gn_nip29_group_item_get_is_private(item))
    add_badge(self->badge_box, "private", "dim-label");
  if (gn_nip29_group_item_get_is_closed(item))
    add_badge(self->badge_box, "closed", "dim-label");
  if (gn_nip29_group_item_get_is_hidden(item))
    add_badge(self->badge_box, "hidden", "dim-label");
  if (gn_nip29_group_item_get_is_restricted(item))
    add_badge(self->badge_box, "restricted", "dim-label");

  if (!gn_nip29_group_item_get_admins_loaded(item))
    add_badge(self->badge_box, "admins?", "warning");
}

void
gn_nip29_group_list_row_unbind(GnNip29GroupListRow *self)
{
  g_return_if_fail(GN_IS_NIP29_GROUP_LIST_ROW(self));

  g_clear_object(&self->item);
  gtk_label_set_text(self->name_label, NULL);
  gtk_label_set_text(self->detail_label, NULL);
  clear_badge_box(self->badge_box);
}
