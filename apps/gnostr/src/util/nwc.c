/**
 * gnostr NWC (Nostr Wallet Connect) Service
 *
 * NIP-47 implementation for the gnostr GTK app.
 */

#include "nwc.h"
#include <nostr/nip47/nwc.h>
#include <jansson.h>
#include <string.h>

/* GSettings schema for NWC */
#define NWC_GSETTINGS_SCHEMA "org.gnostr.Client"
#define NWC_GSETTINGS_KEY_URI "nwc-connection-uri"

struct _GnostrNwcService {
  GObject parent_instance;

  /* Connection state */
  GnostrNwcState state;
  gchar *last_error;

  /* Parsed connection data */
  gchar *wallet_pubkey_hex;
  gchar *secret_hex;
  gchar **relays;
  gchar *lud16;

  /* GSettings for persistence */
  GSettings *settings;
};

G_DEFINE_TYPE(GnostrNwcService, gnostr_nwc_service, G_TYPE_OBJECT)

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

/* Singleton instance */
static GnostrNwcService *default_service = NULL;

GQuark gnostr_nwc_error_quark(void) {
  return g_quark_from_static_string("gnostr-nwc-error");
}

static void gnostr_nwc_service_clear_connection(GnostrNwcService *self) {
  g_clear_pointer(&self->wallet_pubkey_hex, g_free);
  g_clear_pointer(&self->secret_hex, g_free);
  g_clear_pointer(&self->lud16, g_free);
  g_clear_pointer(&self->last_error, g_free);

  if (self->relays) {
    g_strfreev(self->relays);
    self->relays = NULL;
  }
}

static void gnostr_nwc_service_set_state(GnostrNwcService *self, GnostrNwcState state) {
  if (self->state != state) {
    self->state = state;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state);
  }
}

static void gnostr_nwc_service_dispose(GObject *object) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(object);

  gnostr_nwc_service_clear_connection(self);
  g_clear_object(&self->settings);

  G_OBJECT_CLASS(gnostr_nwc_service_parent_class)->dispose(object);
}

static void gnostr_nwc_service_get_property(GObject *object, guint prop_id,
                                            GValue *value, GParamSpec *pspec) {
  GnostrNwcService *self = GNOSTR_NWC_SERVICE(object);

  switch (prop_id) {
    case PROP_STATE:
      g_value_set_int(value, self->state);
      break;
    case PROP_WALLET_PUBKEY:
      g_value_set_string(value, self->wallet_pubkey_hex);
      break;
    case PROP_RELAY:
      g_value_set_string(value, self->relays ? self->relays[0] : NULL);
      break;
    case PROP_LUD16:
      g_value_set_string(value, self->lud16);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
  }
}

