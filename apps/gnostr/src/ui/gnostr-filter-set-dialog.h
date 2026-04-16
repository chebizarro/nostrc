/* gnostr-filter-set-dialog.h — AdwDialog for creating / editing custom
 * GnostrFilterSet entries.
 *
 * SPDX-License-Identifier: MIT
 *
 * The dialog provides form fields for the user-facing filter-set
 * attributes (name, description, kinds, authors, hashtags, since/until,
 * limit) plus a live text summary so the user can see at a glance what
 * the resulting filter will match. Save persists the set via the
 * default #GnostrFilterSetManager and emits "filter-set-saved" with the
 * resulting id so higher-level components (the filter switcher, the
 * main window) can route focus onto the new tab.
 *
 * nostrc-yg8j.6: Custom filter set creation dialog.
 */

#ifndef GNOSTR_FILTER_SET_DIALOG_H
#define GNOSTR_FILTER_SET_DIALOG_H

#include <adwaita.h>

#include "../model/gnostr-filter-set.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_FILTER_SET_DIALOG (gnostr_filter_set_dialog_get_type())
G_DECLARE_FINAL_TYPE(GnostrFilterSetDialog, gnostr_filter_set_dialog,
                     GNOSTR, FILTER_SET_DIALOG, AdwDialog)

/**
 * gnostr_filter_set_dialog_new:
 *
 * Construct a dialog in create mode.
 *
 * Returns: (transfer full): a new dialog instance (sinks the floating
 *   ref automatically via #GtkWidget semantics).
 */
GtkWidget *gnostr_filter_set_dialog_new(void);

/**
 * gnostr_filter_set_dialog_new_for_edit:
 * @fs: the filter set to edit (must be #GNOSTR_FILTER_SET_SOURCE_CUSTOM)
 *
 * Construct a dialog in edit mode, pre-populated from @fs. The dialog
 * stores @fs's id so Save calls
 * gnostr_filter_set_manager_update() instead of add().
 *
 * Returns: (transfer full): a new dialog instance.
 */
GtkWidget *gnostr_filter_set_dialog_new_for_edit(GnostrFilterSet *fs);

/**
 * gnostr_filter_set_dialog_present:
 * @parent: the widget providing the parent window
 *
 * Convenience: construct a create-mode dialog and present it.
 */
void gnostr_filter_set_dialog_present(GtkWidget *parent);

/**
 * gnostr_filter_set_dialog_present_edit:
 * @parent: the widget providing the parent window
 * @fs: the filter set to edit
 *
 * Convenience: construct an edit-mode dialog and present it.
 */
void gnostr_filter_set_dialog_present_edit(GtkWidget *parent,
                                            GnostrFilterSet *fs);

/**
 * Signals:
 *
 *   "filter-set-saved" (const gchar *filter_set_id)
 *     Emitted after the dialog has added or updated the filter set via
 *     the default manager and attempted to persist it. The id refers
 *     to the newly-created (or updated) set and can be routed directly
 *     through the timeline tab dispatcher.
 */

G_END_DECLS

#endif /* GNOSTR_FILTER_SET_DIALOG_H */
