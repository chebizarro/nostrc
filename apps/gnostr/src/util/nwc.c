/**
 * gnostr NWC (Nostr Wallet Connect) service — client of the desktop wallet
 * agent (nostrc-prqu.13).
 *
 * The NIP-47 protocol, the pairing secret and the wallet relay connection
 * live in nostr-wallet-agent (org.nostr.Wallet1, gnome/dbus/
 * org.nostr.Wallet1.xml). GNostr never sees the secret: this service keeps
 * its public API (used by the zap dialog, the wallet dialog and Settings)
 * and forwards to D-Bus, like the nip47-nwc plugin (nostrc-yka8):
 *
 *   connect()           -> Pair(uri)        (confirmed by the agent's dialog)
 *   disconnect()        -> Unpair()         (confirmed by the agent's dialog)
 *   get_balance_async   -> GetBalance()
 *   pay_invoice_async   -> PayInvoice(bolt11, amount)  (agent budget/approval)
 *   make_invoice_async  -> MakeInvoice(amount, description, expiry)
 *   state/pubkey/relay/lud16 <- Paired / WalletPubkey / Relays / Lud16
 *
 * Older GNostr versions stored the whole nostr+walletconnect URI, secret
 * included, in plaintext GSettings (org.gnostr.Client nwc-connection-uri).
 * gnostr_nwc_service_start() hands such a URI to Pair() once and then
 * resets the key; nothing writes it any more.
 */

#include "nwc.h"
#include <nostr/nip47/nwc.h>
#include <string.h>

#define NWC_GSETTINGS_SCHEMA  "org.gnostr.Client"
#define NWC_GSETTINGS_KEY_URI "nwc-connection-uri"   /* legacy, read-only */

#define WALLET_BUS_NAME "org.nostr.Wallet1"
#define WALLET_PATH     "/org/nostr/Wallet1"
#define WALLET_IFACE    "org.nostr.Wallet1"

/* Payments may wait on the agent's approval dialog (120 s) plus the wallet
 * (60 s); do not let D-Bus time out first. */
#define WALLET_CALL_TIMEOUT_MS G_MAXINT

struct _GnostrNwcService {
  GObject parent_instance;

  GnostrNwcState state;
  gchar *last_error;

  /* Cached agent properties (never the secret) */
  gchar *wallet_pubkey_hex;
  gchar *relay;
  gchar *lud16;

  GDBusProxy *proxy;
  GCancellable *cancellable;
  gboolean started;
  gchar *pending_pair_uri;   /* connect() before the proxy exists */
  gboolean pair_is_migration;
};

G_DEFINE_TYPE(GnostrNwcService, gnostr_nwc_service, G_TYPE_OBJECT)

enum {
  SIGNAL_STATE_CHANGED,
  SIGNAL_BALANCE_UPDATED,
  N_SIGNALS
};
static guint signals[N_SIGNALS];

enum {
  PROP_0,
  PROP_STATE,
  PROP_WALLET_PUBKEY,
  PROP_RELAY,
  PROP_LUD16,
  N_PROPERTIES
};
static GParamSpec *properties[N_PROPERTIES];

static GnostrNwcService *default_service = NULL;

GQuark gnostr_nwc_error_quark(void) {
  return g_quark_from_static_string("gnostr-nwc-error");
}

/* ---- helpers ------------------------------------------------------------ */

static void wipe_free(gchar *s) {
  if (!s) return;
  memset(s, 0, strlen(s));
  g_free(s);
}

static void set_state(GnostrNwcService *self, GnostrNwcState state) {
  if (self->state == state) return;
  self->state = state;
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
  g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state);
}

static void set_error(GnostrNwcService *self, const gchar *message) {
  g_free(self->last_error);
  self->last_error = g_strdup(message);
  set_state(self, GNOSTR_NWC_STATE_ERROR);
}

static gchar *proxy_string(GDBusProxy *proxy, const gchar *name) {
  g_autoptr(GVariant) v = proxy ? g_dbus_proxy_get_cached_property(proxy, name) : NULL;
  if (!v || !g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)) return NULL;
  const gchar *s = g_variant_get_string(v, NULL);
  return (s && *s) ? g_strdup(s) : NULL;
}

static gboolean proxy_paired(GDBusProxy *proxy) {
  g_autoptr(GVariant) v = proxy ? g_dbus_proxy_get_cached_property(proxy, "Paired") : NULL;
  return v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
}