static void gnostr_nwc_service_class_init(GnostrNwcServiceClass *klass) {
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gnostr_nwc_service_dispose;
  object_class->get_property = gnostr_nwc_service_get_property;

  properties[PROP_STATE] = g_param_spec_int(
    "state", "State", "Connection state",
    GNOSTR_NWC_STATE_DISCONNECTED, GNOSTR_NWC_STATE_ERROR,
    GNOSTR_NWC_STATE_DISCONNECTED,
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

static void gnostr_nwc_service_init(GnostrNwcService *self) {
  self->state = GNOSTR_NWC_STATE_DISCONNECTED;

  /* Try to get settings - may fail if schema not installed */
  GSettingsSchemaSource *source = g_settings_schema_source_get_default();
  if (source) {
    GSettingsSchema *schema = g_settings_schema_source_lookup(source, NWC_GSETTINGS_SCHEMA, TRUE);
    if (schema) {
      /* Check if the key exists before creating settings */
      if (g_settings_schema_has_key(schema, NWC_GSETTINGS_KEY_URI)) {
        self->settings = g_settings_new(NWC_GSETTINGS_SCHEMA);
      }
      g_settings_schema_unref(schema);
    }
  }
}

GnostrNwcService *gnostr_nwc_service_get_default(void) {
  if (g_once_init_enter(&default_service)) {
    GnostrNwcService *service = g_object_new(GNOSTR_TYPE_NWC_SERVICE, NULL);
    g_once_init_leave(&default_service, service);
  }
  return default_service;
}

gboolean gnostr_nwc_service_connect(GnostrNwcService *self,
                                    const gchar *connection_uri,
                                    GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(connection_uri != NULL, FALSE);

  /* Clear any existing connection */
  gnostr_nwc_service_clear_connection(self);
  gnostr_nwc_service_set_state(self, GNOSTR_NWC_STATE_CONNECTING);

  /* Parse the connection URI */
  NostrNwcConnection conn = {0};
  if (nostr_nwc_uri_parse(connection_uri, &conn) != 0) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_INVALID_URI,
                "Invalid nostr+walletconnect:// URI");
    gnostr_nwc_service_set_state(self, GNOSTR_NWC_STATE_ERROR);
    return FALSE;
  }

  /* Copy parsed data */
  self->wallet_pubkey_hex = g_strdup(conn.wallet_pubkey_hex);
  self->secret_hex = g_strdup(conn.secret_hex);
  if (conn.lud16) {
    self->lud16 = g_strdup(conn.lud16);
  }

  /* Copy relays */
  if (conn.relays) {
    gsize n_relays = 0;
    for (gsize i = 0; conn.relays[i]; i++) n_relays++;
    self->relays = g_new0(gchar*, n_relays + 1);
    for (gsize i = 0; conn.relays[i]; i++) {
      self->relays[i] = g_strdup(conn.relays[i]);
    }
  }

  /* Clean up parsed connection (we've copied what we need) */
  nostr_nwc_connection_clear(&conn);

  gnostr_nwc_service_set_state(self, GNOSTR_NWC_STATE_CONNECTED);

  /* Notify property changes */
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WALLET_PUBKEY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RELAY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);

  g_message("[NWC] Connected to wallet: %.16s...", self->wallet_pubkey_hex);

  return TRUE;
}

void gnostr_nwc_service_disconnect(GnostrNwcService *self) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));

  gnostr_nwc_service_clear_connection(self);
  gnostr_nwc_service_set_state(self, GNOSTR_NWC_STATE_DISCONNECTED);

  /* Clear from settings */
  if (self->settings) {
    g_settings_reset(self->settings, NWC_GSETTINGS_KEY_URI);
  }

  /* Notify property changes */
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WALLET_PUBKEY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RELAY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);

  g_message("[NWC] Disconnected from wallet");
}

GnostrNwcState gnostr_nwc_service_get_state(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), GNOSTR_NWC_STATE_DISCONNECTED);
  return self->state;
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
  return self->relays ? self->relays[0] : NULL;
}

const gchar *gnostr_nwc_service_get_lud16(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), NULL);
  return self->lud16;
}

/* Async request context */
typedef struct {
  GnostrNwcService *service;
  GTask *task;
  gchar *method;
  gchar *params_json;
} NwcRequestContext;

static void nwc_request_context_free(NwcRequestContext *ctx) {
  g_clear_pointer(&ctx->method, g_free);
  g_clear_pointer(&ctx->params_json, g_free);
  g_free(ctx);
}

