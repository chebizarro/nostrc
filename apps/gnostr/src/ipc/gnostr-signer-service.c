/**
 * GnostrSignerService - Unified Signing Service Implementation
 */

#include "gnostr-signer-service.h"
#include "signer_ipc.h"
#include "nostr/nip46/nip46_client.h"
#include "nostr/nip46/nip46_msg.h"
#include "nostr/nip46/nip46_types.h"
#include "nostr/nip04.h"
#include "nostr/nip44/nip44.h"
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

  /* State machine */
  GnostrSignerState state;

  /* User's public key (hex) */
  char *pubkey_hex;

  /* NIP-46 session (owned - sole owner) */
  NostrNip46Session *nip46_session;

  /* NIP-55L proxy (lazy initialized) */
  NostrSignerProxy *nip55l_proxy;

  /* Mutex for thread-safe session access */
  GMutex session_mutex;

  /* Cancellable for pending operations */
  GCancellable *pending_cancellable;
};

/* Signal IDs */
enum {
  SIGNAL_STATE_CHANGED,
  N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_TYPE(GnostrSignerService, gnostr_signer_service, G_TYPE_OBJECT)

/* Global default instance */
static GnostrSignerService *s_default_service = NULL;

static void
gnostr_signer_service_dispose(GObject *object)
{
  GnostrSignerService *self = GNOSTR_SIGNER_SERVICE(object);

  /* Cancel any pending operations */
  if (self->pending_cancellable) {
    g_cancellable_cancel(self->pending_cancellable);
    g_clear_object(&self->pending_cancellable);
  }

  /* nostrc-13gf: Detach under the lock, free OUTSIDE it.
   * nostr_nip46_session_free() blocks (drains in-flight RPCs) and may fire
   * flushed-job callbacks; holding session_mutex across that can deadlock
   * with callbacks that re-enter the signer service. */
  g_mutex_lock(&self->session_mutex);
  NostrNip46Session *old_session = self->nip46_session;
  self->nip46_session = NULL;
  g_mutex_unlock(&self->session_mutex);
  if (old_session) {
    nostr_nip46_session_free(old_session);
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
  g_mutex_clear(&self->session_mutex);

  G_OBJECT_CLASS(gnostr_signer_service_parent_class)->finalize(object);
}

static void
gnostr_signer_service_class_init(GnostrSignerServiceClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);

  object_class->dispose = gnostr_signer_service_dispose;
  object_class->finalize = gnostr_signer_service_finalize;

  /**
   * GnostrSignerService::state-changed:
   * @self: The signer service
   * @old_state: The previous state
   * @new_state: The new state
   *
   * Emitted when the signer state changes.
   */
  signals[SIGNAL_STATE_CHANGED] = g_signal_new(
      "state-changed",
      G_TYPE_FROM_CLASS(klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL, NULL,
      NULL,
      G_TYPE_NONE,
      2,
      G_TYPE_UINT, /* old_state */
      G_TYPE_UINT  /* new_state */
  );
}

static void
gnostr_signer_service_init(GnostrSignerService *self)
{
  self->method = GNOSTR_SIGNER_METHOD_NONE;
  self->state = GNOSTR_SIGNER_STATE_DISCONNECTED;
  self->pubkey_hex = NULL;
  self->nip46_session = NULL;
  self->nip55l_proxy = NULL;
  self->pending_cancellable = NULL;
  g_mutex_init(&self->session_mutex);
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

/* Helper to transition state and emit signal */
static void
set_state(GnostrSignerService *self, GnostrSignerState new_state)
{
  GnostrSignerState old_state = self->state;
  if (old_state == new_state) return;

  self->state = new_state;
  g_debug("[SIGNER_SERVICE] State: %d -> %d", old_state, new_state);

  g_signal_emit(self, signals[SIGNAL_STATE_CHANGED], 0, old_state, new_state);
}

GnostrSignerState
gnostr_signer_service_get_state(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), GNOSTR_SIGNER_STATE_DISCONNECTED);
  return self->state;
}

gboolean
gnostr_signer_service_is_ready(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), FALSE);
  return self->state == GNOSTR_SIGNER_STATE_CONNECTED;
}