/* Refresh cached values from the agent's properties. */
static void sync_from_proxy(GnostrNwcService *self) {
  gboolean paired = proxy_paired(self->proxy);

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
    set_state(self, GNOSTR_NWC_STATE_CONNECTED);
  } else if (self->state != GNOSTR_NWC_STATE_CONNECTING && self->state != GNOSTR_NWC_STATE_ERROR) {
    set_state(self, GNOSTR_NWC_STATE_DISCONNECTED);
  }
}

/* Map an org.nostr.Wallet1 error onto GNOSTR_NWC_ERROR (takes @error). */
static GError *map_wallet_error(GError *error) {
  gint code = GNOSTR_NWC_ERROR_REQUEST_FAILED;
  g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
  if (remote) {
    if (g_str_has_suffix(remote, ".Timeout")) code = GNOSTR_NWC_ERROR_TIMEOUT;
    else if (g_str_has_suffix(remote, ".WalletError")) code = GNOSTR_NWC_ERROR_WALLET_ERROR;
    else if (g_str_has_suffix(remote, ".NotPaired")) code = GNOSTR_NWC_ERROR_CONNECTION_FAILED;
    else if (g_str_has_suffix(remote, ".InvalidArgs")) code = GNOSTR_NWC_ERROR_INVALID_URI;
    else if (g_str_has_suffix(remote, ".ServiceUnknown") ||
             g_str_has_suffix(remote, ".NameHasNoOwner")) code = GNOSTR_NWC_ERROR_CONNECTION_FAILED;
  } else if (g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
             g_error_matches(error, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)) {
    code = GNOSTR_NWC_ERROR_CONNECTION_FAILED;
  }
  g_dbus_error_strip_remote_error(error);
  GError *mapped = g_error_new_literal(GNOSTR_NWC_ERROR, code, error->message);
  g_error_free(error);
  return mapped;
}

/* ---- legacy GSettings URI ------------------------------------------------ */

static GSettings *legacy_settings(void) {
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  g_autoptr(GSettingsSchema) schema = source
    ? g_settings_schema_source_lookup(source, NWC_GSETTINGS_SCHEMA, TRUE) : NULL;
  if (!schema || !g_settings_schema_has_key(schema, NWC_GSETTINGS_KEY_URI))
    return NULL;
  return g_settings_new(NWC_GSETTINGS_SCHEMA);
}

static gchar *legacy_uri_load(void) {
  g_autoptr(GSettings) settings = legacy_settings();
  gchar *uri = settings ? g_settings_get_string(settings, NWC_GSETTINGS_KEY_URI) : NULL;
  if (uri && !*uri) {
    g_free(uri);
    return NULL;
  }
  return uri;
}

static void legacy_uri_clear(void) {
  g_autoptr(GSettings) settings = legacy_settings();
  if (settings)
    g_settings_reset(settings, NWC_GSETTINGS_KEY_URI);
}

/* ---- wallet agent calls -------------------------------------------------- */

static void on_pair_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(user_data);
  gboolean migration = self->pair_is_migration;
  self->pair_is_migration = FALSE;
  GError *error = NULL;
  g_autoptr(GVariant) r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
  if (!r) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_error_free(error);
      g_object_unref(self);
      return;
    }
    g_autofree gchar *remote = g_dbus_error_get_remote_error(error);
    /* The user (or the agent) refused, or the URI is junk: the plaintext
     * copy must not linger. Anything else (agent missing, timeout) keeps
     * it so the next start can retry. */
    gboolean final = remote && (g_str_has_suffix(remote, ".Denied") ||
                                g_str_has_suffix(remote, ".InvalidArgs"));
    error = map_wallet_error(error);
    if (final)
      g_warning("[NWC] Wallet agent did not pair: %s", error->message);
    else
      g_message("[NWC] Wallet agent unavailable, will retry: %s", error->message);
    if (migration && final) {
      legacy_uri_clear();
      g_message("[NWC] Removed the stored wallet connection (not moved into the wallet agent)");
    }
    set_error(self, error->message);
    g_error_free(error);
  } else {
    if (migration) {
      legacy_uri_clear();
      g_message("[NWC] Moved the stored wallet connection into the wallet agent");
    }
    /* Leave CONNECTING without a signal: sync_from_proxy() announces
     * CONNECTED now, or on the agent's PropertiesChanged if that comes
     * after this reply. */
    self->state = GNOSTR_NWC_STATE_DISCONNECTED;
    sync_from_proxy(self);
  }
  g_object_unref(self);
}

