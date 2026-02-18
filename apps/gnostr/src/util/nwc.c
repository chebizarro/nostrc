/**
 * gnostr NWC (Nostr Wallet Connect) Service
 *
 * NIP-47 implementation for the gnostr GTK app.
 * Implements NIP-47 wallet connect protocol with:
 * - NIP-04 encryption for request/response messages
 * - Relay communication via SimplePool
 * - Async request/response handling
 */

#include "nwc.h"
#include <nostr/nip47/nwc.h>
#include <nostr/nip47/nwc_client.h>
#include <nostr/nip47/nwc_envelope.h>
#include <nostr/nip04.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-relay.h>
#include <nostr-subscription.h>
#include <nostr-simple-pool.h>
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <channel.h>
#include <select.h>
#include <context.h>
#include <error.h>
#include <string.h>
#include <time.h>

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

/* NWC response timeout in milliseconds */
#define NWC_RESPONSE_TIMEOUT_MS 30000

/* Async request context */
typedef struct {
  GnostrNwcService *service;
  GTask *task;
  gchar *method;
  gchar *params_json;
  gchar *request_event_id;
  NostrRelay *relay;
  NostrSubscription *sub;
  guint timeout_id;
  GCancellable *cancellable;
} NwcRequestContext;

static void nwc_request_context_free(NwcRequestContext *ctx) {
  if (!ctx) return;

  if (ctx->timeout_id > 0) {
    g_source_remove(ctx->timeout_id);
    ctx->timeout_id = 0;
  }

  if (ctx->sub) {
    nostr_subscription_close(ctx->sub, NULL);
    nostr_subscription_free(ctx->sub);
    ctx->sub = NULL;
  }

  if (ctx->relay) {
    nostr_relay_disconnect(ctx->relay);
    nostr_relay_free(ctx->relay);
    ctx->relay = NULL;
  }

  g_clear_pointer(&ctx->method, g_free);
  g_clear_pointer(&ctx->params_json, g_free);
  g_clear_pointer(&ctx->request_event_id, g_free);
  g_clear_object(&ctx->cancellable);
  g_free(ctx);
}

/* Derive client public key from secret */
static gchar *derive_client_pubkey(const gchar *secret_hex) {
  if (!secret_hex || strlen(secret_hex) != 64) return NULL;

  /* Use nostr_key_get_public from libnostr */
  extern char *nostr_key_get_public(const char *sk);
  return nostr_key_get_public(secret_hex);
}

/* Build and sign a NWC request event with proper encryption */
static NostrEvent *build_signed_nwc_request(GnostrNwcService *self,
                                             const gchar *method,
                                             const gchar *params_json,
                                             gchar **out_event_id,
                                             GError **error) {
  if (!self->secret_hex || !self->wallet_pubkey_hex) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                "NWC connection not initialized");
    return NULL;
  }

  /* Build request body JSON */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);
  gnostr_json_builder_set_key(builder, "method");
  gnostr_json_builder_add_string(builder, method);
  gnostr_json_builder_set_key(builder, "params");
  if (params_json && *params_json && gnostr_json_is_valid(params_json)) {
    gnostr_json_builder_add_raw(builder, params_json);
  } else {
    gnostr_json_builder_begin_object(builder);
    gnostr_json_builder_end_object(builder);
  }
  gnostr_json_builder_end_object(builder);
  gchar *body_str = gnostr_json_builder_finish(builder);

  if (!body_str) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Failed to serialize request body");
    return NULL;
  }

  /* Encrypt content with NIP-04 */
  char *encrypted_content = NULL;
  char *encrypt_error = NULL;

  int enc_result = nostr_nip04_encrypt(
    body_str,
    self->wallet_pubkey_hex,
    self->secret_hex,
    &encrypted_content,
    &encrypt_error);

  g_free(body_str);

  if (enc_result != 0 || !encrypted_content) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "NIP-04 encryption failed: %s", encrypt_error ? encrypt_error : "unknown error");
    if (encrypt_error) free(encrypt_error);
    return NULL;
  }

  /* Build the event */
  NostrEvent *event = nostr_event_new();
  if (!event) {
    free(encrypted_content);
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Failed to create event");
    return NULL;
  }

  nostr_event_set_kind(event, NOSTR_EVENT_KIND_NWC_REQUEST);
  nostr_event_set_content(event, encrypted_content);
  nostr_event_set_created_at(event, (int64_t)time(NULL));
  free(encrypted_content);

  /* Derive client pubkey from secret and set as event pubkey */
  gchar *client_pubkey = derive_client_pubkey(self->secret_hex);
  if (client_pubkey) {
    nostr_event_set_pubkey(event, client_pubkey);
    g_free(client_pubkey);
  }

  /* Tags: [["p", wallet_pubkey]] */
  NostrTags *tags = nostr_tags_new(1);
  NostrTag *p_tag = nostr_tag_new("p", self->wallet_pubkey_hex, NULL);
  nostr_tags_set(tags, 0, p_tag);
  nostr_event_set_tags(event, tags);

  /* Sign the event with the client secret */
  if (nostr_event_sign(event, self->secret_hex) != 0) {
    nostr_event_free(event);
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Failed to sign event");
    return NULL;
  }

  /* Get event ID */
  if (out_event_id) {
    char *eid = nostr_event_get_id(event);
    if (eid) {
      *out_event_id = g_strdup(eid);
      free(eid);
    }
  }

  return event;
}

