/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip99-marketplace-plugin.c - NIP-99 Marketplace Plugin
 *
 * Implements NIP-99 (Classified Listings) for browsing and publishing
 * marketplace listings on Nostr.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip99-marketplace-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>

/* Marketplace UI components */
#include "../../src/ui/gnostr-classifieds-view.h"
#include "../../src/util/nip99_classifieds.h"

/* NIP-99 Event Kinds */
#define NIP99_KIND_CLASSIFIED 30402
#define NIP99_KIND_DRAFT_CLASSIFIED 30403

/* Signal IDs */
enum {
  SIGNAL_LISTINGS_UPDATED,
  N_SIGNALS
};

static guint plugin_signals[N_SIGNALS];

struct _Nip99MarketplacePlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Cached listings */
  GHashTable *listings;  /* event_id -> GnostrClassified* */

  /* Event subscription */
  guint64 listings_subscription;

  /* UI reference */
  GnostrClassifiedsView *classifieds_view;
};

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrEventHandler interface */
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip99MarketplacePlugin, nip99_marketplace_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

/* ============================================================================
 * GObject lifecycle
 * ============================================================================ */

static void
nip99_marketplace_plugin_finalize(GObject *object)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(object);

  g_clear_pointer(&self->listings, g_hash_table_destroy);

  G_OBJECT_CLASS(nip99_marketplace_plugin_parent_class)->finalize(object);
}

static void
nip99_marketplace_plugin_init(Nip99MarketplacePlugin *self)
{
  self->active = FALSE;
  self->listings = g_hash_table_new_full(g_str_hash, g_str_equal,
                                         g_free,
                                         (GDestroyNotify)gnostr_classified_free);
}

static void
nip99_marketplace_plugin_class_init(Nip99MarketplacePluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = nip99_marketplace_plugin_finalize;

  /**
   * Nip99MarketplacePlugin::listings-updated:
   * @plugin: The plugin
   * @count: Number of listings in cache
   *
   * Emitted when new classified listings are received from relays.
   */
  plugin_signals[SIGNAL_LISTINGS_UPDATED] = g_signal_new(
      "listings-updated",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      NULL,
      G_TYPE_NONE, 1,
      G_TYPE_UINT);
}

/* ============================================================================
 * Event Subscription Callback
 * ============================================================================ */

static void
on_classified_received(GnostrPluginEvent *event, gpointer user_data)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(user_data);

  if (!self->active || !event)
    return;

  int kind = gnostr_plugin_event_get_kind(event);
  if (kind != NIP99_KIND_CLASSIFIED)
    return;

  /* Parse the classified listing from event */
  char *json = gnostr_plugin_event_to_json(event);
  if (!json)
    return;

  GnostrClassified *classified = gnostr_classified_parse(json);
  g_free(json);

  if (classified && classified->event_id) {
    /* Check if we already have this listing */
    if (!g_hash_table_contains(self->listings, classified->event_id)) {
      g_hash_table_replace(self->listings, g_strdup(classified->event_id), classified);
      g_debug("[NIP-99] Received classified: %s (id: %.16s...)",
              classified->title ? classified->title : "Untitled",
              classified->event_id);

      /* Add to view if available */
      if (self->classifieds_view) {
        gnostr_classifieds_view_add_listing(self->classifieds_view, classified);
      }

      /* Emit listings-updated signal */
      guint count = g_hash_table_size(self->listings);
      g_signal_emit(self, plugin_signals[SIGNAL_LISTINGS_UPDATED], 0, count);
    } else {
      gnostr_classified_free(classified);
    }
  } else {
    gnostr_classified_free(classified);
  }
}

