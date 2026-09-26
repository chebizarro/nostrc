/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip47-nwc-plugin.c - NIP-47 Nostr Wallet Connect Plugin
 *
 * Thin client of the desktop wallet agent (nostr-wallet-agent,
 * org.nostr.Wallet1, bead nostrc-yka8). The NIP-47 protocol, the pairing
 * secret and the relay connection live in the agent; this plugin keeps its
 * public API, signals and settings page and forwards to D-Bus:
 *
 *   connect()          -> Pair(uri)          (user-confirmed by the agent)
 *   disconnect()       -> Unpair()           (user-confirmed by the agent)
 *   get_balance_async  -> GetBalance()
 *   pay_invoice_async  -> PayInvoice(bolt11, amount)  (agent budget/approval)
 *   make_invoice_async -> MakeInvoice(amount, description, expiry)
 *   state / pubkey / relay / lud16 <- Paired / WalletPubkey / Relays / Lud16
 *
 * A URI stored by earlier versions in plugin storage is handed to Pair()
 * once and then deleted, so no plaintext pairing secret remains.
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip47-nwc-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>
#include <nostr/nip47/nwc.h>
#include <string.h>

/* Legacy (pre-agent) plugin storage key for the connection URI */
#define NWC_STORAGE_KEY_URI "connection-uri"

#define WALLET_BUS_NAME  "org.nostr.Wallet1"
#define WALLET_PATH      "/org/nostr/Wallet1"
#define WALLET_IFACE     "org.nostr.Wallet1"

/* Payments may wait on the agent's approval dialog (120 s) plus the wallet
 * (60 s); do not let D-Bus time out first. */
#define WALLET_CALL_TIMEOUT_MS G_MAXINT

/* Singleton instance */
static Nip47NwcPlugin *default_plugin = NULL;

struct _Nip47NwcPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Connection state (mirrors the agent's Paired property) */
  Nip47NwcState state;
  gchar *last_error;

  /* Cached agent properties (never the secret) */
  gchar *wallet_pubkey_hex;
  gchar *relay;
  gchar *lud16;

  GDBusProxy   *proxy;
  GCancellable *cancellable;
  gchar        *pending_pair_uri;   /* connect() before the proxy exists */
};

/* Signals */
enum {
  SIGNAL_STATE_CHANGED,
  SIGNAL_BALANCE_UPDATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

/* Properties */
enum {
  PROP_0,
  PROP_STATE,
  PROP_WALLET_PUBKEY,
  PROP_RELAY,
  PROP_LUD16,
  N_PROPERTIES
};
static GParamSpec *properties[N_PROPERTIES];

/* Implement interfaces */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip47NwcPlugin, nip47_nwc_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

GQuark nip47_nwc_error_quark(void) {
  return g_quark_from_static_string("nip47-nwc-error");
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

static void wipe_free(gchar *s) {
  if (!s) return;
  memset(s, 0, strlen(s));
  g_free(s);
}

static void nip47_nwc_plugin_set_state(Nip47NwcPlugin *self, Nip47NwcState state) {
  if (self->state != state) {
    self->state = state;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state);
  }
}

static void set_error(Nip47NwcPlugin *self, const gchar *message) {
  g_free(self->last_error);
  self->last_error = g_strdup(message);
  nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_ERROR);
}

static gchar *proxy_string(GDBusProxy *proxy, const gchar *name) {
  g_autoptr(GVariant) v = proxy ? g_dbus_proxy_get_cached_property(proxy, name) : NULL;
  if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) return NULL;
  const gchar *s = g_variant_get_string(v, NULL);
  return (s && *s) ? g_strdup(s) : NULL;
}

