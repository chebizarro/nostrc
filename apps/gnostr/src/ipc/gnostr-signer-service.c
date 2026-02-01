/**
 * GnostrSignerService - Unified Signing Service Implementation
 */

#include "gnostr-signer-service.h"
#include "signer_ipc.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip04.h"
#include "nostr-keys.h"
#include "nostr-event.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "nostr-relay.h"
#include "nostr-subscription.h"
#include "select.h"
#include "context.h"
#include "error.h"
#include "secure_buf.h"
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>

/* Forward declaration for nostrc-1wfi */
void gnostr_signer_service_clear_saved_credentials(GnostrSignerService *self);

struct _GnostrSignerService {
  GObject parent_instance;

  /* Current signing method */
  GnostrSignerMethod method;

  /* User's public key (hex) */
  char *pubkey_hex;

  /* NIP-46 session (owned if method is NIP46) */
  NostrNip46Session *nip46_session;

  /* NIP-55L proxy (lazy initialized) */
  NostrSignerProxy *nip55l_proxy;
};

G_DEFINE_TYPE(GnostrSignerService, gnostr_signer_service, G_TYPE_OBJECT)

/* Global default instance */
static GnostrSignerService *s_default_service = NULL;

static void
gnostr_signer_service_dispose(GObject *object)
{
  GnostrSignerService *self = GNOSTR_SIGNER_SERVICE(object);

  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  /* Don't free the proxy - it's shared via signer_ipc */
  self->nip55l_proxy = NULL;

  G_OBJECT_CLASS(gnostr_signer_service_parent_class)->dispose(object);
}

static void
gnostr_signer_service_finalize(GObject *object)
{
  GnostrSignerService *self = GNOSTR_SIGNER_SERVICE(object);

  g_free(self->pubkey_hex);

  G_OBJECT_CLASS(gnostr_signer_service_parent_class)->finalize(object);
}

static void
gnostr_signer_service_class_init(GnostrSignerServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gnostr_signer_service_dispose;
  object_class->finalize = gnostr_signer_service_finalize;
}

static void
gnostr_signer_service_init(GnostrSignerService *self)
{
  self->method = GNOSTR_SIGNER_METHOD_NONE;
  self->pubkey_hex = NULL;
  self->nip46_session = NULL;
  self->nip55l_proxy = NULL;
}

GnostrSignerService *
gnostr_signer_service_new(void)
{
  return g_object_new(GNOSTR_TYPE_SIGNER_SERVICE, NULL);
}

GnostrSignerService *
gnostr_signer_service_get_default(void)
{
  if (!s_default_service) {
    s_default_service = gnostr_signer_service_new();
  }
  return s_default_service;
}

void
gnostr_signer_service_set_nip46_session(GnostrSignerService *self,
                                         NostrNip46Session *session)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));

  /* Free old session if any */
  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  if (session) {
    self->nip46_session = session;
    self->method = GNOSTR_SIGNER_METHOD_NIP46;
    g_debug("[SIGNER_SERVICE] Switched to NIP-46 remote signer");
  } else {
    /* Check if NIP-55L is available as fallback */
    GError *error = NULL;
    self->nip55l_proxy = gnostr_signer_proxy_get(&error);
    if (self->nip55l_proxy) {
      self->method = GNOSTR_SIGNER_METHOD_NIP55L;
      g_debug("[SIGNER_SERVICE] Using NIP-55L local signer");
    } else {
      self->method = GNOSTR_SIGNER_METHOD_NONE;
      g_debug("[SIGNER_SERVICE] No signer available");
      if (error) g_error_free(error);
    }
  }
}

GnostrSignerMethod
gnostr_signer_service_get_method(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), GNOSTR_SIGNER_METHOD_NONE);
  return self->method;
}

gboolean
gnostr_signer_service_is_available(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), FALSE);
  return self->method != GNOSTR_SIGNER_METHOD_NONE;
}

const char *
gnostr_signer_service_get_pubkey(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), NULL);
  return self->pubkey_hex;
}

void
gnostr_signer_service_set_pubkey(GnostrSignerService *self,
                                  const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));

  g_free(self->pubkey_hex);
  self->pubkey_hex = g_strdup(pubkey_hex);
}

void
gnostr_signer_service_clear(GnostrSignerService *self)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));

  if (self->nip46_session) {
    nostr_nip46_session_free(self->nip46_session);
    self->nip46_session = NULL;
  }

  g_free(self->pubkey_hex);
  self->pubkey_hex = NULL;

  self->nip55l_proxy = NULL;
  self->method = GNOSTR_SIGNER_METHOD_NONE;

  /* nostrc-1wfi: Also clear persisted credentials on logout */
  gnostr_signer_service_clear_saved_credentials(self);

  g_debug("[SIGNER_SERVICE] Cleared all authentication state");
}

/* ---- Async Signing Implementation ---- */

typedef struct {
  GnostrSignerService *service;
  GnostrSignerCallback callback;
  gpointer user_data;
  char *event_json;
} SignContext;

static void
sign_context_free(SignContext *ctx)
{
  if (!ctx) return;
  g_free(ctx->event_json);
  g_free(ctx);
}

