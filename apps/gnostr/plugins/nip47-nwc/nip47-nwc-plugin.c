/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip47-nwc-plugin.c - NIP-47 Nostr Wallet Connect Plugin
 *
 * Implements NIP-47 (Nostr Wallet Connect) for Lightning wallet integration.
 * Handles event kinds 13194 (info), 23194 (request), 23195 (response).
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip47-nwc-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>
#include <nostr/nip47/nwc.h>
#include <nostr/nip04.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <json.h>
#include <string.h>
#include <time.h>

/* Plugin data storage key for connection URI */
#define NWC_STORAGE_KEY_URI "connection-uri"

/* NWC response timeout in milliseconds */
#define NWC_RESPONSE_TIMEOUT_MS 30000

/* Singleton instance */
static Nip47NwcPlugin *default_plugin = NULL;

struct _Nip47NwcPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Connection state */
  Nip47NwcState state;
  gchar *last_error;

  /* Parsed connection data */
  gchar *wallet_pubkey_hex;
  gchar *secret_hex;
  gchar *client_pubkey_hex;
  gchar **relays;
  gchar *lud16;

  /* Pending requests: request_event_id -> GTask */
  GHashTable *pending_requests;

  /* Event subscription for NWC responses */
  guint64 response_subscription;
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

static void nip47_nwc_plugin_clear_connection(Nip47NwcPlugin *self) {
  g_clear_pointer(&self->wallet_pubkey_hex, g_free);
  g_clear_pointer(&self->secret_hex, g_free);
  g_clear_pointer(&self->client_pubkey_hex, g_free);
  g_clear_pointer(&self->lud16, g_free);
  g_clear_pointer(&self->last_error, g_free);

  if (self->relays) {
    g_strfreev(self->relays);
    self->relays = NULL;
  }
}

static void nip47_nwc_plugin_set_state(Nip47NwcPlugin *self, Nip47NwcState state) {
  if (self->state != state) {
    self->state = state;
    g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_STATE]);
    g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, state);
  }
}

/* Derive client public key from secret */
static gchar *derive_client_pubkey(const gchar *secret_hex) {
  if (!secret_hex || strlen(secret_hex) != 64) return NULL;

  /* Use nostr_key_get_public from libnostr */
  extern char *nostr_key_get_public(const char *sk);
  return nostr_key_get_public(secret_hex);
}

/* ============================================================================
 * NWC Request/Response Handling
 * ============================================================================ */

/* Pending request context */
typedef struct {
  Nip47NwcPlugin *plugin;
  gchar *request_event_id;
  gchar *method;
  guint timeout_id;
} NwcPendingRequest;

static void nwc_pending_request_free(NwcPendingRequest *req) {
  if (!req) return;
  if (req->timeout_id > 0) {
    g_source_remove(req->timeout_id);
  }
  g_free(req->request_event_id);
  g_free(req->method);
  g_free(req);
}

static gboolean nwc_request_timeout_cb(gpointer user_data) {
  NwcPendingRequest *req = (NwcPendingRequest *)user_data;
  Nip47NwcPlugin *self = req->plugin;

  req->timeout_id = 0;

  /* Find and complete the task with timeout error */
  GTask *task = g_hash_table_lookup(self->pending_requests, req->request_event_id);
  if (task) {
    g_task_return_new_error(task, NIP47_NWC_ERROR, NIP47_NWC_ERROR_TIMEOUT,
                            "NWC %s request timed out after %d ms",
                            req->method, NWC_RESPONSE_TIMEOUT_MS);
    g_hash_table_remove(self->pending_requests, req->request_event_id);
  }

  return G_SOURCE_REMOVE;
}

