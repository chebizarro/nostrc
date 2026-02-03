/* SPDX-License-Identifier: GPL-3.0-or-later
 * gnostr-plugin-api.h - Public Plugin API for Gnostr NIP Modules
 *
 * This header defines the stable plugin API for extending Gnostr with
 * NIP implementations and custom features. Plugins are loaded via libpeas 2.
 *
 * API Version: 1.0
 * Stability: Stable API functions are marked @stability: Stable
 *            Experimental functions are marked @stability: Experimental
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#ifndef GNOSTR_PLUGIN_API_H
#define GNOSTR_PLUGIN_API_H

#include <glib-object.h>
#include <gio/gio.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* ============================================================================
 * API VERSION
 * ============================================================================
 * Check these at plugin load to ensure compatibility.
 */

#define GNOSTR_PLUGIN_API_MAJOR_VERSION 1
#define GNOSTR_PLUGIN_API_MINOR_VERSION 0

/**
 * gnostr_plugin_api_check_version:
 * @required_major: Major version the plugin requires
 * @required_minor: Minor version the plugin requires
 *
 * Check if the host API version is compatible with plugin requirements.
 * Major version must match exactly. Minor version must be >= required.
 *
 * Returns: %TRUE if compatible, %FALSE otherwise
 * @stability: Stable
 */
gboolean gnostr_plugin_api_check_version(guint required_major, guint required_minor);

/* ============================================================================
 * FORWARD DECLARATIONS
 * ============================================================================
 */

/* Opaque context provided to plugins - do not access fields directly */
typedef struct _GnostrPluginContext GnostrPluginContext;

/* Opaque event wrapper - use accessor functions */
typedef struct _GnostrPluginEvent GnostrPluginEvent;

/* Forward declaration for relay pool (internal use) */
typedef struct _GnostrSimplePool GnostrSimplePool;

/* ============================================================================
 * GNOSTR_PLUGIN INTERFACE
 * ============================================================================
 * Base interface that all plugins must implement.
 * Provides lifecycle management (activate/deactivate).
 */

#define GNOSTR_TYPE_PLUGIN (gnostr_plugin_get_type())
G_DECLARE_INTERFACE(GnostrPlugin, gnostr_plugin, GNOSTR, PLUGIN, GObject)

/**
 * GnostrPluginInterface:
 * @parent_iface: Parent interface
 * @activate: Called when plugin is activated. Setup event handlers, UI, etc.
 * @deactivate: Called when plugin is deactivated. Cleanup resources.
 * @get_name: Return plugin display name (human readable)
 * @get_description: Return plugin description
 * @get_authors: Return NULL-terminated array of author strings
 * @get_version: Return plugin version string
 * @get_supported_kinds: Return NULL-terminated array of event kinds handled
 *
 * Virtual function table for the GnostrPlugin interface.
 */
struct _GnostrPluginInterface
{
  GTypeInterface parent_iface;

  /* Required: Lifecycle */
  void        (*activate)           (GnostrPlugin        *plugin,
                                     GnostrPluginContext *context);
  void        (*deactivate)         (GnostrPlugin        *plugin,
                                     GnostrPluginContext *context);

  /* Required: Metadata */
  const char *(*get_name)           (GnostrPlugin *plugin);
  const char *(*get_description)    (GnostrPlugin *plugin);

  /* Optional: Extended metadata */
  const char *const *(*get_authors) (GnostrPlugin *plugin);
  const char *(*get_version)        (GnostrPlugin *plugin);

  /* Optional: Event handling declaration */
  const int  *(*get_supported_kinds)(GnostrPlugin *plugin,
                                     gsize        *n_kinds);

  /*< private >*/
  gpointer _reserved[8];
};

/**
 * gnostr_plugin_activate:
 * @plugin: A #GnostrPlugin
 * @context: The #GnostrPluginContext providing access to host services
 *
 * Activate the plugin. Called when the plugin is loaded and enabled.
 * Plugins should register event handlers, add UI elements, etc.
 *
 * @stability: Stable
 */
void gnostr_plugin_activate(GnostrPlugin *plugin, GnostrPluginContext *context);

/**
 * gnostr_plugin_deactivate:
 * @plugin: A #GnostrPlugin
 * @context: The #GnostrPluginContext
 *
 * Deactivate the plugin. Called when the plugin is disabled or unloaded.
 * Plugins must unregister all handlers and free resources.
 *
 * @stability: Stable
 */
void gnostr_plugin_deactivate(GnostrPlugin *plugin, GnostrPluginContext *context);