/* Parse and decrypt a NWC response.
 * out_result_json is set to a newly allocated raw JSON string for the "result" field.
 * Caller must g_free() out_result_json. */
static gboolean parse_nwc_response(GnostrNwcService *self,
                                   NostrEvent *event,
                                   const gchar *expected_request_id,
                                   char **out_result_json,
                                   GError **error) {
  if (!event || !self->secret_hex || !self->wallet_pubkey_hex) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Invalid response or connection state");
    return FALSE;
  }

  /* Verify this is a response event */
  if (nostr_event_get_kind(event) != NOSTR_EVENT_KIND_NWC_RESPONSE) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Unexpected event kind: %d", nostr_event_get_kind(event));
    return FALSE;
  }

  /* Check if response matches our request via e tag */
  NostrTags *tags = (NostrTags *)nostr_event_get_tags(event);
  gboolean found_request_ref = FALSE;
  if (tags && expected_request_id) {
    for (size_t i = 0; i < nostr_tags_size(tags); i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      const char *key = nostr_tag_get_key(tag);
      if (key && strcmp(key, "e") == 0 && nostr_tag_size(tag) >= 2) {
        const char *ref_id = nostr_tag_get_value(tag);
        if (ref_id && g_strcmp0(ref_id, expected_request_id) == 0) {
          found_request_ref = TRUE;
          break;
        }
      }
    }
  }

  if (expected_request_id && !found_request_ref) {
    /* Not our response, skip */
    return FALSE;
  }

  /* Decrypt content */
  const char *encrypted_content = nostr_event_get_content(event);
  if (!encrypted_content || !*encrypted_content) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Empty response content");
    return FALSE;
  }

  char *decrypted = NULL;
  char *decrypt_error = NULL;

  /* Get wallet pubkey from event pubkey (sender) */
  const char *sender_pubkey = nostr_event_get_pubkey(event);
  if (!sender_pubkey) sender_pubkey = self->wallet_pubkey_hex;

  int dec_result = nostr_nip04_decrypt(
    encrypted_content,
    sender_pubkey,
    self->secret_hex,
    &decrypted,
    &decrypt_error);

  if (dec_result != 0 || !decrypted) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "NIP-04 decryption failed: %s", decrypt_error ? decrypt_error : "unknown error");
    if (decrypt_error) free(decrypt_error);
    return FALSE;
  }

  /* Validate decrypted JSON */
  if (!gnostr_json_is_valid(decrypted)) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                "Failed to parse response JSON");
    free(decrypted);
    return FALSE;
  }

  /* Check for error in response - look for error.code or error.message */
  char *err_code = NULL;
  char *err_msg = NULL;
  if ((err_code = gnostr_json_get_string_at(decrypted, "error", "code", NULL)) != NULL  ||
      (err_msg = gnostr_json_get_string_at(decrypted, "error", "message", NULL)) != NULL ) {
    g_set_error(error, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_WALLET_ERROR,
                "Wallet error [%s]: %s",
                err_code ? err_code : "UNKNOWN",
                err_msg ? err_msg : "Unknown error");
    g_free(err_code);
    g_free(err_msg);
    free(decrypted);
    return FALSE;
  }

  /* Extract result as raw JSON */
  if (out_result_json) {
    char *result_json = NULL;
    result_json = gnostr_json_get_raw(decrypted, "result", NULL);
    if (result_json) {
      *out_result_json = result_json;
    } else {
      *out_result_json = NULL;
    }
  }

  free(decrypted);
  return TRUE;
}