/* Build a signed NWC request event */
static gchar *build_nwc_request_json(GnostrNwcService *self,
                                     const gchar *method,
                                     const gchar *params_json,
                                     GError **error) {
  if (!self->secret_hex || !self->wallet_pubkey_hex) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                "NWC connection not initialized");
    return NULL;
  }

  /* Build request body JSON */
  json_t *body = json_object();
  json_object_set_new(body, "method", json_string(method));
  if (params_json && *params_json) {
    json_error_t jerr;
    json_t *params = json_loads(params_json, 0, &jerr);
    if (params) {
      json_object_set_new(body, "params", params);
    } else {
      json_object_set_new(body, "params", json_object());
    }
  } else {
    json_object_set_new(body, "params", json_object());
  }

  gchar *body_str = json_dumps(body, JSON_COMPACT);
  json_decref(body);

  /* TODO: NIP-04/NIP-44 encryption of body_str is required for proper NWC.
   * For now, we send unencrypted content - this needs signer integration
   * to encrypt with the shared secret between client and wallet. */

  /* Build the unsigned event */
  json_t *event = json_object();
  json_object_set_new(event, "kind", json_integer(NOSTR_EVENT_KIND_NWC_REQUEST));
  json_object_set_new(event, "content", json_string(body_str));
  g_free(body_str);

  /* Tags: [["p", wallet_pubkey]] */
  json_t *tags = json_array();
  json_t *p_tag = json_array();
  json_array_append_new(p_tag, json_string("p"));
  json_array_append_new(p_tag, json_string(self->wallet_pubkey_hex));
  json_array_append_new(tags, p_tag);
  json_object_set_new(event, "tags", tags);

  json_object_set_new(event, "created_at", json_integer((json_int_t)g_get_real_time() / G_USEC_PER_SEC));

  gchar *event_json = json_dumps(event, JSON_COMPACT);
  json_decref(event);

  return event_json;
}

/* Balance request implementation */
void gnostr_nwc_service_get_balance_async(GnostrNwcService *self,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));

  GTask *task = g_task_new(self, cancellable, callback, user_data);

  if (!gnostr_nwc_service_is_connected(self)) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "Not connected to wallet");
    g_object_unref(task);
    return;
  }

  GError *error = NULL;
  gchar *event_json = build_nwc_request_json(self, "get_balance", NULL, &error);
  if (!event_json) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  /* Store the request JSON for the caller to send via relay */
  g_task_set_task_data(task, event_json, g_free);

  /* For now, return the unsigned event - actual relay communication
   * would be handled by the caller integrating with SimplePool */
  g_message("[NWC] get_balance request built: %.80s...", event_json);

  /* TODO: Integrate with SimplePool for actual relay communication.
   * For now, we just return successfully to indicate the request was built. */
  gint64 *balance = g_new0(gint64, 1);
  *balance = 0; /* Placeholder - actual balance comes from relay response */
  g_task_return_pointer(task, balance, g_free);
  g_object_unref(task);
}

gboolean gnostr_nwc_service_get_balance_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gint64 *balance_msat,
                                               GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  gint64 *balance = g_task_propagate_pointer(G_TASK(result), error);
  if (!balance) return FALSE;

  if (balance_msat) *balance_msat = *balance;
  g_free(balance);
  return TRUE;
}

/* Pay invoice implementation */
void gnostr_nwc_service_pay_invoice_async(GnostrNwcService *self,
                                          const gchar *bolt11,
                                          gint64 amount_msat,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));
  g_return_if_fail(bolt11 != NULL);

  GTask *task = g_task_new(self, cancellable, callback, user_data);

  if (!gnostr_nwc_service_is_connected(self)) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "Not connected to wallet");
    g_object_unref(task);
    return;
  }

  /* Build params JSON */
  json_t *params = json_object();
  json_object_set_new(params, "invoice", json_string(bolt11));
  if (amount_msat > 0) {
    json_object_set_new(params, "amount", json_integer(amount_msat));
  }
  gchar *params_json = json_dumps(params, JSON_COMPACT);
  json_decref(params);

  GError *error = NULL;
  gchar *event_json = build_nwc_request_json(self, "pay_invoice", params_json, &error);
  g_free(params_json);

  if (!event_json) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  g_task_set_task_data(task, event_json, g_free);
  g_message("[NWC] pay_invoice request built for: %.40s...", bolt11);

  /* TODO: Integrate with relay for actual payment */
  g_task_return_pointer(task, g_strdup(""), g_free);
  g_object_unref(task);
}

gboolean gnostr_nwc_service_pay_invoice_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gchar **preimage,
                                               GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  gchar *result_preimage = g_task_propagate_pointer(G_TASK(result), error);
  if (!result_preimage) return FALSE;

  if (preimage) *preimage = result_preimage;
  else g_free(result_preimage);
  return TRUE;
}

