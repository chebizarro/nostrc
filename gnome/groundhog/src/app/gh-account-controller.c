#include "gh-account-controller.h"
#include "gh-identity.h"
#include "gh-signer.h"

#define SIGNER_BUS "org.nostr.Signer"

struct _GhAccountController {
  GObject parent_instance;
  GSettings *settings; /* NULL once disposed */
  GDBusConnection *bus;
  GhAccountListFunc list;
  gpointer list_data;

  GPtrArray *identities; /* NULL until listed or after a failed listing */
  gboolean listed;
  guint list_serial;
  GCancellable *list_cancel;

  GhAccountState state;
  gchar *active_npub;
  guint64 generation;
  GCancellable *generation_cancel;
  GhSigner *signer; /* active generation only; never exposes secret material */

  GhSignerAvailability availability;
  guint watch_id;
  guint signer_serial;
  GCancellable *signer_cancel;
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAccountController, gh_account_controller, G_TYPE_OBJECT)

static GPtrArray *
default_list(gpointer user_data, GError **error)
{
  (void)user_data;
  return gh_identity_list(error);
}

static gboolean
identities_contain(GPtrArray *identities, const gchar *npub)
{
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (g_strcmp0(info->npub, npub) == 0)
      return TRUE;
  }
  return FALSE;
}

/* Revoke first, then cancel: work woken by the cancellation already sees a
 * stale generation. */
static void
revoke_generation(GhAccountController *self, gboolean replace)
{
  GCancellable *old = g_steal_pointer(&self->generation_cancel);
  self->generation++;
  if (replace)
    self->generation_cancel = g_cancellable_new();
  if (old) {
    g_cancellable_cancel(old);
    g_object_unref(old);
  }
}

static gboolean
signer_method_supported(GhAccountController *self)
{
  if (!self->settings) return FALSE;
  g_autofree gchar *method = g_settings_get_string(self->settings, "signer-method");
  return g_strcmp0(method, "auto") == 0 ||
         g_strcmp0(method, "local") == 0 ||
         g_strcmp0(method, "nip55l") == 0;
}

static void
bind_signer(GhAccountController *self)
{
  gh_signer_free(g_steal_pointer(&self->signer));
  if (self->active_npub && self->bus && signer_method_supported(self))
    self->signer = gh_signer_new(self->bus, self->active_npub, NULL);
}

/* Returns TRUE when it emitted "changed". */
static gboolean
update_state(GhAccountController *self)
{
  if (!self->settings)
    return FALSE;
  g_autofree gchar *current = g_settings_get_string(self->settings, "current-npub");
  GhAccountState state;
  if (!self->listed)
    state = GH_ACCOUNT_STATE_DISCOVERING;
  else if (!self->identities)
    state = GH_ACCOUNT_STATE_STORE_UNAVAILABLE;
  else if (*current && !identities_contain(self->identities, current))
    state = GH_ACCOUNT_STATE_SELECTED_MISSING;
  else if (self->identities->len == 0)
    state = GH_ACCOUNT_STATE_NO_IDENTITIES;
  else if (!*current)
    state = GH_ACCOUNT_STATE_UNSELECTED;
  else
    state = GH_ACCOUNT_STATE_ACTIVE;

  const gchar *active = state == GH_ACCOUNT_STATE_ACTIVE ? current : NULL;
  gboolean account_changed = g_strcmp0(active, self->active_npub) != 0;
  if (account_changed) {
    g_free(self->active_npub);
    self->active_npub = g_strdup(active);
    revoke_generation(self, TRUE);
    bind_signer(self);
  }
  if (!account_changed && state == self->state)
    return FALSE;
  self->state = state;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

static void
list_thread(GTask *task, gpointer source, gpointer task_data, GCancellable *cancellable)
{
  GhAccountController *self = source;
  GError *error = NULL;
  (void)task_data;
  (void)cancellable;
  GPtrArray *identities = self->list(self->list_data, &error);
  if (identities)
    g_task_return_pointer(task, identities, (GDestroyNotify)g_ptr_array_unref);
  else if (error)
    g_task_return_error(task, error);
  else
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Identity listing failed");
}

static void
list_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(source);
  guint serial = GPOINTER_TO_UINT(g_task_get_task_data(G_TASK(result)));
  g_autoptr(GError) error = NULL;
  GPtrArray *identities = g_task_propagate_pointer(G_TASK(result), &error);
  (void)user_data;
  if (!self->settings || serial != self->list_serial) {
    g_clear_pointer(&identities, g_ptr_array_unref);
    return;
  }
  if (error)
    g_message("Groundhog could not list signer identities: %s", error->message);
  g_clear_pointer(&self->identities, g_ptr_array_unref);
  self->identities = identities;
  self->listed = TRUE;
  /* A new listing can change the identity menu without changing state. */
  if (!update_state(self))
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

void
gh_account_controller_refresh(GhAccountController *self)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  if (!self->settings)
    return;
  /* NULL before the first listing. */
  if (self->list_cancel)
    g_cancellable_cancel(self->list_cancel);
  g_clear_object(&self->list_cancel);
  self->list_cancel = g_cancellable_new();
  GTask *task = g_task_new(self, self->list_cancel, list_done, NULL);
  g_task_set_source_tag(task, gh_account_controller_refresh);
  g_task_set_task_data(task, GUINT_TO_POINTER(++self->list_serial), NULL);
  g_task_run_in_thread(task, list_thread);
  g_object_unref(task);
}

