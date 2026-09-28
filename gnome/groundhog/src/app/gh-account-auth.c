#include "gh-account-auth.h"
#include "gh-identity.h"
#include "gh-signer.h"

#include <nostr-event.h>
#include <nostr-tag.h>

typedef struct _Request Request;

/* R6 state of one relay URL for this generation. */
typedef struct {
  gchar *url;
  gboolean approved; /* the signer signed for it at least once */
  gboolean declined; /* the user denied it: never asked again */
  Request *active;   /* the request the signer is being asked for */
  GQueue waiting;    /* Request, oldest first */
} Relay;

struct _Request {
  GhAccountAuth *owner; /* strong until the request is answered */
  Relay *relay;
  GTask *task;
  gchar *unsigned_json;
  gulong cancelled_handler; /* while waiting */
};

struct _GhAccountAuth {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  guint64 generation;
  GhRelayAuthSigner *signer;
  GHashTable *relays; /* url -> Relay */
  gboolean revoked;
};

enum { SIGNAL_RELAY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAccountAuth, gh_account_auth, G_TYPE_OBJECT)

static GhAccountAuthRelayState
relay_state(const Relay *relay)
{
  if (relay->declined)
    return GH_ACCOUNT_AUTH_RELAY_DECLINED;
  if (relay->active)
    return GH_ACCOUNT_AUTH_RELAY_WAITING;
  return relay->approved ? GH_ACCOUNT_AUTH_RELAY_APPROVED : GH_ACCOUNT_AUTH_RELAY_NONE;
}

static void
relay_free(gpointer data)
{
  Relay *relay = data;
  /* Every request holds its owner, so none is left at finalize. */
  g_warn_if_fail(!relay->active && g_queue_is_empty(&relay->waiting));
  g_free(relay->url);
  g_free(relay);
}

static void
notify_relay(GhAccountAuth *self, Relay *relay, GhAccountAuthRelayState before)
{
  if (relay_state(relay) != before)
    g_signal_emit(self, signals[SIGNAL_RELAY_CHANGED], 0, relay->url);
}

static gboolean
usable(GhAccountAuth *self)
{
  return !self->revoked && self->accounts && !gh_relay_auth_signer_is_revoked(self->signer) &&
         gh_account_controller_is_current(self->accounts, self->generation);
}

static void
request_free(Request *request)
{
  GCancellable *cancellable = g_task_get_cancellable(request->task);
  if (request->cancelled_handler)
    g_signal_handler_disconnect(cancellable, request->cancelled_handler);
  g_object_unref(request->task);
  g_free(request->unsigned_json);
  g_object_unref(request->owner);
  g_free(request);
}

static void
request_return_error(Request *request, const GError *error)
{
  g_task_return_error(request->task, g_error_copy(error));
  request_free(request);
}

static void sign_done(GObject *source, GAsyncResult *result, gpointer data);

static void
request_start(Request *request)
{
  request->relay->active = request;
  gh_account_controller_sign_with_cancellable_async(request->owner->accounts,
                                                    request->unsigned_json,
                                                    g_task_get_cancellable(request->task),
                                                    sign_done, request);
}

/* The waiting requests share an outcome: each fails with error. */
static void
fail_waiting(Relay *relay, const GError *error)
{
  Request *request;
  while ((request = g_queue_pop_head(&relay->waiting)))
    request_return_error(request, error);
}

/* The oldest waiting request asks the signer, unless one already does. */
static void
start_next(GhAccountAuth *self, Relay *relay)
{
  while (!relay->active && !g_queue_is_empty(&relay->waiting)) {
    Request *next = g_queue_pop_head(&relay->waiting);
    if (!usable(self)) {
      g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                                    "Account AUTH was revoked");
      request_return_error(next, error);
      continue;
    }
    GCancellable *cancellable = g_task_get_cancellable(next->task);
    if (next->cancelled_handler) {
      g_signal_handler_disconnect(cancellable, next->cancelled_handler);
      next->cancelled_handler = 0;
    }
    if (g_task_return_error_if_cancelled(next->task)) {
      request_free(next);
      continue;
    }
    request_start(next);
  }
}

