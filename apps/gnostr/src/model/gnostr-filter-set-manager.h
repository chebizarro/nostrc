/* gnostr-filter-set-manager.h — CRUD layer over a collection of
 * #GnostrFilterSet objects.
 *
 * SPDX-License-Identifier: MIT
 *
 * The manager owns an in-memory collection exposed as a #GListModel for
 * GTK binding, and persists the set of user-owned (custom) filter sets
 * to a JSON file under the user data directory. Predefined (built-in)
 * filter sets are registered at runtime and not persisted.
 *
 * nostrc-yg8j.2: FilterSet manager for CRUD operations.
 */

#ifndef GNOSTR_FILTER_SET_MANAGER_H
#define GNOSTR_FILTER_SET_MANAGER_H

#include <glib-object.h>
#include <gio/gio.h>

#include "model/gnostr-filter-set.h"

G_BEGIN_DECLS

#define GNOSTR_TYPE_FILTER_SET_MANAGER (gnostr_filter_set_manager_get_type())
G_DECLARE_FINAL_TYPE(GnostrFilterSetManager,
                     gnostr_filter_set_manager,
                     GNOSTR, FILTER_SET_MANAGER,
                     GObject)

/* ------------------------------------------------------------------------
 * Construction
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_manager_new:
 *
 * Create a new manager that persists to the default storage path
 * (`$XDG_DATA_HOME/gnostr/filter-sets.json`). The on-disk file is
 * not touched until gnostr_filter_set_manager_load() is invoked.
 *
 * Returns: (transfer full): a new #GnostrFilterSetManager.
 */
GnostrFilterSetManager *gnostr_filter_set_manager_new(void);

/**
 * gnostr_filter_set_manager_new_for_path:
 * @path: (nullable): absolute path to the JSON storage file, or %NULL
 *   to use the default location
 *
 * Create a manager backed by a specific file (primarily useful for tests).
 * The file is not created until the first save.
 *
 * Returns: (transfer full): a new #GnostrFilterSetManager.
 */
GnostrFilterSetManager *gnostr_filter_set_manager_new_for_path(const gchar *path);

/**
 * gnostr_filter_set_manager_get_default:
 *
 * Returns the process-wide default manager, creating it on first call.
 * The caller does not own the returned reference.
 *
 * Returns: (transfer none): the default #GnostrFilterSetManager.
 */
GnostrFilterSetManager *gnostr_filter_set_manager_get_default(void);

/* ------------------------------------------------------------------------
 * Persistence
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_manager_load:
 * @self: manager
 * @error: (out) (nullable): error location
 *
 * Populate the manager from the backing JSON file. If the file does not
 * exist, the manager is initialised with defaults (see
 * gnostr_filter_set_manager_install_defaults()) and %TRUE is returned.
 *
 * Returns: %TRUE on success.
 */
gboolean gnostr_filter_set_manager_load(GnostrFilterSetManager *self,
                                        GError **error);

/**
 * gnostr_filter_set_manager_save:
 * @self: manager
 * @error: (out) (nullable): error location
 *
 * Persist all custom (non-predefined) filter sets to the backing file.
 * Predefined sets are always recomputed at runtime and are not written.
 *
 * Returns: %TRUE on success.
 */
gboolean gnostr_filter_set_manager_save(GnostrFilterSetManager *self,
                                        GError **error);

/**
 * gnostr_filter_set_manager_install_defaults:
 * @self: manager
 *
 * Register the built-in predefined filter sets (Global, Follows,
 * Mentions, Media). Safe to call multiple times; existing entries
 * with matching ids are left untouched.
 */
void gnostr_filter_set_manager_install_defaults(GnostrFilterSetManager *self);

/* ------------------------------------------------------------------------
 * CRUD
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_manager_add:
 * @self: manager
 * @filter_set: (transfer none): a filter set to add
 *
 * Adds @filter_set to the collection. If a set with the same id already
 * exists, the add is rejected. An id is auto-generated when @filter_set
 * has an empty one. The manager holds its own reference.
 *
 * Returns: %TRUE if inserted, %FALSE if rejected due to a duplicate id.
 */
gboolean gnostr_filter_set_manager_add(GnostrFilterSetManager *self,
                                       GnostrFilterSet *filter_set);

/**
 * gnostr_filter_set_manager_update:
 * @self: manager
 * @filter_set: (transfer none): a filter set with a known id
 *
 * Replace the existing entry with the same id with a deep clone of
 * @filter_set. The original instance stored in the manager is removed
 * and its slot in the list model is reused, so consumers observe a
 * single items-changed(1, 1) transition.
 *
 * Returns: %TRUE if an entry with that id was found and updated.
 */
gboolean gnostr_filter_set_manager_update(GnostrFilterSetManager *self,
                                          GnostrFilterSet *filter_set);

/**
 * gnostr_filter_set_manager_remove:
 * @self: manager
 * @id: id of the filter set to remove
 *
 * Remove the filter set matching @id. Predefined sets cannot be removed
 * and the call will return %FALSE in that case.
 *
 * Returns: %TRUE if removed.
 */
gboolean gnostr_filter_set_manager_remove(GnostrFilterSetManager *self,
                                          const gchar *id);

/* ------------------------------------------------------------------------
 * Lookup
 * ------------------------------------------------------------------------ */

/**
 * gnostr_filter_set_manager_get:
 * @self: manager
 * @id: id to look up
 *
 * Returns: (transfer none) (nullable): the filter set or %NULL if no
 *   match exists.
 */
GnostrFilterSet *gnostr_filter_set_manager_get(GnostrFilterSetManager *self,
                                               const gchar *id);

/**
 * gnostr_filter_set_manager_contains:
 * @self: manager
 * @id: id to look up
 *
 * Returns: %TRUE if a filter set with @id exists.
 */
gboolean gnostr_filter_set_manager_contains(GnostrFilterSetManager *self,
                                            const gchar *id);

/**
 * gnostr_filter_set_manager_count:
 * @self: manager
 *
 * Returns: the total number of filter sets (custom + predefined).
 */
guint gnostr_filter_set_manager_count(GnostrFilterSetManager *self);

/**
 * gnostr_filter_set_manager_get_model:
 * @self: manager
 *
 * The returned model is owned by the manager and lives as long as the
 * manager does. Consumers can freely bind it to list widgets or filter
 * it with #GtkCustomFilter / #GtkFilterListModel.
 *
 * Returns: (transfer none): a #GListModel of #GnostrFilterSet items.
 */
GListModel *gnostr_filter_set_manager_get_model(GnostrFilterSetManager *self);

G_END_DECLS

#endif /* GNOSTR_FILTER_SET_MANAGER_H */