/* Refresh cached values from the agent's properties. */
static void sync_from_proxy(Nip47NwcPlugin *self) {
  g_autoptr(GVariant) paired_v = self->proxy
    ? g_dbus_proxy_get_cached_property(self->proxy, "Paired") : NULL;
  gboolean paired = paired_v && g_variant_get_boolean(paired_v);

  g_free(self->wallet_pubkey_hex);
  g_free(self->lud16);
  g_free(self->relay);
  self->wallet_pubkey_hex = paired ? proxy_string(self->proxy, "WalletPubkey") : NULL;
  self->lud16 = paired ? proxy_string(self->proxy, "Lud16") : NULL;
  self->relay = NULL;
  if (paired) {
    g_autoptr(GVariant) rv = g_dbus_proxy_get_cached_property(self->proxy, "Relays");
    if (rv && g_variant_is_of_type(rv, G_VARIANT_TYPE_STRING_ARRAY) && g_variant_n_children(rv) > 0) {
      g_autoptr(GVariant) first = g_variant_get_child_value(rv, 0);
      self->relay = g_variant_dup_string(first, NULL);
    }
  }

  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WALLET_PUBKEY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RELAY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);

  if (paired && self->wallet_pubkey_hex) {
    g_clear_pointer(&self->last_error, g_free);
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_CONNECTED);
  } else if (self->state != NIP47_NWC_STATE_CONNECTING && self->state != NIP47_NWC_STATE_ERROR) {
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_DISCONNECTED);
  }
}

/* Map an org.nostr.Wallet1 error onto the plugin's error domain. */
static GError *map_wallet_error(GError *error) {
  gint code = NIP47_NWC_ERROR_REQUEST_FAILED;
  g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
  if (remote) {
    if (g_str_has_suffix(remote, ".Timeout")) code = NIP47_NWC_ERROR_TIMEOUT;
    else if (g_str_has_suffix(remote, ".WalletError")) code = NIP47_NWC_ERROR_WALLET_ERROR;
    else if (g_str_has_suffix(remote, ".NotPaired")) code = NIP47_NWC_ERROR_CONNECTION_FAILED;
    else if (g_str_has_suffix(remote, ".InvalidArgs")) code = NIP47_NWC_ERROR_INVALID_URI;
  } else if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
             g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)) {
    code = NIP47_NWC_ERROR_CONNECTION_FAILED;
  }
  g_dbus_error_strip_remote_error(error);
  GError *mapped = g_error_new_literal(NIP47_NWC_ERROR, code, error->message);
  g_error_free(error);
  return mapped;
}

/* ============================================================================
 * Wallet agent calls
 * ============================================================================ */

static void on_pair_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(user_data);
  GError *error = NULL;
  g_autoptr(GVariant) r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
  if (!r) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      error = map_wallet_error(error);
      g_warning("[NIP-47] Wallet agent refused the pairing: %s", error->message);
      set_error(self, error->message);
    }
    g_error_free(error);
  } else {
    /* PropertiesChanged normally arrives first; resync regardless. */
    sync_from_proxy(self);
    g_message("[NIP-47] Paired through the wallet agent");
    if (self->context)
      gnostr_plugin_context_delete_data(self->context, NWC_STORAGE_KEY_URI);
  }
  g_object_unref(self);
}

static void wallet_pair(Nip47NwcPlugin *self, const gchar *uri) {
  nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_CONNECTING);
  g_dbus_proxy_call(self->proxy, "Pair", g_variant_new("(s)", uri),
                    G_DBUS_CALL_FLAGS_NONE, WALLET_CALL_TIMEOUT_MS, self->cancellable,
                    on_pair_done, g_object_ref(self));
}

static void on_unpair_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(user_data);
  GError *error = NULL;
  g_autoptr(GVariant) r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
  if (!r) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      error = map_wallet_error(error);
      g_message("[NIP-47] Wallet agent did not disconnect: %s", error->message);
    }
    g_error_free(error);
  }
  sync_from_proxy(self);
  g_object_unref(self);
}

static void on_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
                                  gpointer user_data) {
  (void)proxy; (void)changed; (void)invalidated;
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(user_data);
  if (self->state == NIP47_NWC_STATE_CONNECTING)
    return; /* on_pair_done decides */
  sync_from_proxy(self);
}

static void on_name_owner(GObject *proxy, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(user_data);
  g_autofree gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!owner && self->state == NIP47_NWC_STATE_CONNECTED)
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_DISCONNECTED);
}