void
gnostr_signer_service_set_nip46_session(GnostrSignerService *self,
                                         NostrNip46Session *session)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));

  /* nostrc-13gf: Detach the old session under the lock and free it OUTSIDE.
   * nostr_nip46_session_free() blocks (drains in-flight RPCs) and may fire
   * flushed-job callbacks that re-enter the signer service. */
  g_mutex_lock(&self->session_mutex);
  NostrNip46Session *old_session = self->nip46_session;
  self->nip46_session = session;
  if (session) {
    self->method = GNOSTR_SIGNER_METHOD_NIP46;
  }
  g_mutex_unlock(&self->session_mutex);

  if (old_session) {
    g_message("[SIGNER_SERVICE] Replacing NIP-46 session %p with %p",
              (void *)old_session, (void *)session);
    nostr_nip46_session_free(old_session);
  }

  if (session) {
    set_state(self, GNOSTR_SIGNER_STATE_CONNECTED);
    g_debug("[SIGNER_SERVICE] Switched to NIP-46 remote signer");
  } else {

    /* nostrc-dbus1: Reset cached D-Bus failure before retrying.
     * The signer service may have started since our last attempt. */
    gnostr_signer_proxy_reset();
    GError *error = NULL;
    self->nip55l_proxy = gnostr_signer_proxy_get(&error);
    if (self->nip55l_proxy) {
      self->method = GNOSTR_SIGNER_METHOD_NIP55L;
      set_state(self, GNOSTR_SIGNER_STATE_CONNECTED);
      g_debug("[SIGNER_SERVICE] Using NIP-55L local signer");
    } else {
      self->method = GNOSTR_SIGNER_METHOD_NONE;
      set_state(self, GNOSTR_SIGNER_STATE_DISCONNECTED);
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

  /* nostrc-svsj: guarded so worker-context readers can snapshot safely */
  g_mutex_lock(&self->session_mutex);
  g_free(self->pubkey_hex);
  self->pubkey_hex = g_strdup(pubkey_hex);
  g_mutex_unlock(&self->session_mutex);
}

void
gnostr_signer_service_clear(GnostrSignerService *self)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));

  /* Cancel pending operations first */
  if (self->pending_cancellable) {
    g_cancellable_cancel(self->pending_cancellable);
    g_clear_object(&self->pending_cancellable);
  }

  /* nostrc-13gf: Detach the session under the lock, free it OUTSIDE.
   * nostr_nip46_session_free() blocks (drains in-flight RPCs) and may fire
   * flushed-job callbacks that re-enter the signer service. */
  g_mutex_lock(&self->session_mutex);
  NostrNip46Session *old_session = self->nip46_session;
  self->nip46_session = NULL;
  /* nostrc-svsj: free the pubkey under the same lock used by readers */
  g_free(self->pubkey_hex);
  self->pubkey_hex = NULL;
  g_mutex_unlock(&self->session_mutex);

  if (old_session) {
    g_message("[SIGNER_SERVICE] Clearing NIP-46 session %p", (void *)old_session);
    nostr_nip46_session_free(old_session);
  }

  self->nip55l_proxy = NULL;
  self->method = GNOSTR_SIGNER_METHOD_NONE;

  set_state(self, GNOSTR_SIGNER_STATE_DISCONNECTED);

  /* nostrc-1wfi: Also clear persisted credentials on logout */
  gnostr_signer_service_clear_saved_credentials(self);

  g_debug("[SIGNER_SERVICE] Cleared all authentication state");
}

void
gnostr_signer_service_logout(GnostrSignerService *self)
{
  gnostr_signer_service_clear(self);
}

/* ---- Async Signing Implementation ---- */

typedef struct {
  GnostrSignerService *service;
  GnostrSignerCallback callback;
  gpointer user_data;
  char *event_json;
  /* nostrc-svsj: identity/kind the signed result is expected to carry
   * (expected_pubkey may be NULL when the template had no pubkey). */
  char *expected_pubkey;
  int expected_kind;
} SignContext;