/**
 * gnostr_plugin_get_name:
 * @plugin: A #GnostrPlugin
 *
 * Get the human-readable name of the plugin.
 *
 * Returns: (transfer none): Plugin name string
 * @stability: Stable
 */
const char *gnostr_plugin_get_name(GnostrPlugin *plugin);

/**
 * gnostr_plugin_get_description:
 * @plugin: A #GnostrPlugin
 *
 * Get the description of the plugin.
 *
 * Returns: (transfer none): Plugin description string
 * @stability: Stable
 */
const char *gnostr_plugin_get_description(GnostrPlugin *plugin);

/**
 * gnostr_plugin_get_authors:
 * @plugin: A #GnostrPlugin
 *
 * Get the list of plugin authors.
 *
 * Returns: (transfer none) (array zero-terminated=1) (nullable):
 *          NULL-terminated array of author strings, or %NULL
 * @stability: Stable
 */
const char *const *gnostr_plugin_get_authors(GnostrPlugin *plugin);

/**
 * gnostr_plugin_get_version:
 * @plugin: A #GnostrPlugin
 *
 * Get the version string of the plugin.
 *
 * Returns: (transfer none) (nullable): Version string, or %NULL
 * @stability: Stable
 */
const char *gnostr_plugin_get_version(GnostrPlugin *plugin);

/**
 * gnostr_plugin_get_supported_kinds:
 * @plugin: A #GnostrPlugin
 * @n_kinds: (out): Location to store number of kinds
 *
 * Get the array of event kinds this plugin handles.
 * Used by the host to route events to appropriate plugins.
 *
 * Returns: (transfer none) (array length=n_kinds) (nullable):
 *          Array of event kind integers, or %NULL if plugin doesn't handle events
 * @stability: Stable
 */
const int *gnostr_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds);

/* ============================================================================
 * GNOSTR_EVENT_HANDLER INTERFACE
 * ============================================================================
 * Optional interface for plugins that process Nostr events.
 */

#define GNOSTR_TYPE_EVENT_HANDLER (gnostr_event_handler_get_type())
G_DECLARE_INTERFACE(GnostrEventHandler, gnostr_event_handler, GNOSTR, EVENT_HANDLER, GObject)

/**
 * GnostrEventHandlerInterface:
 * @parent_iface: Parent interface
 * @handle_event: Process an incoming event
 * @can_handle_kind: Check if handler supports a specific kind
 *
 * Interface for plugins that process Nostr events.
 */
struct _GnostrEventHandlerInterface
{
  GTypeInterface parent_iface;

  /**
   * handle_event:
   * @handler: The event handler
   * @context: Plugin context
   * @event: The event to process
   *
   * Process an incoming Nostr event. Called on the main thread.
   *
   * Returns: %TRUE if event was handled, %FALSE to pass to other handlers
   */
  gboolean (*handle_event)    (GnostrEventHandler  *handler,
                               GnostrPluginContext *context,
                               GnostrPluginEvent   *event);

  /**
   * can_handle_kind:
   * @handler: The event handler
   * @kind: Event kind to check
   *
   * Check if this handler processes events of the given kind.
   *
   * Returns: %TRUE if handler supports this kind
   */
  gboolean (*can_handle_kind) (GnostrEventHandler *handler,
                               int                 kind);

  /*< private >*/
  gpointer _reserved[4];
};

/**
 * gnostr_event_handler_handle_event:
 * @handler: A #GnostrEventHandler
 * @context: The #GnostrPluginContext
 * @event: The #GnostrPluginEvent to process
 *
 * Process an incoming event.
 *
 * Returns: %TRUE if handled, %FALSE to pass to other handlers
 * @stability: Stable
 */
gboolean gnostr_event_handler_handle_event(GnostrEventHandler  *handler,
                                           GnostrPluginContext *context,
                                           GnostrPluginEvent   *event);

/**
 * gnostr_event_handler_can_handle_kind:
 * @handler: A #GnostrEventHandler
 * @kind: Event kind to check
 *
 * Check if this handler processes events of the given kind.
 *
 * Returns: %TRUE if handler supports this kind
 * @stability: Stable
 */
gboolean gnostr_event_handler_can_handle_kind(GnostrEventHandler *handler, int kind);

/* ============================================================================
 * GNOSTR_UI_EXTENSION INTERFACE
 * ============================================================================
 * Optional interface for plugins that extend the user interface.
 */

#define GNOSTR_TYPE_UI_EXTENSION (gnostr_ui_extension_get_type())
G_DECLARE_INTERFACE(GnostrUIExtension, gnostr_ui_extension, GNOSTR, UI_EXTENSION, GObject)

