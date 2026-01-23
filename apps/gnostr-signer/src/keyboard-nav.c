/* keyboard-nav.c - Keyboard navigation helpers for gnostr-signer
 *
 * SPDX-License-Identifier: MIT
 *
 * Implements full keyboard navigation support following GTK4/GNOME HIG:
 * - Tab navigation through interactive elements
 * - Arrow keys for list navigation
 * - Enter/Space for button activation
 * - Focus visible indicators
 * - Skip links for main content
 * - Modal focus trapping
 * - Escape to close dialogs
 */
#include "keyboard-nav.h"
#include <gdk/gdkkeysyms.h>

/* ======== Dialog Focus Management ======== */

typedef struct {
  GtkWidget *first_focus;
  GtkWidget *default_button;
} DialogNavData;

static void
on_dialog_map(GtkWidget *widget, gpointer user_data)
{
  DialogNavData *data = user_data;

  if (data->first_focus && gtk_widget_get_visible(data->first_focus)) {
    /* Use idle to ensure widget is fully realized */
    g_idle_add_once((GSourceOnceFunc)gtk_widget_grab_focus, data->first_focus);
  }
}

static void
dialog_nav_data_free(gpointer data, GClosure *closure)
{
  (void)closure;
  g_free(data);
}

void
gn_keyboard_nav_setup_dialog(AdwDialog *dialog,
                              GtkWidget *first_focus,
                              GtkWidget *default_button)
{
  g_return_if_fail(ADW_IS_DIALOG(dialog));

  DialogNavData *data = g_new0(DialogNavData, 1);
  data->first_focus = first_focus;
  data->default_button = default_button;

  /* Connect to map signal to focus first widget when dialog appears */
  g_signal_connect_data(dialog, "map", G_CALLBACK(on_dialog_map),
                        data, dialog_nav_data_free, 0);

  /* Set default button for Enter key activation */
  if (default_button && GTK_IS_BUTTON(default_button)) {
    gtk_widget_add_css_class(default_button, "suggested-action");
    /* AdwDialog handles default widget differently - use CSS class */
  }

  /* Ensure dialog can be closed with Escape (AdwDialog default) */
  adw_dialog_set_can_close(dialog, TRUE);
}

/* ======== ListBox Arrow Navigation ======== */

static void
announce_row_change(GtkListBox *listbox)
{
  GtkListBoxRow *row = gtk_list_box_get_selected_row(listbox);
  if (!row) return;

  GtkAccessible *accessible = GTK_ACCESSIBLE(row);
  if (accessible) {
    /* Announce the row change for screen readers */
    gtk_accessible_update_state(accessible,
                                 GTK_ACCESSIBLE_STATE_SELECTED, TRUE,
                                 -1);
  }
}

static void
on_listbox_row_selected(GtkListBox *listbox, GtkListBoxRow *row, gpointer user_data)
{
  (void)user_data;
  (void)row;
  announce_row_change(listbox);
}

void
gn_keyboard_nav_setup_listbox_arrows(GtkListBox *listbox)
{
  g_return_if_fail(GTK_IS_LIST_BOX(listbox));

  /* GTK4 listboxes already handle arrow keys natively.
   * We add screen reader announcements for accessibility. */
  g_signal_connect(listbox, "row-selected",
                   G_CALLBACK(on_listbox_row_selected), NULL);
}

/* ======== Sidebar Navigation ======== */

typedef struct {
  GtkListBox *sidebar;
  AdwViewStack *stack;
  char **page_names;
  int n_pages;
} SidebarNavData;

static gboolean
on_sidebar_key_pressed(GtkEventControllerKey *controller,
                       guint keyval, guint keycode,
                       GdkModifierType state,
                       gpointer user_data)
{
  (void)controller;
  (void)keycode;
  SidebarNavData *data = user_data;

  /* Handle Ctrl+PageUp/PageDown for quick page navigation */
  if (state & GDK_CONTROL_MASK) {
    GtkListBoxRow *current = gtk_list_box_get_selected_row(data->sidebar);
    int current_idx = current ? gtk_list_box_row_get_index(current) : 0;
    int new_idx = current_idx;

    if (keyval == GDK_KEY_Page_Up || keyval == GDK_KEY_KP_Page_Up) {
      new_idx = (current_idx > 0) ? current_idx - 1 : data->n_pages - 1;
    } else if (keyval == GDK_KEY_Page_Down || keyval == GDK_KEY_KP_Page_Down) {
      new_idx = (current_idx < data->n_pages - 1) ? current_idx + 1 : 0;
    } else {
      return FALSE;
    }

    GtkListBoxRow *new_row = gtk_list_box_get_row_at_index(data->sidebar, new_idx);
    if (new_row) {
      gtk_list_box_select_row(data->sidebar, new_row);
      /* Activate the row to switch pages */
      g_signal_emit_by_name(data->sidebar, "row-activated", new_row);
    }
    return TRUE;
  }

  /* Enter activates the selected row */
  if (keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) {
    GtkListBoxRow *row = gtk_list_box_get_selected_row(data->sidebar);
    if (row) {
      g_signal_emit_by_name(data->sidebar, "row-activated", row);
      return TRUE;
    }
  }

  return FALSE;
}