static void
sign_context_free(SignContext *ctx)
{
  if (!ctx) return;
  g_clear_pointer(&ctx->event_json, g_free);
  g_clear_pointer(&ctx->expected_pubkey, g_free);
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

/* nostrc-svsj: Normalize an unsigned event template before sending it to a
 * NIP-46 remote signer.
 *
 * Several publish paths (kind 1 composer, kind 7 reactions, reposts,
 * deletions, labels, NIP-34 bug reports, ...) build unsigned JSON with
 * GNostrJsonBuilder containing only kind/created_at/content/tags.  The
 * working NIP-59 seal/gift-wrap path, by contrast, serializes a NostrEvent
 * with the author pubkey set.  Many remote signers reject or mishandle
 * sign_event templates that omit "pubkey", so inject the logged-in user's
 * pubkey centrally here instead of patching every builder.
 *
 * Returns a newly-allocated normalized JSON string, or NULL with @error set.
 * On success, *out_expected_pubkey receives the pubkey (if any) the signed
 * result is expected to carry (caller frees). */
static gboolean
is_valid_pubkey_hex64(const char *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (size_t i = 0; i < 64; i++) {
    if (!g_ascii_isxdigit(s[i])) return FALSE;
  }
  return TRUE;
}

static char *
nip46_normalize_sign_request(GnostrSignerService *self,
                             const char *event_json,
                             char **out_expected_pubkey,
                             int *out_expected_kind,
                             GError **error)
{
  if (out_expected_pubkey) *out_expected_pubkey = NULL;
  if (out_expected_kind) *out_expected_kind = -1;

  NostrEvent *ev = nostr_event_new();
  if (!ev) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Out of memory normalizing sign request");
    return NULL;
  }

  if (nostr_event_deserialize_compact(ev, event_json, NULL) != 1) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                "Invalid unsigned event JSON for signing");
    nostr_event_free(ev);
    return NULL;
  }

  if (out_expected_kind) *out_expected_kind = nostr_event_get_kind(ev);

  /* Snapshot the user pubkey under the mutex: set_pubkey/clear may run on a
   * different context than the one invoking sign_event_async. */
  g_mutex_lock(&self->session_mutex);
  g_autofree char *user_pubkey = g_strdup(self->pubkey_hex);
  g_mutex_unlock(&self->session_mutex);

  const char *tpl_pubkey = nostr_event_get_pubkey(ev);
  gboolean have_user_pubkey = is_valid_pubkey_hex64(user_pubkey);

  if (tpl_pubkey && *tpl_pubkey) {
    /* Template already carries an author pubkey (e.g. NIP-59 seal path). */
    if (!is_valid_pubkey_hex64(tpl_pubkey)) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Unsigned event carries a malformed pubkey");
      nostr_event_free(ev);
      return NULL;
    }
    if (have_user_pubkey && g_ascii_strcasecmp(tpl_pubkey, user_pubkey) != 0) {
      g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                  "Unsigned event pubkey %.8s... does not match logged-in user %.8s...",
                  tpl_pubkey, user_pubkey);
      nostr_event_free(ev);
      return NULL;
    }
    if (out_expected_pubkey) *out_expected_pubkey = g_strdup(tpl_pubkey);
    nostr_event_free(ev);
    return g_strdup(event_json);
  }

  if (!have_user_pubkey) {
    /* nostrc-svsj: Refuse to send a pubkey-less template to a remote signer -
     * several implementations reject or mishandle it, which was the original
     * silent-failure mode. The identity is known once login/get_public_key
     * completes, so ask the caller to retry instead. */
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                "Signer identity not yet known - please retry in a moment");
    nostr_event_free(ev);
    return NULL;
  }

  nostr_event_set_pubkey(ev, user_pubkey);

  char *normalized = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  if (!normalized) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                "Failed to serialize normalized sign request");
    return NULL;
  }

  g_debug("[SIGNER_SERVICE] Normalized NIP-46 sign request: injected pubkey %.8s...",
          user_pubkey);
  if (out_expected_pubkey) *out_expected_pubkey = g_strdup(user_pubkey);
  return normalized;
}