/* Move a URI saved by pre-agent versions into the agent, then delete it. */
static void migrate_legacy_uri(Nip47NwcPlugin *self) {
  if (!self->context) return;
  GBytes *stored = gnostr_plugin_context_load_data(self->context, NWC_STORAGE_KEY_URI, NULL);
  if (!stored) return;
  gsize size = 0;
  const gchar *data = g_bytes_get_data(stored, &size);
  gchar *uri = (data && size > 0) ? g_strndup(data, size) : NULL;
  g_bytes_unref(stored);
  if (!uri || !*uri) {
    wipe_free(uri);
    return;
  }
  if (nip47_nwc_plugin_is_connected(self)) {
    /* the agent already has a wallet: drop the stale plaintext copy */
    gnostr_plugin_context_delete_data(self->context, NWC_STORAGE_KEY_URI);
    g_message("[NIP-47] Removed legacy stored connection; the wallet agent is already paired");
  } else {
    g_message("[NIP-47] Moving the stored wallet connection into the wallet agent");
    wallet_pair(self, uri);  /* deleted from plugin storage on success */
  }
  wipe_free(uri);
}

static void on_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(user_data);
  GError *error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
  if (!proxy) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_warning("[NIP-47] Wallet agent unavailable: %s", error->message);
      set_error(self, "Nostr Wallet agent (org.nostr.Wallet1) is not available");
    }
    g_error_free(error);
    g_object_unref(self);
    return;
  }
  self->proxy = proxy;
  g_signal_connect(proxy, "g-properties-changed", G_CALLBACK(on_properties_changed), self);
  g_signal_connect(proxy, "notify::g-name-owner", G_CALLBACK(on_name_owner), self);
  sync_from_proxy(self);

  if (self->pending_pair_uri) {
    gchar *uri = g_steal_pointer(&self->pending_pair_uri);
    wallet_pair(self, uri);
    wipe_free(uri);
  } else {
    migrate_legacy_uri(self);
  }
  g_object_unref(self);
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void nip47_nwc_plugin_dispose(GObject *object) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(object);

  if (self == default_plugin) {
    default_plugin = NULL;
  }
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  if (self->proxy) g_signal_handlers_disconnect_by_data(self->proxy, self);
  g_clear_object(&self->proxy);

  G_OBJECT_CLASS(nip47_nwc_plugin_parent_class)->dispose(object);
}

static void nip47_nwc_plugin_finalize(GObject *object) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(object);
  g_free(self->wallet_pubkey_hex);
  g_free(self->relay);
  g_free(self->lud16);
  g_free(self->last_error);
  wipe_free(self->pending_pair_uri);
  G_OBJECT_CLASS(nip47_nwc_plugin_parent_class)->finalize(object);
}