static void
on_request_listings_done(GObject      *source_object,
                         GAsyncResult *result,
                         gpointer      user_data)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(user_data);
  (void)source_object;
  (void)result;

  g_debug("[NIP-99] Request listings completed, cache has %u listings",
          g_hash_table_size(self->listings));

  /* Emit listings-updated to refresh the UI */
  guint count = g_hash_table_size(self->listings);
  g_signal_emit(self, plugin_signals[SIGNAL_LISTINGS_UPDATED], 0, count);

  /* Hide loading spinner */
  if (self->classifieds_view) {
    gnostr_classifieds_view_set_loading(self->classifieds_view, FALSE);
  }

  g_object_unref(self);
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip99_marketplace_plugin_activate(GnostrPlugin        *plugin,
                                  GnostrPluginContext *context)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(plugin);

  g_debug("[NIP-99] Marketplace plugin activated");

  self->context = context;
  self->active = TRUE;

  /* Subscribe to NIP-99 classified listings from relays */
  gchar *filter = g_strdup_printf("{\"kinds\":[%d],\"limit\":100}", NIP99_KIND_CLASSIFIED);
  self->listings_subscription = gnostr_plugin_context_subscribe_events(
      context, filter, G_CALLBACK(on_classified_received), self, NULL);
  g_free(filter);

  if (self->listings_subscription > 0) {
    g_debug("[NIP-99] Subscribed to classified listings (subscription_id: %lu)",
            (unsigned long)self->listings_subscription);
  }
}

static void
nip99_marketplace_plugin_deactivate(GnostrPlugin        *plugin,
                                    GnostrPluginContext *context)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(plugin);

  g_debug("[NIP-99] Marketplace plugin deactivated");

  /* Unsubscribe from classified events */
  if (self->listings_subscription > 0 && context) {
    gnostr_plugin_context_unsubscribe_events(context, self->listings_subscription);
    self->listings_subscription = 0;
  }

  self->active = FALSE;
  self->context = NULL;
  self->classifieds_view = NULL;
}

static const char *
nip99_marketplace_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-99 Marketplace";
}

static const char *
nip99_marketplace_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Browse and publish classified listings on Nostr";
}

static const int *
nip99_marketplace_plugin_get_supported_kinds(GnostrPlugin *plugin,
                                             gsize        *n_kinds)
{
  (void)plugin;
  static const int kinds[] = { NIP99_KIND_CLASSIFIED, NIP99_KIND_DRAFT_CLASSIFIED };
  *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip99_marketplace_plugin_activate;
  iface->deactivate = nip99_marketplace_plugin_deactivate;
  iface->get_name = nip99_marketplace_plugin_get_name;
  iface->get_description = nip99_marketplace_plugin_get_description;
  iface->get_supported_kinds = nip99_marketplace_plugin_get_supported_kinds;
}

/* ============================================================================
 * GnostrEventHandler interface implementation
 * ============================================================================ */

static gboolean
nip99_marketplace_plugin_handle_event(GnostrEventHandler  *handler,
                                      GnostrPluginContext *context,
                                      GnostrPluginEvent   *event)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(handler);
  (void)context;

  if (!self->active)
    return FALSE;

  int kind = gnostr_plugin_event_get_kind(event);

  if (kind != NIP99_KIND_CLASSIFIED && kind != NIP99_KIND_DRAFT_CLASSIFIED)
    return FALSE;

  /* Parse and cache the classified listing */
  char *json = gnostr_plugin_event_to_json(event);
  GnostrClassified *classified = gnostr_classified_parse(json);
  g_free(json);

  if (classified && classified->event_id) {
    g_hash_table_replace(self->listings,
                         g_strdup(classified->event_id), classified);
    g_debug("[NIP-99] Cached classified: %s",
            classified->title ? classified->title : "Untitled");

    /* Add to view if available */
    if (self->classifieds_view) {
      gnostr_classifieds_view_add_listing(self->classifieds_view, classified);
    }
  } else {
    gnostr_classified_free(classified);
  }

  return TRUE;
}