/* NIP-46 signing (runs in thread) - use library's RPC function for consistency with login */
static void
nip46_sign_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  SignContext *ctx = (SignContext *)task_data;
  (void)source;
  (void)cancellable;

  /* nostrc-svsj: Normalize in the worker so failures flow through the normal
   * async GTask error path. Completing synchronously from sign_event_async
   * re-entered GTK signal emission (composer "post-requested") and is unsafe. */
  {
    GError *norm_error = NULL;
    char *normalized = nip46_normalize_sign_request(ctx->service, ctx->event_json,
                                                    &ctx->expected_pubkey,
                                                    &ctx->expected_kind,
                                                    &norm_error);
    if (!normalized) {
      g_warning("[SIGNER_SERVICE] NIP-46 sign request normalization failed: %s",
                norm_error ? norm_error->message : "unknown");
      g_task_return_error(task, norm_error); /* transfers ownership */
      return;
    }
    g_free(ctx->event_json);
    ctx->event_json = normalized;
  }

  /* Lock mutex for session access */
  g_mutex_lock(&ctx->service->session_mutex);

  if (!ctx->service->nip46_session) {
    g_mutex_unlock(&ctx->service->session_mutex);
    g_warning("[SIGNER_SERVICE] NIP-46 sign failed: session is NULL");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session not available - please sign in again");
    return;
  }

  /* nostrc-13gf: Take a real reference while holding the lock so a
   * concurrent logout/replace can never free the session under us. */
  NostrNip46Session *session = nostr_nip46_session_ref(ctx->service->nip46_session);

  g_mutex_unlock(&ctx->service->session_mutex);

  g_debug("[SIGNER_SERVICE] NIP-46 sign RPC begin: session=%p", (void *)session);

  /* Use the nip46 library's sign_event function - same code path as get_public_key */
  char *signed_event_json = NULL;
  int rc = nostr_nip46_client_sign_event(session,
                                          ctx->event_json,
                                          &signed_event_json);

  /* Session no longer needed - validation below only touches local data */
  nostr_nip46_session_unref(session);

  if (rc != 0 || !signed_event_json) {
    g_warning("[SIGNER_SERVICE] NIP-46 sign_event RPC failed: rc=%d", rc);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Signer did not respond to sign request");
    return;
  }

  /* nostrc-svsj: Validate the signed result before handing it to publishers.
   * A remote signer returning an unsigned or wrong-identity event would
   * otherwise surface much later as a silent relay rejection. */
  {
    NostrEvent *signed_ev = nostr_event_new();
    gboolean parsed = signed_ev &&
        nostr_event_deserialize_compact(signed_ev, signed_event_json, NULL) == 1;
    const char *res_pubkey = parsed ? nostr_event_get_pubkey(signed_ev) : NULL;
    const char *res_sig = parsed ? nostr_event_get_sig(signed_ev) : NULL;

    if (!parsed || !res_pubkey || !*res_pubkey || !res_sig || !*res_sig) {
      g_warning("[SIGNER_SERVICE] NIP-46 signer returned invalid signed event (parsed=%d)",
                parsed);
      if (signed_ev) nostr_event_free(signed_ev);
      free(signed_event_json);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Signer returned an invalid signed event");
      return;
    }

    if (ctx->expected_pubkey &&
        g_ascii_strcasecmp(res_pubkey, ctx->expected_pubkey) != 0) {
      g_warning("[SIGNER_SERVICE] NIP-46 signed event pubkey %.8s... does not match expected %.8s...",
                res_pubkey, ctx->expected_pubkey);
      nostr_event_free(signed_ev);
      free(signed_event_json);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Signer returned an event for a different identity");
      return;
    }

    if (ctx->expected_kind >= 0 &&
        nostr_event_get_kind(signed_ev) != ctx->expected_kind) {
      g_warning("[SIGNER_SERVICE] NIP-46 signed event kind %d does not match requested kind %d",
                nostr_event_get_kind(signed_ev), ctx->expected_kind);
      nostr_event_free(signed_ev);
      free(signed_event_json);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Signer returned an event of the wrong kind");
      return;
    }

    if (!nostr_event_check_signature(signed_ev)) {
      g_warning("[SIGNER_SERVICE] NIP-46 signed event failed signature verification");
      nostr_event_free(signed_ev);
      free(signed_event_json);
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Signer returned an event with an invalid signature");
      return;
    }

    nostr_event_free(signed_ev);
  }

  g_debug("[SIGNER_SERVICE] NIP-46 sign succeeded");
  g_task_return_pointer(task, g_strdup(signed_event_json), g_free);
  free(signed_event_json);
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

  /* Check availability BEFORE dispatching to thread - this is the fix for
   * the race condition where session could become NULL after dispatch */
  if (self->state != GNOSTR_SIGNER_STATE_CONNECTED) {
    GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                 "Signer not connected - please sign in first");
    callback(self, NULL, error, user_data);
    g_error_free(error);
    return;
  }

  SignContext *ctx = g_new0(SignContext, 1);
  ctx->service = self;
  ctx->callback = callback;
  ctx->user_data = user_data;
  ctx->event_json = g_strdup(event_json);

  switch (self->method) {
    case GNOSTR_SIGNER_METHOD_NIP46:
      g_debug("[SIGNER_SERVICE] Signing via NIP-46 remote signer");
      {
        /* nostrc-svsj: template normalization (author pubkey injection) and
         * all of its error reporting happen inside nip46_sign_thread so this
         * function never completes synchronously on the connected path. */
        ctx->expected_kind = -1;
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

#include "nip46-relay-defaults.h"

#define SETTINGS_SCHEMA_CLIENT "org.gnostr.Client"

gboolean
gnostr_signer_service_restore_from_settings(GnostrSignerService *self)
{
  g_return_val_if_fail(GNOSTR_IS_SIGNER_SERVICE(self), FALSE);

  g_autoptr(GSettings) settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (!settings) {
    g_warning("[SIGNER_SERVICE] Failed to open GSettings");
    return FALSE;
  }

  g_autofree char *client_secret = g_settings_get_string(settings, "nip46-client-secret");
  g_autofree char *signer_pubkey = g_settings_get_string(settings, "nip46-signer-pubkey");

  /* Read relay array from settings */
  g_autoptr(GVariant) relays_variant = g_settings_get_value(settings, "nip46-relays");
  gsize n_relays = 0;
  const gchar **relay_urls = NULL;

  if (relays_variant && g_variant_is_of_type(relays_variant, G_VARIANT_TYPE_STRING_ARRAY)) {
    relay_urls = g_variant_get_strv(relays_variant, &n_relays);
  }


  /* Check if we have valid credentials */
  if (!client_secret || !*client_secret || !signer_pubkey || !*signer_pubkey) {
    g_debug("[SIGNER_SERVICE] No saved NIP-46 credentials found");
    g_free(relay_urls);
    return FALSE;
  }

  /* Validate secret length (must be 64 hex chars = 32 bytes) */
  if (strlen(client_secret) != 64) {
    g_warning("[SIGNER_SERVICE] Invalid saved client secret length: %zu",
              strlen(client_secret));
    g_free(relay_urls);
    return FALSE;
  }

  /* Validate pubkey length */
  if (strlen(signer_pubkey) != 64) {
    g_warning("[SIGNER_SERVICE] Invalid saved signer pubkey length: %zu",
              strlen(signer_pubkey));
    g_free(relay_urls);
    return FALSE;
  }

  fprintf(stderr, "\n*** RESTORING SESSION FROM SETTINGS ***\n");
  fprintf(stderr, "signer_pubkey from settings: %s\n", signer_pubkey);
  fprintf(stderr, "relay count from settings: %zu\n", n_relays);
  for (gsize i = 0; i < n_relays && relay_urls; i++) {
    fprintf(stderr, "  relay[%zu]: %s\n", i, relay_urls[i] ? relay_urls[i] : "(null)");
  }

  /* Create a new NIP-46 session */
  NostrNip46Session *session = nostr_nip46_client_new();
  if (!session) {
    g_warning("[SIGNER_SERVICE] Failed to create NIP-46 session");
    g_free(relay_urls);
    return FALSE;
  }

  /* Set the client secret key for ECDH encryption */
  if (nostr_nip46_client_set_secret(session, client_secret) != 0) {
    g_warning("[SIGNER_SERVICE] Failed to set client secret for ECDH");
    nostr_nip46_session_free(session);
    g_free(relay_urls);
    return FALSE;
  }

  /* Set the signer pubkey */
  if (nostr_nip46_client_set_signer_pubkey(session, signer_pubkey) != 0) {
    g_warning("[SIGNER_SERVICE] Failed to set signer pubkey");
    nostr_nip46_session_free(session);
    g_free(relay_urls);
    return FALSE;
  }

  /* Set relays directly on the session */
  if (n_relays > 0 && relay_urls) {
    nostr_nip46_session_set_relays(session, relay_urls, n_relays);
  } else {
    /* Fallback to default relay list if none saved */
    gsize n_defaults = 0;
    const char *const *defaults = nip46_get_fallback_relays(&n_defaults);
    nostr_nip46_session_set_relays(session, defaults, n_defaults);
    g_warning("[SIGNER_SERVICE] No relays in settings, using %zu default(s): %s",
              n_defaults, defaults[0]);
  }

  g_free(relay_urls);

  /* Install the session */
  gnostr_signer_service_set_nip46_session(self, session);

  g_message("[SIGNER_SERVICE] NIP-46 session restored successfully (signer: %.16s..., relays: %zu)",
            signer_pubkey, n_relays);

  return TRUE;
}

void
gnostr_signer_service_clear_saved_credentials(GnostrSignerService *self)
{
  (void)self;  /* Unused but kept for API consistency */

  g_autoptr(GSettings) settings = g_settings_new(SETTINGS_SCHEMA_CLIENT);
  if (!settings) return;

  g_settings_set_string(settings, "nip46-client-secret", "");
  g_settings_set_string(settings, "nip46-signer-pubkey", "");
  /* Clear relay array */
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE_STRING_ARRAY);
  g_settings_set_value(settings, "nip46-relays", g_variant_builder_end(&builder));


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
  g_clear_pointer(&ctx->peer_pubkey, g_free);
  g_clear_pointer(&ctx->data, g_free);
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

  /* Lock mutex for session access */
  g_mutex_lock(&ctx->service->session_mutex);

  if (!ctx->service->nip46_session) {
    g_mutex_unlock(&ctx->service->session_mutex);
    g_warning("[SIGNER_SERVICE] NIP-44 %s failed: session is NULL",
              ctx->is_encrypt ? "encrypt" : "decrypt");
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session not available - please sign in again");
    return;
  }

  /* nostrc-13gf: Take a real reference while holding the lock so a
   * concurrent logout/replace can never free the session under us. */
  NostrNip46Session *session = nostr_nip46_session_ref(ctx->service->nip46_session);

  g_mutex_unlock(&ctx->service->session_mutex);

  char *result = NULL;
  int rc;

  if (ctx->is_encrypt) {
    g_debug("[SIGNER_SERVICE] NIP-46 NIP-44 encrypting for %.16s...", ctx->peer_pubkey);
    /* nostrc-u1qh: Use _rpc variant to delegate to remote signer (not local transport key) */
    rc = nostr_nip46_client_nip44_encrypt_rpc(session,
                                               ctx->peer_pubkey,
                                               ctx->data,
                                               &result);
  } else {
    g_debug("[SIGNER_SERVICE] NIP-46 NIP-44 decrypting from %.16s...", ctx->peer_pubkey);
    /* nostrc-u1qh: Use _rpc variant to delegate to remote signer (not local transport key) */
    rc = nostr_nip46_client_nip44_decrypt_rpc(session,
                                               ctx->peer_pubkey,
                                               ctx->data,
                                               &result);
  }

  nostr_nip46_session_unref(session);

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

  /* Check state before dispatching */
  if (self->state != GNOSTR_SIGNER_STATE_CONNECTED) {
    GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                 "Signer not connected - please sign in first");
    callback(self, NULL, error, user_data);
    g_error_free(error);
    return;
  }

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

  /* Check state before dispatching */
  if (self->state != GNOSTR_SIGNER_STATE_CONNECTED) {
    GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                 "Signer not connected - please sign in first");
    callback(self, NULL, error, user_data);
    g_error_free(error);
    return;
  }

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

