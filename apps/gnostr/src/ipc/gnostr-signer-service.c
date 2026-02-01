/**
 * GnostrSignerService - Unified Signing Service Implementation
 */

#include "gnostr-signer-service.h"
#include "signer_ipc.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr-keys.h"
#include <string.h>

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

/* NIP-46 signing (runs in thread) */
static void
nip46_sign_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  SignContext *ctx = (SignContext *)task_data;
  (void)source;
  (void)cancellable;

  if (!ctx->service->nip46_session) {
    /* nostrc-rrfr: Log when session is missing */
    g_warning("[SIGNER_SERVICE] NIP-46 sign failed: session is NULL - "
              "user may not be logged in or session was not persisted after login");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session not available - please sign in again");
    return;
  }

  g_debug("[SIGNER_SERVICE] NIP-46 signing event: %.80s...", ctx->event_json);

  char *signed_event_json = NULL;
  int rc = nostr_nip46_client_sign_event(ctx->service->nip46_session,
                                          ctx->event_json,
                                          &signed_event_json);

  if (rc != 0 || !signed_event_json) {
    /* nostrc-rrfr: Log with more context about the error */
    g_warning("[SIGNER_SERVICE] NIP-46 sign failed with rc=%d - "
              "check stderr for [nip46] sign_event details", rc);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 signing failed (error %d) - check logs for details", rc);
    return;
  }

  g_debug("[SIGNER_SERVICE] NIP-46 sign succeeded, got encrypted response");

  g_task_return_pointer(task, signed_event_json, g_free);
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

  /* Parse URI to populate session with secret and relays */
  int rc = nostr_nip46_client_connect(session, connect_uri, NULL);
  if (rc != 0) {
    g_warning("[SIGNER_SERVICE] Failed to restore session from URI: %d", rc);
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