static void nip47_nwc_plugin_get_property(GObject *object, guint prop_id,
                                          GValue *value, GParamSpec *pspec) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(object);

  switch (prop_id) {
    case PROP_STATE:
      g_value_set_int(value, self->state);
      break;
    case PROP_WALLET_PUBKEY:
      g_value_set_string(value, self->wallet_pubkey_hex);
      break;
    case PROP_RELAY:
      g_value_set_string(value, self->relay);
      break;
    case PROP_LUD16:
      g_value_set_string(value, self->lud16);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void nip47_nwc_plugin_class_init(Nip47NwcPluginClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = nip47_nwc_plugin_dispose;
  object_class->finalize = nip47_nwc_plugin_finalize;
  object_class->get_property = nip47_nwc_plugin_get_property;

  properties[PROP_STATE] = g_param_spec_int(
    "state", "State", "Connection state",
    NIP47_NWC_STATE_DISCONNECTED, NIP47_NWC_STATE_ERROR,
    NIP47_NWC_STATE_DISCONNECTED,
    G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_WALLET_PUBKEY] = g_param_spec_string(
    "wallet-pubkey", "Wallet Pubkey", "Connected wallet public key",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_RELAY] = g_param_spec_string(
    "relay", "Relay", "Primary relay URL",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  properties[PROP_LUD16] = g_param_spec_string(
    "lud16", "LUD16", "Lightning address",
    NULL, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties(object_class, N_PROPERTIES, properties);

  signals[SIGNAL_STATE_CHANGED] = g_signal_new(
    "state-changed",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT);

  signals[SIGNAL_BALANCE_UPDATED] = g_signal_new(
    "balance-updated",
    G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL,
    G_TYPE_NONE, 1, G_TYPE_INT64);
}

static void nip47_nwc_plugin_init(Nip47NwcPlugin *self) {
  self->state = NIP47_NWC_STATE_DISCONNECTED;
}

/* ============================================================================
 * GnostrPlugin Interface Implementation
 * ============================================================================ */

static void nip47_nwc_plugin_activate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(plugin);

  g_debug("[NIP-47] Activating Nostr Wallet Connect plugin (wallet agent client)");

  self->context = context;
  self->active = TRUE;
  default_plugin = self;

  g_clear_object(&self->cancellable);
  self->cancellable = g_cancellable_new();
  /* Flags NONE: auto-starts the D-Bus-activatable agent. */
  g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                           WALLET_BUS_NAME, WALLET_PATH, WALLET_IFACE,
                           self->cancellable, on_proxy_ready, g_object_ref(self));
}

static void nip47_nwc_plugin_deactivate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-47] Deactivating Nostr Wallet Connect plugin");

  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  if (self->proxy) g_signal_handlers_disconnect_by_data(self->proxy, self);
  g_clear_object(&self->proxy);

  self->active = FALSE;
  self->context = NULL;

  if (self == default_plugin) {
    default_plugin = NULL;
  }
}

static const char *nip47_nwc_plugin_get_name_impl(GnostrPlugin *plugin) {
  (void)plugin;
  return "NIP-47 Nostr Wallet Connect";
}

static const char *nip47_nwc_plugin_get_description_impl(GnostrPlugin *plugin) {
  (void)plugin;
  return "Lightning wallet integration via Nostr Wallet Connect protocol";
}

static const char *const *nip47_nwc_plugin_get_authors_impl(GnostrPlugin *plugin) {
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *nip47_nwc_plugin_get_version_impl(GnostrPlugin *plugin) {
  (void)plugin;
  return "2.0";
}

static const int *nip47_nwc_plugin_get_supported_kinds_impl(GnostrPlugin *plugin, gsize *n_kinds) {
  static const int kinds[] = { NWC_KIND_INFO, NWC_KIND_REQUEST, NWC_KIND_RESPONSE };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void gnostr_plugin_iface_init(GnostrPluginInterface *iface) {
  iface->activate = nip47_nwc_plugin_activate;
  iface->deactivate = nip47_nwc_plugin_deactivate;
  iface->get_name = nip47_nwc_plugin_get_name_impl;
  iface->get_description = nip47_nwc_plugin_get_description_impl;
  iface->get_authors = nip47_nwc_plugin_get_authors_impl;
  iface->get_version = nip47_nwc_plugin_get_version_impl;
  iface->get_supported_kinds = nip47_nwc_plugin_get_supported_kinds_impl;
}

/* ============================================================================
 * GnostrEventHandler Interface Implementation
 * ============================================================================ */

/* NWC events are exchanged by the wallet agent on the pairing's relays; the
 * app's timeline never needs to route them here. */
static gboolean nip47_event_handler_can_handle_kind(GnostrEventHandler *handler, int kind) {
  (void)handler; (void)kind;
  return FALSE;
}

static gboolean nip47_event_handler_handle_event(GnostrEventHandler *handler,
                                                  GnostrPluginContext *context,
                                                  GnostrPluginEvent *event) {
  (void)handler; (void)context; (void)event;
  return FALSE;
}

static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface) {
  iface->can_handle_kind = nip47_event_handler_can_handle_kind;
  iface->handle_event = nip47_event_handler_handle_event;
}

/* ============================================================================
 * GnostrUIExtension Interface Implementation
 * ============================================================================ */

static GtkWidget *create_nwc_settings_page(Nip47NwcPlugin *self, GnostrPluginContext *context);

static GtkWidget *nip47_ui_extension_create_settings_page(GnostrUIExtension *extension,
                                                          GnostrPluginContext *context) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(extension);
  return create_nwc_settings_page(self, context);
}