/* ---- nostrc-3m86: binary-safe NIP-44 ----------------------------------
 *
 * Both signer transports carry their parameters as strings — NIP-46 as JSON
 * params, NIP-55L as D-Bus strings — and both require valid UTF-8. A payload
 * of raw bytes therefore cannot ride the pair above at all: an encoder either
 * rejects it or substitutes U+FFFD, and the signer then encrypts corrupted
 * bytes with nobody noticing. A format whose payload width is its own
 * declaration (a Concord CORD-06 rekey blob is 72, 104 or 136 bytes, and any
 * other width is dropped as malformed) cannot absorb that.
 *
 * The fix is the `_b64` method pair on both transports: the *plaintext* side
 * travels as standard base64 while the ciphertext stays an ordinary NIP-44 v2
 * payload — byte-identical to what the string lane would produce for the same
 * bytes, so the two lanes interoperate and a recipient opens a blob with
 * whichever one suits its own payload. A signer that does not know the
 * methods fails the call rather than encrypting base64 text as if it were the
 * plaintext, which is why they are distinct methods and not a flag. */

typedef struct {
  GnostrSignerService *service;
  GnostrNip44Callback encrypt_callback;    /* encrypt: result is the payload */
  GnostrNip44BytesCallback decrypt_callback; /* decrypt: result is the bytes */
  gpointer user_data;
  char *peer_pubkey;
  GBytes *plaintext;   /* encrypt */
  char *ciphertext;    /* decrypt */
  gboolean is_encrypt;
} Nip44BytesContext;