/* NIP-55L callback */
static void
on_nip55l_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SignContext *ctx = (SignContext *)user_data;
  NostrSignerProxy *proxy = (NostrSignerProxy *)source;

  GError *error = NULL;
  char *signed_event_json = NULL;

  gboolean ok = nostr_org_nostr_signer_call_sign_event_finish(
      proxy, &signed_event_json, res, &error);

  if (!ok) {
    if (!error) {
      error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "Signing failed");
    }
    ctx->callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
  } else {
    ctx->callback(ctx->service, signed_event_json, NULL, ctx->user_data);
    g_free(signed_event_json);
  }

  sign_context_free(ctx);
}

/* nostrc-e5k6: NIP-46 signing relay round-trip context
 * Uses channel-based waiting with go_select instead of timeout-based pthread_cond */
typedef struct {
  char *expected_client_pk;   /* Our pubkey to filter responses */
  volatile gboolean relay_disconnected;  /* Set by state callback on disconnect */
} Nip46SignRoundtripCtx;

/* Relay state callback - signals when relay disconnects */
static void
nip46_relay_state_callback(NostrRelay *relay,
                            NostrRelayConnectionState old_state,
                            NostrRelayConnectionState new_state,
                            void *user_data)
{
  Nip46SignRoundtripCtx *ctx = (Nip46SignRoundtripCtx *)user_data;
  (void)relay;
  (void)old_state;

  if (new_state == NOSTR_RELAY_STATE_DISCONNECTED) {
    g_warning("[SIGNER_SERVICE] Relay disconnected during NIP-46 round-trip");
    if (ctx) {
      ctx->relay_disconnected = TRUE;
    }
  }
}

/* Helper to check if an event is a NIP-46 response addressed to us */
static gboolean
nip46_event_is_for_client(NostrEvent *ev, const char *client_pubkey)
{
  if (!ev || !client_pubkey) return FALSE;

  int kind = nostr_event_get_kind(ev);
  if (kind != NOSTR_EVENT_KIND_NIP46) return FALSE;

  NostrTags *tags = nostr_event_get_tags(ev);
  if (!tags) return FALSE;

  size_t tag_count = nostr_tags_size(tags);
  for (size_t i = 0; i < tag_count; i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    if (!tag || nostr_tag_size(tag) < 2) continue;
    const char *key = nostr_tag_get(tag, 0);
    const char *value = nostr_tag_get(tag, 1);
    if (key && value && strcmp(key, "p") == 0 &&
        strcmp(value, client_pubkey) == 0) {
      return TRUE;
    }
  }
  return FALSE;
}

/* Helper to convert hex string to bytes */
static int
hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
  if (!hex || !out || strlen(hex) != out_len * 2) return -1;
  for (size_t i = 0; i < out_len; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
    out[i] = (uint8_t)byte;
  }
  return 0;
}