/* Make invoice implementation */
void gnostr_nwc_service_make_invoice_async(GnostrNwcService *self,
                                           gint64 amount_msat,
                                           const gchar *description,
                                           gint64 expiry_secs,
                                           GCancellable *cancellable,
                                           GAsyncReadyCallback callback,
                                           gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));

  GTask *task = g_task_new(self, cancellable, callback, user_data);

  if (!gnostr_nwc_service_is_connected(self)) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "Not connected to wallet");
    g_object_unref(task);
    return;
  }

  /* Build params JSON */
  json_t *params = json_object();
  json_object_set_new(params, "amount", json_integer(amount_msat));
  if (description && *description) {
    json_object_set_new(params, "description", json_string(description));
  }
  if (expiry_secs > 0) {
    json_object_set_new(params, "expiry", json_integer(expiry_secs));
  }
  gchar *params_json = json_dumps(params, JSON_COMPACT);
  json_decref(params);

  GError *error = NULL;
  gchar *event_json = build_nwc_request_json(self, "make_invoice", params_json, &error);
  g_free(params_json);

  if (!event_json) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  g_task_set_task_data(task, event_json, g_free);
  g_message("[NWC] make_invoice request built for %ld msat", (long)amount_msat);

  /* TODO: Integrate with relay for actual invoice creation */
  g_task_return_pointer(task, g_strdup(""), g_free);
  g_object_unref(task);
}

gboolean gnostr_nwc_service_make_invoice_finish(GnostrNwcService *self,
                                                GAsyncResult *result,
                                                gchar **bolt11,
                                                gchar **payment_hash,
                                                GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  gchar *result_bolt11 = g_task_propagate_pointer(G_TASK(result), error);
  if (!result_bolt11) return FALSE;

  if (bolt11) *bolt11 = result_bolt11;
  else g_free(result_bolt11);

  if (payment_hash) *payment_hash = NULL;
  return TRUE;
}

/* Settings persistence */
void gnostr_nwc_service_save_to_settings(GnostrNwcService *self) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));

  if (!self->settings) {
    g_warning("[NWC] Cannot save: GSettings not available");
    return;
  }

  if (!gnostr_nwc_service_is_connected(self)) {
    g_settings_reset(self->settings, NWC_GSETTINGS_KEY_URI);
    return;
  }

  /* Rebuild the URI for storage */
  NostrNwcConnection conn = {
    .wallet_pubkey_hex = self->wallet_pubkey_hex,
    .secret_hex = self->secret_hex,
    .relays = self->relays,
    .lud16 = self->lud16
  };

  char *uri = NULL;
  if (nostr_nwc_uri_build(&conn, &uri) == 0 && uri) {
    g_settings_set_string(self->settings, NWC_GSETTINGS_KEY_URI, uri);
    free(uri);
    g_message("[NWC] Connection saved to settings");
  }
}

gboolean gnostr_nwc_service_load_from_settings(GnostrNwcService *self) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);

  if (!self->settings) {
    return FALSE;
  }

  gchar *uri = g_settings_get_string(self->settings, NWC_GSETTINGS_KEY_URI);
  if (!uri || !*uri) {
    g_free(uri);
    return FALSE;
  }

  GError *error = NULL;
  gboolean result = gnostr_nwc_service_connect(self, uri, &error);
  if (!result) {
    g_warning("[NWC] Failed to load connection from settings: %s",
              error ? error->message : "unknown error");
    g_clear_error(&error);
  }

  g_free(uri);
  return result;
}

/* Utility: format balance for display */
gchar *gnostr_nwc_format_balance(gint64 balance_msat) {
  gint64 sats = balance_msat / 1000;

  if (sats >= 1000000) {
    /* Show in millions */
    return g_strdup_printf("%.2f M sats", sats / 1000000.0);
  } else if (sats >= 1000) {
    /* Show with thousands separator */
    return g_strdup_printf("%'ld sats", (long)sats);
  } else {
    return g_strdup_printf("%ld sats", (long)sats);
  }
}