static void
nip44_bytes_context_free(Nip44BytesContext *ctx)
{
  if (!ctx) return;
  g_clear_pointer(&ctx->peer_pubkey, g_free);
  g_clear_pointer(&ctx->plaintext, g_bytes_unref);
  if (ctx->ciphertext) {
    memset(ctx->ciphertext, 0, strlen(ctx->ciphertext));
    g_clear_pointer(&ctx->ciphertext, g_free);
  }
  g_free(ctx);
}

static void
nip44_bytes_fail(Nip44BytesContext *ctx, GError *error)
{
  if (ctx->is_encrypt)
    ctx->encrypt_callback(ctx->service, NULL, error, ctx->user_data);
  else
    ctx->decrypt_callback(ctx->service, NULL, error, ctx->user_data);
}

/* GLib's base64 decoder silently skips characters outside the alphabet, which
 * would turn a corrupted response into a shorter plaintext rather than a
 * failure — exactly the mangling this lane exists to prevent. Validate first,
 * and require the decoded length to be the one the encoding declares. */
static GBytes *
nip44_b64_decode_strict(const char *s)
{
  if (!s) return NULL;
  size_t len = strlen(s);
  if (len == 0 || (len % 4) != 0) return NULL;
  size_t pad = 0;
  if (s[len - 1] == '=') pad = (s[len - 2] == '=') ? 2 : 1;
  for (size_t i = 0; i < len - pad; i++) {
    char c = s[i];
    gboolean ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '+' || c == '/';
    if (!ok) return NULL;
  }
  size_t expected = (len / 4) * 3 - pad;
  gsize decoded_len = 0;
  guchar *decoded = g_base64_decode(s, &decoded_len);
  if (!decoded) return NULL;
  if ((size_t)decoded_len != expected) {
    memset(decoded, 0, decoded_len);
    g_free(decoded);
    return NULL;
  }
  return g_bytes_new_take(decoded, decoded_len);
}

/* NIP-46 worker. The client-side b64 helpers own the encoding, so the raw
 * bytes never become a C string on the way out. */
