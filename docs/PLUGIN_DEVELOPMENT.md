# Gnostr Plugin Development Guide

This guide covers developing plugins for Gnostr using the libpeas 2 plugin system.

## Overview

Gnostr plugins are GObject-based modules loaded by libpeas 2. Plugins can:

1. **Handle Events** - Process specific Nostr event kinds (NIPs)
2. **Extend UI** - Add menus, settings pages, note decorations
3. **Access Network** - Subscribe to relays, publish events
4. **Store Data** - Persist plugin-specific data across sessions

## API Version

The plugin API uses semantic versioning:

- **Major version** must match exactly
- **Minor version** must be >= what plugin requires

Check compatibility at plugin load:

```c
if (!gnostr_plugin_api_check_version(1, 0)) {
    g_warning("Plugin requires API 1.0, host incompatible");
    return;
}
```

## Plugin Structure

### Directory Layout

```
my-nip-plugin/
├── my-nip-plugin.plugin          # Plugin metadata (INI format)
├── libmy-nip-plugin.so           # Compiled plugin
├── my-nip-plugin.gschema.xml     # GSettings schema (optional)
└── meson.build                   # Build configuration
```

### Plugin Metadata File

```ini
[Plugin]
Module=my-nip-plugin
Loader=c
Name=My NIP Plugin
Description=Implements NIP-XX for feature Y
Authors=Your Name <your@email.com>
Copyright=Copyright © 2026 Your Name
Version=1.0.0
Website=https://github.com/you/my-nip-plugin
```

## Interfaces

### GnostrPlugin (Required)

All plugins must implement `GnostrPlugin`:

```c
#include <gnostr-plugin-api.h>

struct _MyNipPlugin {
    GObject parent_instance;
    /* Private data */
    guint64 subscription_id;
};

static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MyNipPlugin, my_nip_plugin, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

static void
my_nip_plugin_activate(GnostrPlugin *plugin, GnostrPluginContext *ctx)
{
    MyNipPlugin *self = MY_NIP_PLUGIN(plugin);

    /* Subscribe to events, setup UI, etc. */
    self->subscription_id = gnostr_plugin_context_subscribe_events(
        ctx, "{\"kinds\":[1984]}", /* NIP-56 Reports */
        G_CALLBACK(on_report_event), self, NULL);
}

static void
my_nip_plugin_deactivate(GnostrPlugin *plugin, GnostrPluginContext *ctx)
{
    MyNipPlugin *self = MY_NIP_PLUGIN(plugin);

    /* Cleanup */
    if (self->subscription_id > 0) {
        gnostr_plugin_context_unsubscribe_events(ctx, self->subscription_id);
        self->subscription_id = 0;
    }
}

static const char *
my_nip_plugin_get_name(GnostrPlugin *plugin)
{
    return "My NIP Plugin";
}

static const char *
my_nip_plugin_get_description(GnostrPlugin *plugin)
{
    return "Implements NIP-XX for doing Y";
}

static const int *
my_nip_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
    static const int kinds[] = { 1984 }; /* NIP-56 Report */
    *n_kinds = G_N_ELEMENTS(kinds);
    return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
    iface->activate = my_nip_plugin_activate;
    iface->deactivate = my_nip_plugin_deactivate;
    iface->get_name = my_nip_plugin_get_name;
    iface->get_description = my_nip_plugin_get_description;
    iface->get_supported_kinds = my_nip_plugin_get_supported_kinds;
}
```

### GnostrEventHandler (Optional)

For plugins that process events:

```c
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MyNipPlugin, my_nip_plugin, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
    G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init))

static gboolean
my_nip_plugin_handle_event(GnostrEventHandler *handler,
                           GnostrPluginContext *ctx,
                           GnostrPluginEvent *event)
{
    int kind = gnostr_plugin_event_get_kind(event);

    if (kind == 1984) {
        const char *content = gnostr_plugin_event_get_content(event);
        const char *reported = gnostr_plugin_event_get_tag_value(event, "p", 0);

        /* Process the report... */
        g_debug("Report against %s: %s", reported, content);
        return TRUE; /* Event handled */
    }

    return FALSE; /* Pass to other handlers */
}

static gboolean
my_nip_plugin_can_handle_kind(GnostrEventHandler *handler, int kind)
{
    return kind == 1984;
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
    iface->handle_event = my_nip_plugin_handle_event;
    iface->can_handle_kind = my_nip_plugin_can_handle_kind;
}
```