static gboolean
nip99_marketplace_plugin_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  (void)handler;
  return kind == NIP99_KIND_CLASSIFIED || kind == NIP99_KIND_DRAFT_CLASSIFIED;
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->handle_event = nip99_marketplace_plugin_handle_event;
  iface->can_handle_kind = nip99_marketplace_plugin_can_handle_kind;
}

/* ============================================================================
 * GnostrUIExtension interface implementation
 * ============================================================================ */

static GList *
nip99_marketplace_plugin_get_sidebar_items(GnostrUIExtension   *extension,
                                           GnostrPluginContext *context)
{
  (void)extension;
  (void)context;

  GnostrSidebarItem *item = gnostr_sidebar_item_new(
    "nip99-marketplace",      /* id */
    "Marketplace",            /* label */
    "emblem-sales-symbolic"   /* icon */
  );

  /* Marketplace doesn't require auth to browse, but does to post */
  gnostr_sidebar_item_set_requires_auth(item, FALSE);
  gnostr_sidebar_item_set_position(item, 60); /* After Chess */

  return g_list_append(NULL, item);
}

static void
on_listing_clicked(GnostrClassifiedsView *view,
                   const gchar *event_id,
                   const gchar *naddr,
                   gpointer user_data)
{
  (void)view;
  (void)user_data;
  g_debug("[NIP-99] Listing clicked: %s (naddr: %s)", event_id, naddr ? naddr : "none");
  /* TODO: Open listing detail view */
}

static void
on_contact_seller(GnostrClassifiedsView *view,
                  const gchar *pubkey_hex,
                  const gchar *lud16,
                  gpointer user_data)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(user_data);
  (void)view;
  g_debug("[NIP-99] Contact seller: %s (lud16: %s)", pubkey_hex, lud16 ? lud16 : "none");

  /* Open DM to seller via plugin context */
  if (self->context && pubkey_hex) {
    /* TODO: Use plugin context to open DM view */
  }
}

static void
on_open_profile(GnostrClassifiedsView *view,
                const gchar *pubkey_hex,
                gpointer user_data)
{
  (void)view;
  (void)user_data;
  g_debug("[NIP-99] Open profile: %s", pubkey_hex);
  /* TODO: Implement profile navigation when plugin API supports it */
}

static GtkWidget *
nip99_marketplace_plugin_create_panel_widget(GnostrUIExtension   *extension,
                                             GnostrPluginContext *context,
                                             const char          *panel_id)
{
  Nip99MarketplacePlugin *self = NIP99_MARKETPLACE_PLUGIN(extension);
  (void)context;

  if (g_strcmp0(panel_id, "nip99-marketplace") != 0)
    return NULL;

  g_debug("[NIP-99] Creating marketplace panel widget");

  /* Create the classifieds view */
  self->classifieds_view = gnostr_classifieds_view_new();

  /* Connect signals */
  g_signal_connect(self->classifieds_view, "listing-clicked",
                   G_CALLBACK(on_listing_clicked), self);
  g_signal_connect(self->classifieds_view, "contact-seller",
                   G_CALLBACK(on_contact_seller), self);
  g_signal_connect(self->classifieds_view, "open-profile",
                   G_CALLBACK(on_open_profile), self);

  /* Populate with cached listings */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->listings);
  while (g_hash_table_iter_next(&iter, &key, &value)) {
    GnostrClassified *classified = (GnostrClassified *)value;
    gnostr_classifieds_view_add_listing(self->classifieds_view, classified);
  }

  /* Check login state */
  const char *pubkey = gnostr_plugin_context_get_user_pubkey(context);
  if (pubkey) {
    gnostr_classifieds_view_set_logged_in(self->classifieds_view, TRUE);
    gnostr_classifieds_view_set_user_pubkey(self->classifieds_view, pubkey);
  }

  /* Start fetching listings if we don't have any */
  if (g_hash_table_size(self->listings) == 0) {
    gnostr_classifieds_view_set_loading(self->classifieds_view, TRUE);
    nip99_marketplace_plugin_request_listings(self);
  }

  return GTK_WIDGET(self->classifieds_view);
}