static void
sidebar_nav_data_free(gpointer data)
{
  SidebarNavData *nav = data;
  g_strfreev(nav->page_names);
  g_free(nav);
}

void
gn_keyboard_nav_setup_sidebar(GtkListBox *sidebar,
                               AdwViewStack *stack,
                               const char **page_names)
{
  g_return_if_fail(GTK_IS_LIST_BOX(sidebar));
  g_return_if_fail(ADW_IS_VIEW_STACK(stack));

  SidebarNavData *data = g_new0(SidebarNavData, 1);
  data->sidebar = sidebar;
  data->stack = stack;
  data->page_names = g_strdupv((gchar **)page_names);
  data->n_pages = g_strv_length(data->page_names);

  /* Add key controller for navigation shortcuts */
  GtkEventController *controller = gtk_event_controller_key_new();
  g_signal_connect(controller, "key-pressed",
                   G_CALLBACK(on_sidebar_key_pressed), data);
  g_object_set_data_full(G_OBJECT(sidebar), "sidebar-nav-data", data,
                         sidebar_nav_data_free);
  gtk_widget_add_controller(GTK_WIDGET(sidebar), controller);

  /* Setup arrow key announcements */
  gn_keyboard_nav_setup_listbox_arrows(sidebar);
}

/* ======== Skip Links ======== */

typedef struct {
  GtkWidget *target;
  char *target_id;
} SkipLinkData;

static void
on_skip_link_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  SkipLinkData *data = user_data;

  if (data->target) {
    gtk_widget_grab_focus(data->target);
  }
}

static void
skip_link_data_free(gpointer data, GClosure *closure)
{
  (void)closure;
  SkipLinkData *skip = data;
  g_free(skip->target_id);
  g_free(skip);
}

GtkWidget *
gn_keyboard_nav_add_skip_link(GtkWidget *window,
                               const char *target_id,
                               const char *label)
{
  g_return_val_if_fail(GTK_IS_WIDGET(window), NULL);
  g_return_val_if_fail(target_id != NULL, NULL);
  g_return_val_if_fail(label != NULL, NULL);

  /* Create skip link button */
  GtkWidget *skip_btn = gtk_button_new_with_label(label);
  gtk_widget_add_css_class(skip_btn, "skip-link");

  /* Initially hidden, shown only on focus */
  gtk_widget_set_opacity(skip_btn, 0);

  SkipLinkData *data = g_new0(SkipLinkData, 1);
  data->target_id = g_strdup(target_id);
  /* Target will be resolved when clicked */

  g_signal_connect_data(skip_btn, "clicked",
                        G_CALLBACK(on_skip_link_clicked),
                        data, skip_link_data_free, 0);

  /* Show on focus */
  GtkEventController *focus = gtk_event_controller_focus_new();
  g_signal_connect_swapped(focus, "enter",
                           G_CALLBACK(gtk_widget_set_opacity),
                           GINT_TO_POINTER(1));
  g_signal_connect_swapped(focus, "leave",
                           G_CALLBACK(gtk_widget_set_opacity),
                           GINT_TO_POINTER(0));
  gtk_widget_add_controller(skip_btn, focus);

  return skip_btn;
}

/* ======== Focus First Entry ======== */

static gboolean
find_and_focus_entry_cb(GtkWidget *widget, gpointer user_data)
{
  gboolean *found = user_data;

  if (*found) return FALSE;

  /* Check if this is a focusable entry-like widget */
  if (ADW_IS_ENTRY_ROW(widget) ||
      ADW_IS_PASSWORD_ENTRY_ROW(widget) ||
      GTK_IS_ENTRY(widget) ||
      GTK_IS_TEXT(widget)) {
    if (gtk_widget_get_visible(widget) &&
        gtk_widget_get_sensitive(widget) &&
        gtk_widget_get_focusable(widget)) {
      gtk_widget_grab_focus(widget);
      *found = TRUE;
      return TRUE;
    }
  }

  /* Recurse into containers */
  GtkWidget *child = gtk_widget_get_first_child(widget);
  while (child) {
    if (find_and_focus_entry_cb(child, user_data)) {
      return TRUE;
    }
    child = gtk_widget_get_next_sibling(child);
  }

  return FALSE;
}