/* Common async response handler context */
typedef struct {
  NwcRequestContext *req_ctx;
  GSourceFunc complete_callback;
  gpointer complete_data;
} NwcAsyncHandler;

/* Timeout callback for NWC requests */
static gboolean nwc_request_timeout(gpointer user_data) {
  NwcRequestContext *ctx = (NwcRequestContext *)user_data;
  ctx->timeout_id = 0;

  g_task_return_new_error(ctx->task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_TIMEOUT,
                          "NWC request timed out after %d ms", NWC_RESPONSE_TIMEOUT_MS);
  g_object_unref(ctx->task);
  nwc_request_context_free(ctx);

  return G_SOURCE_REMOVE;
}

/* Send NWC request and wait for response */
static void nwc_send_request_async(GnostrNwcService *self,
                                   const gchar *method,
                                   const gchar *params_json,
                                   GCancellable *cancellable,
                                   GAsyncReadyCallback callback,
                                   gpointer user_data);

/* Poll relay for NWC response - runs in background thread */
static gpointer nwc_response_poll_thread(gpointer user_data) {
  NwcRequestContext *ctx = (NwcRequestContext *)user_data;
  GnostrNwcService *self = ctx->service;

  if (!ctx->relay || !ctx->sub) {
    g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NWC relay or subscription not available");
    g_object_unref(ctx->task);
    nwc_request_context_free(ctx);
    return NULL;
  }

  /* nostrc-3gd6: Replaced try_receive + 10ms sleep with go_select_timeout.
   * Blocks until event/eose channel ready or 100ms timeout, then drains. */
  GoChannel *ch_events = nostr_subscription_get_events_channel(ctx->sub);
  GoChannel *ch_eose = nostr_subscription_get_eose_channel(ctx->sub);

  guint64 start_time = g_get_monotonic_time();
  const guint64 timeout_us = NWC_RESPONSE_TIMEOUT_MS * 1000;

  while (TRUE) {
    /* Check cancellation */
    if (ctx->cancellable && g_cancellable_is_cancelled(ctx->cancellable)) {
      g_task_return_new_error(ctx->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                              "NWC request cancelled");
      g_object_unref(ctx->task);
      nwc_request_context_free(ctx);
      return NULL;
    }

    /* Check timeout */
    if ((g_get_monotonic_time() - start_time) > timeout_us) {
      g_task_return_new_error(ctx->task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_TIMEOUT,
                              "NWC request timed out");
      g_object_unref(ctx->task);
      nwc_request_context_free(ctx);
      return NULL;
    }

    /* Block until any channel ready (100ms timeout for periodic checks) */
    {
      GoSelectCase cases[2];
      size_t nc = 0;
      if (ch_events) cases[nc++] = (GoSelectCase){.op = GO_SELECT_RECEIVE, .chan = ch_events, .recv_buf = NULL};
      if (ch_eose) cases[nc++] = (GoSelectCase){.op = GO_SELECT_RECEIVE, .chan = ch_eose, .recv_buf = NULL};
      if (nc > 0) go_select_timeout(cases, nc, 100);
    }

    /* Drain events */
    void *msg = NULL;
    while (ch_events && go_channel_try_receive(ch_events, &msg) == 0 && msg) {
      NostrEvent *event = (NostrEvent *)msg;

      char *result_json = NULL;
      GError *error = NULL;

      if (parse_nwc_response(self, event, ctx->request_event_id, &result_json, &error)) {
        g_task_return_pointer(ctx->task, result_json, g_free);
        g_object_unref(ctx->task);
        nostr_event_free(event);
        nwc_request_context_free(ctx);
        return NULL;
      }

      if (error) {
        g_task_return_error(ctx->task, error);
        g_object_unref(ctx->task);
        nostr_event_free(event);
        nwc_request_context_free(ctx);
        return NULL;
      }

      nostr_event_free(event);
      msg = NULL;
    }

    /* Drain EOSE (informational only) */
    if (ch_eose) go_channel_try_receive(ch_eose, NULL);
  }

  return NULL;
}

