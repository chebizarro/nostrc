#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Moves the window's keyboard focus out of @widget (a popover, typically)
 * before it is hidden, emptied or unparented.
 *
 * GTK 4.14 (Ubuntu 24.04) leaks a reference otherwise: hiding or removing
 * the widget that holds the focus stores a reference in the window's pending
 * "move focus" slot, cleared after the next frame, and doing it again before
 * that frame (gtk_popover_popdown() then gtk_popover_set_child() or
 * gtk_widget_unparent()) stores another one over it, never released.
 * A popover leaked that way stays alive but unrealized, and GTK 4.14's
 * tooltip, which lets go of a popover it hovered only when that popover is
 * finalized, then runs its hover timeout on a popover without a surface:
 * 'gdk_surface_get_device_position: assertion GDK_IS_SURFACE (surface)'.
 *
 * The focus goes to @widget's parent when that can take it, as GTK itself
 * would after the frame, and nowhere otherwise. */
static inline void
gh_widget_release_focus(GtkWidget *widget)
{
  GtkRoot *root = gtk_widget_get_root(widget);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  if (!focus || (focus != widget && !gtk_widget_is_ancestor(focus, widget)))
    return;
  GtkWidget *parent = gtk_widget_get_parent(widget);
  if (parent)
    gtk_widget_grab_focus(parent);
  focus = gtk_root_get_focus(root);
  if (focus && (focus == widget || gtk_widget_is_ancestor(focus, widget)))
    gtk_root_set_focus(root, NULL);
}

/* gtk_widget_unparent() after gh_widget_release_focus(). */
static inline void
gh_widget_unparent_unfocused(GtkWidget *widget)
{
  gh_widget_release_focus(widget);
  gtk_widget_unparent(widget);
}

G_END_DECLS