static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface) {
  iface->create_settings_page = nip47_ui_extension_create_settings_page;
  /* Menu items and note decorations not used by NWC */
  iface->create_menu_items = NULL;
  iface->create_note_decoration = NULL;
}

/* ============================================================================
 * Settings Page Widget
 * ============================================================================ */

typedef struct {
  Nip47NwcPlugin *plugin;
  GnostrPluginContext *context;
  gulong state_handler;
  GtkWidget *uri_entry;
  GtkWidget *connect_button;
  GtkWidget *disconnect_button;
  GtkWidget *status_label;
  GtkWidget *wallet_info_box;
  GtkWidget *balance_label;
} NwcSettingsPage;

static void update_settings_page_ui(NwcSettingsPage *page) {
  Nip47NwcPlugin *self = page->plugin;
  gboolean connected = nip47_nwc_plugin_is_connected(self);

  gtk_widget_set_sensitive(page->uri_entry, !connected);
  gtk_widget_set_visible(page->connect_button, !connected);
  gtk_widget_set_visible(page->disconnect_button, connected);
  gtk_widget_set_visible(page->wallet_info_box, connected);

  if (connected) {
    gchar *status = g_strdup_printf("Connected to %.16s...", self->wallet_pubkey_hex);
    gtk_label_set_text(GTK_LABEL(page->status_label), status);
    g_free(status);
    /* the pairing secret lives in the wallet agent, not in this entry */
    gtk_editable_set_text(GTK_EDITABLE(page->uri_entry), "");
  } else if (self->state == NIP47_NWC_STATE_CONNECTING) {
    gtk_label_set_text(GTK_LABEL(page->status_label),
                       "Waiting for approval in the Nostr Wallet agent...");
  } else if (self->state == NIP47_NWC_STATE_ERROR && self->last_error) {
    gtk_label_set_text(GTK_LABEL(page->status_label), self->last_error);
  } else {
    gtk_label_set_text(GTK_LABEL(page->status_label), "Not connected");
  }
}

static void
on_plugin_state_changed(Nip47NwcPlugin *plugin, gint state, gpointer user_data)
{
  (void)plugin; (void)state;
  update_settings_page_ui(user_data);
}

static void on_connect_clicked(GtkButton *button, NwcSettingsPage *page) {
  (void)button;
  const char *uri = gtk_editable_get_text(GTK_EDITABLE(page->uri_entry));

  GError *error = NULL;
  /* Hands the URI to org.nostr.Wallet1, which stores it in the keyring after
   * the user confirms; nothing is kept in plugin storage any more. */
  if (nip47_nwc_plugin_connect(page->plugin, uri, &error)) {
    update_settings_page_ui(page);
  } else {
    gtk_label_set_text(GTK_LABEL(page->status_label),
                       error ? error->message : "Connection failed");
    g_clear_error(&error);
  }
}

static void on_disconnect_clicked(GtkButton *button, NwcSettingsPage *page) {
  (void)button;
  nip47_nwc_plugin_disconnect(page->plugin);
  gtk_editable_set_text(GTK_EDITABLE(page->uri_entry), "");
  update_settings_page_ui(page);
}

static void on_balance_received(GObject *source, GAsyncResult *result, gpointer user_data) {
  NwcSettingsPage *page = (NwcSettingsPage *)user_data;
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(source);

  GError *error = NULL;
  gint64 balance_msat = 0;

  if (nip47_nwc_plugin_get_balance_finish(self, result, &balance_msat, &error)) {
    gchar *formatted = nip47_nwc_format_balance(balance_msat);
    gchar *text = g_strdup_printf("Balance: %s", formatted);
    gtk_label_set_text(GTK_LABEL(page->balance_label), text);
    g_free(text);
    g_free(formatted);
  } else {
    gtk_label_set_text(GTK_LABEL(page->balance_label),
                       error ? error->message : "Failed to get balance");
    g_clear_error(&error);
  }
}