static void
set_availability(GhAccountController *self, GhSignerAvailability availability)
{
  if (self->availability == availability)
    return;
  self->availability = availability;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

typedef struct {
  GhAccountController *self;
  guint serial;
} ActivatableQuery;

static void
activatable_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  ActivatableQuery *query = user_data;
  GhAccountController *self = query->self;
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source),
                                                            result, &error);
  if (self->settings && query->serial == self->signer_serial) {
    gboolean activatable = FALSE;
    if (reply) {
      g_autofree const gchar **names = NULL;
      g_variant_get(reply, "(^a&s)", &names);
      activatable = names && g_strv_contains((const gchar *const *)names, SIGNER_BUS);
    }
    set_availability(self, activatable ? GH_SIGNER_AVAILABILITY_ACTIVATABLE
                                       : GH_SIGNER_AVAILABILITY_ABSENT);
  }
  g_object_unref(self);
  g_free(query);
}

static void
signer_appeared(GDBusConnection *bus, const gchar *name, const gchar *owner,
                gpointer user_data)
{
  GhAccountController *self = user_data;
  (void)bus;
  (void)name;
  (void)owner;
  self->signer_serial++;
  set_availability(self, GH_SIGNER_AVAILABILITY_RUNNING);
}

static void
signer_vanished(GDBusConnection *bus, const gchar *name, gpointer user_data)
{
  GhAccountController *self = user_data;
  (void)name;
  self->signer_serial++;
  if (!bus || g_dbus_connection_is_closed(bus)) {
    set_availability(self, GH_SIGNER_AVAILABILITY_NO_BUS);
    return;
  }
  ActivatableQuery *query = g_new0(ActivatableQuery, 1);
  query->self = g_object_ref(self);
  query->serial = self->signer_serial;
  g_dbus_connection_call(bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                         "org.freedesktop.DBus", "ListActivatableNames", NULL,
                         G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, -1,
                         self->signer_cancel, activatable_done, query);
}

static void
on_settings_changed(GSettings *settings, const gchar *key, gpointer user_data)
{
  GhAccountController *self = user_data;
  (void)settings;
  if (g_strcmp0(key, "current-npub") == 0)
    (void)update_state(self);
  else if (g_strcmp0(key, "signer-method") == 0) {
    revoke_generation(self, TRUE);
    bind_signer(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
}

GhAccountController *
gh_account_controller_new_full(GSettings *settings, GDBusConnection *bus,
                               GhAccountListFunc list, gpointer list_data)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  g_return_val_if_fail(!bus || G_IS_DBUS_CONNECTION(bus), NULL);
  g_return_val_if_fail(list != NULL, NULL);
  GhAccountController *self = g_object_new(GH_TYPE_ACCOUNT_CONTROLLER, NULL);
  self->settings = g_object_ref(settings);
  self->list = list;
  self->list_data = list_data;
  g_signal_connect_object(settings, "changed", G_CALLBACK(on_settings_changed), self, 0);
  if (bus) {
    self->bus = g_object_ref(bus);
    self->availability = GH_SIGNER_AVAILABILITY_UNKNOWN;
    self->watch_id = g_bus_watch_name_on_connection(bus, SIGNER_BUS,
                                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                                    signer_appeared, signer_vanished,
                                                    self, NULL);
  } else {
    self->availability = GH_SIGNER_AVAILABILITY_NO_BUS;
  }
  gh_account_controller_refresh(self);
  return self;
}

GhAccountController *
gh_account_controller_new(GSettings *settings, GDBusConnection *bus)
{
  return gh_account_controller_new_full(settings, bus, default_list, NULL);
}

gboolean
gh_account_controller_select(GhAccountController *self, const gchar *npub,
                             GError **error)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  if (!self->settings) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                        "Groundhog account controller is shut down");
    return FALSE;
  }
  if (npub && *npub && !self->identities) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        "Signer identities are not available");
    return FALSE;
  }
  if (!gh_identity_select_from_list(self->settings, self->identities,
                                    npub ? npub : "", error))
    return FALSE;
  /* The settings change handler usually ran already; this is idempotent. */
  (void)update_state(self);
  return TRUE;
}