static void
sign_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  Request *request = data;
  GhAccountAuth *self = g_object_ref(request->owner);
  Relay *relay = request->relay;
  GhAccountAuthRelayState before = relay_state(relay);
  GError *error = NULL;
  gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  relay->active = NULL;
  g_autoptr(GError) shared = NULL; /* the outcome the waiting requests share */
  if (signed_json) {
    relay->approved = TRUE;
    g_task_return_pointer(request->task, signed_json, g_free);
  } else {
    gboolean cancelled = g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED) ||
                         g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    if (g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED) && usable(self))
      relay->declined = TRUE;
    /* Only a cancelled request (its connection went away) lets the next one
     * ask at once; any other failure would be asked again immediately. */
    if (!cancelled)
      shared = g_error_copy(error);
    g_task_return_error(request->task, error);
  }
  request_free(request);
  if (shared)
    fail_waiting(relay, shared);
  else
    start_next(self, relay);
  notify_relay(self, relay, before);
  g_object_unref(self);
}

static void
on_waiting_cancelled(GCancellable *cancellable, gpointer data)
{
  Request *request = data;
  g_queue_remove(&request->relay->waiting, request);
  /* A plain handler, so disconnecting it from its own emission is safe. */
  g_signal_handler_disconnect(cancellable, request->cancelled_handler);
  request->cancelled_handler = 0;
  g_task_return_new_error(request->task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                          "AUTH request cancelled");
  request_free(request);
}

/* The single "relay" tag of an unsigned kind-22242 template, or NULL. */
static gchar *
relay_of(const gchar *unsigned_json)
{
  NostrEvent *event = nostr_event_new();
  gchar *url = NULL;
  gboolean ambiguous = FALSE;
  if (event && unsigned_json &&
      nostr_event_deserialize_compact(event, unsigned_json, NULL) == 1 &&
      nostr_event_get_kind(event) == GH_RELAY_AUTH_KIND) {
    NostrTags *tags = nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
      NostrTag *tag = nostr_tags_get(tags, i);
      if (g_strcmp0(nostr_tag_get_key(tag), "relay") != 0 || nostr_tag_size(tag) < 2)
        continue;
      ambiguous |= url != NULL;
      if (!url)
        url = g_strdup(nostr_tag_get_value(tag));
    }
  }
  if (event)
    nostr_event_free(event);
  if (ambiguous)
    g_clear_pointer(&url, g_free);
  return url;
}

/* GhRelayAuthSignAsyncFunc; user_data is a GWeakRef to the adapter, so a
 * signer that outlives it (held by a scope) signs nothing. */
static void
sign_async(gpointer user_data, const gchar *unsigned_json, GCancellable *cancellable,
           GAsyncReadyCallback callback, gpointer callback_data)
{
  GTask *task = g_task_new(NULL, cancellable, callback, callback_data);
  g_task_set_source_tag(task, sign_async);
  GhAccountAuth *self = g_weak_ref_get(user_data);
  if (!self || !usable(self)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Account AUTH belongs to an account session that has ended");
    g_object_unref(task);
    g_clear_object(&self);
    return;
  }
  g_autofree gchar *url = relay_of(unsigned_json);
  if (!url) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "The AUTH event names no single relay");
    g_object_unref(task);
    g_object_unref(self);
    return;
  }
  Relay *relay = g_hash_table_lookup(self->relays, url);
  if (!relay) {
    relay = g_new0(Relay, 1);
    relay->url = g_steal_pointer(&url);
    g_queue_init(&relay->waiting);
    g_hash_table_insert(self->relays, relay->url, relay);
  }
  if (relay->declined) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_DENIED,
                            "Signing in to this relay was declined for this session");
    g_object_unref(task);
    g_object_unref(self);
    return;
  }
  if (g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    g_object_unref(self);
    return;
  }
  Request *request = g_new0(Request, 1);
  request->owner = self; /* the reference from g_weak_ref_get */
  request->relay = relay;
  request->task = task;
  request->unsigned_json = g_strdup(unsigned_json);
  if (relay->active) {
    if (cancellable)
      request->cancelled_handler = g_signal_connect(cancellable, "cancelled",
                                                    G_CALLBACK(on_waiting_cancelled),
                                                    request);
    g_queue_push_tail(&relay->waiting, request);
    return;
  }
  GhAccountAuthRelayState before = relay_state(relay);
  g_object_ref(self);
  request_start(request);
  notify_relay(self, relay, before);
  g_object_unref(self);
}