/**
 * GnostrUIExtensionPoint:
 * @GNOSTR_UI_EXTENSION_MENU_APP: Application menu
 * @GNOSTR_UI_EXTENSION_MENU_NOTE: Note context menu (right-click)
 * @GNOSTR_UI_EXTENSION_MENU_PROFILE: Profile context menu
 * @GNOSTR_UI_EXTENSION_TOOLBAR: Main toolbar
 * @GNOSTR_UI_EXTENSION_SIDEBAR: Navigation sidebar
 * @GNOSTR_UI_EXTENSION_SETTINGS: Settings dialog
 * @GNOSTR_UI_EXTENSION_NOTE_CARD: Note card content (below text)
 * @GNOSTR_UI_EXTENSION_PROFILE_HEADER: Profile header area
 *
 * Predefined UI extension points where plugins can add content.
 */
typedef enum
{
  GNOSTR_UI_EXTENSION_MENU_APP,
  GNOSTR_UI_EXTENSION_MENU_NOTE,
  GNOSTR_UI_EXTENSION_MENU_PROFILE,
  GNOSTR_UI_EXTENSION_TOOLBAR,
  GNOSTR_UI_EXTENSION_SIDEBAR,
  GNOSTR_UI_EXTENSION_SETTINGS,
  GNOSTR_UI_EXTENSION_NOTE_CARD,
  GNOSTR_UI_EXTENSION_PROFILE_HEADER,
} GnostrUIExtensionPoint;

/**
 * GnostrUIExtensionInterface:
 * @parent_iface: Parent interface
 * @create_menu_items: Create menu items for an extension point
 * @create_settings_page: Create settings page widget
 * @create_note_decoration: Create decoration widget for a note card
 *
 * Interface for plugins that extend the user interface.
 */
struct _GnostrUIExtensionInterface
{
  GTypeInterface parent_iface;

  /**
   * create_menu_items:
   * @extension: The UI extension
   * @context: Plugin context
   * @point: Which menu to extend
   * @target_data: (nullable): Context data (event/profile for context menus)
   *
   * Create menu items for the specified extension point.
   *
   * Returns: (transfer full) (element-type GMenuItem) (nullable):
   *          List of GMenuItem objects to add, or %NULL
   */
  GList *(*create_menu_items)     (GnostrUIExtension     *extension,
                                   GnostrPluginContext   *context,
                                   GnostrUIExtensionPoint point,
                                   gpointer               target_data);

  /**
   * create_settings_page:
   * @extension: The UI extension
   * @context: Plugin context
   *
   * Create a settings page widget for the plugin preferences.
   *
   * Returns: (transfer full) (nullable): Settings page widget, or %NULL
   */
  GtkWidget *(*create_settings_page)(GnostrUIExtension   *extension,
                                     GnostrPluginContext *context);

  /**
   * create_note_decoration:
   * @extension: The UI extension
   * @context: Plugin context
   * @event: The note event
   *
   * Create a decoration widget to display below a note card.
   * Used for NIP-specific displays (badges, highlights, reactions, etc.)
   *
   * Returns: (transfer full) (nullable): Decoration widget, or %NULL
   */
  GtkWidget *(*create_note_decoration)(GnostrUIExtension   *extension,
                                       GnostrPluginContext *context,
                                       GnostrPluginEvent   *event);

  /*< private >*/
  gpointer _reserved[4];
};

/**
 * gnostr_ui_extension_create_menu_items:
 * @extension: A #GnostrUIExtension
 * @context: The #GnostrPluginContext
 * @point: The extension point
 * @target_data: (nullable): Context-specific data
 *
 * Create menu items for the specified extension point.
 *
 * Returns: (transfer full) (element-type GMenuItem) (nullable): Menu items
 * @stability: Stable
 */
GList *gnostr_ui_extension_create_menu_items(GnostrUIExtension     *extension,
                                             GnostrPluginContext   *context,
                                             GnostrUIExtensionPoint point,
                                             gpointer               target_data);

/**
 * gnostr_ui_extension_create_settings_page:
 * @extension: A #GnostrUIExtension
 * @context: The #GnostrPluginContext
 *
 * Create the plugin's settings page widget.
 *
 * Returns: (transfer full) (nullable): Settings widget
 * @stability: Stable
 */
GtkWidget *gnostr_ui_extension_create_settings_page(GnostrUIExtension   *extension,
                                                    GnostrPluginContext *context);