/* Build and sign a NWC request event using proper NostrEvent API */
static gchar *build_nwc_request_json(Nip47NwcPlugin *self,
                                     const gchar *method,
                                     const gchar *params_json,
                                     gchar **out_event_id,
                                     GError **error) {
  if (!self->secret_hex || !self->wallet_pubkey_hex || !self->client_pubkey_hex) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_CONNECTION_FAILED,
                "NWC connection not initialized");
    return NULL;
  }

  /* Build request body JSON: {"method": "...", "params": {...}} */
  GString *body = g_string_new("{\"method\":\"");
  g_string_append(body, method);
  g_string_append(body, "\",\"params\":");
  if (params_json && *params_json) {
    g_string_append(body, params_json);
  } else {
    g_string_append(body, "{}");
  }
  g_string_append(body, "}");

  /* Encrypt content with NIP-04 */
  char *encrypted_content = NULL;
  char *encrypt_error = NULL;

  int enc_result = nostr_nip04_encrypt(
    body->str,
    self->wallet_pubkey_hex,
    self->secret_hex,
    &encrypted_content,
    &encrypt_error);

  g_string_free(body, TRUE);

  if (enc_result != 0 || !encrypted_content) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "NIP-04 encryption failed: %s", encrypt_error ? encrypt_error : "unknown error");
    if (encrypt_error) free(encrypt_error);
    return NULL;
  }

  /* Create NostrEvent using proper API */
  NostrEvent *event = nostr_event_new();
  if (!event) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "Failed to create event");
    free(encrypted_content);
    return NULL;
  }

  /* Set event fields */
  nostr_event_set_kind(event, NWC_KIND_REQUEST);
  nostr_event_set_created_at(event, (int64_t)time(NULL));
  nostr_event_set_pubkey(event, self->client_pubkey_hex);
  nostr_event_set_content(event, encrypted_content);
  free(encrypted_content);

  /* Create "p" tag for wallet pubkey */
  NostrTag *p_tag = nostr_tag_new("p", self->wallet_pubkey_hex, NULL);
  NostrTags *tags = nostr_tags_new(1, p_tag);
  nostr_event_set_tags(event, tags);

  /* Sign the event */
  int sign_result = nostr_event_sign(event, self->secret_hex);
  if (sign_result != 0) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "Failed to sign event (code %d)", sign_result);
    nostr_event_free(event);
    return NULL;
  }

  /* Get event ID for tracking */
  if (out_event_id) {
    const char *id = nostr_event_get_id(event);
    *out_event_id = id ? g_strdup(id) : NULL;
  }

  /* Serialize to JSON */
  char *json = nostr_event_serialize(event);
  nostr_event_free(event);

  if (!json) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "Failed to serialize event");
    return NULL;
  }

  /* Transfer ownership to GLib */
  gchar *result = g_strdup(json);
  free(json);
  return result;
}

/* Parse and decrypt a NWC response */
static gboolean parse_nwc_response(Nip47NwcPlugin *self,
                                   GnostrPluginEvent *event,
                                   const gchar *expected_request_id,
                                   char **out_result_json,
                                   GError **error) {
  if (!event || !self->secret_hex || !self->wallet_pubkey_hex) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "Invalid response or connection state");
    return FALSE;
  }

  /* Verify this is a response event */
  if (gnostr_plugin_event_get_kind(event) != NWC_KIND_RESPONSE) {
    return FALSE;
  }

  /* Check if response matches our request via e tag */
  const char *ref_id = gnostr_plugin_event_get_tag_value(event, "e", 0);
  if (expected_request_id && (!ref_id || strcmp(ref_id, expected_request_id) != 0)) {
    /* Not our response */
    return FALSE;
  }

  /* Decrypt content */
  const char *encrypted_content = gnostr_plugin_event_get_content(event);
  if (!encrypted_content || !*encrypted_content) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "Empty response content");
    return FALSE;
  }

  char *decrypted = NULL;
  char *decrypt_error = NULL;

  /* Get sender pubkey for decryption */
  const char *sender_pubkey = gnostr_plugin_event_get_pubkey(event);
  if (!sender_pubkey) sender_pubkey = self->wallet_pubkey_hex;

  int dec_result = nostr_nip04_decrypt(
    encrypted_content,
    sender_pubkey,
    self->secret_hex,
    &decrypted,
    &decrypt_error);

  if (dec_result != 0 || !decrypted) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_REQUEST_FAILED,
                "NIP-04 decryption failed: %s", decrypt_error ? decrypt_error : "unknown error");
    if (decrypt_error) free(decrypt_error);
    return FALSE;
  }

  /* Check for error in response */
  extern int nostr_json_get_string_at(const char *json, const char *key1, const char *key2, char **out);
  char *err_code = NULL;
  char *err_msg = NULL;
  if (nostr_json_get_string_at(decrypted, "error", "code", &err_code) == 0 ||
      nostr_json_get_string_at(decrypted, "error", "message", &err_msg) == 0) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_WALLET_ERROR,
                "Wallet error [%s]: %s",
                err_code ? err_code : "UNKNOWN",
                err_msg ? err_msg : "Unknown error");
    g_free(err_code);
    g_free(err_msg);
    free(decrypted);
    return FALSE;
  }

  /* Extract result */
  if (out_result_json) {
    extern int nostr_json_get_raw(const char *json, const char *key, char **out);
    char *result = NULL;
    if (nostr_json_get_raw(decrypted, "result", &result) == 0 && result) {
      *out_result_json = result;
    } else {
      *out_result_json = NULL;
    }
  }

  free(decrypted);
  return TRUE;
}