/* NIP-46 signing (runs in thread) - nostrc-stsz: Complete relay round-trip */
static void
nip46_sign_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  SignContext *ctx = (SignContext *)task_data;
  (void)source;
  (void)cancellable;

  if (!ctx->service->nip46_session) {
    g_warning("[SIGNER_SERVICE] NIP-46 sign failed: session is NULL");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session not available - please sign in again");
    return;
  }

  g_debug("[SIGNER_SERVICE] NIP-46 signing event: %.80s...", ctx->event_json);

  /* Get session info */
  char *client_secret = NULL;
  char *signer_pubkey = NULL;
  char **relays = NULL;
  size_t n_relays = 0;

  nostr_nip46_session_get_secret(ctx->service->nip46_session, &client_secret);
  nostr_nip46_session_get_remote_pubkey(ctx->service->nip46_session, &signer_pubkey);
  nostr_nip46_session_get_relays(ctx->service->nip46_session, &relays, &n_relays);

  if (!client_secret || !signer_pubkey) {
    g_warning("[SIGNER_SERVICE] NIP-46 session missing credentials");
    free(client_secret);
    free(signer_pubkey);
    if (relays) { for (size_t i = 0; i < n_relays; i++) free(relays[i]); free(relays); }
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session credentials not configured");
    return;
  }

  /* Get relay URL from settings if not in session */
  char *relay_url = NULL;
  if (n_relays > 0 && relays && relays[0]) {
    relay_url = g_strdup(relays[0]);
  } else {
    GSettings *settings = g_settings_new("org.gnostr.Client");
    if (settings) {
      relay_url = g_settings_get_string(settings, "nip46-relay");
      g_object_unref(settings);
    }
    if (!relay_url || !*relay_url) {
      g_free(relay_url);
      relay_url = g_strdup("wss://relay.nsec.app");
    }
  }

  /* Derive client pubkey for subscription filter */
  char *client_pubkey = nostr_key_get_public(client_secret);
  if (!client_pubkey) {
    g_warning("[SIGNER_SERVICE] Failed to derive client pubkey");
    free(client_secret);
    free(signer_pubkey);
    g_free(relay_url);
    if (relays) { for (size_t i = 0; i < n_relays; i++) free(relays[i]); free(relays); }
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to derive client pubkey");
    return;
  }

  g_debug("[SIGNER_SERVICE] NIP-46 round-trip: relay=%s, client=%.16s..., signer=%.16s...",
          relay_url, client_pubkey, signer_pubkey);

  /* Build NIP-46 sign_event request */
  char req_id[32];
  snprintf(req_id, sizeof(req_id), "sign_%ld", (long)time(NULL));

  const char *params[1] = { ctx->event_json };
  char *request_json = nostr_nip46_request_build(req_id, "sign_event", params, 1);
  if (!request_json) {
    g_warning("[SIGNER_SERVICE] Failed to build NIP-46 request");
    goto cleanup;
  }

  g_debug("[SIGNER_SERVICE] NIP-46 request: %.100s...", request_json);

  /* Encrypt request using NIP-04 */
  nostr_secure_buf sb = secure_alloc(32);
  if (!sb.ptr || hex_to_bytes(client_secret, (uint8_t*)sb.ptr, 32) != 0) {
    g_warning("[SIGNER_SERVICE] Failed to parse client secret");
    secure_free(&sb);
    free(request_json);
    goto cleanup;
  }

  char *encrypted_request = NULL;
  char *encrypt_err = NULL;
  if (nostr_nip04_encrypt_secure(request_json, signer_pubkey, &sb, &encrypted_request, &encrypt_err) != 0) {
    g_warning("[SIGNER_SERVICE] NIP-04 encryption failed: %s", encrypt_err ? encrypt_err : "unknown");
    secure_free(&sb);
    free(request_json);
    free(encrypt_err);
    goto cleanup;
  }
  free(request_json);

  /* Create kind 24133 wrapper event */
  NostrEvent *wrapper = nostr_event_new();
  nostr_event_set_kind(wrapper, NOSTR_EVENT_KIND_NIP46);
  nostr_event_set_pubkey(wrapper, client_pubkey);
  nostr_event_set_content(wrapper, encrypted_request);
  nostr_event_set_created_at(wrapper, (int64_t)time(NULL));

  /* Add p-tag for signer */
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("p", signer_pubkey, NULL));
  nostr_event_set_tags(wrapper, tags);

  /* Sign wrapper event with client key */
  if (nostr_event_sign_secure(wrapper, &sb) != 0) {
    g_warning("[SIGNER_SERVICE] Failed to sign wrapper event");
    secure_free(&sb);
    nostr_event_free(wrapper);
    free(encrypted_request);
    goto cleanup;
  }

  /* nostrc-e5k6: Use direct subscription with go_select instead of pool+timeout */

  /* Set up roundtrip context for relay state callback */
  Nip46SignRoundtripCtx roundtrip = {
    .expected_client_pk = client_pubkey,
    .relay_disconnected = FALSE
  };

  /* Subscribe for response events (kind 24133, p-tag = client_pubkey) */
  NostrFilter *filter = nostr_filter_new();
  int kinds[] = { NOSTR_EVENT_KIND_NIP46 };
  nostr_filter_set_kinds(filter, kinds, 1);
  nostr_filter_tags_append(filter, "p", client_pubkey, NULL);
  /* Set since to a few seconds ago to catch any responses */
  nostr_filter_set_since_i64(filter, (int64_t)time(NULL) - 5);

  NostrFilters *filters = nostr_filters_new();
  nostr_filters_add(filters, filter);

  /* Connect to relay directly with error checking */
  g_debug("[SIGNER_SERVICE] Connecting to relay %s", relay_url);
  NostrRelay *relay = nostr_relay_new(NULL, relay_url, NULL);
  if (!relay) {
    g_warning("[SIGNER_SERVICE] Failed to create relay for %s", relay_url);
    secure_free(&sb);
    nostr_event_free(wrapper);
    free(encrypted_request);
    nostr_filters_free(filters);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to create relay connection");
    goto cleanup_final;
  }

  /* Set state callback to detect disconnects */
  nostr_relay_set_state_callback(relay, nip46_relay_state_callback, &roundtrip);

  Error *conn_err = NULL;
  if (!nostr_relay_connect(relay, &conn_err)) {
    g_warning("[SIGNER_SERVICE] Failed to connect to relay %s: %s",
              relay_url, conn_err ? conn_err->message : "unknown");
    if (conn_err) free_error(conn_err);
    nostr_relay_free(relay);
    secure_free(&sb);
    nostr_event_free(wrapper);
    free(encrypted_request);
    nostr_filters_free(filters);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to connect to NIP-46 relay");
    goto cleanup_final;
  }

  g_debug("[SIGNER_SERVICE] Relay %s connected", relay_url);

  /* Create subscription with accessible channels */
  GoContext *sub_ctx = go_context_background();
  NostrSubscription *sub = nostr_relay_prepare_subscription(relay, sub_ctx, filters);
  if (!sub) {
    g_warning("[SIGNER_SERVICE] Failed to create subscription");
    nostr_relay_free(relay);
    secure_free(&sb);
    nostr_event_free(wrapper);
    free(encrypted_request);
    nostr_filters_free(filters);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Failed to create relay subscription");
    goto cleanup_final;
  }

  /* Start the subscription */
  Error *sub_err = NULL;
  if (!nostr_subscription_fire(sub, &sub_err)) {
    g_warning("[SIGNER_SERVICE] Failed to start subscription: %s",
              sub_err ? sub_err->message : "unknown");
    if (sub_err) free_error(sub_err);
    nostr_subscription_free(sub);
    nostr_relay_free(relay);
    secure_free(&sb);
    nostr_event_free(wrapper);
    free(encrypted_request);
    nostr_filters_free(filters);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Subscription rejected by relay");
    goto cleanup_final;
  }

  g_debug("[SIGNER_SERVICE] Subscribed for NIP-46 responses");

  /* Publish the request */
  nostr_relay_publish(relay, wrapper);
  g_debug("[SIGNER_SERVICE] Published NIP-46 request to %s", relay_url);

  nostr_event_free(wrapper);
  free(encrypted_request);
  nostr_filters_free(filters);

  /* Get subscription channels for go_select */
  GoChannel *events_ch = nostr_subscription_get_events_channel(sub);
  GoChannel *eose_ch = nostr_subscription_get_eose_channel(sub);
  GoChannel *closed_ch = nostr_subscription_get_closed_channel(sub);

  if (!events_ch) {
    g_warning("[SIGNER_SERVICE] No events channel on subscription");
    nostr_subscription_free(sub);
    nostr_relay_free(relay);
    secure_free(&sb);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Internal error: subscription has no events channel");
    goto cleanup_final;
  }

  /* Wait for response using go_select on channels */
  char *signed_event_json = NULL;
  char *error_message = NULL;
  gboolean eose_received = FALSE;
  gboolean keep_waiting = TRUE;

  void *received_event = NULL;
  void *eose_signal = NULL;
  void *closed_reason = NULL;

  /* Prepare select cases */
  GoSelectCase cases[3] = {
    { .op = GO_SELECT_RECEIVE, .chan = events_ch, .recv_buf = &received_event },
    { .op = GO_SELECT_RECEIVE, .chan = eose_ch, .recv_buf = &eose_signal },
    { .op = GO_SELECT_RECEIVE, .chan = closed_ch, .recv_buf = &closed_reason }
  };
  int num_cases = closed_ch ? 3 : (eose_ch ? 2 : 1);

  g_debug("[SIGNER_SERVICE] Waiting for NIP-46 response via go_select");

  while (keep_waiting) {
    /* Check cancellable */
    if (g_cancellable_is_cancelled(cancellable)) {
      g_info("[SIGNER_SERVICE] Signing cancelled by user");
      error_message = g_strdup("Signing cancelled");
      break;
    }

    /* Check relay connection state before blocking */
    if (roundtrip.relay_disconnected || !nostr_relay_is_connected(relay)) {
      g_warning("[SIGNER_SERVICE] Relay disconnected");
      error_message = g_strdup("NIP-46 relay disconnected");
      break;
    }

    /* Use go_select_timeout with short interval to check cancellable/relay state */
    GoSelectResult result = go_select_timeout(cases, num_cases, 500); /* 500ms poll */

    if (result.selected_case == 0) {
      /* Got an event */
      NostrEvent *ev = (NostrEvent *)received_event;
      if (ev && nip46_event_is_for_client(ev, client_pubkey)) {
        const char *content = nostr_event_get_content(ev);
        const char *sender = nostr_event_get_pubkey(ev);

        if (content && sender) {
          g_debug("[SIGNER_SERVICE] Got NIP-46 response from %.16s...", sender);

          /* Decrypt the response */
          char *plaintext = NULL;
          char *decrypt_err = NULL;
          if (nostr_nip04_decrypt_secure(content, sender, &sb, &plaintext, &decrypt_err) == 0 && plaintext) {
            g_debug("[SIGNER_SERVICE] Decrypted NIP-46 response: %.100s...", plaintext);

            /* Parse the NIP-46 response */
            NostrNip46Response resp = {0};
            if (nostr_nip46_response_parse(plaintext, &resp) == 0) {
              if (resp.error) {
                g_warning("[SIGNER_SERVICE] NIP-46 signer error: %s", resp.error);
                error_message = g_strdup_printf("Signer error: %s", resp.error);
              } else if (resp.result) {
                /* Success! */
                signed_event_json = g_strdup(resp.result);
                g_debug("[SIGNER_SERVICE] Got signed event: %.100s...", signed_event_json);
              }
              nostr_nip46_response_free(&resp);
            } else {
              g_warning("[SIGNER_SERVICE] Failed to parse NIP-46 response JSON");
              error_message = g_strdup("Invalid response from signer");
            }
            free(plaintext);
          } else {
            g_warning("[SIGNER_SERVICE] Failed to decrypt NIP-46 response: %s",
                      decrypt_err ? decrypt_err : "unknown");
            error_message = g_strdup_printf("Failed to decrypt signer response: %s",
                                             decrypt_err ? decrypt_err : "unknown");
            free(decrypt_err);
          }
          keep_waiting = FALSE;
        }
      }
      received_event = NULL; /* Reset for next iteration */

    } else if (result.selected_case == 1) {
      /* EOSE - all stored events delivered */
      g_debug("[SIGNER_SERVICE] EOSE received - historical events done");
      eose_received = TRUE;
      eose_signal = NULL;

    } else if (result.selected_case == 2) {
      /* CLOSED - subscription was closed by relay */
      const char *reason = closed_reason ? (const char *)closed_reason : "unknown";
      g_warning("[SIGNER_SERVICE] Subscription closed by relay: %s", reason);
      error_message = g_strdup_printf("Relay closed subscription: %s", reason);
      keep_waiting = FALSE;

    } else if (result.selected_case == -1) {
      /* Timeout - check state and continue */
      if (roundtrip.relay_disconnected || !nostr_relay_is_connected(relay)) {
        g_warning("[SIGNER_SERVICE] Relay disconnected during wait");
        error_message = g_strdup("NIP-46 relay connection lost");
        keep_waiting = FALSE;
      }
      if (eose_received && nostr_subscription_is_closed(sub)) {
        g_warning("[SIGNER_SERVICE] Subscription ended after EOSE with no response");
        error_message = g_strdup("Remote signer not responding - is it online?");
        keep_waiting = FALSE;
      }
    }
  }

  /* Cleanup subscription and relay */
  nostr_subscription_close(sub, NULL);
  nostr_subscription_free(sub);
  nostr_relay_set_state_callback(relay, NULL, NULL);
  nostr_relay_disconnect(relay);
  nostr_relay_free(relay);
  secure_free(&sb);

  if (signed_event_json) {
    g_debug("[SIGNER_SERVICE] NIP-46 sign succeeded");
    g_task_return_pointer(task, signed_event_json, g_free);
    g_free(error_message);
    goto cleanup_final;
  }

  if (error_message) {
    g_warning("[SIGNER_SERVICE] NIP-46 sign failed: %s", error_message);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "%s", error_message);
    g_free(error_message);
    goto cleanup_final;
  }

  /* Should not reach here, but just in case */
  g_warning("[SIGNER_SERVICE] NIP-46 sign failed - unexpected state");
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "NIP-46 signing failed unexpectedly");
  goto cleanup_final;