/**
 * gnostr_ui_extension_create_note_decoration:
 * @extension: A #GnostrUIExtension
 * @context: The #GnostrPluginContext
 * @event: The note event
 *
 * Create a decoration widget for a note card.
 *
 * Returns: (transfer full) (nullable): Decoration widget
 * @stability: Stable
 */
GtkWidget *gnostr_ui_extension_create_note_decoration(GnostrUIExtension   *extension,
                                                      GnostrPluginContext *context,
                                                      GnostrPluginEvent   *event);

/* ============================================================================
 * PLUGIN CONTEXT API
 * ============================================================================
 * Functions for accessing host services from plugins.
 * The context is provided during activate/deactivate and event handling.
 */

/* --- Context Lifecycle (Host internal use) --- */

/**
 * gnostr_plugin_context_new:
 * @app: The #GtkApplication
 * @plugin_id: Plugin identifier for namespacing
 *
 * Create a new plugin context. For host internal use.
 *
 * Returns: (transfer full): A new #GnostrPluginContext
 * @stability: Private
 */
GnostrPluginContext *gnostr_plugin_context_new(GtkApplication *app, const char *plugin_id);

/**
 * gnostr_plugin_context_free:
 * @context: A #GnostrPluginContext
 *
 * Free a plugin context. For host internal use.
 *
 * @stability: Private
 */
void gnostr_plugin_context_free(GnostrPluginContext *context);

/**
 * gnostr_plugin_context_set_main_window:
 * @context: A #GnostrPluginContext
 * @window: The main #GtkWindow
 *
 * Set the main window on a context. For host internal use.
 *
 * @stability: Private
 */
void gnostr_plugin_context_set_main_window(GnostrPluginContext *context, GtkWindow *window);

/**
 * gnostr_plugin_context_set_pool:
 * @context: A #GnostrPluginContext
 * @pool: A #GnostrSimplePool (or compatible GObject)
 *
 * Set the relay pool on a context. For host internal use.
 *
 * @stability: Private
 */
void gnostr_plugin_context_set_pool(GnostrPluginContext *context, GnostrSimplePool *pool);

/* --- Application Access --- */

/**
 * gnostr_plugin_context_get_application:
 * @context: A #GnostrPluginContext
 *
 * Get the main GtkApplication instance.
 *
 * Returns: (transfer none): The application
 * @stability: Stable
 */
GtkApplication *gnostr_plugin_context_get_application(GnostrPluginContext *context);

/**
 * gnostr_plugin_context_get_main_window:
 * @context: A #GnostrPluginContext
 *
 * Get the main application window.
 *
 * Returns: (transfer none) (nullable): The main window, or %NULL if not yet created
 * @stability: Stable
 */
GtkWindow *gnostr_plugin_context_get_main_window(GnostrPluginContext *context);

/* --- Repository Browser (NIP-34) --- */

/**
 * gnostr_plugin_context_add_repository:
 * @context: A #GnostrPluginContext
 * @id: Repository ID (d-tag from event)
 * @name: (nullable): Repository name
 * @description: (nullable): Repository description
 * @clone_url: (nullable): Git clone URL
 * @web_url: (nullable): Web interface URL
 * @maintainer_pubkey: (nullable): Primary maintainer pubkey (hex)
 * @updated_at: Last update timestamp (Unix seconds)
 *
 * Add or update a repository in the main repository browser.
 * Used by NIP-34 plugins to push discovered repositories to the UI.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_add_repository(GnostrPluginContext *context,
                                          const char          *id,
                                          const char          *name,
                                          const char          *description,
                                          const char          *clone_url,
                                          const char          *web_url,
                                          const char          *maintainer_pubkey,
                                          gint64               updated_at);

/**
 * gnostr_plugin_context_clear_repositories:
 * @context: A #GnostrPluginContext
 *
 * Clear all repositories from the main repository browser.
 * Typically called before a refresh operation.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_clear_repositories(GnostrPluginContext *context);

/* --- Network Access --- */

/**
 * gnostr_plugin_context_get_pool:
 * @context: A #GnostrPluginContext
 *
 * Get the shared relay pool for network operations.
 * Do not destroy this object.
 *
 * Returns: (transfer none): The relay pool
 * @stability: Stable
 */
GObject *gnostr_plugin_context_get_pool(GnostrPluginContext *context);

/**
 * gnostr_plugin_context_get_relay_urls:
 * @context: A #GnostrPluginContext
 * @n_urls: (out): Number of URLs returned
 *
 * Get the user's configured relay URLs.
 *
 * Returns: (transfer full) (array length=n_urls): Array of relay URL strings.
 *          Free with g_strfreev().
 * @stability: Stable
 */