static void wallet_pair(GnostrNwcService *self, const gchar *uri, gboolean migration) {
  self->pair_is_migration = migration;
  set_state(self, GNOSTR_NWC_STATE_CONNECTING);
  g_dbus_proxy_call(self->proxy, "Pair", g_variant_new("(s)", uri),
                    G_DBUS_CALL_FLAGS_NONE, WALLET_CALL_TIMEOUT_MS, self->cancellable,
                    on_pair_done, g_object_ref(self));
}

static void on_unpair_done(GObject *source, GAsyncResult *res, gpointer user_data) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(user_data);
  GError *error = NULL;
  g_autoptr(GVariant) r = g_dbus_proxy_call_finish(G_DBUS_PROXY(source), res, &error);
  if (!r) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      error = map_wallet_error(error);
      g_message("[NWC] Wallet agent did not disconnect: %s", error->message);
    }
    g_error_free(error);
  }
  sync_from_proxy(self);
  g_object_unref(self);
}

static void on_properties_changed(GDBusProxy *proxy, GVariant *changed, GStrv invalidated,
                                  gpointer user_data) {
  (void)proxy; (void)changed; (void)invalidated;
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(user_data);
  if (self->state == GNOSTR_NWC_STATE_CONNECTING)
    return; /* on_pair_done decides */
  sync_from_proxy(self);
}

static void on_name_owner(GObject *proxy, GParamSpec *pspec, gpointer user_data) {
  (void)pspec;
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(user_data);
  g_autofree gchar *owner = g_dbus_proxy_get_name_owner(G_DBUS_PROXY(proxy));
  if (!owner && self->state == GNOSTR_NWC_STATE_CONNECTED)
    set_state(self, GNOSTR_NWC_STATE_DISCONNECTED);
}

/* Move a URI saved by pre-agent versions into the agent, then reset it. */
static void migrate_legacy_uri(GnostrNwcService *self) {
  gchar *uri = legacy_uri_load();
  if (!uri) return;
  if (proxy_paired(self->proxy)) {
    /* The agent already has a wallet: drop the stale plaintext copy. */
    legacy_uri_clear();
    g_message("[NWC] Removed the stored wallet connection; the wallet agent is already paired");
  } else {
    g_message("[NWC] Moving the stored wallet connection into the wallet agent");
    wallet_pair(self, uri, TRUE);
  }
  wipe_free(uri);
}

static void on_proxy_ready(GObject *source, GAsyncResult *res, gpointer user_data) {
  (void)source;
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(user_data);
  GError *error = NULL;
  GDBusProxy *proxy = g_dbus_proxy_new_for_bus_finish(res, &error);
  if (!proxy) {
    if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_message("[NWC] Wallet agent unavailable: %s", error->message);
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
    wallet_pair(self, uri, FALSE);
    wipe_free(uri);
  } else {
    migrate_legacy_uri(self);
  }
  g_object_unref(self);
}

/* ---- GObject -------------------------------------------------------------- */

static void gnostr_nwc_service_dispose(GObject *object) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(object);
  if (self->cancellable) g_cancellable_cancel(self->cancellable);
  g_clear_object(&self->cancellable);
  if (self->proxy) g_signal_handlers_disconnect_by_data(self->proxy, self);
  g_clear_object(&self->proxy);
  G_OBJECT_CLASS(gnostr_nwc_service_parent_class)->dispose(object);
}

static void gnostr_nwc_service_finalize(GObject *object) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(object);
  if (self == default_service) default_service = NULL;
  g_free(self->wallet_pubkey_hex);
  g_free(self->relay);
  g_free(self->lud16);
  g_free(self->last_error);
  wipe_free(self->pending_pair_uri);
  G_OBJECT_CLASS(gnostr_nwc_service_parent_class)->finalize(object);
}