cleanup:
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                          "NIP-46 signing failed - check logs for details");

cleanup_final:
  free(client_secret);
  free(signer_pubkey);
  free(client_pubkey);
  g_free(relay_url);
  if (relays) {
    for (size_t i = 0; i < n_relays; i++) free(relays[i]);
    free(relays);
  }
}

static void
on_nip46_sign_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  SignContext *ctx = (SignContext *)user_data;
  (void)source;

  GError *error = NULL;
  char *signed_event_json = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    ctx->callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
  } else {
    ctx->callback(ctx->service, signed_event_json, NULL, ctx->user_data);
    g_free(signed_event_json);
  }

  sign_context_free(ctx);
}

void
gnostr_signer_service_sign_event_async(GnostrSignerService *self,
                                        const char *event_json,
                                        GCancellable *cancellable,
                                        GnostrSignerCallback callback,
                                        gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));
  g_return_if_fail(event_json != NULL);
  g_return_if_fail(callback != NULL);

  SignContext *ctx = g_new0(SignContext, 1);
  ctx->service = self;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->event_json = g_strdup(event_json);

  switch (self->method) {
    case GNOSTR_SIGNER_METHOD_NIP46:
      g_debug("[SIGNER_SERVICE] Signing via NIP-46 remote signer");
      {
        GTask *task = g_task_new(NULL, cancellable, on_nip46_sign_complete, ctx);
        g_task_set_task_data(task, ctx, NULL); /* ctx freed in callback */
        g_task_run_in_thread(task, nip46_sign_thread);
        g_object_unref(task);
      }
      break;

    case GNOSTR_SIGNER_METHOD_NIP55L:
      g_debug("[SIGNER_SERVICE] Signing via NIP-55L local signer");
      if (!self->nip55l_proxy) {
        GError *error = NULL;
        self->nip55l_proxy = gnostr_signer_proxy_get(&error);
        if (!self->nip55l_proxy) {
          GError *cb_error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                          "Failed to connect to local signer: %s",
                                          error ? error->message : "unknown");
          callback(self, NULL, cb_error, user_data);
          g_error_free(cb_error);
          if (error) g_error_free(error);
          sign_context_free(ctx);
          return;
        }
      }
      nostr_org_nostr_signer_call_sign_event(
          self->nip55l_proxy,
          event_json,
          "",  /* current_user: empty = use default */
          "",  /* app_id: empty = use default */
          cancellable,
          on_nip55l_sign_complete,
          ctx);
      break;

    case GNOSTR_SIGNER_METHOD_NONE:
    default:
      {
        GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No signing method available");
        callback(self, NULL, error, user_data);
        g_error_free(error);
        sign_context_free(ctx);
      }
      break;
  }
}

