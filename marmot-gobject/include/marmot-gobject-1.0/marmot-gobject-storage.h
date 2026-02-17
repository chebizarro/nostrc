/*
 * marmot-gobject - GObject wrapper for libmarmot
 *
 * MarmotGobjectStorage: GInterface wrapping the MarmotStorage vtable.
 * Provides GObject-based storage backends with GError error reporting.
 *
 * Built-in implementations:
 *   - MarmotGobjectMemoryStorage: in-memory (for testing)
 *   - MarmotGobjectSqliteStorage: SQLite persistent storage
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef MARMOT_GOBJECT_STORAGE_H
#define MARMOT_GOBJECT_STORAGE_H

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectStorage — GInterface
 * ══════════════════════════════════════════════════════════════════════════ */

#define MARMOT_GOBJECT_TYPE_STORAGE (marmot_gobject_storage_get_type())
G_DECLARE_INTERFACE(MarmotGobjectStorage, marmot_gobject_storage, MARMOT_GOBJECT, STORAGE, GObject)

/**
 * MarmotGobjectStorageInterface:
 * @parent_interface: parent
 * @get_raw_storage: get the underlying C MarmotStorage pointer (for passing to libmarmot)
 *
 * The GObject storage interface. Implementations wrap a MarmotStorage*
 * and expose it to the MarmotGobjectClient.
 *
 * Since: 1.0
 */
struct _MarmotGobjectStorageInterface {
    GTypeInterface parent_interface;

    /**
     * get_raw_storage:
     * @self: a #MarmotGobjectStorage
     *
     * Returns the underlying C MarmotStorage pointer.
     * The interface implementation retains ownership.
     *
     * Returns: (transfer none): the underlying MarmotStorage
     */
    gpointer (*get_raw_storage)(MarmotGobjectStorage *self);
};

/**
 * marmot_gobject_storage_get_raw_storage:
 * @self: a #MarmotGobjectStorage
 *
 * Returns: (transfer none): the underlying C MarmotStorage pointer
 */
gpointer marmot_gobject_storage_get_raw_storage(MarmotGobjectStorage *self);

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectMemoryStorage — in-memory implementation
 * ══════════════════════════════════════════════════════════════════════════ */

#define MARMOT_GOBJECT_TYPE_MEMORY_STORAGE (marmot_gobject_memory_storage_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectMemoryStorage, marmot_gobject_memory_storage, MARMOT_GOBJECT, MEMORY_STORAGE, GObject)

/**
 * marmot_gobject_memory_storage_new:
 *
 * Creates a new in-memory storage backend. Useful for testing.
 * All data is lost when the object is finalized.
 *
 * Returns: (transfer full): a new #MarmotGobjectMemoryStorage
 */
MarmotGobjectMemoryStorage *marmot_gobject_memory_storage_new(void);

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectSqliteStorage — SQLite implementation
 * ══════════════════════════════════════════════════════════════════════════ */

#define MARMOT_GOBJECT_TYPE_SQLITE_STORAGE (marmot_gobject_sqlite_storage_get_type())
G_DECLARE_FINAL_TYPE(MarmotGobjectSqliteStorage, marmot_gobject_sqlite_storage, MARMOT_GOBJECT, SQLITE_STORAGE, GObject)

/**
 * marmot_gobject_sqlite_storage_new:
 * @path: path to the SQLite database file
 * @encryption_key: (nullable): optional encryption key
 * @error: (nullable): return location for a #GError
 *
 * Creates a new SQLite-backed storage.
 *
 * Returns: (transfer full) (nullable): a new #MarmotGobjectSqliteStorage, or NULL on error
 */
MarmotGobjectSqliteStorage *marmot_gobject_sqlite_storage_new(const gchar *path,
                                                                const gchar *encryption_key,
                                                                GError **error);

G_END_DECLS

#endif /* MARMOT_GOBJECT_STORAGE_H */
