/* keyboard-nav.h - Keyboard navigation helpers for gnostr-signer
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
 * gn_keyboard_nav_setup_dialog:
 * @dialog: an #AdwDialog
 * @first_focus: (nullable): the widget to focus when dialog opens
 * @default_button: (nullable): the button to activate on Enter key
 *
 * Sets up keyboard navigation for a dialog:
 * - Focuses @first_focus when the dialog is shown
 * - Makes @default_button the default widget (activated on Enter)
 * - Ensures focus trap is properly configured
 */
void gn_keyboard_nav_setup_dialog(AdwDialog *dialog,
                                   GtkWidget *first_focus,
                                   GtkWidget *default_button);

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
 * gn_keyboard_nav_setup_sidebar:
 * @sidebar: a #GtkListBox used as navigation sidebar
 * @stack: the #AdwViewStack that the sidebar controls
 * @page_names: NULL-terminated array of page names
 *
 * Sets up keyboard navigation between sidebar and content:
 * - Arrow keys navigate within sidebar
 * - Enter activates selected item
 * - Ctrl+Page Up/Down navigates between pages
 */
void gn_keyboard_nav_setup_sidebar(GtkListBox *sidebar,
                                    AdwViewStack *stack,
                                    const char **page_names);

/**
 * gn_keyboard_nav_add_skip_link:
 * @window: the main window
 * @target_id: ID of the target widget to skip to
 * @label: accessibility label for the skip link
 *
 * Adds a skip link that appears on Tab from the beginning.
 * Skip links allow keyboard users to jump directly to main content.
 * Returns: the skip link widget (for further configuration)
 */
GtkWidget *gn_keyboard_nav_add_skip_link(GtkWidget *window,
                                          const char *target_id,
                                          const char *label);

/**
 * gn_keyboard_nav_focus_first_entry:
 * @container: a container widget
 *
 * Finds and focuses the first entry/password field in the container.
 * Returns: %TRUE if an entry was found and focused
 */
gboolean gn_keyboard_nav_focus_first_entry(GtkWidget *container);

/**
 * gn_keyboard_nav_connect_enter_activate:
 * @entry: an entry widget (GtkEntry, AdwEntryRow, AdwPasswordEntryRow)
 * @button: the button to activate on Enter
 *
 * Connects Enter key in @entry to activate @button.
 * This supplements the dialog's default button behavior.
 */
void gn_keyboard_nav_connect_enter_activate(GtkWidget *entry, GtkWidget *button);

/**
 * gn_keyboard_nav_setup_focus_chain:
 * @widgets: NULL-terminated array of widgets
 *
 * Sets up explicit focus chain for the given widgets.
 * Tab will cycle through the widgets in order.
 */
void gn_keyboard_nav_setup_focus_chain(GtkWidget **widgets);

/**
 * gn_keyboard_nav_trap_focus:
 * @container: a modal container (dialog content area)
 *
 * Ensures Tab/Shift+Tab cycle only within the container.
 * Used for modal dialogs to prevent focus from escaping.
 */
void gn_keyboard_nav_trap_focus(GtkWidget *container);

G_END_DECLS

#endif /* KEYBOARD_NAV_H */