static void on_refresh_balance_clicked(GtkButton *button, NwcSettingsPage *page) {
  (void)button;
  if (nip47_nwc_plugin_is_connected(page->plugin)) {
    gtk_label_set_text(GTK_LABEL(page->balance_label), "Loading...");
    nip47_nwc_plugin_get_balance_async(page->plugin, NULL, on_balance_received, page);
  }
}

static void settings_page_destroy(GtkWidget *widget, NwcSettingsPage *page) {
  (void)widget;
  if (page->state_handler)
    g_signal_handler_disconnect(page->plugin, page->state_handler);
  g_free(page);
}

static GtkWidget *create_nwc_settings_page(Nip47NwcPlugin *self, GnostrPluginContext *context) {
  NwcSettingsPage *page = g_new0(NwcSettingsPage, 1);
  page->plugin = self;
  page->context = context;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(box, 18);
  gtk_widget_set_margin_end(box, 18);
  gtk_widget_set_margin_top(box, 18);
  gtk_widget_set_margin_bottom(box, 18);

  /* Title */
  GtkWidget *title = gtk_label_new("Nostr Wallet Connect");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), title);

  /* Description */
  GtkWidget *desc = gtk_label_new(
    "Connect a Lightning wallet using the NIP-47 protocol. "
    "Paste your nostr+walletconnect:// URI below. The connection is kept by "
    "the Nostr Wallet agent (org.nostr.Wallet1) in your keyring and shared "
    "with other apps, each within its own spending budget.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_box_append(GTK_BOX(box), desc);

  /* URI Entry */
  page->uri_entry = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(page->uri_entry),
                                  "nostr+walletconnect://...");
  gtk_box_append(GTK_BOX(box), page->uri_entry);

  /* Button box */
  GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

  page->connect_button = gtk_button_new_with_label("Connect");
  gtk_widget_add_css_class(page->connect_button, "suggested-action");
  g_signal_connect(page->connect_button, "clicked", G_CALLBACK(on_connect_clicked), page);
  gtk_box_append(GTK_BOX(button_box), page->connect_button);

  page->disconnect_button = gtk_button_new_with_label("Disconnect");
  gtk_widget_add_css_class(page->disconnect_button, "destructive-action");
  g_signal_connect(page->disconnect_button, "clicked", G_CALLBACK(on_disconnect_clicked), page);
  gtk_box_append(GTK_BOX(button_box), page->disconnect_button);

  gtk_box_append(GTK_BOX(box), button_box);

  /* Status */
  page->status_label = gtk_label_new("");
  gtk_widget_set_halign(page->status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(box), page->status_label);

  /* Wallet info box (shown when connected) */
  page->wallet_info_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);

  GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_append(GTK_BOX(page->wallet_info_box), sep);

  GtkWidget *balance_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  page->balance_label = gtk_label_new("Balance: --");
  gtk_widget_set_hexpand(page->balance_label, TRUE);
  gtk_widget_set_halign(page->balance_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(balance_row), page->balance_label);

  GtkWidget *refresh_btn = gtk_button_new_from_icon_name("view-refresh-symbolic");
  g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_refresh_balance_clicked), page);
  gtk_box_append(GTK_BOX(balance_row), refresh_btn);

  gtk_box_append(GTK_BOX(page->wallet_info_box), balance_row);
  gtk_box_append(GTK_BOX(box), page->wallet_info_box);

  /* Cleanup */
  g_signal_connect(box, "destroy", G_CALLBACK(settings_page_destroy), page);
  page->state_handler = g_signal_connect(self, "state-changed",
                                         G_CALLBACK(on_plugin_state_changed), page);

  /* Initial state */
  update_settings_page_ui(page);

  return box;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

Nip47NwcPlugin *nip47_nwc_plugin_get_default(void) {
  return default_plugin;
}