char **gnostr_plugin_context_get_relay_urls(GnostrPluginContext *context, gsize *n_urls);

/**
 * gnostr_plugin_context_publish_event:
 * @context: A #GnostrPluginContext
 * @event_json: The event as a JSON string (must be signed)
 * @error: (out) (optional): Return location for error
 *
 * Publish an event to the user's write relays.
 * The event must already be signed.
 *
 * Returns: %TRUE on success, %FALSE on error
 * @stability: Stable
 */
gboolean gnostr_plugin_context_publish_event(GnostrPluginContext *context,
                                             const char          *event_json,
                                             GError             **error);

/**
 * gnostr_plugin_context_publish_event_async:
 * @context: A #GnostrPluginContext
 * @event_json: The event as a JSON string (must be signed)
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when complete
 * @user_data: User data for callback
 *
 * Asynchronously publish an event to the user's write relays.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_publish_event_async(GnostrPluginContext *context,
                                               const char          *event_json,
                                               GCancellable        *cancellable,
                                               GAsyncReadyCallback  callback,
                                               gpointer             user_data);

gboolean gnostr_plugin_context_publish_event_finish(GnostrPluginContext *context,
                                                    GAsyncResult        *result,
                                                    GError             **error);

/**
 * gnostr_plugin_context_request_relay_events_async:
 * @context: A #GnostrPluginContext
 * @kinds: Array of event kinds to request
 * @n_kinds: Number of kinds in array
 * @limit: Maximum number of events to fetch (0 for relay default)
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when request completes
 * @user_data: User data for callback
 *
 * Request events of the specified kinds from configured relays.
 * Events are streamed into local storage as they arrive.
 * Plugin subscriptions will fire when events are ingested.
 *
 * This is useful for on-demand fetching of event types not included
 * in the main subscription (e.g., NIP-34 repository events).
 *
 * @stability: Stable
 */
void gnostr_plugin_context_request_relay_events_async(GnostrPluginContext *context,
                                                      const int           *kinds,
                                                      gsize                n_kinds,
                                                      int                  limit,
                                                      GCancellable        *cancellable,
                                                      GAsyncReadyCallback  callback,
                                                      gpointer             user_data);

gboolean gnostr_plugin_context_request_relay_events_finish(GnostrPluginContext *context,
                                                           GAsyncResult        *result,
                                                           GError             **error);

/* --- Storage Access --- */

/**
 * gnostr_plugin_context_query_events:
 * @context: A #GnostrPluginContext
 * @filter_json: NIP-01 filter as JSON string
 * @error: (out) (optional): Return location for error
 *
 * Query events from local storage matching the filter.
 *
 * Returns: (transfer full) (element-type utf8) (nullable):
 *          Array of event JSON strings, free with g_ptr_array_unref()
 * @stability: Stable
 */
GPtrArray *gnostr_plugin_context_query_events(GnostrPluginContext *context,
                                              const char          *filter_json,
                                              GError             **error);

/**
 * gnostr_plugin_context_get_event_by_id:
 * @context: A #GnostrPluginContext
 * @event_id_hex: 64-character hex event ID
 * @error: (out) (optional): Return location for error
 *
 * Get a single event by ID from local storage.
 *
 * Returns: (transfer full) (nullable): Event JSON string, or %NULL if not found.
 *          Free with g_free().
 * @stability: Stable
 */
char *gnostr_plugin_context_get_event_by_id(GnostrPluginContext *context,
                                            const char          *event_id_hex,
                                            GError             **error);

/**
 * gnostr_plugin_context_subscribe_events:
 * @context: A #GnostrPluginContext
 * @filter_json: NIP-01 filter as JSON string
 * @callback: Callback invoked when new events match
 * @user_data: User data for callback
 * @destroy_notify: (nullable): Destroy function for user_data
 *
 * Subscribe to storage notifications for events matching the filter.
 * Callback is invoked on the main thread when new events arrive.
 *
 * Returns: Subscription ID (>0), or 0 on failure.
 *          Use gnostr_plugin_context_unsubscribe_events() to cancel.
 * @stability: Stable
 */
guint64 gnostr_plugin_context_subscribe_events(GnostrPluginContext *context,
                                               const char          *filter_json,
                                               GCallback            callback,
                                               gpointer             user_data,
                                               GDestroyNotify       destroy_notify);

/**
 * gnostr_plugin_context_unsubscribe_events:
 * @context: A #GnostrPluginContext
 * @subscription_id: ID returned by subscribe_events
 *
 * Cancel an event subscription.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_unsubscribe_events(GnostrPluginContext *context,
                                              guint64              subscription_id);

/* --- Plugin Data Storage --- */

