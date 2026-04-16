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
 * gnostr_filter_switcher_set_pubkey:
 * @self: a switcher
 * @pubkey_hex: (nullable): 64-character hex pubkey of the currently
 *   connected signer, or %NULL when none is connected
 *
 * Tells the switcher which pubkey to thread into the create / seeded
 * dialogs so the "Import from NIP-51 list…" shortcut can query NDB
 * for the user's kind-30000 lists. Passing %NULL (or an empty string)
 * disables the shortcut on subsequently-presented dialogs. Safe to
 * call at any time.
 *
 * nostrc-yg8j.8: List-based filter sets (NIP-51).
 */
void gnostr_filter_switcher_set_pubkey(GnostrFilterSwitcher *self,
                                        const gchar *pubkey_hex);

/**
 * gnostr_filter_switcher_set_active_hashtag:
 * @self: a switcher
 * @hashtag: (nullable): hashtag of the currently-visible timeline tab
 *   (without the leading `#`), or %NULL when the active tab is not a
 *   hashtag tab
 *
 * Tells the switcher that the user is currently looking at a hashtag
 * feed. When non-%NULL, the popover footer grows a "Save "#tag" as
 * filter set…" row that opens the create dialog pre-seeded with the
 * tag and a proposed name. Call with %NULL to hide the row when the
 * user navigates off the hashtag tab.
 *
 * nostrc-yg8j.7: Hashtag-based filter sets.
 */
void gnostr_filter_switcher_set_active_hashtag(GnostrFilterSwitcher *self,
                                                const gchar *hashtag);

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