/* Event subscription callback for NWC responses */
static void on_nwc_response_event(GnostrPluginEvent *event, gpointer user_data) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(user_data);

  if (gnostr_plugin_event_get_kind(event) != NWC_KIND_RESPONSE) {
    return;
  }

  /* Check which pending request this matches */
  const char *ref_id = gnostr_plugin_event_get_tag_value(event, "e", 0);
  if (!ref_id) return;

  GTask *task = g_hash_table_lookup(self->pending_requests, ref_id);
  if (!task) {
    /* Not our response or already handled */
    return;
  }

  /* Parse the response */
  GError *error = NULL;
  char *result_json = NULL;

  if (parse_nwc_response(self, event, ref_id, &result_json, &error)) {
    g_task_return_pointer(task, result_json, g_free);
  } else {
    g_task_return_error(task, error);
  }

  g_hash_table_remove(self->pending_requests, ref_id);
}

/* hq-gflmf: Context for async publish callback in NWC request flow */
typedef struct {
  Nip47NwcPlugin *plugin;    /* borrowed ref — lives as long as plugin is active */
  gchar *request_event_id;
  gchar *method;
  GTask *caller_task;        /* the task returned to the NWC caller */
} NwcPublishCtx;

static void
nwc_publish_ctx_free(NwcPublishCtx *ctx)
{
  if (!ctx) return;
  g_free(ctx->request_event_id);
  g_free(ctx->method);
  g_free(ctx);
}

/* hq-gflmf: Callback when async publish completes — runs on main thread */
static void
on_nwc_publish_done(GObject      *source G_GNUC_UNUSED,
                    GAsyncResult *res,
                    gpointer      user_data)
{
  NwcPublishCtx *ctx = (NwcPublishCtx *)user_data;
  GError *pub_error = NULL;

  gboolean ok = gnostr_plugin_context_publish_event_finish(NULL, res, &pub_error);

  if (!ok) {
    g_warning("[NIP-47] Failed to publish %s request: %s", ctx->method,
              pub_error ? pub_error->message : "unknown error");
    /* Remove from pending — the timeout will be cleaned up via task data */
    if (ctx->plugin && ctx->plugin->pending_requests) {
      g_hash_table_remove(ctx->plugin->pending_requests, ctx->request_event_id);
    }
    g_task_return_error(ctx->caller_task, pub_error);
    g_object_unref(ctx->caller_task);
    nwc_publish_ctx_free(ctx);
    return;
  }

  g_debug("[NIP-47] Published %s request (event_id=%.16s...)",
          ctx->method, ctx->request_event_id);

  /* Task stays alive in pending_requests until response or timeout */
  g_object_unref(ctx->caller_task);
  nwc_publish_ctx_free(ctx);
}

