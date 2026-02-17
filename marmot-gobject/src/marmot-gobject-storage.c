/*
 * marmot-gobject - MarmotGobjectStorage interface + built-in implementations
 *
 * SPDX-License-Identifier: MIT
 */

#include "marmot-gobject-storage.h"
#include <marmot/marmot.h>
#include <marmot/marmot-storage.h>

/* ══════════════════════════════════════════════════════════════════════════
 * GInterface
 * ══════════════════════════════════════════════════════════════════════════ */

G_DEFINE_INTERFACE(MarmotGobjectStorage, marmot_gobject_storage, G_TYPE_OBJECT)

static void
marmot_gobject_storage_default_init(MarmotGobjectStorageInterface *iface)
{
    (void)iface;
}

gpointer
marmot_gobject_storage_get_raw_storage(MarmotGobjectStorage *self)
{
    g_return_val_if_fail(MARMOT_GOBJECT_IS_STORAGE(self), NULL);
    MarmotGobjectStorageInterface *iface = MARMOT_GOBJECT_STORAGE_GET_IFACE(self);
    g_return_val_if_fail(iface->get_raw_storage != NULL, NULL);
    return iface->get_raw_storage(self);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectMemoryStorage
 * ══════════════════════════════════════════════════════════════════════════ */

struct _MarmotGobjectMemoryStorage {
    GObject parent_instance;
    MarmotStorage *storage;
};

static void marmot_gobject_memory_storage_iface_init(MarmotGobjectStorageInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MarmotGobjectMemoryStorage,
                         marmot_gobject_memory_storage,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE(MARMOT_GOBJECT_TYPE_STORAGE,
                                              marmot_gobject_memory_storage_iface_init))

static gpointer
memory_storage_get_raw(MarmotGobjectStorage *self)
{
    MarmotGobjectMemoryStorage *mem_storage = MARMOT_GOBJECT_MEMORY_STORAGE(self);
    MarmotStorage *raw = mem_storage->storage;
    /* Transfer ownership: marmot_new() will take ownership of this pointer.
     * Set to NULL to prevent double-free in finalizer. */
    mem_storage->storage = NULL;
    return raw;
}

static void
marmot_gobject_memory_storage_iface_init(MarmotGobjectStorageInterface *iface)
{
    iface->get_raw_storage = memory_storage_get_raw;
}

static void
marmot_gobject_memory_storage_finalize(GObject *object)
{
    MarmotGobjectMemoryStorage *self = MARMOT_GOBJECT_MEMORY_STORAGE(object);
    /* Only free if ownership wasn't transferred via get_raw_storage() */
    if (self->storage) {
        marmot_storage_free(self->storage);
        self->storage = NULL;
    }
    G_OBJECT_CLASS(marmot_gobject_memory_storage_parent_class)->finalize(object);
}

static void
marmot_gobject_memory_storage_class_init(MarmotGobjectMemoryStorageClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = marmot_gobject_memory_storage_finalize;
}

static void
marmot_gobject_memory_storage_init(MarmotGobjectMemoryStorage *self)
{
    self->storage = marmot_storage_memory_new();
}

MarmotGobjectMemoryStorage *
marmot_gobject_memory_storage_new(void)
{
    return g_object_new(MARMOT_GOBJECT_TYPE_MEMORY_STORAGE, NULL);
}

/* ══════════════════════════════════════════════════════════════════════════
 * MarmotGobjectSqliteStorage
 * ══════════════════════════════════════════════════════════════════════════ */

struct _MarmotGobjectSqliteStorage {
    GObject parent_instance;
    MarmotStorage *storage;
};

static void marmot_gobject_sqlite_storage_iface_init(MarmotGobjectStorageInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MarmotGobjectSqliteStorage,
                         marmot_gobject_sqlite_storage,
                         G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE(MARMOT_GOBJECT_TYPE_STORAGE,
                                              marmot_gobject_sqlite_storage_iface_init))

static gpointer
sqlite_storage_get_raw(MarmotGobjectStorage *self)
{
    MarmotGobjectSqliteStorage *sql_storage = MARMOT_GOBJECT_SQLITE_STORAGE(self);
    MarmotStorage *raw = sql_storage->storage;
    /* Transfer ownership: marmot_new() will take ownership of this pointer.
     * Set to NULL to prevent double-free in finalizer. */
    sql_storage->storage = NULL;
    return raw;
}

static void
marmot_gobject_sqlite_storage_iface_init(MarmotGobjectStorageInterface *iface)
{
    iface->get_raw_storage = sqlite_storage_get_raw;
}

static void
marmot_gobject_sqlite_storage_finalize(GObject *object)
{
    MarmotGobjectSqliteStorage *self = MARMOT_GOBJECT_SQLITE_STORAGE(object);
    /* Only free if ownership wasn't transferred via get_raw_storage() */
    if (self->storage) {
        marmot_storage_free(self->storage);
        self->storage = NULL;
    }
    G_OBJECT_CLASS(marmot_gobject_sqlite_storage_parent_class)->finalize(object);
}

static void
marmot_gobject_sqlite_storage_class_init(MarmotGobjectSqliteStorageClass *klass)
{
    G_OBJECT_CLASS(klass)->finalize = marmot_gobject_sqlite_storage_finalize;
}

static void
marmot_gobject_sqlite_storage_init(MarmotGobjectSqliteStorage *self)
{
    self->storage = NULL;
}

#define MARMOT_GOBJECT_STORAGE_ERROR (marmot_gobject_storage_error_quark())

typedef enum {
    MARMOT_GOBJECT_STORAGE_ERROR_FAILED = 1,
} MarmotGobjectStorageErrorCode;

static GQuark marmot_gobject_storage_error_quark(void) {
    return g_quark_from_static_string("marmot-gobject-storage-error");
}

MarmotGobjectSqliteStorage *
marmot_gobject_sqlite_storage_new(const gchar *path,
                                    const gchar *encryption_key,
                                    GError **error)
{
    g_return_val_if_fail(path != NULL, NULL);

    MarmotStorage *storage = marmot_storage_sqlite_new(path, encryption_key);
    if (!storage) {
        g_set_error(error, MARMOT_GOBJECT_STORAGE_ERROR,
                    MARMOT_GOBJECT_STORAGE_ERROR_FAILED,
                    "Failed to create SQLite storage at %s", path);
        return NULL;
    }

    MarmotGobjectSqliteStorage *self = g_object_new(MARMOT_GOBJECT_TYPE_SQLITE_STORAGE, NULL);
    self->storage = storage;
    return self;
}