/**
 * gnostr_plugin_context_store_data:
 * @context: A #GnostrPluginContext
 * @key: Storage key (plugin-namespaced automatically)
 * @data: Data to store
 * @error: (out) (optional): Return location for error
 *
 * Store plugin-specific data. Data is persisted across sessions.
 * Keys are automatically namespaced by plugin ID.
 *
 * Returns: %TRUE on success
 * @stability: Stable
 */
gboolean gnostr_plugin_context_store_data(GnostrPluginContext *context,
                                          const char          *key,
                                          GBytes              *data,
                                          GError             **error);

/**
 * gnostr_plugin_context_load_data:
 * @context: A #GnostrPluginContext
 * @key: Storage key
 * @error: (out) (optional): Return location for error
 *
 * Load plugin-specific data.
 *
 * Returns: (transfer full) (nullable): Data bytes, or %NULL if not found.
 *          Free with g_bytes_unref().
 * @stability: Stable
 */
GBytes *gnostr_plugin_context_load_data(GnostrPluginContext *context,
                                        const char          *key,
                                        GError             **error);

/**
 * gnostr_plugin_context_delete_data:
 * @context: A #GnostrPluginContext
 * @key: Storage key
 *
 * Delete plugin-specific data.
 *
 * Returns: %TRUE if data was deleted, %FALSE if not found
 * @stability: Stable
 */
gboolean gnostr_plugin_context_delete_data(GnostrPluginContext *context,
                                           const char          *key);

/* --- Settings Access --- */

/**
 * gnostr_plugin_context_get_settings:
 * @context: A #GnostrPluginContext
 * @schema_id: GSettings schema ID (must be installed)
 *
 * Get a GSettings instance for the plugin's schema.
 * The schema must be installed in the standard locations.
 *
 * Returns: (transfer full) (nullable): GSettings instance, or %NULL if schema not found.
 *          Free with g_object_unref().
 * @stability: Stable
 */
GSettings *gnostr_plugin_context_get_settings(GnostrPluginContext *context,
                                              const char          *schema_id);

/* --- User Identity --- */

/**
 * gnostr_plugin_context_get_user_pubkey:
 * @context: A #GnostrPluginContext
 *
 * Get the current user's public key (hex encoded).
 *
 * Returns: (transfer none) (nullable): User's pubkey hex string, or %NULL if not logged in
 * @stability: Stable
 */
const char *gnostr_plugin_context_get_user_pubkey(GnostrPluginContext *context);

/**
 * gnostr_plugin_context_is_logged_in:
 * @context: A #GnostrPluginContext
 *
 * Check if a user is currently logged in.
 *
 * Returns: %TRUE if logged in
 * @stability: Stable
 */
gboolean gnostr_plugin_context_is_logged_in(GnostrPluginContext *context);

/**
 * gnostr_plugin_context_request_sign_event:
 * @context: A #GnostrPluginContext
 * @unsigned_event_json: Unsigned event JSON (id and sig fields will be set)
 * @cancellable: (nullable): A #GCancellable
 * @callback: Callback when signing completes
 * @user_data: User data for callback
 *
 * Request the signer to sign an event. This may prompt the user.
 * The callback receives the signed event JSON or an error.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_request_sign_event(GnostrPluginContext *context,
                                              const char          *unsigned_event_json,
                                              GCancellable        *cancellable,
                                              GAsyncReadyCallback  callback,
                                              gpointer             user_data);

char *gnostr_plugin_context_request_sign_event_finish(GnostrPluginContext *context,
                                                      GAsyncResult        *result,
                                                      GError             **error);

/* --- Action Handlers --- */

/**
 * GnostrPluginActionFunc:
 * @context: The plugin context
 * @action_name: Name of the action being invoked
 * @parameter: (nullable): Action parameter as GVariant (may be NULL)
 * @user_data: User data provided at registration
 *
 * Callback type for plugin action handlers.
 * Actions allow the host application to invoke plugin functionality.
 */
typedef void (*GnostrPluginActionFunc)(GnostrPluginContext *context,
                                        const char          *action_name,
                                        GVariant            *parameter,
                                        gpointer             user_data);