/* Execute a NWC request */
static void nwc_execute_request_async(Nip47NwcPlugin *self,
                                      const gchar *method,
                                      const gchar *params_json,
                                      GCancellable *cancellable,
                                      GAsyncReadyCallback callback,
                                      gpointer user_data) {
  GTask *task = g_task_new(self, cancellable, callback, user_data);

  if (!nip47_nwc_plugin_is_connected(self)) {
    g_task_return_new_error(task, NIP47_NWC_ERROR, NIP47_NWC_ERROR_CONNECTION_FAILED,
                            "Not connected to wallet");
    g_object_unref(task);
    return;
  }

  if (!self->context) {
    g_task_return_new_error(task, NIP47_NWC_ERROR, NIP47_NWC_ERROR_CONNECTION_FAILED,
                            "Plugin not activated");
    g_object_unref(task);
    return;
  }

  /* Build and sign the request event */
  GError *error = NULL;
  gchar *request_event_id = NULL;
  gchar *event_json = build_nwc_request_json(self, method, params_json,
                                              &request_event_id, &error);
  if (!event_json) {
    g_task_return_error(task, error);
    g_object_unref(task);
    return;
  }

  /* Register as pending request */
  g_hash_table_insert(self->pending_requests,
                      g_strdup(request_event_id),
                      g_object_ref(task));

  /* Set up timeout */
  NwcPendingRequest *pending = g_new0(NwcPendingRequest, 1);
  pending->plugin = self;
  pending->request_event_id = g_strdup(request_event_id);
  pending->method = g_strdup(method);
  pending->timeout_id = g_timeout_add(NWC_RESPONSE_TIMEOUT_MS,
                                       nwc_request_timeout_cb, pending);

  g_task_set_task_data(task, pending, (GDestroyNotify)nwc_pending_request_free);

  /* hq-gflmf: Publish asynchronously — relay connect + publish runs on a
   * GTask worker thread instead of blocking the main GTK thread. */
  NwcPublishCtx *pub_ctx = g_new0(NwcPublishCtx, 1);
  pub_ctx->plugin = self;
  pub_ctx->request_event_id = g_strdup(request_event_id);
  pub_ctx->method = g_strdup(method);
  pub_ctx->caller_task = g_object_ref(task);

  gnostr_plugin_context_publish_event_async(self->context, event_json,
                                            cancellable, on_nwc_publish_done,
                                            pub_ctx);

  g_free(request_event_id);
  g_free(event_json);
  g_object_unref(task);
}

/* ============================================================================
 * GObject Implementation
 * ============================================================================ */

static void nip47_nwc_plugin_dispose(GObject *object) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(object);

  if (self == default_plugin) {
    default_plugin = NULL;
  }

  nip47_nwc_plugin_clear_connection(self);
  g_clear_pointer(&self->pending_requests, g_hash_table_unref);

  G_OBJECT_CLASS(nip47_nwc_plugin_parent_class)->dispose(object);
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
      g_value_set_string(value, self->relays ? self->relays[0] : NULL);
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
  self->pending_requests = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                  g_free, g_object_unref);
}

/* ============================================================================
 * GnostrPlugin Interface Implementation
 * ============================================================================ */

static void nip47_nwc_plugin_activate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(plugin);

  g_debug("[NIP-47] Activating Nostr Wallet Connect plugin");

  self->context = context;
  self->active = TRUE;
  default_plugin = self;

  /* Subscribe to NWC response events */
  gchar *filter_json = g_strdup_printf(
    "{\"kinds\":[%d]}", NWC_KIND_RESPONSE);

  self->response_subscription = gnostr_plugin_context_subscribe_events(
    context, filter_json,
    G_CALLBACK(on_nwc_response_event), self, NULL);

  g_free(filter_json);

  /* Load saved connection from plugin data storage */
  GError *error = NULL;
  GBytes *stored = gnostr_plugin_context_load_data(context, NWC_STORAGE_KEY_URI, &error);
  if (stored) {
    gsize size;
    const gchar *uri = g_bytes_get_data(stored, &size);
    if (uri && size > 0) {
      GError *connect_error = NULL;
      if (!nip47_nwc_plugin_connect(self, uri, &connect_error)) {
        g_warning("[NIP-47] Failed to load saved connection: %s",
                  connect_error ? connect_error->message : "unknown error");
        g_clear_error(&connect_error);
      }
    }
    g_bytes_unref(stored);
  }
}

static void nip47_nwc_plugin_deactivate(GnostrPlugin *plugin, GnostrPluginContext *context) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(plugin);

  g_debug("[NIP-47] Deactivating Nostr Wallet Connect plugin");

  /* Unsubscribe from events */
  if (self->response_subscription > 0) {
    gnostr_plugin_context_unsubscribe_events(context, self->response_subscription);
    self->response_subscription = 0;
  }

  /* Cancel pending requests */
  g_hash_table_remove_all(self->pending_requests);

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
  return "1.0";
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