static GtkWidget *
nip99_marketplace_plugin_create_settings_page(GnostrUIExtension   *extension,
                                              GnostrPluginContext *context)
{
  (void)extension;
  (void)context;

  /* Simple settings page */
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);

  GtkWidget *title = gtk_label_new("Marketplace Settings");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), title);

  GtkWidget *desc = gtk_label_new(
    "NIP-99 Marketplace allows you to browse and post classified listings.\n\n"
    "Listings are stored as kind 30402 events with structured metadata\n"
    "including title, description, price, location, and images."
  );
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_box_append(GTK_BOX(box), desc);

  return box;
}

static GList *
nip99_marketplace_plugin_create_menu_items(GnostrUIExtension     *extension,
                                           GnostrPluginContext   *context,
                                           GnostrUIExtensionPoint point,
                                           gpointer               extra)
{
  (void)extension;
  (void)context;
  (void)point;
  (void)extra;

  /* No menu items for now */
  return NULL;
}

static GtkWidget *
nip99_marketplace_plugin_create_note_decoration(GnostrUIExtension   *extension,
                                                GnostrPluginContext *context,
                                                GnostrPluginEvent   *event)
{
  (void)extension;
  (void)context;

  int kind = gnostr_plugin_event_get_kind(event);

  if (kind != NIP99_KIND_CLASSIFIED)
    return NULL;

  /* Could return a classified card widget for inline viewing */
  /* For now, return NULL - listings shown in dedicated view */
  return NULL;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->get_sidebar_items = nip99_marketplace_plugin_get_sidebar_items;
  iface->create_panel_widget = nip99_marketplace_plugin_create_panel_widget;
  iface->create_settings_page = nip99_marketplace_plugin_create_settings_page;
  iface->create_menu_items = nip99_marketplace_plugin_create_menu_items;
  iface->create_note_decoration = nip99_marketplace_plugin_create_note_decoration;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

GHashTable *
nip99_marketplace_plugin_get_listings(Nip99MarketplacePlugin *self)
{
  g_return_val_if_fail(NIP99_IS_MARKETPLACE_PLUGIN(self), NULL);
  return self->listings;
}

void
nip99_marketplace_plugin_request_listings(Nip99MarketplacePlugin *self)
{
  g_return_if_fail(NIP99_IS_MARKETPLACE_PLUGIN(self));

  if (!self->context || !self->active) {
    g_debug("[NIP-99] Cannot request listings - context=%p active=%d",
            (void*)self->context, self->active);
    return;
  }

  g_debug("[NIP-99] Requesting fresh classified listings from relays...");

  /* Show loading state */
  if (self->classifieds_view) {
    gnostr_classifieds_view_set_loading(self->classifieds_view, TRUE);
  }

  /* Request fresh classified listings from relays */
  static const int kinds[] = { NIP99_KIND_CLASSIFIED };
  gnostr_plugin_context_request_relay_events_async(
      self->context,
      kinds, 1,
      100,  /* limit */
      NULL,
      on_request_listings_done,
      g_object_ref(self));
}

guint
nip99_marketplace_plugin_get_listing_count(Nip99MarketplacePlugin *self)
{
  g_return_val_if_fail(NIP99_IS_MARKETPLACE_PLUGIN(self), 0);
  return g_hash_table_size(self->listings);
}

/* ============================================================================
 * Plugin entry point for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_PLUGIN,
                                             NIP99_TYPE_MARKETPLACE_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_EVENT_HANDLER,
                                             NIP99_TYPE_MARKETPLACE_PLUGIN);
  peas_object_module_register_extension_type(module,
                                             GNOSTR_TYPE_UI_EXTENSION,
                                             NIP99_TYPE_MARKETPLACE_PLUGIN);
}