/* Send NWC request to relay and subscribe for response */
static void nwc_execute_request_async(GnostrNwcService *self,
                                      const gchar *method,
                                      const gchar *params_json,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);

  if (!gnostr_nwc_service_is_connected(self)) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "Not connected to wallet");
    g_object_unref(task);
    return;
  }

  if (!self->relays || !self->relays[0]) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "No relay configured");
    g_object_unref(task);
    return;
  }

  /* Build and sign the request event */
  GError *error = NULL;
  gchar *request_event_id = NULL;
  NostrEvent *request_event = build_signed_nwc_request(self, method, params_json,
                                                        &request_event_id, &error);
  if (!request_event) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  /* Create request context */
  NwcRequestContext *ctx = g_new0(NwcRequestContext, 1);
  ctx->service = self;
  ctx->task = task;
  ctx->method = g_strdup(method);
  ctx->params_json = params_json ? g_strdup(params_json) : NULL;
  ctx->request_event_id = request_event_id;
  ctx->cancellable = cancellable ? g_object_ref(cancellable) : NULL;

  /* Connect to relay */
  const char *relay_url = self->relays[0];
  GoContext *bg = go_context_background();
  Error *relay_err = NULL;

  ctx->relay = nostr_relay_new(bg, relay_url, &relay_err);
  if (!ctx->relay) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "Failed to create relay: %s",
                            relay_err ? relay_err->message : "unknown error");
    if (relay_err) free_error(relay_err);
    nostr_event_free(request_event);
    g_object_unref(task);
    nwc_request_context_free(ctx);
    return;
  }

  if (!nostr_relay_connect(ctx->relay, &relay_err)) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_CONNECTION_FAILED,
                            "Failed to connect to relay: %s",
                            relay_err ? relay_err->message : "unknown error");
    if (relay_err) free_error(relay_err);
    nostr_event_free(request_event);
    g_object_unref(task);
    nwc_request_context_free(ctx);
    return;
  }

  /* Derive client pubkey for the subscription filter */
  gchar *client_pubkey = derive_client_pubkey(self->secret_hex);
  if (!client_pubkey) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                            "Failed to derive client pubkey");
    nostr_event_free(request_event);
    g_object_unref(task);
    nwc_request_context_free(ctx);
    return;
  }

  /* Create subscription filter for NWC responses */
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();

  /* Filter: kind 23195 (NWC response), addressed to our pubkey, referencing our request */
  int kinds[] = { NOSTR_EVENT_KIND_NWC_RESPONSE };
  nostr_filter_set_kinds(filter, kinds, 1);

  /* Filter by wallet pubkey (author of response) */
  const char *authors[] = { self->wallet_pubkey_hex };
  nostr_filter_set_authors(filter, authors, 1);

  /* Filter by p-tag (response is to us) */
  nostr_filter_tags_append(filter, "p", client_pubkey, NULL);

  /* Filter by e-tag (references our request) */
  nostr_filter_tags_append(filter, "e", request_event_id, NULL);

  /* Set since to slightly before now to catch response */
  nostr_filter_set_since_i64(filter, (int64_t)(time(NULL) - 10));

  nostr_filters_add(filters, filter);
  g_free(client_pubkey);

  /* Prepare subscription */
  ctx->sub = nostr_relay_prepare_subscription(ctx->relay, bg, filters);
  if (!ctx->sub) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                            "Failed to prepare subscription");
    nostr_event_free(request_event);
    nostr_filters_free(filters);
    g_object_unref(task);
    nwc_request_context_free(ctx);
    return;
  }

  /* Fire subscription */
  Error *sub_err = NULL;
  if (!nostr_subscription_fire(ctx->sub, &sub_err)) {
    g_task_return_new_error(task, GNOSTR_NWC_ERROR, GNOSTR_NWC_ERROR_REQUEST_FAILED,
                            "Failed to fire subscription: %s",
                            sub_err ? sub_err->message : "unknown error");
    if (sub_err) free_error(sub_err);
    nostr_event_free(request_event);
    g_object_unref(task);
    nwc_request_context_free(ctx);
    return;
  }

  /* Publish the request event */
  nostr_relay_publish(ctx->relay, request_event);
  g_message("[NWC] Published %s request (event_id=%.16s...)", method, request_event_id);

  /* nostr_relay_publish doesn't take ownership, so we free here */
  nostr_event_free(request_event);

  /* Start background thread to poll for response */
  GThread *thread = g_thread_new("nwc-response-poll", nwc_response_poll_thread, ctx);
  g_thread_unref(thread);
}