static gboolean nip47_event_handler_can_handle_kind(GnostrEventHandler *handler, int kind) {
  (void)handler;
  return kind == NWC_KIND_INFO || kind == NWC_KIND_REQUEST || kind == NWC_KIND_RESPONSE;
}

static gboolean nip47_event_handler_handle_event(GnostrEventHandler *handler,
                                                  GnostrPluginContext *context,
                                                  GnostrPluginEvent *event) {
  Nip47NwcPlugin *self = NIP47_NWC_PLUGIN(handler);
  (void)context;

  int kind = gnostr_plugin_event_get_kind(event);

  if (kind == NWC_KIND_RESPONSE) {
    /* Response handling is done via subscription callback */
    on_nwc_response_event(event, self);
    return TRUE;
  }

  /* INFO and REQUEST events would be handled by a wallet service, not a client */
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
  } else {
    gtk_label_set_text(GTK_LABEL(page->status_label), "Not connected");
  }
}

static void on_connect_clicked(GtkButton *button, NwcSettingsPage *page) {
  (void)button;
  const char *uri = gtk_editable_get_text(GTK_EDITABLE(page->uri_entry));

  GError *error = NULL;
  if (nip47_nwc_plugin_connect(page->plugin, uri, &error)) {
    /* Save to plugin storage */
    GBytes *bytes = g_bytes_new(uri, strlen(uri) + 1);
    gnostr_plugin_context_store_data(page->context, NWC_STORAGE_KEY_URI, bytes, NULL);
    g_bytes_unref(bytes);
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
  gnostr_plugin_context_delete_data(page->context, NWC_STORAGE_KEY_URI);
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
    "Paste your nostr+walletconnect:// URI below.");
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

  /* Clear any existing connection */
  nip47_nwc_plugin_clear_connection(self);
  nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_CONNECTING);

  /* Parse the connection URI using libnostr */
  NostrNwcConnection conn = {0};
  if (nostr_nwc_uri_parse(connection_uri, &conn) != 0) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_INVALID_URI,
                "Invalid nostr+walletconnect:// URI");
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_ERROR);
    return FALSE;
  }

  /* Copy parsed data */
  self->wallet_pubkey_hex = g_strdup(conn.wallet_pubkey_hex);
  self->secret_hex = g_strdup(conn.secret_hex);
  if (conn.lud16) {
    self->lud16 = g_strdup(conn.lud16);
  }

  /* Derive client pubkey */
  self->client_pubkey_hex = derive_client_pubkey(self->secret_hex);
  if (!self->client_pubkey_hex) {
    g_set_error(error, NIP47_NWC_ERROR, NIP47_NWC_ERROR_INVALID_URI,
                "Failed to derive client public key");
    nostr_nwc_connection_clear(&conn);
    nip47_nwc_plugin_clear_connection(self);
    nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_ERROR);
    return FALSE;
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

  nostr_nwc_connection_clear(&conn);

  nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_CONNECTED);

  /* Notify property changes */
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WALLET_PUBKEY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RELAY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);

  g_message("[NIP-47] Connected to wallet: %.16s...", self->wallet_pubkey_hex);

  return TRUE;
}

void nip47_nwc_plugin_disconnect(Nip47NwcPlugin *self) {
  g_return_if_fail(NIP47_IS_NWC_PLUGIN(self));

  nip47_nwc_plugin_clear_connection(self);
  nip47_nwc_plugin_set_state(self, NIP47_NWC_STATE_DISCONNECTED);

  /* Notify property changes */
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_WALLET_PUBKEY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_RELAY]);
  g_object_notify_by_pspec(G_OBJECT(self), properties[PROP_LUD16]);

  g_message("[NIP-47] Disconnected from wallet");
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
  return self->relays ? self->relays[0] : NULL;
}

const gchar *nip47_nwc_plugin_get_lud16(Nip47NwcPlugin *self) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), NULL);
  return self->lud16;
}

/* ============================================================================
 * Async Operations
 * ============================================================================ */

void nip47_nwc_plugin_get_balance_async(Nip47NwcPlugin *self,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer user_data) {
  g_return_if_fail(NIP47_IS_NWC_PLUGIN(self));
  nwc_execute_request_async(self, "get_balance", NULL, cancellable, callback, user_data);
}