### GnostrUIExtension (Optional)

For plugins that extend the UI:

```c
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(MyNipPlugin, my_nip_plugin, G_TYPE_OBJECT,
    G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
    G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

static GList *
my_nip_plugin_create_menu_items(GnostrUIExtension *ext,
                                GnostrPluginContext *ctx,
                                GnostrUIExtensionPoint point,
                                gpointer target_data)
{
    if (point != GNOSTR_UI_EXTENSION_MENU_NOTE)
        return NULL;

    GnostrPluginEvent *event = target_data;
    GMenuItem *item = g_menu_item_new("Report Note", "app.report-note");
    g_menu_item_set_attribute(item, "target", "s",
        gnostr_plugin_event_get_id(event));

    return g_list_append(NULL, item);
}

static GtkWidget *
my_nip_plugin_create_settings_page(GnostrUIExtension *ext,
                                   GnostrPluginContext *ctx)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);

    /* Add settings widgets */
    GtkWidget *label = gtk_label_new("Configure reporting preferences:");
    gtk_box_append(GTK_BOX(page), label);

    return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
    iface->create_menu_items = my_nip_plugin_create_menu_items;
    iface->create_settings_page = my_nip_plugin_create_settings_page;
}
```

## Registration

Register your plugin type in the peas entry point:

```c
#include <libpeas.h>
#include <gnostr-plugin-api.h>

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
    /* Simple registration */
    GNOSTR_PLUGIN_REGISTER(MyNipPlugin, my_nip_plugin);

    /* Or with additional interfaces */
    GNOSTR_PLUGIN_REGISTER_WITH_INTERFACES(MyNipPlugin, my_nip_plugin,
        GNOSTR_TYPE_EVENT_HANDLER,
        GNOSTR_TYPE_UI_EXTENSION);
}
```

## Context API

The `GnostrPluginContext` provides access to host services:

### Network Operations

```c
/* Get relay URLs */
gsize n_urls;
char **urls = gnostr_plugin_context_get_relay_urls(ctx, &n_urls);

/* Get the relay pool (for advanced operations) */
GObject *pool = gnostr_plugin_context_get_pool(ctx);

/* Publish an event */
gnostr_plugin_context_publish_event_async(ctx, event_json,
    NULL, on_publish_complete, self);
```

### Storage Operations

```c
/* Query local storage */
GPtrArray *events = gnostr_plugin_context_query_events(ctx,
    "{\"kinds\":[30023],\"limit\":10}", &error);

/* Subscribe to new events */
guint64 sub = gnostr_plugin_context_subscribe_events(ctx,
    "{\"kinds\":[1984]}", G_CALLBACK(on_event), self, NULL);

/* Unsubscribe */
gnostr_plugin_context_unsubscribe_events(ctx, sub);
```

### Plugin Data Storage

```c
/* Store data */
GBytes *data = g_bytes_new(buffer, len);
gnostr_plugin_context_store_data(ctx, "my-key", data, &error);
g_bytes_unref(data);

/* Load data */
GBytes *loaded = gnostr_plugin_context_load_data(ctx, "my-key", &error);
if (loaded) {
    gsize size;
    const void *contents = g_bytes_get_data(loaded, &size);
    /* Use data... */
    g_bytes_unref(loaded);
}
```

### User Identity

```c
/* Check if logged in */
if (gnostr_plugin_context_is_logged_in(ctx)) {
    const char *pubkey = gnostr_plugin_context_get_user_pubkey(ctx);

    /* Request event signing */
    gnostr_plugin_context_request_sign_event(ctx, unsigned_json,
        NULL, on_signed, self);
}
```

### GSettings

```c
GSettings *settings = gnostr_plugin_context_get_settings(ctx,
    "org.gnome.gnostr.plugins.my-nip");

if (settings) {
    gboolean enabled = g_settings_get_boolean(settings, "feature-enabled");
    g_object_unref(settings);
}
```

## UI Extension Points