static void gnostr_nwc_service_get_property(GObject *object, guint prop_id,
                                            GValue *value, GParamSpec *pspec) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(object);
  switch (prop_id) {
    case PROP_STATE:         g_value_set_int(value, self->state); break;
    case PROP_WALLET_PUBKEY: g_value_set_string(value, self->wallet_pubkey_hex); break;
    case PROP_RELAY:         g_value_set_string(value, self->relay); break;
    case PROP_LUD16:         g_value_set_string(value, self->lud16); break;
    default: G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gnostr_nwc_service_class_init(GnostrNwcServiceClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gnostr_nwc_service_dispose;
  object_class->finalize = gnostr_nwc_service_finalize;
  object_class->get_property = gnostr_nwc_service_get_property;

  properties[PROP_STATE] = g_param_spec_int(
    "state", "State", "Connection state",
    GNOSTR_NWC_STATE_DISCONNECTED, GNOSTR_NWC_STATE_ERROR, GNOSTR_NWC_STATE_DISCONNECTED,
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
    "state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT);
  signals[SIGNAL_BALANCE_UPDATED] = g_signal_new(
    "balance-updated", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_INT64);
}

static void gnostr_nwc_service_init(GnostrNwcService *self) {
  self->state = GNOSTR_NWC_STATE_DISCONNECTED;
}

/* ---- public API ------------------------------------------------------------ */

GnostrNwcService *gnostr_nwc_service_get_default(void) {
  if (!default_service)
    default_service = g_object_new(GNOSTR_TYPE_NWC_SERVICE, NULL);
  return default_service;
}

void gnostr_nwc_service_start(GnostrNwcService *self) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));
  if (self->started) return;
  self->started = TRUE;
  self->cancellable = g_cancellable_new();
  /* Flags NONE: auto-starts the D-Bus-activatable agent, so a paired
   * wallet is known before the first zap. */
  g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_NONE, NULL,
                           WALLET_BUS_NAME, WALLET_PATH, WALLET_IFACE,
                           self->cancellable, on_proxy_ready, g_object_ref(self));
}

gboolean gnostr_nwc_service_connect(GnostrNwcService *self,
                                    const gchar *connection_uri,
                                    GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(connection_uri != NULL, FALSE);

  /* Validate locally for immediate feedback; the agent validates again. */
  NostrNwcConnection conn = {0};
  if (nostr_nwc_uri_parse(connection_uri, &conn) != 0) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_INVALID_URI,
                "Invalid nostr+walletconnect:// URI");
    return FALSE;
  }
  nostr_nwc_connection_clear(&conn);
  g_clear_pointer(&self->last_error, g_free);

  if (!self->proxy) {
    /* Proxy still being created: pair as soon as it is ready. */
    gnostr_nwc_service_start(self);
    wipe_free(self->pending_pair_uri);
    self->pending_pair_uri = g_strdup(connection_uri);
    set_state(self, GNOSTR_NWC_STATE_CONNECTING);
    return TRUE;
  }
  wallet_pair(self, connection_uri, FALSE);
  return TRUE;
}

void gnostr_nwc_service_disconnect(GnostrNwcService *self) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));
  if (!self->proxy) {
    set_state(self, GNOSTR_NWC_STATE_DISCONNECTED);
    return;
  }
  g_dbus_proxy_call(self->proxy, "Unpair", NULL, G_DBUS_CALL_FLAGS_NONE,
                    WALLET_CALL_TIMEOUT_MS, self->cancellable,
                    on_unpair_done, g_object_ref(self));
}

GnostrNwcState gnostr_nwc_service_get_state(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), GNOSTR_NWC_STATE_DISCONNECTED);
  return self->state;
}

const gchar *gnostr_nwc_service_get_last_error(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), NULL);
  return self->last_error;
}

gboolean gnostr_nwc_service_is_connected(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  return self->state == GNOSTR_NWC_STATE_CONNECTED && self->wallet_pubkey_hex != NULL;
}

const gchar *gnostr_nwc_service_get_wallet_pubkey(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), NULL);
  return self->wallet_pubkey_hex;
}

const gchar *gnostr_nwc_service_get_relay(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), NULL);
  return self->relay;
}

const gchar *gnostr_nwc_service_get_lud16(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), NULL);
  return self->lud16;
}

/* ---- async requests ---------------------------------------------------------- */

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

static void wallet_call(GnostrNwcService *self, const gchar *method, GVariant *params,
                        gpointer source_tag, GCancellable *cancellable,
                        GAsyncReadyCallback callback, gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, source_tag);
  if (!self->proxy || !gnostr_nwc_service_is_connected(self)) {
    if (params) g_variant_unref(g_variant_ref_sink(params));
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "No wallet connected");
    g_object_unref(task);
    return;
  }
  g_dbus_proxy_call(self->proxy, method, params, G_DBUS_CALL_FLAGS_NONE,
                    WALLET_CALL_TIMEOUT_MS, cancellable, on_wallet_call_done, task);
}