/* ---- Convenience Wrapper for Easy Migration ---- */

typedef struct {
  GAsyncReadyCallback callback;
  gpointer user_data;
  char *signed_event;
  GError *error;
} WrapperContext;

static void
on_wrapper_sign_complete(GnostrSignerService *service,
                          const char *signed_event_json,
                          GError *error,
                          gpointer user_data)
{
  WrapperContext *ctx = (WrapperContext *)user_data;
  (void)service;

  /* Create a GTask to hold the result */
  GTask *task = g_task_new(NULL, NULL, NULL, NULL);

  if (error) {
    g_task_return_error(task, g_error_copy(error));
  } else if (signed_event_json) {
    g_task_return_pointer(task, g_strdup(signed_event_json), g_free);
  } else {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Signing returned no result");
  }

  /* Call the original callback */
  ctx->callback(NULL, G_ASYNC_RESULT(task), ctx->user_data);

  g_object_unref(task);
  g_free(ctx);
}

void
gnostr_sign_event_async(const char *event_json,
                         const char *current_user,
                         const char *app_id,
                         GCancellable *cancellable,
                         GAsyncReadyCallback callback,
                         gpointer user_data)
{
  (void)current_user;
  (void)app_id;

  WrapperContext *ctx = g_new0(WrapperContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  gnostr_signer_service_sign_event_async(
      gnostr_signer_service_get_default(),
      event_json,
      cancellable,
      on_wrapper_sign_complete,
      ctx);
}

gboolean
gnostr_sign_event_finish(GAsyncResult *res,
                          char **out_signed_event,
                          GError **error)
{
  g_return_val_if_fail(G_IS_TASK(res), FALSE);

  char *result = g_task_propagate_pointer(G_TASK(res), error);
  if (result) {
    if (out_signed_event) {
      *out_signed_event = result;
    } else {
      g_free(result);
    }
    return TRUE;
  }
  return FALSE;
}

/* ---- nostrc-1wfi: NIP-46 Session Persistence ---- */

#define SETTINGS_SCHEMA_CLIENT "org.gnostr.Client"

gboolean
gnostr_signer_service_restore_from_settings(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), FALSE);

  GSettings *settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (!settings) {
    g_warning("[SIGNER_SERVICE] Failed to open GSettings");
    return FALSE;
  }

  g_autofree char *client_secret = g_settings_get_string(settings, "nip46-client-secret");
  g_autofree char *signer_pubkey = g_settings_get_string(settings, "nip46-signer-pubkey");
  g_autofree char *relay_url = g_settings_get_string(settings, "nip46-relay");

  g_object_unref(settings);

  /* Check if we have valid credentials */
  if (!client_secret || !*client_secret || !signer_pubkey || !*signer_pubkey) {
    g_debug("[SIGNER_SERVICE] No saved NIP-46 credentials found");
    return FALSE;
  }

  /* Validate secret length (must be 64 hex chars = 32 bytes) */
  if (strlen(client_secret) != 64) {
    g_warning("[SIGNER_SERVICE] Invalid saved client secret length: %zu",
              strlen(client_secret));
    return FALSE;
  }

  /* Validate pubkey length */
  if (strlen(signer_pubkey) != 64) {
    g_warning("[SIGNER_SERVICE] Invalid saved signer pubkey length: %zu",
              strlen(signer_pubkey));
    return FALSE;
  }

  g_message("[SIGNER_SERVICE] Restoring NIP-46 session from settings...");

  /* Create a new NIP-46 session */
  NostrNip46Session *session = nostr_nip46_client_new();
  if (!session) {
    g_warning("[SIGNER_SERVICE] Failed to create NIP-46 session");
    return FALSE;
  }

  /* Build a nostrconnect:// URI to populate the session.
   * We need to derive the client pubkey from the secret, but for simplicity
   * we'll just set the fields directly on the session.
   * First, use the connect function with a synthetic URI. */
  const char *default_relay = (relay_url && *relay_url) ? relay_url : "wss://relay.nsec.app";

  /* Derive client pubkey from secret for the URI */
  char *client_pubkey = nostr_key_get_public(client_secret);
  if (!client_pubkey) {
    g_warning("[SIGNER_SERVICE] Failed to derive client pubkey from secret");
    nostr_nip46_session_free(session);
    return FALSE;
  }

  /* Build synthetic nostrconnect:// URI */
  g_autofree char *connect_uri = g_strdup_printf(
    "nostrconnect://%s?relay=%s&secret=%s",
    client_pubkey, default_relay, client_secret);
  free(client_pubkey);

  /* Parse URI to populate session with relays */
  int rc = nostr_nip46_client_connect(session, connect_uri, NULL);
  if (rc != 0) {
    g_warning("[SIGNER_SERVICE] Failed to restore session from URI: %d", rc);
    nostr_nip46_session_free(session);
    return FALSE;
  }

  /* nostrc-1wfi: Set the REAL client secret key for ECDH encryption.
   * The URI's secret= param is just an auth token, NOT the crypto key. */
  if (nostr_nip46_client_set_secret(session, client_secret) != 0) {
    g_warning("[SIGNER_SERVICE] Failed to set client secret for ECDH");
    nostr_nip46_session_free(session);
    return FALSE;
  }

  /* Set the signer pubkey */
  if (nostr_nip46_client_set_signer_pubkey(session, signer_pubkey) != 0) {
    g_warning("[SIGNER_SERVICE] Failed to set signer pubkey");
    nostr_nip46_session_free(session);
    return FALSE;
  }

  /* Install the session */
  gnostr_signer_service_set_nip46_session(self, session);

  g_message("[SIGNER_SERVICE] NIP-46 session restored successfully (signer: %.16s...)",
            signer_pubkey);

  return TRUE;
}