static void
nip46_nip44_bytes_thread(GTask *task, gpointer source, gpointer task_data,
                         GCancellable *cancellable)
{
  Nip44BytesContext *ctx = (Nip44BytesContext *)task_data;
  (void)source;
  (void)cancellable;

  g_mutex_lock(&ctx->service->session_mutex);
  if (!ctx->service->nip46_session) {
    g_mutex_unlock(&ctx->service->session_mutex);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-46 session not available - please sign in again");
    return;
  }
  /* nostrc-13gf: take a real reference under the lock so a concurrent
   * logout can never free the session under us. */
  NostrNip46Session *session = nostr_nip46_session_ref(ctx->service->nip46_session);
  g_mutex_unlock(&ctx->service->session_mutex);

  if (ctx->is_encrypt) {
    gsize len = 0;
    const guchar *bytes = g_bytes_get_data(ctx->plaintext, &len);
    char *payload = NULL;
    int rc = nostr_nip46_client_nip44_encrypt_b64_rpc(session, ctx->peer_pubkey,
                                                      (const uint8_t *)bytes, (size_t)len,
                                                      &payload);
    nostr_nip46_session_unref(session);
    if (rc != 0 || !payload) {
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                              "NIP-44 binary encryption failed (error %d)", rc);
      return;
    }
    g_task_return_pointer(task, payload, g_free);
    return;
  }

  uint8_t *plaintext = NULL;
  size_t plaintext_len = 0;
  int rc = nostr_nip46_client_nip44_decrypt_b64_rpc(session, ctx->peer_pubkey,
                                                    ctx->ciphertext,
                                                    &plaintext, &plaintext_len);
  nostr_nip46_session_unref(session);
  if (rc != 0 || !plaintext) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "NIP-44 binary decryption failed (error %d)", rc);
    return;
  }
  GBytes *out = g_bytes_new(plaintext, plaintext_len);
  memset(plaintext, 0, plaintext_len);
  free(plaintext);
  g_task_return_pointer(task, out, (GDestroyNotify)g_bytes_unref);
}

static void
on_nip46_nip44_bytes_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip44BytesContext *ctx = (Nip44BytesContext *)user_data;
  (void)source;

  GError *error = NULL;
  gpointer result = g_task_propagate_pointer(G_TASK(res), &error);

  if (error) {
    nip44_bytes_fail(ctx, error);
    g_error_free(error);
  } else if (ctx->is_encrypt) {
    ctx->encrypt_callback(ctx->service, (const char *)result, NULL, ctx->user_data);
    g_free(result);
  } else {
    ctx->decrypt_callback(ctx->service, (GBytes *)result, NULL, ctx->user_data);
    g_bytes_unref((GBytes *)result);
  }

  nip44_bytes_context_free(ctx);
}

static void
on_nip55l_nip44_encrypt_b64_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip44BytesContext *ctx = (Nip44BytesContext *)user_data;
  NostrSignerProxy *proxy = (NostrSignerProxy *)source;

  GError *error = NULL;
  char *ciphertext = NULL;
  gboolean ok = nostr_org_nostr_signer_call_nip44_encrypt_b64_finish(
      proxy, &ciphertext, res, &error);

  if (!ok) {
    if (!error)
      error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "NIP-44 binary encryption failed");
    ctx->encrypt_callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
  } else {
    ctx->encrypt_callback(ctx->service, ciphertext, NULL, ctx->user_data);
    g_free(ciphertext);
  }
  nip44_bytes_context_free(ctx);
}

static void
on_nip55l_nip44_decrypt_b64_complete(GObject *source, GAsyncResult *res, gpointer user_data)
{
  Nip44BytesContext *ctx = (Nip44BytesContext *)user_data;
  NostrSignerProxy *proxy = (NostrSignerProxy *)source;

  GError *error = NULL;
  char *plaintext_b64 = NULL;
  gboolean ok = nostr_org_nostr_signer_call_nip44_decrypt_b64_finish(
      proxy, &plaintext_b64, res, &error);

  if (!ok) {
    if (!error)
      error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED, "NIP-44 binary decryption failed");
    ctx->decrypt_callback(ctx->service, NULL, error, ctx->user_data);
    g_error_free(error);
    nip44_bytes_context_free(ctx);
    return;
  }

  GBytes *plaintext = nip44_b64_decode_strict(plaintext_b64);
  if (plaintext_b64) memset(plaintext_b64, 0, strlen(plaintext_b64));
  g_free(plaintext_b64);
  if (!plaintext) {
    GError *bad = g_error_new(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                              "Signer returned a malformed base64 plaintext");
    ctx->decrypt_callback(ctx->service, NULL, bad, ctx->user_data);
    g_error_free(bad);
  } else {
    ctx->decrypt_callback(ctx->service, plaintext, NULL, ctx->user_data);
    g_bytes_unref(plaintext);
  }
  nip44_bytes_context_free(ctx);
}

/* Shared prologue: both directions differ only in what they hand the
 * transport, so the state checks and proxy acquisition live in one place. */
static Nip44BytesContext *
nip44_bytes_begin(GnostrSignerService *self, const char *peer_pubkey,
                  gboolean is_encrypt,
                  GnostrNip44Callback encrypt_callback,
                  GnostrNip44BytesCallback decrypt_callback,
                  gpointer user_data)
{
  if (self->state != GNOSTR_SIGNER_STATE_CONNECTED) {
    GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                                "Signer not connected - please sign in first");
    if (is_encrypt) encrypt_callback(self, NULL, error, user_data);
    else decrypt_callback(self, NULL, error, user_data);
    g_error_free(error);
    return NULL;
  }

  Nip44BytesContext *ctx = g_new0(Nip44BytesContext, 1);
  ctx->service = self;
  ctx->encrypt_callback = encrypt_callback;
  ctx->decrypt_callback = decrypt_callback;
  ctx->user_data = user_data;
  ctx->peer_pubkey = g_strdup(peer_pubkey);
  ctx->is_encrypt = is_encrypt;
  return ctx;
}

