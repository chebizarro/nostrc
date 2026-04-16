/* gnostr-filter-switcher.h — Popover-backed widget for picking a
 * #GnostrFilterSet. Emits "filter-set-activated" with the chosen
 * filter-set id; a higher-level component (the main window) is
 * responsible for routing that id into the timeline dispatcher.
 *
 * SPDX-License-Identifier: MIT
 *
 * nostrc-yg8j.5: Filter set switcher UI component.
 */

#ifndef GNOSTR_FILTER_SWITCHER_H
#define GNOSTR_FILTER_SWITCHER_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_FILTER_SWITCHER (gnostr_filter_switcher_get_type())
G_DECLARE_FINAL_TYPE(GnostrFilterSwitcher, gnostr_filter_switcher,
                     GNOSTR, FILTER_SWITCHER, GtkWidget)

/**
 * gnostr_filter_switcher_new:
 *
 * Returns: (transfer full): a new #GnostrFilterSwitcher bound to the
 *   process-wide #GnostrFilterSetManager singleton.
 */
GtkWidget *gnostr_filter_switcher_new(void);

/**
 * gnostr_filter_switcher_set_active_id:
 * @self: a switcher
 * @id: (nullable): filter-set id currently active, or %NULL for none
 *
 * Updates the button label/icon and the in-popover active-row indicator
 * to reflect the currently-selected filter set. Safe to call with an id
 * that doesn't exist — the widget just falls back to its default label.
 */
void gnostr_filter_switcher_set_active_id(GnostrFilterSwitcher *self,
                                           const gchar *id);

/**
 * gnostr_filter_switcher_activate_position:
 * @self: a switcher
 * @position: 1-based slot number (1 = first filter set in the manager)
 *
 * Programmatically emits #GnostrFilterSwitcher::filter-set-activated for
 * the filter set at the given slot. Silently no-ops when @position is
 * out of range. Intended for keyboard-shortcut routing
 * (`<Primary>1`–`<Primary>9`).
 *
 * Returns: %TRUE if a filter set was activated.
 */
gboolean gnostr_filter_switcher_activate_position(GnostrFilterSwitcher *self,
                                                   guint position);

/**
 * Signals:
 *
 *   "filter-set-activated" (const gchar *filter_set_id)
 *     Emitted when the user picks a filter set from the popover or when
 *     gnostr_filter_switcher_activate_position() finds a slot. Listeners
 *     should route the id through the timeline tab dispatcher.
 */

G_END_DECLS

#endif /* GNOSTR_FILTER_SWITCHER_H */