void
gnostr_signer_service_clear_saved_credentials(GnostrSignerService *self)
{
  (void)self;  /* Unused but kept for API consistency */

  GSettings *settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (!settings) return;

  g_settings_set_string(settings, "nip46-client-secret", "");
  g_settings_set_string(settings, "nip46-signer-pubkey", "");
  g_settings_set_string(settings, "nip46-relay", "");

  g_object_unref(settings);

  g_debug("[SIGNER_SERVICE] Cleared saved NIP-46 credentials");
}

/* ---- nostrc-n44s: NIP-44 Encryption/Decryption ---- */

typedef struct {
  GnostrSignerService *service;
  GnostrNip44Callback callback;
  gpointer user_data;
  char *peer_pubkey;
  char *data;  /* plaintext for encrypt, ciphertext for decrypt */
  gboolean is_encrypt;  /* TRUE for encrypt, FALSE for decrypt */
} Nip44Context;

static void
nip44_context_free(Nip44Context *ctx)
{
  if (!ctx) return;
  g_free(ctx->peer_pubkey);
  g_free(ctx->data);
  g_free(ctx);
}

/* NIP-55L encrypt callback */
static void
on_nip55l_nip44_encrypt_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip44Context *ctx = (Nip44Context *)user_data;
  NostrSignerProxy *proxy = (NostrSignerProxy *)source;

  GError *error = NULL;
  char *ciphertext = NULL;

  gboolean ok = nostr_org_nostr_signer_call_nip44_encrypt_finish(
      proxy, &ciphertext, res, &error);

  if (!ok) {
    if (!error) {
      error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "NIP-44 encryption failed");
    }
    ctx->callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
  } else {
    ctx->callback(ctx->service, ciphertext, NULL, ctx->user_data);
    g_free(ciphertext);
  }

  nip44_context_free(ctx);
}