static gchar *
sign_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) == sign_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
weak_ref_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

GhAccountAuth *
gh_account_auth_new(GhAccountController *accounts)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  if (gh_account_controller_get_state(accounts) != GH_ACCOUNT_STATE_ACTIVE)
    return NULL;
  const gchar *npub = gh_account_controller_get_active_npub(accounts);
  g_autofree gchar *pubkey = npub ? gh_identity_pubkey_hex(npub) : NULL;
  GCancellable *generation_cancel = gh_account_controller_get_cancellable(accounts);
  if (!pubkey || !generation_cancel)
    return NULL;
  GhAccountAuth *self = g_object_new(GH_TYPE_ACCOUNT_AUTH, NULL);
  self->accounts = g_object_ref(accounts);
  self->generation = gh_account_controller_get_generation(accounts);
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, self);
  self->signer = gh_relay_auth_signer_new(self->generation, generation_cancel, pubkey,
                                          sign_async, sign_finish, ref, weak_ref_free);
  return self;
}

GhRelayAuthSigner *
gh_account_auth_get_signer(GhAccountAuth *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_AUTH(self), NULL);
  return self->signer;
}

guint64
gh_account_auth_get_generation(GhAccountAuth *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_AUTH(self), 0);
  return self->generation;
}

GhAccountAuthRelayState
gh_account_auth_get_relay_state(GhAccountAuth *self, const gchar *url)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_AUTH(self), GH_ACCOUNT_AUTH_RELAY_NONE);
  Relay *relay = url ? g_hash_table_lookup(self->relays, url) : NULL;
  return relay ? relay_state(relay) : GH_ACCOUNT_AUTH_RELAY_NONE;
}

void
gh_account_auth_revoke(GhAccountAuth *self)
{
  g_return_if_fail(GH_IS_ACCOUNT_AUTH(self));
  if (self->revoked)
    return;
  self->revoked = TRUE;
  g_object_ref(self);
  /* Cancels the cancellable of every attempt the signer serves: signer calls
   * in flight are revoked, and waiting requests fail from their handlers. */
  if (self->signer)
    gh_relay_auth_signer_revoke(self->signer);
  g_autoptr(GError) error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                                "Account AUTH was revoked");
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->relays);
  while (g_hash_table_iter_next(&iter, NULL, &value))
    fail_waiting(value, error);
  g_object_unref(self);
}

static void
gh_account_auth_dispose(GObject *object)
{
  GhAccountAuth *self = GH_ACCOUNT_AUTH(object);
  gh_account_auth_revoke(self);
  g_clear_object(&self->accounts);
  G_OBJECT_CLASS(gh_account_auth_parent_class)->dispose(object);
}

static void
gh_account_auth_finalize(GObject *object)
{
  GhAccountAuth *self = GH_ACCOUNT_AUTH(object);
  gh_relay_auth_signer_unref(self->signer);
  g_hash_table_unref(self->relays);
  G_OBJECT_CLASS(gh_account_auth_parent_class)->finalize(object);
}

static void
gh_account_auth_class_init(GhAccountAuthClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_account_auth_dispose;
  object_class->finalize = gh_account_auth_finalize;
  signals[SIGNAL_RELAY_CHANGED] = g_signal_new("relay-changed", G_TYPE_FROM_CLASS(klass),
                                               G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                               G_TYPE_NONE, 1, G_TYPE_STRING);
}

static void
gh_account_auth_init(GhAccountAuth *self)
{
  self->relays = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, relay_free);
}
