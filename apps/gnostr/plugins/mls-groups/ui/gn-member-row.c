/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-member-row.c - Group member row widget
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "gn-member-row.h"

struct _GnMemberRow
{
  GtkBox parent_instance;

  /* Widgets */
  GtkImage  *avatar;
  GtkLabel  *name_label;
  GtkLabel  *pubkey_label;
  GtkLabel  *badge_label;      /* "Admin" / "You" badge */
  GtkButton *remove_button;

  /* Data */
  gchar    *pubkey_hex;
  gboolean  is_admin;
  gboolean  is_self;
  gboolean  removable;
};

enum
{
  SIGNAL_REMOVE_REQUESTED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnMemberRow, gn_member_row, GTK_TYPE_BOX)

/* ── Helpers ─────────────────────────────────────────────────────── */

static gchar *
truncate_pubkey(const gchar *hex)
{
  if (hex == NULL || strlen(hex) < 16)
    return g_strdup(hex ? hex : "");
  return g_strdup_printf("%.8s…%.8s", hex, hex + strlen(hex) - 8);
}

/* ── Callbacks ───────────────────────────────────────────────────── */

static void
on_remove_clicked(GtkButton *button, gpointer user_data)
{
  GnMemberRow *self = GN_MEMBER_ROW(user_data);
  if (self->pubkey_hex != NULL)
    g_signal_emit(self, signals[SIGNAL_REMOVE_REQUESTED], 0, self->pubkey_hex);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_member_row_dispose(GObject *object)
{
  GnMemberRow *self = GN_MEMBER_ROW(object);
  (void)self;
  G_OBJECT_CLASS(gn_member_row_parent_class)->dispose(object);
}

static void
gn_member_row_finalize(GObject *object)
{
  GnMemberRow *self = GN_MEMBER_ROW(object);
  g_free(self->pubkey_hex);
  G_OBJECT_CLASS(gn_member_row_parent_class)->finalize(object);
}

static void
gn_member_row_class_init(GnMemberRowClass *klass)
{
  GObjectClass *oc = G_OBJECT_CLASS(klass);
  oc->dispose  = gn_member_row_dispose;
  oc->finalize = gn_member_row_finalize;

  /**
   * GnMemberRow::remove-requested:
   * @self: The member row
   * @pubkey_hex: Public key hex of the member to remove
   */
  signals[SIGNAL_REMOVE_REQUESTED] = g_signal_new(
    "remove-requested",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0,
    NULL, NULL,
    NULL,
    G_TYPE_NONE, 1,
    G_TYPE_STRING);
}

static void
gn_member_row_init(GnMemberRow *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_widget_set_margin_start(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_end(GTK_WIDGET(self), 12);
  gtk_widget_set_margin_top(GTK_WIDGET(self), 6);
  gtk_widget_set_margin_bottom(GTK_WIDGET(self), 6);
  gtk_box_set_spacing(GTK_BOX(self), 12);

  /* Avatar placeholder */
  self->avatar = GTK_IMAGE(gtk_image_new_from_icon_name("avatar-default-symbolic"));
  gtk_image_set_pixel_size(self->avatar, 32);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->avatar));

  /* Text column */
  GtkWidget *text_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text_box, TRUE);
  gtk_widget_set_valign(text_box, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(self), text_box);

  /* Top row: name + badge */
  GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_box_append(GTK_BOX(text_box), name_row);

  self->name_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_ellipsize(self->name_label, PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(GTK_WIDGET(self->name_label), GTK_ALIGN_START);
  gtk_widget_add_css_class(GTK_WIDGET(self->name_label), "heading");
  gtk_box_append(GTK_BOX(name_row), GTK_WIDGET(self->name_label));

  self->badge_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_widget_add_css_class(GTK_WIDGET(self->badge_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->badge_label), "caption");
  gtk_widget_set_halign(GTK_WIDGET(self->badge_label), GTK_ALIGN_START);
  gtk_widget_set_visible(GTK_WIDGET(self->badge_label), FALSE);
  gtk_box_append(GTK_BOX(name_row), GTK_WIDGET(self->badge_label));

  /* Pubkey label (truncated) */
  self->pubkey_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_ellipsize(self->pubkey_label, PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_halign(GTK_WIDGET(self->pubkey_label), GTK_ALIGN_START);
  gtk_widget_add_css_class(GTK_WIDGET(self->pubkey_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->pubkey_label), "caption");
  gtk_box_append(GTK_BOX(text_box), GTK_WIDGET(self->pubkey_label));

  /* Remove button (hidden by default) */
  self->remove_button = GTK_BUTTON(gtk_button_new_from_icon_name("user-trash-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(self->remove_button), "flat");
  gtk_widget_add_css_class(GTK_WIDGET(self->remove_button), "circular");
  gtk_widget_add_css_class(GTK_WIDGET(self->remove_button), "destructive-action");
  gtk_widget_set_valign(GTK_WIDGET(self->remove_button), GTK_ALIGN_CENTER);
  gtk_widget_set_visible(GTK_WIDGET(self->remove_button), FALSE);
  gtk_widget_set_tooltip_text(GTK_WIDGET(self->remove_button), "Remove member");
  g_signal_connect(self->remove_button, "clicked",
                   G_CALLBACK(on_remove_clicked), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->remove_button));
}

/* ── Public API ──────────────────────────────────────────────────── */

GnMemberRow *
gn_member_row_new(void)
{
  return g_object_new(GN_TYPE_MEMBER_ROW, NULL);
}

void
gn_member_row_set_pubkey(GnMemberRow *self,
                          const gchar *pubkey_hex,
                          gboolean     is_admin,
                          gboolean     is_self)
{
  g_return_if_fail(GN_IS_MEMBER_ROW(self));

  g_free(self->pubkey_hex);
  self->pubkey_hex = g_strdup(pubkey_hex);
  self->is_admin   = is_admin;
  self->is_self    = is_self;

  /* Name: show "You" for self, otherwise truncated pubkey as placeholder */
  if (is_self)
    gtk_label_set_text(self->name_label, "You");
  else
    {
      g_autofree gchar *short_pk = truncate_pubkey(pubkey_hex);
      gtk_label_set_text(self->name_label, short_pk);
    }

  /* Pubkey subtitle */
  g_autofree gchar *pk_display = truncate_pubkey(pubkey_hex);
  gtk_label_set_text(self->pubkey_label, pk_display);

  /* Badge */
  if (is_admin && is_self)
    {
      gtk_label_set_text(self->badge_label, "Admin · You");
      gtk_widget_set_visible(GTK_WIDGET(self->badge_label), TRUE);
    }
  else if (is_admin)
    {
      gtk_label_set_text(self->badge_label, "Admin");
      gtk_widget_set_visible(GTK_WIDGET(self->badge_label), TRUE);
    }
  else if (is_self)
    {
      gtk_label_set_text(self->badge_label, "You");
      gtk_widget_set_visible(GTK_WIDGET(self->badge_label), TRUE);
    }
  else
    {
      gtk_widget_set_visible(GTK_WIDGET(self->badge_label), FALSE);
    }

  /* Never show remove button for self */
  if (is_self)
    gtk_widget_set_visible(GTK_WIDGET(self->remove_button), FALSE);
}

const gchar *
gn_member_row_get_pubkey_hex(GnMemberRow *self)
{
  g_return_val_if_fail(GN_IS_MEMBER_ROW(self), NULL);
  return self->pubkey_hex;
}

void
gn_member_row_set_removable(GnMemberRow *self,
                             gboolean     removable)
{
  g_return_if_fail(GN_IS_MEMBER_ROW(self));
  self->removable = removable;

  /* Don't show remove button for self */
  if (self->is_self)
    gtk_widget_set_visible(GTK_WIDGET(self->remove_button), FALSE);
  else
    gtk_widget_set_visible(GTK_WIDGET(self->remove_button), removable);
}