/* NIP-55L decrypt callback */
static void
on_nip55l_nip44_decrypt_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip44Context *ctx = (Nip44Context *)user_data;
  NostrSignerProxy *proxy = (NostrSignerProxy *)source;

  GError *error = NULL;
  char *plaintext = NULL;

  gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(
      proxy, &plaintext, res, &error);

  if (!ok) {
    if (!error) {
      error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "NIP-44 decryption failed");
    }
    ctx->callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
  } else {
    ctx->callback(ctx->service, plaintext, NULL, ctx->user_data);
    g_free(plaintext);
  }

  nip44_context_free(ctx);
}

/* NIP-46 encrypt/decrypt thread */
static void
nip46_nip44_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  Nip44Context *ctx = (Nip44Context *)task_data;
  (void)source;
  (void)cancellable;

  if (!ctx->service->nip46_session) {
    g_warning("[SIGNER_SERVICE] NIP-44 %s failed: session is NULL",
              ctx->is_encrypt ? "encrypt" : "decrypt");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session not available - please sign in again");
    return;
  }

  char *result = NULL;
  int rc;

  if (ctx->is_encrypt) {
    g_debug("[SIGNER_SERVICE] NIP-46 NIP-44 encrypting for %.16s...", ctx->peer_pubkey);
    rc = nostr_nip46_client_nip44_encrypt(ctx->service->nip46_session,
                                           ctx->peer_pubkey,
                                           ctx->data,
                                           &result);
  } else {
    g_debug("[SIGNER_SERVICE] NIP-46 NIP-44 decrypting from %.16s...", ctx->peer_pubkey);
    rc = nostr_nip46_client_nip44_decrypt(ctx->service->nip46_session,
                                           ctx->peer_pubkey,
                                           ctx->data,
                                           &result);
  }

  if (rc != 0 || !result) {
    g_warning("[SIGNER_SERVICE] NIP-46 NIP-44 %s failed with rc=%d",
              ctx->is_encrypt ? "encrypt" : "decrypt", rc);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-44 %s failed (error %d)",
                            ctx->is_encrypt ? "encryption" : "decryption", rc);
    return;
  }

  g_debug("[SIGNER_SERVICE] NIP-46 NIP-44 %s succeeded",
          ctx->is_encrypt ? "encrypt" : "decrypt");
  g_task_return_pointer(task, result, g_free);
}

static void
on_nip46_nip44_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip44Context *ctx = (Nip44Context *)user_data;
  (void)source;

  GError *error = NULL;
  char *result = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    ctx->callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
  } else {
    ctx->callback(ctx->service, result, NULL, ctx->user_data);
    g_free(result);
  }

  nip44_context_free(ctx);
}

void
gnostr_signer_service_nip44_encrypt_async(GnostrSignerService *self,
                                           const char *peer_pubkey,
                                           const char *plaintext,
                                           GCancellable *cancellable,
                                           GnostrNip44Callback callback,
                                           gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));
  g_return_if_fail(peer_pubkey != NULL);
  g_return_if_fail(plaintext != NULL);
  g_return_if_fail(callback != NULL);

  Nip44Context *ctx = g_new0(Nip44Context, 1);
  ctx->service = self;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->peer_pubkey = g_strdup(peer_pubkey);
  ctx->data = g_strdup(plaintext);
  ctx->is_encrypt = TRUE;

  switch (self->method) {
    case GNOSTR_SIGNER_METHOD_NIP46:
      g_debug("[SIGNER_SERVICE] NIP-44 encrypt via NIP-46 remote signer");
      {
        GTask *task = g_task_new(NULL, cancellable, on_nip46_nip44_complete, ctx);
        g_task_set_task_data(task, ctx, NULL);
        g_task_run_in_thread(task, nip46_nip44_thread);
        g_object_unref(task);
      }
      break;

    case GNOSTR_SIGNER_METHOD_NIP55L:
      g_debug("[SIGNER_SERVICE] NIP-44 encrypt via NIP-55L local signer");
      if (!self->nip55l_proxy) {
        GError *error = NULL;
        self->nip55l_proxy = gnostr_signer_proxy_get(&error);
        if (!self->nip55l_proxy) {
          GError *cb_error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                          "Failed to connect to local signer: %s",
                                          error ? error->message : "unknown");
          callback(self, NULL, cb_error, user_data);
          g_error_free(cb_error);
          if (error) g_error_free(error);
          nip44_context_free(ctx);
          return;
        }
      }
      nostr_org_nostr_signer_call_nip44_encrypt(
          self->nip55l_proxy,
          plaintext,
          peer_pubkey,
          "",  /* current_user: empty = use default */
          cancellable,
          on_nip55l_nip44_encrypt_complete,
          ctx);
      break;

    case GNOSTR_SIGNER_METHOD_NONE:
    default:
      {
        GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No signing method available");
        callback(self, NULL, error, user_data);
        g_error_free(error);
        nip44_context_free(ctx);
      }
      break;
  }
}