/**
 * gnostr_plugin_context_register_action:
 * @context: A #GnostrPluginContext
 * @action_name: Action name (unique within this plugin)
 * @callback: Handler function
 * @user_data: (nullable): User data for callback
 *
 * Register an action handler that can be invoked by the host application.
 * Use this to expose plugin functionality that the host can trigger.
 *
 * Example: A git plugin might register "open-git-client" action.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_register_action(GnostrPluginContext    *context,
                                            const char             *action_name,
                                            GnostrPluginActionFunc  callback,
                                            gpointer                user_data);

/**
 * gnostr_plugin_context_unregister_action:
 * @context: A #GnostrPluginContext
 * @action_name: Action name to unregister
 *
 * Unregister a previously registered action handler.
 *
 * @stability: Stable
 */
void gnostr_plugin_context_unregister_action(GnostrPluginContext *context,
                                              const char          *action_name);

/**
 * gnostr_plugin_context_dispatch_action:
 * @context: A #GnostrPluginContext
 * @action_name: Action to dispatch
 * @parameter: (nullable): Action parameter
 *
 * Dispatch an action to this plugin context.
 * Called by host application or plugin manager.
 *
 * Returns: %TRUE if the action was handled, %FALSE if not registered
 * @stability: Private (for host/manager use)
 */
gboolean gnostr_plugin_context_dispatch_action(GnostrPluginContext *context,
                                                const char          *action_name,
                                                GVariant            *parameter);

/* ============================================================================
 * PLUGIN EVENT API
 * ============================================================================
 * Functions for accessing event data in a stable way.
 */

/* Forward declaration for internal event type */
struct _NostrEvent;
typedef struct _NostrEvent NostrEvent;

/**
 * gnostr_plugin_event_wrap:
 * @event: (transfer none): Internal NostrEvent to wrap
 *
 * Wrap an internal NostrEvent for plugin access.
 * The wrapper holds a borrowed reference - the caller retains ownership
 * of the underlying event.
 *
 * Returns: (transfer full) (nullable): New event wrapper, or %NULL
 * @stability: Private (for host use only)
 */
GnostrPluginEvent *gnostr_plugin_event_wrap(NostrEvent *event);

/**
 * gnostr_plugin_event_free:
 * @event: (transfer full) (nullable): Event wrapper to free
 *
 * Free an event wrapper. Does not free the underlying NostrEvent.
 *
 * @stability: Private (for host use only)
 */