static GVariant *wallet_call_finish(GnostrNwcService *self, GAsyncResult *result,
                                    gpointer source_tag, GError **error) {
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == source_tag, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void gnostr_nwc_service_get_balance_async(GnostrNwcService *self,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));
  wallet_call(self, "GetBalance", NULL, gnostr_nwc_service_get_balance_async,
              cancellable, callback, user_data);
}

gboolean gnostr_nwc_service_get_balance_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gint64 *balance_msat,
                                               GError **error) {
  g_autoptr(GVariant) r = wallet_call_finish(self, result,
                                             gnostr_nwc_service_get_balance_async, error);
  if (!r) return FALSE;
  guint64 msat = 0;
  g_variant_get(r, "(t)", &msat);
  gint64 value = msat > (guint64)G_MAXINT64 ? G_MAXINT64 : (gint64)msat;
  if (balance_msat) *balance_msat = value;
  g_signal_emit(self, signals[SIGNAL_BALANCE_UPDATED], 0, value);
  return TRUE;
}

void gnostr_nwc_service_pay_invoice_async(GnostrNwcService *self,
                                          const gchar *bolt11,
                                          gint64 amount_msat,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));
  g_return_if_fail(bolt11 != NULL);
  if (amount_msat < 0 || amount_msat > G_MAXUINT32) {
    /* org.nostr.Wallet1 amounts are u (msat) per call. */
    g_task_report_new_error(self, callback, user_data, gnostr_nwc_service_pay_invoice_async,
                            GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                            "Amount out of range for the wallet agent");
    return;
  }
  wallet_call(self, "PayInvoice", g_variant_new("(su)", bolt11, (guint32)amount_msat),
              gnostr_nwc_service_pay_invoice_async, cancellable, callback, user_data);
}

gboolean gnostr_nwc_service_pay_invoice_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gchar **preimage,
                                               GError **error) {
  g_autoptr(GVariant) r = wallet_call_finish(self, result,
                                             gnostr_nwc_service_pay_invoice_async, error);
  if (!r) return FALSE;
  const gchar *pre = NULL;
  guint64 fees = 0;
  g_variant_get(r, "(&st)", &pre, &fees);
  if (preimage) *preimage = g_strdup(pre);
  return TRUE;
}

void gnostr_nwc_service_make_invoice_async(GnostrNwcService *self,
                                           gint64 amount_msat,
                                           const gchar *description,
                                           gint64 expiry_secs,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));
  if (amount_msat <= 0 || amount_msat > G_MAXUINT32 || expiry_secs < 0 || expiry_secs > G_MAXUINT32) {
    g_task_report_new_error(self, callback, user_data, gnostr_nwc_service_make_invoice_async,
                            GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                            "Amount or expiry out of range for the wallet agent");
    return;
  }
  wallet_call(self, "MakeInvoice",
              g_variant_new("(usu)", (guint32)amount_msat, description ? description : "",
                            (guint32)expiry_secs),
              gnostr_nwc_service_make_invoice_async, cancellable, callback, user_data);
}

gboolean gnostr_nwc_service_make_invoice_finish(GnostrNwcService *self,
                                                GAsyncResult *result,
                                                gchar **bolt11,
                                                gchar **payment_hash,
                                                GError **error) {
  g_autoptr(GVariant) r = wallet_call_finish(self, result,
                                             gnostr_nwc_service_make_invoice_async, error);
  if (!r) return FALSE;
  const gchar *inv = NULL, *hash = NULL;
  g_variant_get(r, "(&s&s)", &inv, &hash);
  if (bolt11) *bolt11 = g_strdup(inv);
  if (payment_hash) *payment_hash = g_strdup(hash);
  return TRUE;
}

/* Utility: format balance for display */
gchar *gnostr_nwc_format_balance(gint64 balance_msat) {
  gint64 sats = balance_msat / 1000;
  if (sats >= 1000000)
    return g_strdup_printf("%.2f M sats", sats / 1000000.0);
  else if (sats >= 1000)
    return g_strdup_printf("%'ld sats", (long)sats);
  return g_strdup_printf("%ld sats", (long)sats);
}
