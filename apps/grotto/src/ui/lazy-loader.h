/* lazy-loader.h - Lazy loading infrastructure for UI components
 *
 * Provides deferred initialization of heavy UI components to improve
 * startup time and reduce initial memory footprint.
 *
 * Features:
 * - Lazy instantiation of widgets on first access
 * - Background pre-loading after initial render
 * - Memory-aware unloading of unused components
 * - Integration with GTK/Adwaita lifecycle
 *
 * Usage:
 * 1. Register lazy components at startup
 * 2. Call gn_lazy_get() to instantiate on demand
 * 3. Components auto-unload after timeout if not visible
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef GNOSTR_LAZY_LOADER_H
#define GNOSTR_LAZY_LOADER_H

#include <gtk/gtk.h>
#include <adwaita.h>

G_BEGIN_DECLS

/* Lazy component identifier */
typedef enum {
  GN_LAZY_PAGE_PERMISSIONS,
  GN_LAZY_PAGE_APPLICATIONS,
  GN_LAZY_PAGE_SESSIONS,
  GN_LAZY_PAGE_SETTINGS,
  GN_LAZY_ONBOARDING,
  GN_LAZY_PROFILE_DASHBOARD,
  GN_LAZY_EVENTS_PAGE,
  GN_LAZY_SHEET_CREATE_PROFILE,
  GN_LAZY_SHEET_IMPORT_PROFILE,
  GN_LAZY_SHEET_BACKUP,
  GN_LAZY_MAX
} GnLazyComponent;

/* Component state */
typedef enum {
  GN_LAZY_STATE_UNLOADED,
  GN_LAZY_STATE_LOADING,
  GN_LAZY_STATE_LOADED,
  GN_LAZY_STATE_ERROR
} GnLazyState;

/* Factory function type for creating widgets */
typedef GtkWidget *(*GnLazyFactory)(void);

/* Callback when async load completes */
typedef void (*GnLazyCallback)(GnLazyComponent id, GtkWidget *widget, gpointer user_data);

/* Configuration for a lazy component */
typedef struct {
  GnLazyComponent id;
  const gchar *name;
  GnLazyFactory factory;
  guint unload_timeout_sec;   /* 0 = never unload */
  gboolean preload_on_idle;   /* Preload after startup */
  gsize estimated_size;       /* Estimated memory footprint */
} GnLazyConfig;

/**
 * gn_lazy_init:
 *
 * Initialize the lazy loading system. Call once at startup.
 */
void gn_lazy_init(void);

/**
 * gn_lazy_shutdown:
 *
 * Shutdown and free all lazy-loaded components.
 */
void gn_lazy_shutdown(void);

/**
 * gn_lazy_register:
 * @config: Component configuration
 *
 * Register a lazy-loadable component.
 */
void gn_lazy_register(const GnLazyConfig *config);

/**
 * gn_lazy_get:
 * @id: Component ID to get
 *
 * Get a lazy-loaded component, instantiating if needed.
 * This is synchronous - use gn_lazy_get_async() for heavy components.
 *
 * Returns: (transfer none): The widget, or %NULL on error
 */
GtkWidget *gn_lazy_get(GnLazyComponent id);

/**
 * gn_lazy_get_async:
 * @id: Component ID to get
 * @callback: Function to call when ready
 * @user_data: Data for the callback
 *
 * Get a lazy-loaded component asynchronously.
 */
void gn_lazy_get_async(GnLazyComponent id,
                       GnLazyCallback callback,
                       gpointer user_data);

/**
 * gn_lazy_preload:
 * @id: Component ID to preload
 *
 * Schedule a component for background preloading.
 */
void gn_lazy_preload(GnLazyComponent id);

/**
 * gn_lazy_preload_all:
 *
 * Schedule all registered components for background preloading.
 */
void gn_lazy_preload_all(void);

/**
 * gn_lazy_unload:
 * @id: Component ID to unload
 *
 * Unload a component to free memory.
 */
void gn_lazy_unload(GnLazyComponent id);

/**
 * gn_lazy_unload_unused:
 * @max_age_sec: Maximum age of last access
 *
 * Unload components not accessed within the specified time.
 *
 * Returns: Number of components unloaded
 */
guint gn_lazy_unload_unused(guint max_age_sec);

/**
 * gn_lazy_get_state:
 * @id: Component ID
 *
 * Get the current state of a component.
 *
 * Returns: Component state
 */
GnLazyState gn_lazy_get_state(GnLazyComponent id);

/**
 * gn_lazy_touch:
 * @id: Component ID
 *
 * Update the last-access time for a component (prevents unloading).
 */
void gn_lazy_touch(GnLazyComponent id);

/**
 * gn_lazy_get_memory_usage:
 *
 * Get total estimated memory used by loaded components.
 *
 * Returns: Bytes used
 */
gsize gn_lazy_get_memory_usage(void);

/**
 * gn_lazy_component_name:
 * @id: Component ID
 *
 * Get the name of a component for logging.
 *
 * Returns: Component name
 */
const gchar *gn_lazy_component_name(GnLazyComponent id);

G_END_DECLS

#endif /* GNOSTR_LAZY_LOADER_H */