| Point | Description | Target Data |
|-------|-------------|-------------|
| `GNOSTR_UI_EXTENSION_MENU_APP` | Application menu | NULL |
| `GNOSTR_UI_EXTENSION_MENU_NOTE` | Note context menu | GnostrPluginEvent* |
| `GNOSTR_UI_EXTENSION_MENU_PROFILE` | Profile menu | Profile pubkey (char*) |
| `GNOSTR_UI_EXTENSION_TOOLBAR` | Main toolbar | NULL |
| `GNOSTR_UI_EXTENSION_SIDEBAR` | Navigation sidebar | NULL |
| `GNOSTR_UI_EXTENSION_SETTINGS` | Settings dialog | NULL |
| `GNOSTR_UI_EXTENSION_NOTE_CARD` | Note card decoration | GnostrPluginEvent* |
| `GNOSTR_UI_EXTENSION_PROFILE_HEADER` | Profile header | Profile pubkey (char*) |

## Event Access

```c
void process_event(GnostrPluginEvent *event)
{
    /* Basic fields */
    const char *id = gnostr_plugin_event_get_id(event);
    const char *pubkey = gnostr_plugin_event_get_pubkey(event);
    int kind = gnostr_plugin_event_get_kind(event);
    gint64 created_at = gnostr_plugin_event_get_created_at(event);
    const char *content = gnostr_plugin_event_get_content(event);

    /* Tag access */
    const char *e_tag = gnostr_plugin_event_get_tag_value(event, "e", 0);
    char **all_p_tags = gnostr_plugin_event_get_tag_values(event, "p");

    /* Full JSON */
    char *json = gnostr_plugin_event_to_json(event);
    g_free(json);

    g_strfreev(all_p_tags);
}
```

## Error Handling

```c
GError *error = NULL;
GPtrArray *events = gnostr_plugin_context_query_events(ctx, filter, &error);

if (error) {
    switch (error->code) {
    case GNOSTR_PLUGIN_ERROR_NOT_LOGGED_IN:
        /* Handle not logged in */
        break;
    case GNOSTR_PLUGIN_ERROR_STORAGE:
        g_warning("Storage error: %s", error->message);
        break;
    default:
        g_warning("Query failed: %s", error->message);
    }
    g_error_free(error);
    return;
}
```

## Building Plugins

### Meson Build

```meson
project('my-nip-plugin', 'c',
  version: '1.0.0',
  meson_version: '>= 0.59')

gnome = import('gnome')

gnostr_dep = dependency('gnostr-plugin-api')
gtk4_dep = dependency('gtk4')
peas_dep = dependency('libpeas-2')

shared_module('my-nip-plugin',
  'my-nip-plugin.c',
  dependencies: [gnostr_dep, gtk4_dep, peas_dep],
  install: true,
  install_dir: get_option('libdir') / 'gnostr' / 'plugins')

install_data('my-nip-plugin.plugin',
  install_dir: get_option('libdir') / 'gnostr' / 'plugins')

# Optional: GSettings schema
gnome.compile_schemas(
  depend_files: 'my-nip-plugin.gschema.xml')
install_data('my-nip-plugin.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')
```

## Plugin Installation

Plugins are installed to:
- System: `/usr/lib/gnostr/plugins/`
- User: `~/.local/share/gnostr/plugins/`

Each plugin needs:
1. Shared library (`.so` file)
2. Plugin metadata (`.plugin` file)
3. Optional: GSettings schema (`.gschema.xml`)

## Best Practices

1. **Check API version** at load time
2. **Cleanup in deactivate** - unsubscribe, free resources
3. **Use async APIs** for network/signing to avoid blocking UI
4. **Handle errors gracefully** - don't crash on failures
5. **Namespace your data keys** - though the API does this automatically
6. **Test without login** - handle the not-logged-in case
7. **Prefer GTK 4 widgets** - Gnostr uses GTK 4 / Adwaita

## Example: NIP-58 Badges Plugin

See `plugins/nip58-badges/` for a complete example implementing:
- Badge definition parsing (kind 30009)
- Badge award display (kind 8)
- Profile badge display (kind 30008)
- UI extension for badge settings

## Resources

- [libpeas 2 Migration Guide](https://gnome.pages.gitlab.gnome.org/libpeas/libpeas-2/migrating-1to2.html)
- [GObject Tutorial](https://docs.gtk.org/gobject/tutorial.html)
- [Nostr NIPs](https://github.com/nostr-protocol/nips)