gboolean nip47_nwc_plugin_get_balance_finish(Nip47NwcPlugin *self,
                                             GAsyncResult *result,
                                             gint64 *balance_msat,
                                             GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  char *response_json = g_task_propagate_pointer(G_TASK(result), error);
  if (!response_json) return FALSE;

  /* Extract balance from response: {"balance": <msats>} */
  if (balance_msat) {
    extern int nostr_json_get_int64(const char *json, const char *key, int64_t *out);
    int64_t bal_val = 0;
    if (nostr_json_get_int64(response_json, "balance", &bal_val) == 0) {
      *balance_msat = bal_val;
    } else {
      *balance_msat = 0;
    }

    g_signal_emit(self, signals[SIGNAL_BALANCE_UPDATED], 0, *balance_msat);
  }

  g_free(response_json);
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

  /* Build params JSON */
  GString *params = g_string_new("{\"invoice\":\"");
  g_string_append(params, bolt11);
  g_string_append(params, "\"");
  if (amount_msat > 0) {
    g_string_append_printf(params, ",\"amount\":%" G_GINT64_FORMAT, amount_msat);
  }
  g_string_append(params, "}");

  g_debug("[NIP-47] Initiating pay_invoice for: %.40s...", bolt11);

  nwc_execute_request_async(self, "pay_invoice", params->str, cancellable, callback, user_data);
  g_string_free(params, TRUE);
}

gboolean nip47_nwc_plugin_pay_invoice_finish(Nip47NwcPlugin *self,
                                             GAsyncResult *result,
                                             gchar **preimage,
                                             GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  char *response_json = g_task_propagate_pointer(G_TASK(result), error);
  if (!response_json) return FALSE;

  if (preimage) {
    extern int nostr_json_get_string(const char *json, const char *key, char **out);
    char *preimage_val = NULL;
    if (nostr_json_get_string(response_json, "preimage", &preimage_val) == 0) {
      *preimage = preimage_val;
    } else {
      *preimage = NULL;
    }
  }

  g_free(response_json);
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

  /* Build params JSON */
  GString *params = g_string_new("{");
  g_string_append_printf(params, "\"amount\":%" G_GINT64_FORMAT, amount_msat);
  if (description && *description) {
    g_string_append(params, ",\"description\":\"");
    /* Escape description for JSON */
    for (const char *p = description; *p; p++) {
      switch (*p) {
        case '"': g_string_append(params, "\\\""); break;
        case '\\': g_string_append(params, "\\\\"); break;
        case '\n': g_string_append(params, "\\n"); break;
        default: g_string_append_c(params, *p); break;
      }
    }
    g_string_append(params, "\"");
  }
  if (expiry_secs > 0) {
    g_string_append_printf(params, ",\"expiry\":%" G_GINT64_FORMAT, expiry_secs);
  }
  g_string_append(params, "}");

  g_debug("[NIP-47] Initiating make_invoice for %" G_GINT64_FORMAT " msat", amount_msat);

  nwc_execute_request_async(self, "make_invoice", params->str, cancellable, callback, user_data);
  g_string_free(params, TRUE);
}

gboolean nip47_nwc_plugin_make_invoice_finish(Nip47NwcPlugin *self,
                                              GAsyncResult *result,
                                              gchar **bolt11,
                                              gchar **payment_hash,
                                              GError **error) {
  g_return_val_if_fail(NIP47_IS_NWC_PLUGIN(self), FALSE);
  g_return_val_if_fail(g_task_is_valid(result, self), FALSE);

  char *response_json = g_task_propagate_pointer(G_TASK(result), error);
  if (!response_json) return FALSE;

  extern int nostr_json_get_string(const char *json, const char *key, char **out);

  if (bolt11) {
    char *invoice_val = NULL;
    if (nostr_json_get_string(response_json, "invoice", &invoice_val) == 0) {
      *bolt11 = invoice_val;
    } else {
      *bolt11 = NULL;
    }
  }

  if (payment_hash) {
    char *hash_val = NULL;
    if (nostr_json_get_string(response_json, "payment_hash", &hash_val) == 0) {
      *payment_hash = hash_val;
    } else {
      *payment_hash = NULL;
    }
  }

  g_free(response_json);
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