/* Balance request implementation */
void gnostr_nwc_service_get_balance_async(GnostrNwcService *self,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback,
                                          gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_NWC_SERVICE(self));

  nwc_execute_request_async(self, "get_balance", NULL, cancellable, callback, user_data);
}

gboolean gnostr_nwc_service_get_balance_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gint64 *balance_msat,
                                               GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  char *response_json = g_task_propagate_pointer(G_TASK(result), error);
  if (!response_json) return FALSE;

  /* Extract balance from response: {"balance": <msats>} */
  if (balance_msat) {
    int64_t bal_val = 0;
    if ((bal_val = gnostr_json_get_int64(response_json, "balance", NULL), TRUE)) {
      *balance_msat = bal_val;
    } else {
      *balance_msat = 0;
    }

    /* Emit signal for balance update */
    g_signal_emit(self, signals[SIGNAL_BALANCE_UPDATED], 0, *balance_msat);
  }

  g_free(response_json);
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

  /* Build params JSON */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);
  gnostr_json_builder_set_key(builder, "invoice");
  gnostr_json_builder_add_string(builder, bolt11);
  if (amount_msat > 0) {
    gnostr_json_builder_set_key(builder, "amount");
    gnostr_json_builder_add_int(builder, amount_msat);
  }
  gnostr_json_builder_end_object(builder);
  gchar *params_json = gnostr_json_builder_finish(builder);

  g_message("[NWC] Initiating pay_invoice for: %.40s...", bolt11);

  nwc_execute_request_async(self, "pay_invoice", params_json, cancellable, callback, user_data);
  g_free(params_json);
}

gboolean gnostr_nwc_service_pay_invoice_finish(GnostrNwcService *self,
                                               GAsyncResult *result,
                                               gchar **preimage,
                                               GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  char *response_json = g_task_propagate_pointer(G_TASK(result), error);
  if (!response_json) return FALSE;

  /* Extract preimage from response: {"preimage": "..."} */
  if (preimage) {
    char *preimage_val = NULL;
    preimage_val = gnostr_json_get_string(response_json, "preimage", NULL);
    if (preimage_val) {
      *preimage = preimage_val;
    } else {
      *preimage = NULL;
    }
  }

  g_free(response_json);
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

  /* Build params JSON */
  g_autoptr(GNostrJsonBuilder) builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);
  gnostr_json_builder_set_key(builder, "amount");
  gnostr_json_builder_add_int(builder, amount_msat);
  if (description && *description) {
    gnostr_json_builder_set_key(builder, "description");
    gnostr_json_builder_add_string(builder, description);
  }
  if (expiry_secs > 0) {
    gnostr_json_builder_set_key(builder, "expiry");
    gnostr_json_builder_add_int(builder, expiry_secs);
  }
  gnostr_json_builder_end_object(builder);
  gchar *params_json = gnostr_json_builder_finish(builder);

  g_message("[NWC] Initiating make_invoice for %ld msat", (long)amount_msat);

  nwc_execute_request_async(self, "make_invoice", params_json, cancellable, callback, user_data);
  g_free(params_json);
}

gboolean gnostr_nwc_service_make_invoice_finish(GnostrNwcService *self,
                                                GAsyncResult *result,
                                                gchar **bolt11,
                                                gchar **payment_hash,
                                                GError **error) {
  g_return_val_if_fail(GNOSTR_IS_NWC_SERVICE(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  char *response_json = g_task_propagate_pointer(G_TASK(result), error);
  if (!response_json) return FALSE;

  /* Extract invoice from response: {"invoice": "...", "payment_hash": "..."} */
  if (bolt11) {
    char *invoice_val = NULL;
    invoice_val = gnostr_json_get_string(response_json, "invoice", NULL);
    if (invoice_val) {
      *bolt11 = invoice_val;
    } else {
      *bolt11 = NULL;
    }
  }

  if (payment_hash) {
    char *hash_val = NULL;
    hash_val = gnostr_json_get_string(response_json, "payment_hash", NULL);
    if (hash_val) {
      *payment_hash = hash_val;
    } else {
      *payment_hash = NULL;
    }
  }

  g_free(response_json);
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