gboolean
gn_keyboard_nav_focus_first_entry(GtkWidget *container)
{
  g_return_val_if_fail(GTK_IS_WIDGET(container), FALSE);

  gboolean found = FALSE;
  find_and_focus_entry_cb(container, &found);
  return found;
}

/* ======== Enter Key to Button Activation ======== */

static void
on_entry_activate_button(GtkWidget *entry, gpointer user_data)
{
  (void)entry;
  GtkWidget *button = GTK_WIDGET(user_data);

  if (gtk_widget_get_sensitive(button)) {
    g_signal_emit_by_name(button, "clicked");
  }
}

void
gn_keyboard_nav_connect_enter_activate(GtkWidget *entry, GtkWidget *button)
{
  g_return_if_fail(GTK_IS_WIDGET(entry));
  g_return_if_fail(GTK_IS_BUTTON(button));

  /* Connect to the apply/activate signal of entry-like widgets */
  if (ADW_IS_ENTRY_ROW(entry) || ADW_IS_PASSWORD_ENTRY_ROW(entry)) {
    g_signal_connect(entry, "apply",
                     G_CALLBACK(on_entry_activate_button), button);
  } else if (GTK_IS_ENTRY(entry)) {
    g_signal_connect(entry, "activate",
                     G_CALLBACK(on_entry_activate_button), button);
  }
}

/* ======== Focus Chain Setup ======== */

void
gn_keyboard_nav_setup_focus_chain(GtkWidget **widgets)
{
  g_return_if_fail(widgets != NULL);

  /* In GTK4, focus chain is determined by widget hierarchy and focusable property.
   * We ensure all widgets are focusable and in correct order. */
  for (int i = 0; widgets[i] != NULL; i++) {
    GtkWidget *w = widgets[i];
    if (!gtk_widget_get_focusable(w)) {
      gtk_widget_set_focusable(w, TRUE);
    }
  }
}

/* ======== Focus Trap for Modals ======== */

static gboolean
on_focus_trap_key(GtkEventControllerKey *controller,
                  guint keyval, guint keycode,
                  GdkModifierType state,
                  gpointer user_data)
{
  (void)controller;
  (void)keycode;
  GtkWidget *container = GTK_WIDGET(user_data);

  if (keyval != GDK_KEY_Tab) {
    return FALSE;
  }

  /* Find all focusable children */
  GList *focusable = NULL;
  GtkWidget *child = gtk_widget_get_first_child(container);

  /* Helper to collect focusable widgets */
  while (child) {
    if (gtk_widget_get_focusable(child) &&
        gtk_widget_get_visible(child) &&
        gtk_widget_get_sensitive(child)) {
      focusable = g_list_append(focusable, child);
    }

    /* Check children recursively */
    GtkWidget *grandchild = gtk_widget_get_first_child(child);
    while (grandchild) {
      if (gtk_widget_get_focusable(grandchild) &&
          gtk_widget_get_visible(grandchild) &&
          gtk_widget_get_sensitive(grandchild)) {
        focusable = g_list_append(focusable, grandchild);
      }
      grandchild = gtk_widget_get_next_sibling(grandchild);
    }

    child = gtk_widget_get_next_sibling(child);
  }

  if (!focusable) {
    return FALSE;
  }

  /* Find current focus */
  GtkWidget *focused = gtk_root_get_focus(GTK_ROOT(gtk_widget_get_root(container)));
  GList *current = g_list_find(focusable, focused);

  GtkWidget *next = NULL;
  if (state & GDK_SHIFT_MASK) {
    /* Shift+Tab: go backward */
    if (current && current->prev) {
      next = current->prev->data;
    } else {
      /* Wrap to end */
      next = g_list_last(focusable)->data;
    }
  } else {
    /* Tab: go forward */
    if (current && current->next) {
      next = current->next->data;
    } else {
      /* Wrap to beginning */
      next = focusable->data;
    }
  }

  g_list_free(focusable);

  if (next) {
    gtk_widget_grab_focus(next);
    return TRUE;
  }

  return FALSE;
}

void
gn_keyboard_nav_trap_focus(GtkWidget *container)
{
  g_return_if_fail(GTK_IS_WIDGET(container));

  GtkEventController *controller = gtk_event_controller_key_new();
  gtk_event_controller_set_propagation_phase(controller, GTK_PHASE_CAPTURE);
  g_signal_connect(controller, "key-pressed",
                   G_CALLBACK(on_focus_trap_key), container);
  gtk_widget_add_controller(container, controller);
}