void gnostr_plugin_event_free(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_id:
 * @event: A #GnostrPluginEvent
 *
 * Get the event ID (64-char hex).
 *
 * Returns: (transfer none): Event ID string
 * @stability: Stable
 */
const char *gnostr_plugin_event_get_id(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_pubkey:
 * @event: A #GnostrPluginEvent
 *
 * Get the author's public key (64-char hex).
 *
 * Returns: (transfer none): Author pubkey string
 * @stability: Stable
 */
const char *gnostr_plugin_event_get_pubkey(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_created_at:
 * @event: A #GnostrPluginEvent
 *
 * Get the event creation timestamp.
 *
 * Returns: Unix timestamp
 * @stability: Stable
 */
gint64 gnostr_plugin_event_get_created_at(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_kind:
 * @event: A #GnostrPluginEvent
 *
 * Get the event kind.
 *
 * Returns: Event kind integer
 * @stability: Stable
 */
int gnostr_plugin_event_get_kind(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_content:
 * @event: A #GnostrPluginEvent
 *
 * Get the event content string.
 *
 * Returns: (transfer none): Content string
 * @stability: Stable
 */
const char *gnostr_plugin_event_get_content(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_sig:
 * @event: A #GnostrPluginEvent
 *
 * Get the event signature (128-char hex).
 *
 * Returns: (transfer none): Signature string
 * @stability: Stable
 */
const char *gnostr_plugin_event_get_sig(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_tags_json:
 * @event: A #GnostrPluginEvent
 *
 * Get the event tags as a JSON array string.
 *
 * Returns: (transfer full): Tags as JSON string. Free with g_free().
 * @stability: Stable
 */
char *gnostr_plugin_event_get_tags_json(GnostrPluginEvent *event);

/**
 * gnostr_plugin_event_get_tag_value:
 * @event: A #GnostrPluginEvent
 * @tag_name: Single-character tag name (e.g., "e", "p", "t")
 * @index: Index of the tag (0 for first occurrence)
 *
 * Get the first value of a tag by name and occurrence index.
 *
 * Returns: (transfer none) (nullable): Tag value, or %NULL if not found
 * @stability: Stable
 */
const char *gnostr_plugin_event_get_tag_value(GnostrPluginEvent *event,
                                              const char        *tag_name,
                                              guint              index);

/**
 * gnostr_plugin_event_get_tag_values:
 * @event: A #GnostrPluginEvent
 * @tag_name: Single-character tag name
 *
 * Get all values for tags with the given name.
 *
 * Returns: (transfer full) (array zero-terminated=1) (nullable):
 *          NULL-terminated array of values. Free with g_strfreev().
 * @stability: Stable
 */
char **gnostr_plugin_event_get_tag_values(GnostrPluginEvent *event,
                                          const char        *tag_name);

/**
 * gnostr_plugin_event_to_json:
 * @event: A #GnostrPluginEvent
 *
 * Serialize the event to JSON.
 *
 * Returns: (transfer full): Event as JSON string. Free with g_free().
 * @stability: Stable
 */
char *gnostr_plugin_event_to_json(GnostrPluginEvent *event);

/* ============================================================================
 * ERROR DOMAIN
 * ============================================================================
 */

#define GNOSTR_PLUGIN_ERROR (gnostr_plugin_error_quark())
GQuark gnostr_plugin_error_quark(void);

/**
 * GnostrPluginError:
 * @GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN: User not logged in
 * @GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED: Signer refused the request
 * @GNOSTR_PLUGIN_ERROR_SIGNER_TIMEOUT: Signer request timed out
 * @GNOSTR_PLUGIN_ERROR_NETWORK: Network error
 * @GNOSTR_PLUGIN_ERROR_STORAGE: Storage error
 * @GNOSTR_PLUGIN_ERROR_INVALID_DATA: Invalid data provided
 * @GNOSTR_PLUGIN_ERROR_SCHEMA_NOT_FOUND: GSettings schema not found
 *
 * Error codes for plugin operations.
 */
typedef enum
{
  GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN,
  GNOSTR_PLUGIN_ERROR_SIGNER_REFUSED,
  GNOSTR_PLUGIN_ERROR_SIGNER_TIMEOUT,
  GNOSTR_PLUGIN_ERROR_NETWORK,
  GNOSTR_PLUGIN_ERROR_STORAGE,
  GNOSTR_PLUGIN_ERROR_INVALID_DATA,
  GNOSTR_PLUGIN_ERROR_SCHEMA_NOT_FOUND,
} GnostrPluginError;

/* ============================================================================
 * CONVENIENCE MACROS FOR PLUGIN AUTHORS
 * ============================================================================
 */

/**
 * GNOSTR_PLUGIN_REGISTER:
 * @TypeName: The GObject type name (e.g., MyNipPlugin)
 * @type_name: The type function prefix (e.g., my_nip_plugin)
 *
 * Convenience macro to register a plugin type with libpeas.
 * Use in your plugin's module init function.
 *
 * Example:
 * |[<!-- language="C" -->
 * G_MODULE_EXPORT void
 * peas_register_types(PeasObjectModule *module)
 * {
 *   GNOSTR_PLUGIN_REGISTER(MyNipPlugin, my_nip_plugin);
 * }
 * ]|
 */
#define GNOSTR_PLUGIN_REGISTER(TypeName, type_name) \
  peas_object_module_register_extension_type( \
    module, \
    GNOSTR_TYPE_PLUGIN, \
    type_name##_get_type())

/**
 * GNOSTR_PLUGIN_REGISTER_WITH_INTERFACES:
 * @TypeName: The GObject type name
 * @type_name: The type function prefix
 * @...: Additional interface types (e.g., GNOSTR_TYPE_EVENT_HANDLER)
 *
 * Register a plugin type with additional interfaces.
 *
 * Example:
 * |[<!-- language="C" -->
 * G_MODULE_EXPORT void
 * peas_register_types(PeasObjectModule *module)
 * {
 *   GNOSTR_PLUGIN_REGISTER_WITH_INTERFACES(MyNipPlugin, my_nip_plugin,
 *     GNOSTR_TYPE_EVENT_HANDLER,
 *     GNOSTR_TYPE_UI_EXTENSION);
 * }
 * ]|
 */
#define GNOSTR_PLUGIN_REGISTER_WITH_INTERFACES(TypeName, type_name, ...) \
  do { \
    GType _type = type_name##_get_type(); \
    peas_object_module_register_extension_type(module, GNOSTR_TYPE_PLUGIN, _type); \
    GType _ifaces[] = { __VA_ARGS__, G_TYPE_INVALID }; \
    for (int _i = 0; _ifaces[_i] != G_TYPE_INVALID; _i++) { \
      peas_object_module_register_extension_type(module, _ifaces[_i], _type); \
    } \
  } while (0)

G_END_DECLS

#endif /* GNOSTR_PLUGIN_API_H */