GhAccountState
gh_account_controller_get_state(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), GH_ACCOUNT_STATE_DISCOVERING);
  return self->state;
}

GhSignerAvailability
gh_account_controller_get_signer_availability(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), GH_SIGNER_AVAILABILITY_UNKNOWN);
  return self->availability;
}

const gchar *
gh_account_controller_get_active_npub(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), NULL);
  return self->active_npub;
}

GPtrArray *
gh_account_controller_get_identities(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), NULL);
  return self->identities;
}

guint64
gh_account_controller_get_generation(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), 0);
  return self->generation;
}

GCancellable *
gh_account_controller_get_cancellable(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), NULL);
  return self->generation_cancel;
}

gboolean
gh_account_controller_is_current(GhAccountController *self, guint64 generation)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  return self->generation_cancel && generation == self->generation;
}

typedef struct {
  GTask *task;
  guint64 generation;
  gboolean sign;
  GCancellable *cancel;
  GCancellable *caller;
  GCancellable *generation_cancel;
  gulong caller_handler;
  gulong generation_handler;
} SignerCall;

static void
cancel_signer_call(GCancellable *source, gpointer data)
{
  (void)source;
  g_cancellable_cancel(data);
}

static void
signer_call_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  SignerCall *call = user_data;
  GhAccountController *self = g_task_get_source_object(call->task);
  g_autoptr(GError) error = NULL;
  gchar *value = call->sign ? gh_signer_sign_finish(result, &error) :
                              gh_signer_nip44_finish(result, &error);
  (void)source;
  if (!gh_account_controller_is_current(self, call->generation) || !self->signer) {
    g_free(value);
    g_task_return_new_error(call->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                            "Account signer generation was revoked");
  } else if (g_cancellable_is_cancelled(call->cancel)) {
    g_free(value);
    g_task_return_new_error(call->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                            "Signer operation was cancelled");
  } else if (error) {
    g_task_return_error(call->task, g_steal_pointer(&error));
  } else {
    g_task_return_pointer(call->task, value, g_free);
  }
  if (call->caller_handler)
    g_cancellable_disconnect(call->caller, call->caller_handler);
  if (call->generation_handler)
    g_cancellable_disconnect(call->generation_cancel, call->generation_handler);
  g_clear_object(&call->caller);
  g_clear_object(&call->generation_cancel);
  g_clear_object(&call->cancel);
  g_object_unref(call->task);
  g_free(call);
}

static SignerCall *
new_signer_call(GhAccountController *self, gpointer tag, GCancellable *caller,
                GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(self, NULL, callback, user_data);
  g_task_set_source_tag(task, tag);
  if (!self->signer || !self->settings) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                            "No supported signer is bound to the active account");
    g_object_unref(task);
    return NULL;
  }
  SignerCall *call = g_new0(SignerCall, 1);
  call->task = task;
  call->generation = self->generation;
  call->cancel = g_cancellable_new();
  call->generation_cancel = g_object_ref(self->generation_cancel);
  call->generation_handler = g_cancellable_connect(call->generation_cancel,
    G_CALLBACK(cancel_signer_call), call->cancel, NULL);
  if (caller) {
    call->caller = g_object_ref(caller);
    call->caller_handler = g_cancellable_connect(caller,
      G_CALLBACK(cancel_signer_call), call->cancel, NULL);
  }
  return call;
}

void
gh_account_controller_sign_with_cancellable_async(GhAccountController *self,
                                 const gchar *unsigned_event, GCancellable *cancellable,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  SignerCall *call = new_signer_call(self, gh_account_controller_sign_async,
                                    cancellable, callback, user_data);
  if (!call) return;
  call->sign = TRUE;
  gh_signer_sign_async(self->signer, unsigned_event, call->cancel,
                       signer_call_done, call);
}

void
gh_account_controller_sign_async(GhAccountController *self, const gchar *unsigned_event,
                                 GAsyncReadyCallback callback, gpointer user_data)
{
  gh_account_controller_sign_with_cancellable_async(self, unsigned_event, NULL,
                                                    callback, user_data);
}

