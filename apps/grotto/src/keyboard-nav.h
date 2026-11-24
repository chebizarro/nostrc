/* keyboard-nav.h - Keyboard navigation helpers for grotto
 *
 * SPDX-License-Identifier: MIT
 *
 * Provides utilities for implementing keyboard navigation support:
 * - Focus management for dialogs
 * - Arrow key navigation for lists
 * - Skip links for content areas
 * - Focus trap for modal dialogs
 */
#ifndef KEYBOARD_NAV_H
#define KEYBOARD_NAV_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

/**
 * gn_keyboard_nav_setup_listbox_arrows:
 * @listbox: a #GtkListBox
 *
 * Ensures arrow key navigation works properly for the listbox.
 * GTK4 listboxes have this by default, but this function adds
 * additional accessibility announcements.
 */
void gn_keyboard_nav_setup_listbox_arrows(GtkListBox *listbox);

/**
 * gn_keyboard_nav_connect_enter_activate:
 * @entry: an entry widget (GtkEntry, AdwEntryRow, AdwPasswordEntryRow)
 * @button: the button to activate on Enter
 *
 * Connects Enter key in @entry to activate @button.
 * This supplements the dialog's default button behavior.
 */
void gn_keyboard_nav_connect_enter_activate(GtkWidget *entry, GtkWidget *button);

G_END_DECLS

#endif /* KEYBOARD_NAV_H */