gboolean nip47_nwc_plugin_connect(Nip47NwcPlugin *self,
                                  const gchar *connection_uri,
                                  GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(connection_uri != NULL, FALSE);

  /* Validate locally for immediate feedback; the agent validates again. */
  NostrNwcConnection conn = {0};
  if (nostr_nwc_uri_parse(connection_uri, &conn) != 0) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_INVALID_URI,
                "Invalid nostr+walletconnect:// URI");
    return FALSE;
  }
  nostr_nwc_connection_clear(&conn);

  g_clear_pointer(&self->last_error, g_free);
  if (!self->proxy) {
    if (!self->active) {
      g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_CONNECTION_FAILED,
                  "Plugin not activated");
      return FALSE;
    }
    /* proxy still being created: pair as soon as it is ready */
    wipe_free(self->pending_pair_uri);
    self->pending_pair_uri = g_strdup(connection_uri);
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_CONNECTING);
    return TRUE;
  }
  wallet_pair(self, connection_uri);
  return TRUE;
}

void nip47_nwc_plugin_disconnect(Nip47NwcPlugin *self) {
  g_return_if_fail(NIP47_IS_NWC_PLUGIN(self));
  if (!self->proxy) {
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_DISCONNECTED);
    return;
  }
  g_dbus_proxy_call(self->proxy, "Unpair", NULL, G_DBUS_CALL_FLAGS_NONE,
                    WALLET_CALL_TIMEOUT_MS, self->cancellable,
                    on_unpair_done, g_object_ref(self));
}

Nip47NwcState nip47_nwc_plugin_get_state(Nip47NwcPlugin *self) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), NIP47_NWC_STATE_DISCONNECTED);
  return self->state;
}

gboolean nip47_nwc_plugin_is_connected(Nip47NwcPlugin *self) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  return self->state == NIP47_NWC_STATE_CONNECTED && self->wallet_pubkey_hex != NULL;
}

const gchar *nip47_nwc_plugin_get_wallet_pubkey(Nip47NwcPlugin *self) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), NULL);
  return self->wallet_pubkey_hex;
}

const gchar *nip47_nwc_plugin_get_relay(Nip47NwcPlugin *self) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), NULL);
  return self->relay;
}

const gchar *nip47_nwc_plugin_get_lud16(Nip47NwcPlugin *self) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), NULL);
  return self->lud16;
}

/* ============================================================================
 * Async Operations
 * ============================================================================ */

static void on_wallet_call_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GTask *task = user_data;
  GError *error = NULL;
  GVariant *r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
  if (!r)
    g_task_return_error(task, map_wallet_error(error));
  else
    g_task_return_pointer(task, r, (GDestroyNotify)g_variant_unref);
  g_object_unref(task);
}

static void wallet_call(Nip47NwcPlugin *self, const gchar *method, GVariant *params,
                        GCancellable *cancellable, GAsyncReadyCallback callback,
                        gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  if (!self->proxy || !nip47_nwc_plugin_is_connected(self)) {
    if (params) g_variant_unref(g_variant_ref_sink(params));
    g_task_return_new_error(task, NIP47_NWC_ERROR, NIP47_NWC_ERROR_CONNECTION_FAILED,
                            "Not connected to wallet");
    g_object_unref(task);
    return;
  }
  g_dbus_proxy_call(self->proxy, method, params, G_DBUS_CALL_FLAGS_NONE,
                    WALLET_CALL_TIMEOUT_MS, cancellable, on_wallet_call_done, task);
}

static gboolean amount_to_u32(gint64 amount_msat, guint32 *out, GError **error) {
  if (amount_msat < 0 || amount_msat > (gint64)G_MAXUINT32) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "Amount %" G_GINT64_FORMAT " msat is out of range", amount_msat);
    return FALSE;
  }
  *out = (guint32)amount_msat;
  return TRUE;
}

void nip47_nwc_plugin_get_balance_async(Nip47NwcPlugin *self,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  g_return_if_fail(NIP47_IS_NWC_PLUGIN(self));
  wallet_call(self, "GetBalance", NULL, cancellable, callback, user_data);
}

gboolean nip47_nwc_plugin_get_balance_finish(Nip47NwcPlugin *self,
                                             GAsyncResult *result,
                                             gint64 *balance_msat,
                                             GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  g_autoptr(GVariant) r = g_task_propagate_pointer(G_TASK(result), error);
  if (!r) return FALSE;

  guint64 bal = 0;
  g_variant_get(r, "(t)", &bal);
  if (balance_msat) {
    *balance_msat = (gint64)MIN(bal, (guint64)G_MAXINT64);
    g_signal_emit(self, signals[SIGNAL_BALANCE_UPDATED], 0, *balance_msat);
  }
  return TRUE;
}