gchar *
gh_account_controller_sign_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  g_return_val_if_fail(g_task_get_source_tag(G_TASK(result)) ==
                       gh_account_controller_sign_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

void
gh_account_controller_nip44_encrypt_with_cancellable_async(GhAccountController *self,
                                          const gchar *plaintext, const gchar *peer,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  SignerCall *call = new_signer_call(self, gh_account_controller_nip44_encrypt_async,
                                    cancellable, callback, user_data);
  if (!call) return;
  gh_signer_nip44_encrypt_async(self->signer, plaintext, peer, call->cancel,
                                signer_call_done, call);
}

void
gh_account_controller_nip44_encrypt_async(GhAccountController *self,
                                          const gchar *plaintext, const gchar *peer,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  gh_account_controller_nip44_encrypt_with_cancellable_async(self, plaintext, peer,
                                                              NULL, callback, user_data);
}

void
gh_account_controller_nip44_decrypt_with_cancellable_async(GhAccountController *self,
                                          const gchar *ciphertext, const gchar *peer,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  SignerCall *call = new_signer_call(self, gh_account_controller_nip44_decrypt_async,
                                    cancellable, callback, user_data);
  if (!call) return;
  gh_signer_nip44_decrypt_async(self->signer, ciphertext, peer, call->cancel,
                                 signer_call_done, call);
}

void
gh_account_controller_nip44_decrypt_async(GhAccountController *self,
                                          const gchar *ciphertext, const gchar *peer,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  gh_account_controller_nip44_decrypt_with_cancellable_async(self, ciphertext, peer,
                                                              NULL, callback, user_data);
}

gchar *
gh_account_controller_nip44_finish(GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(G_IS_TASK(result), NULL);
  gpointer tag = g_task_get_source_tag(G_TASK(result));
  g_return_val_if_fail(tag == gh_account_controller_nip44_encrypt_async ||
                       tag == gh_account_controller_nip44_decrypt_async, NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

gchar *
gh_account_describe_limits(GhAccountState state, GhSignerAvailability availability,
                           const gchar *requested_method, gboolean network_available)
{
  if (!network_available)
    return g_strdup("Offline: no network connection. Nothing can be sent or received.");
  switch (state) {
  case GH_ACCOUNT_STATE_DISCOVERING:
    return g_strdup("Read-only: looking for accounts");
  case GH_ACCOUNT_STATE_STORE_UNAVAILABLE:
    return g_strdup("Read-only: the account store could not be read");
  case GH_ACCOUNT_STATE_NO_IDENTITIES:
  case GH_ACCOUNT_STATE_UNSELECTED:
    return g_strdup("Read-only: no Groundhog account is selected");
  case GH_ACCOUNT_STATE_SELECTED_MISSING:
    return g_strdup("Read-only: the selected account is no longer available");
  case GH_ACCOUNT_STATE_ACTIVE:
  default:
    break;
  }
  if (g_strcmp0(requested_method, "nip46") == 0)
    return g_strdup("Read-only: NIP-46 remote signing is not supported in this build");
  if (g_strcmp0(requested_method, "auto") != 0 &&
      g_strcmp0(requested_method, "local") != 0 &&
      g_strcmp0(requested_method, "nip55l") != 0)
    return g_strdup_printf("Read-only: unknown signer method \"%s\"",
                           requested_method ? requested_method : "");
  switch (availability) {
  case GH_SIGNER_AVAILABILITY_UNKNOWN:
    return g_strdup("Read-only: checking for the Nostr signer");
  case GH_SIGNER_AVAILABILITY_NO_BUS:
    return g_strdup("Read-only: no session bus, so the Nostr signer cannot be reached");
  case GH_SIGNER_AVAILABILITY_ABSENT:
    return g_strdup("Read-only: the Nostr signer service is not installed or running");
  case GH_SIGNER_AVAILABILITY_ACTIVATABLE:
  case GH_SIGNER_AVAILABILITY_RUNNING:
  default:
    return g_strdup("Signer available, but sending is not implemented in this build");
  }
}

static void
gh_account_controller_dispose(GObject *object)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(object);
  if (self->settings) {
    g_signal_handlers_disconnect_by_data(self->settings, self);
    g_clear_object(&self->settings);
    revoke_generation(self, FALSE);
    gh_signer_free(g_steal_pointer(&self->signer));
  }
  if (self->watch_id) {
    g_bus_unwatch_name(self->watch_id);
    self->watch_id = 0;
  }
  if (self->signer_cancel)
    g_cancellable_cancel(self->signer_cancel);
  if (self->list_cancel)
    g_cancellable_cancel(self->list_cancel);
  g_clear_object(&self->list_cancel);
  g_clear_object(&self->bus);
  G_OBJECT_CLASS(gh_account_controller_parent_class)->dispose(object);
}

static void
gh_account_controller_finalize(GObject *object)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(object);
  g_clear_object(&self->signer_cancel);
  g_clear_pointer(&self->identities, g_ptr_array_unref);
  g_free(self->active_npub);
  G_OBJECT_CLASS(gh_account_controller_parent_class)->finalize(object);
}

static void
gh_account_controller_class_init(GhAccountControllerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_account_controller_dispose;
  object_class->finalize = gh_account_controller_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
gh_account_controller_init(GhAccountController *self)
{
  self->state = GH_ACCOUNT_STATE_DISCOVERING;
  self->generation = 1;
  self->generation_cancel = g_cancellable_new();
  self->signer_cancel = g_cancellable_new();
}