/* Returns the proxy, or NULL after reporting the failure through @ctx. */
static NostrSignerProxy *
nip44_bytes_nip55l_proxy(Nip44BytesContext *ctx)
{
  GnostrSignerService *self = ctx->service;
  if (self->nip55l_proxy) return self->nip55l_proxy;

  GError *error = NULL;
  self->nip55l_proxy = gnostr_signer_proxy_get(&error);
  if (!self->nip55l_proxy) {
    GError *cb_error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                   "Failed to connect to local signer: %s",
                                   error ? error->message : "unknown");
    nip44_bytes_fail(ctx, cb_error);
    g_error_free(cb_error);
    if (error) g_error_free(error);
    nip44_bytes_context_free(ctx);
    return NULL;
  }
  return self->nip55l_proxy;
}

void
gnostr_signer_service_nip44_encrypt_bytes_async(GnostrSignerService *self,
                                                const char *peer_pubkey,
                                                GBytes *plaintext,
                                                GCancellable *cancellable,
                                                GnostrNip44Callback callback,
                                                gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));
  g_return_if_fail(peer_pubkey != NULL);
  g_return_if_fail(plaintext != NULL);
  g_return_if_fail(callback != NULL);

  Nip44BytesContext *ctx =
    nip44_bytes_begin(self, peer_pubkey, TRUE, callback, NULL, user_data);
  if (!ctx) return;
  ctx->plaintext = g_bytes_ref(plaintext);

  switch (self->method) {
    case GNOSTR_SIGNER_METHOD_NIP46: {
      GTask *task = g_task_new(NULL, cancellable, on_nip46_nip44_bytes_complete, ctx);
      g_task_set_task_data(task, ctx, NULL);
      g_task_run_in_thread(task, nip46_nip44_bytes_thread);
      g_object_unref(task);
      break;
    }

    case GNOSTR_SIGNER_METHOD_NIP55L: {
      NostrSignerProxy *proxy = nip44_bytes_nip55l_proxy(ctx);
      if (!proxy) return;
      gsize len = 0;
      const guchar *bytes = g_bytes_get_data(plaintext, &len);
      char *b64 = g_base64_encode(bytes, len);
      nostr_org_nostr_signer_call_nip44_encrypt_b64(
          proxy, b64, peer_pubkey, "" /* current_user: default */,
          cancellable, on_nip55l_nip44_encrypt_b64_complete, ctx);
      /* The encoded plaintext is as sensitive as the plaintext itself. */
      memset(b64, 0, strlen(b64));
      g_free(b64);
      break;
    }

    case GNOSTR_SIGNER_METHOD_NONE:
    default: {
      GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                  "No signing method available");
      callback(self, NULL, error, user_data);
      g_error_free(error);
      nip44_bytes_context_free(ctx);
      break;
    }
  }
}

void
gnostr_signer_service_nip44_decrypt_bytes_async(GnostrSignerService *self,
                                                const char *peer_pubkey,
                                                const char *ciphertext,
                                                GCancellable *cancellable,
                                                GnostrNip44BytesCallback callback,
                                                gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_SIGNER_SERVICE(self));
  g_return_if_fail(peer_pubkey != NULL);
  g_return_if_fail(ciphertext != NULL);
  g_return_if_fail(callback != NULL);

  Nip44BytesContext *ctx =
    nip44_bytes_begin(self, peer_pubkey, FALSE, NULL, callback, user_data);
  if (!ctx) return;
  ctx->ciphertext = g_strdup(ciphertext);

  switch (self->method) {
    case GNOSTR_SIGNER_METHOD_NIP46: {
      GTask *task = g_task_new(NULL, cancellable, on_nip46_nip44_bytes_complete, ctx);
      g_task_set_task_data(task, ctx, NULL);
      g_task_run_in_thread(task, nip46_nip44_bytes_thread);
      g_object_unref(task);
      break;
    }

    case GNOSTR_SIGNER_METHOD_NIP55L: {
      NostrSignerProxy *proxy = nip44_bytes_nip55l_proxy(ctx);
      if (!proxy) return;
      nostr_org_nostr_signer_call_nip44_decrypt_b64(
          proxy, ciphertext, peer_pubkey, "" /* current_user: default */,
          cancellable, on_nip55l_nip44_decrypt_b64_complete, ctx);
      break;
    }

    case GNOSTR_SIGNER_METHOD_NONE:
    default: {
      GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                  "No signing method available");
      callback(self, NULL, error, user_data);
      g_error_free(error);
      nip44_bytes_context_free(ctx);
      break;
    }
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