void
gnostr_signer_service_nip44_decrypt_async(GnostrSignerService *self,
                                           const char *peer_pubkey,
                                           const char *ciphertext,
                                           GCancellable *cancellable,
                                           GnostrNip44Callback callback,
                                           gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));
  g_return_if_fail(peer_pubkey != NULL);
  g_return_if_fail(ciphertext != NULL);
  g_return_if_fail(callback != NULL);

  Nip44Context *ctx = g_new0(Nip44Context, 1);
  ctx->service = self;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->peer_pubkey = g_strdup(peer_pubkey);
  ctx->data = g_strdup(ciphertext);
  ctx->is_encrypt = FALSE;

  switch (self->method) {
    case GNOSTR_SIGNER_METHOD_NIP46:
      g_debug("[SIGNER_SERVICE] NIP-44 decrypt via NIP-46 remote signer");
      {
        GTask *task = g_task_new(NULL, cancellable, on_nip46_nip44_complete, ctx);
        g_task_set_task_data(task, ctx, NULL);
        g_task_run_in_thread(task, nip46_nip44_thread);
        g_object_unref(task);
      }
      break;

    case GNOSTR_SIGNER_METHOD_NIP55L:
      g_debug("[SIGNER_SERVICE] NIP-44 decrypt via NIP-55L local signer");
      if (!self->nip55l_proxy) {
        GError *error = NULL;
        self->nip55l_proxy = gnostr_signer_proxy_get(&error);
        if (!self->nip55l_proxy) {
          GError *cb_error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                          "Failed to connect to local signer: %s",
                                          error ? error->message : "unknown");
          callback(self, NULL, cb_error, user_data);
          g_error_free(cb_error);
          if (error) g_error_free(error);
          nip44_context_free(ctx);
          return;
        }
      }
      nostr_org_nostr_signer_call_nip44_decrypt(
          self->nip55l_proxy,
          ciphertext,
          peer_pubkey,
          "",  /* current_user: empty = use default */
          cancellable,
          on_nip55l_nip44_decrypt_complete,
          ctx);
      break;

    case GNOSTR_SIGNER_METHOD_NONE:
    default:
      {
        GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                     "No signing method available");
        callback(self, NULL, error, user_data);
        g_error_free(error);
        nip44_context_free(ctx);
      }
      break;
  }
}

/* ---- NIP-44 Convenience Wrappers ---- */

typedef struct {
  GAsyncReadyCallback callback;
  gpointer user_data;
} Nip44WrapperContext;

static void
on_nip44_wrapper_complete(GnostrSignerService *service,
                           const char *result,
                           GError *error,
                           gpointer user_data)
{
  Nip44WrapperContext *ctx = (Nip44WrapperContext *)user_data;
  (void)service;

  GTask *task = g_task_new(NULL, NULL, NULL, NULL);

  if (error) {
    g_task_return_error(task, g_error_copy(error));
  } else if (result) {
    g_task_return_pointer(task, g_strdup(result), g_free);
  } else {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "NIP-44 operation returned no result");
  }

  ctx->callback(NULL, G_ASYNC_RESULT(task), ctx->user_data);

  g_object_unref(task);
  g_free(ctx);
}

void
gnostr_nip44_encrypt_async(const char *peer_pubkey,
                            const char *plaintext,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  Nip44WrapperContext *ctx = g_new0(Nip44WrapperContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  gnostr_signer_service_nip44_encrypt_async(
      gnostr_signer_service_get_default(),
      peer_pubkey,
      plaintext,
      cancellable,
      on_nip44_wrapper_complete,
      ctx);
}

gboolean
gnostr_nip44_encrypt_finish(GAsyncResult *res,
                             char **out_ciphertext,
                             GError **error)
{
  g_return_val_if_fail(G_IS_TASK(res), FALSE);

  char *result = g_task_propagate_pointer(G_TASK(res), error);
  if (result) {
    if (out_ciphertext) {
      *out_ciphertext = result;
    } else {
      g_free(result);
    }
    return TRUE;
  }
  return FALSE;
}

void
gnostr_nip44_decrypt_async(const char *peer_pubkey,
                            const char *ciphertext,
                            GCancellable *cancellable,
                            GAsyncReadyCallback callback,
                            gpointer user_data)
{
  Nip44WrapperContext *ctx = g_new0(Nip44WrapperContext, 1);
  ctx->callback = callback;
  ctx->user_data = user_data;

  gnostr_signer_service_nip44_decrypt_async(
      gnostr_signer_service_get_default(),
      peer_pubkey,
      ciphertext,
      cancellable,
      on_nip44_wrapper_complete,
      ctx);
}

gboolean
gnostr_nip44_decrypt_finish(GAsyncResult *res,
                             char **out_plaintext,
                             GError **error)
{
  g_return_val_if_fail(G_IS_TASK(res), FALSE);

  char *result = g_task_propagate_pointer(G_TASK(res), error);
  if (result) {
    if (out_plaintext) {
      *out_plaintext = result;
    } else {
      g_free(result);
    }
    return TRUE;
  }
  return FALSE;
}