void nip47_nwc_plugin_pay_invoice_async(Nip47NwcPlugin *self,
                                        const gchar *bolt11,
                                        gint64 amount_msat,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  g_return_if_fail(NIP47_IS_NWC_PLUGIN(self));
  g_return_if_fail(bolt11 != NULL);

  GError *error = NULL;
  guint32 amount = 0;
  if (amount_msat > 0 && !amount_to_u32(amount_msat, &amount, &error)) {
    g_task_report_error(self, callback, user_data, nip47_nwc_plugin_pay_invoice_async, error);
    return;
  }
  g_debug("[NIP-47] Initiating pay_invoice via wallet agent for: %.40s...", bolt11);
  /* The agent applies this app's budget and asks the user when needed. */
  wallet_call(self, "PayInvoice", g_variant_new("(su)", bolt11, amount),
              cancellable, callback, user_data);
}

gboolean nip47_nwc_plugin_pay_invoice_finish(Nip47NwcPlugin *self,
                                             GAsyncResult *result,
                                             gchar **preimage,
                                             GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  g_autoptr(GVariant) r = g_task_propagate_pointer(G_TASK(result), error);
  if (!r) return FALSE;

  const gchar *pre = NULL;
  guint64 fees = 0;
  g_variant_get(r, "(&st)", &pre, &fees);
  if (preimage)
    *preimage = (pre && *pre) ? g_strdup(pre) : NULL;
  return TRUE;
}

void nip47_nwc_plugin_make_invoice_async(Nip47NwcPlugin *self,
                                         gint64 amount_msat,
                                         const gchar *description,
                                         gint64 expiry_secs,
                                         GCancellable *cancellable,
                                         GAsyncReadyCallback callback,
                                         gpointer user_data) {
  g_return_if_fail(NIP47_IS_NWC_PLUGIN(self));

  GError *error = NULL;
  guint32 amount = 0;
  if (!amount_to_u32(amount_msat, &amount, &error)) {
    g_task_report_error(self, callback, user_data, nip47_nwc_plugin_make_invoice_async, error);
    return;
  }
  guint32 expiry = expiry_secs > 0 ? (guint32)MIN(expiry_secs, (gint64)G_MAXUINT32) : 0;
  g_debug("[NIP-47] Initiating make_invoice via wallet agent for %" G_GINT64_FORMAT " msat",
          amount_msat);
  wallet_call(self, "MakeInvoice",
              g_variant_new("(usu)", amount, description ? description : "", expiry),
              cancellable, callback, user_data);
}

gboolean nip47_nwc_plugin_make_invoice_finish(Nip47NwcPlugin *self,
                                              GAsyncResult *result,
                                              gchar **bolt11,
                                              gchar **payment_hash,
                                              GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  g_autoptr(GVariant) r = g_task_propagate_pointer(G_TASK(result), error);
  if (!r) return FALSE;

  const gchar *inv = NULL, *hash = NULL;
  g_variant_get(r, "(&s&s)", &inv, &hash);
  if (bolt11) *bolt11 = (inv && *inv) ? g_strdup(inv) : NULL;
  if (payment_hash) *payment_hash = (hash && *hash) ? g_strdup(hash) : NULL;
  return TRUE;
}

/* ============================================================================
 * Utilities
 * ============================================================================ */

gchar *nip47_nwc_format_balance(gint64 balance_msat) {
  gint64 sats = balance_msat / 1000;

  if (sats >= 1000000) {
    return g_strdup_printf("%.2f M sats", sats / 1000000.0);
  } else if (sats >= 1000) {
    return g_strdup_printf("%'ld sats", (long)sats);
  } else {
    return g_strdup_printf("%ld sats", (long)sats);
  }
}

/* ============================================================================
 * Plugin Registration
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP47_TYPE_NWC_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_EVENT_HANDLER,
                                              NIP47_TYPE_NWC_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP47_TYPE_NWC_PLUGIN);
}
