#include "gh-account-controller.h"
#include "gh-identity.h"
#include "gh-signer.h"
#include "gh-nip46-session.h"

#define SIGNER_BUS "org.nostr.Signer"
#define CREDENTIAL_WAIT_MS 1500
#define CREDENTIAL_TIMEOUT_MS 120000
/* A health ping after restore or reconnect: READY means the signer answered,
 * not merely that a relay sent EOSE (nostrc-8xfib.1). A ping with no answer
 * within this bound reports OFFLINE, which still lets requests queue. */
#define PING_TIMEOUT_MS 20000

typedef enum { LOCK_NONE, LOCK_WAITING, LOCK_TIMED_OUT, LOCK_LOCKED } LockReason;

struct _GhAccountController {
  GObject parent_instance;
  GSettings *settings; /* NULL once disposed */
  GDBusConnection *bus;
  GhAccountListFunc list;
  gpointer list_data;
  GhNip46CredentialStore *credentials;
  GhAccountListFunc remote_list; /* injectable metadata-only test seam */
  gpointer remote_list_data;
  GhAccountSessionFactory session_factory; /* fake transport test seam */
  gpointer session_factory_data;
  GPtrArray *grotto_results;
  GPtrArray *remote_results;
  gboolean source_ok[2];
  gboolean list_pending[2];
  gboolean remote_fresh; /* a remote listing arrived since the last merge */
  guint reconcile_id;
  gboolean pair_reconciliation_pending;
  gboolean own_pair_write;
  guint mode_rebind_id;
  guint64 mode_rebind_generation;

  GPtrArray *identities; /* NULL until listed or after a failed listing */
  gboolean listed;
  guint list_serial;
  GCancellable *list_cancel;

  GhAccountState state;
  gchar *active_npub;
  GhSignerBackend active_backend;
  GhRemoteSignerState remote_state;
  gboolean remote_storage_ready;
  guint64 remote_gate_epoch;
  GCancellable *remote_gate_cancel;
  GhNip46Session *remote_session;
  guint64 generation;
  GCancellable *generation_cancel;
  GhSigner *signer; /* active generation only; never exposes secret material */

  /* The in-flight credential read (lookup) for the active generation. */
  GCancellable *lookup_cancel;
  guint lookup_wait_id;
  guint lookup_timeout_id;
  guint credential_wait_ms;
  guint credential_timeout_ms;
  LockReason lock_reason;
  gboolean lookup_interactive;   /* the next lookup may prompt (Unlock) */
  guint credential_lookups;      /* test counter */

  /* A remote account adopted from a live pairing session, kept listed until
   * a keyring listing includes it (a listing started before the save). */
  gchar *adopted_npub;
  GCancellable *ping_cancel;
  guint ping_timeout_id;
  guint ping_timeout_ms;
  gboolean ping_factory_sessions;
  gboolean factory_session;

  GhSignerAvailability availability;
  guint watch_id;
  guint signer_serial;
  GCancellable *signer_cancel;
};

enum { SIGNAL_CHANGED, SIGNAL_AUTH_URL, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAccountController, gh_account_controller, G_TYPE_OBJECT)

static void stop_lookup(GhAccountController *self, gboolean cancel);
static void stop_ping(GhAccountController *self);

static GPtrArray *
default_list(gpointer user_data, GError **error)
{
  (void)user_data;
  return gh_identity_list(error);
}

static GhSignerBackend
backend_from_settings(GSettings *settings)
{
  g_autofree gchar *name = g_settings_get_string(settings, "current-backend");
  return g_strcmp0(name, "nip46") == 0 ? GH_SIGNER_BACKEND_NIP46 :
                                         GH_SIGNER_BACKEND_GROTTO;
}

static const gchar *
backend_name(GhSignerBackend backend)
{
  return backend == GH_SIGNER_BACKEND_NIP46 ? "nip46" : "grotto";
}

static gboolean
identities_contain(GPtrArray *identities, GhSignerBackend backend, const gchar *npub)
{
  for (guint i = 0; identities && i < identities->len; i++) {
    const GhIdentityInfo *info = g_ptr_array_index(identities, i);
    if (info->backend == backend && g_strcmp0(info->npub, npub) == 0)
      return TRUE;
  }
  return FALSE;
}

static gint
compare_identity(gconstpointer a, gconstpointer b)
{
  const GhIdentityInfo *ia = *(GhIdentityInfo *const *)a;
  const GhIdentityInfo *ib = *(GhIdentityInfo *const *)b;
  gint cmp = g_strcmp0(ia->npub, ib->npub);
  return cmp ? cmp : (gint)ia->backend - (gint)ib->backend;
}

/* Revoke first, then cancel: work woken by the cancellation already sees a
 * stale generation. */
static void
revoke_generation(GhAccountController *self, gboolean replace)
{
  GCancellable *old = g_steal_pointer(&self->generation_cancel);
  self->generation++;
  self->remote_storage_ready = FALSE;
  self->remote_gate_epoch++;
  if (self->remote_gate_cancel) g_cancellable_cancel(self->remote_gate_cancel);
  g_clear_object(&self->remote_gate_cancel);
  stop_lookup(self, TRUE);
  stop_ping(self);
  if (replace)
    self->generation_cancel = g_cancellable_new();
  if (old) {
    g_cancellable_cancel(old);
    g_object_unref(old);
  }
}

static void
set_remote_state(GhAccountController *self, GhRemoteSignerState state)
{
  if (self->remote_state == state) return;
  self->remote_state = state;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static void
stop_ping(GhAccountController *self)
{
  g_clear_handle_id(&self->ping_timeout_id, g_source_remove);
  if (self->ping_cancel) g_cancellable_cancel(self->ping_cancel);
  g_clear_object(&self->ping_cancel);
}

typedef struct {
  GhAccountController *self;
  GhNip46Session *session;
  GCancellable *cancel;
} PingCall;

static void
ping_done(GObject *source, GAsyncResult *result, gpointer data)
{
  PingCall *call = data;
  GhAccountController *self = call->self;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *pong = gh_nip46_session_call_finish(GH_NIP46_SESSION(source), result,
                                                        &error);
  if (self->settings && call->cancel == self->ping_cancel &&
      call->session == self->remote_session) {
    g_clear_handle_id(&self->ping_timeout_id, g_source_remove);
    g_clear_object(&self->ping_cancel);
    /* Any answer from the signer (even "unsupported") proves it is there. */
    gboolean answered = !error ||
      (!g_error_matches(error, GH_NIP46_SESSION_ERROR, GH_NIP46_SESSION_ERROR_TIMED_OUT) &&
       !g_error_matches(error, GH_NIP46_SESSION_ERROR, GH_NIP46_SESSION_ERROR_UNAVAILABLE) &&
       !g_error_matches(error, GH_NIP46_SESSION_ERROR, GH_NIP46_SESSION_ERROR_CANCELLED));
    g_debug("Groundhog remote signer ping: %s", answered ? "answered" :
            error ? error->message : "?");
    set_remote_state(self, answered ? GH_REMOTE_SIGNER_READY : GH_REMOTE_SIGNER_OFFLINE);
  }
  g_object_unref(call->session);
  g_object_unref(call->cancel);
  g_object_unref(self);
  g_free(call);
}

static gboolean
ping_timed_out(gpointer data)
{
  GhAccountController *self = data;
  self->ping_timeout_id = 0;
  if (self->ping_cancel) g_cancellable_cancel(self->ping_cancel);
  g_clear_object(&self->ping_cancel);
  if (self->settings && self->remote_session)
    set_remote_state(self, GH_REMOTE_SIGNER_OFFLINE);
  return G_SOURCE_REMOVE;
}

/* After restore or reconnect: ping the signer and report READY on its answer. */
static void
start_ping(GhAccountController *self)
{
  if (!self->remote_session || self->ping_cancel) return;
  if (self->factory_session && !self->ping_factory_sessions) {
    set_remote_state(self, GH_REMOTE_SIGNER_READY);
    return;
  }
  PingCall *call = g_new0(PingCall, 1);
  call->self = g_object_ref(self);
  call->session = g_object_ref(self->remote_session);
  call->cancel = g_cancellable_new();
  self->ping_cancel = g_object_ref(call->cancel);
  self->ping_timeout_id = g_timeout_add(self->ping_timeout_ms, ping_timed_out, self);
  gh_nip46_session_call_async(self->remote_session, "ping", NULL, 0, call->cancel,
                              ping_done, call);
}

static void
remote_ready(GhNip46Session *session, GhAccountController *self)
{
  if (session == self->remote_session && self->settings &&
      self->remote_state != GH_REMOTE_SIGNER_READY)
    start_ping(self);
}

/* Listening without an EOSE (a relay that never sends one): ping anyway. */
static void
remote_listening(GhNip46Session *session, GhAccountController *self)
{
  if (session == self->remote_session && self->settings &&
      self->remote_state == GH_REMOTE_SIGNER_CONNECTING)
    start_ping(self);
}

static void
remote_offline(GhNip46Session *session, GhAccountController *self)
{
  if (session == self->remote_session && self->settings)
    set_remote_state(self, GH_REMOTE_SIGNER_OFFLINE);
}

static gboolean
remote_auth_url(GhNip46Session *session, const gchar *url, gpointer data)
{
  GhAccountController *self = data;
  const guint64 *bound_generation = g_object_get_data(G_OBJECT(session),
                                                       "gh-account-generation");
  if (!self->settings || session != self->remote_session || !bound_generation ||
      *bound_generation != self->generation ||
      self->active_backend != GH_SIGNER_BACKEND_NIP46 ||
      (self->generation_cancel && g_cancellable_is_cancelled(self->generation_cancel)))
    return FALSE;
  gboolean handled = FALSE;
  g_signal_emit(self, signals[SIGNAL_AUTH_URL], 0, url, &handled);
  return handled;
}

static void
activate_remote_session(GhAccountController *self, GhNip46Session *session)
{
  self->remote_session = session; /* takes ownership */
  if (!session) {
    set_remote_state(self, GH_REMOTE_SIGNER_ERROR);
    return;
  }
  self->signer = gh_signer_new_nip46(session, self->active_npub, NULL);
  if (!self->signer) {
    gh_nip46_session_cancel(session);
    g_clear_object(&self->remote_session);
    set_remote_state(self, GH_REMOTE_SIGNER_ERROR);
    return;
  }
  guint64 *bound_generation = g_new(guint64, 1);
  *bound_generation = self->generation;
  g_object_set_data_full(G_OBJECT(session), "gh-account-generation",
                         bound_generation, g_free);
  gh_nip46_session_set_auth_url_handler(session, remote_auth_url, self);
  g_signal_connect_object(session, "ready", G_CALLBACK(remote_ready), self, 0);
  g_signal_connect_object(session, "listening", G_CALLBACK(remote_listening), self, 0);
  g_signal_connect_object(session, "offline", G_CALLBACK(remote_offline), self, 0);
  set_remote_state(self, GH_REMOTE_SIGNER_CONNECTING);
  gh_nip46_session_start(session);
  if (gh_nip46_session_is_listening(session)) start_ping(self);
}

typedef struct {
  GhAccountController *self;
  guint64 generation;
  GCancellable *cancel;
} CredentialQuery;

static void
stop_lookup(GhAccountController *self, gboolean cancel)
{
  g_clear_handle_id(&self->lookup_wait_id, g_source_remove);
  g_clear_handle_id(&self->lookup_timeout_id, g_source_remove);
  if (cancel && self->lookup_cancel) g_cancellable_cancel(self->lookup_cancel);
  g_clear_object(&self->lookup_cancel);
}

static gboolean
lookup_waiting(gpointer data)
{
  GhAccountController *self = data;
  self->lookup_wait_id = 0;
  if (!self->settings || self->remote_state != GH_REMOTE_SIGNER_LOADING_CREDENTIAL)
    return G_SOURCE_REMOVE;
  g_message("Groundhog is waiting for access to the remote signer credential");
  self->lock_reason = LOCK_WAITING;
  self->remote_state = GH_REMOTE_SIGNER_LOCKED;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return G_SOURCE_REMOVE;
}

static gboolean
lookup_timed_out(gpointer data)
{
  GhAccountController *self = data;
  self->lookup_timeout_id = 0;
  g_message("Groundhog gave up waiting for the remote signer credential");
  g_clear_handle_id(&self->lookup_wait_id, g_source_remove);
  if (self->lookup_cancel) g_cancellable_cancel(self->lookup_cancel);
  g_clear_object(&self->lookup_cancel);
  if (self->settings) {
    self->lock_reason = LOCK_TIMED_OUT;
    self->remote_state = GH_REMOTE_SIGNER_LOCKED;
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
  return G_SOURCE_REMOVE;
}

static void
credential_lookup_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  CredentialQuery *query = user_data;
  GhAccountController *self = query->self;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip46Credential) credential =
    gh_nip46_credential_store_lookup_finish(GH_NIP46_CREDENTIAL_STORE(source), result,
                                             &error);
  gboolean current = query->cancel == self->lookup_cancel;
  if (current) stop_lookup(self, FALSE);
  if (current && self->settings && query->generation == self->generation &&
      self->active_backend == GH_SIGNER_BACKEND_NIP46 && self->active_npub) {
    if (!credential) {
      gboolean locked = g_error_matches(error, GH_NIP46_CREDENTIAL_ERROR,
                                        GH_NIP46_CREDENTIAL_ERROR_LOCKED);
      self->lock_reason = locked ? LOCK_LOCKED : LOCK_NONE;
      if (locked && self->remote_state == GH_REMOTE_SIGNER_LOCKED)
        g_signal_emit(self, signals[SIGNAL_CHANGED], 0); /* reason changed */
      set_remote_state(self, locked ? GH_REMOTE_SIGNER_LOCKED : GH_REMOTE_SIGNER_ERROR);
    } else {
      self->lock_reason = LOCK_NONE;
      activate_remote_session(self, gh_nip46_session_new(
        gh_nip46_credential_get_client_secret_hex(credential),
        gh_nip46_credential_get_remote_signer_pubkey_hex(credential),
        gh_nip46_credential_get_relays(credential), NULL, NULL, NULL, NULL, NULL, &error));
    }
  }
  g_object_unref(query->cancel);
  g_object_unref(self);
  g_free(query);
}

static void
bind_signer(GhAccountController *self)
{
  if (self->remote_session) {
    gh_nip46_session_cancel(self->remote_session);
    g_clear_object(&self->remote_session);
  }
  gh_signer_free(g_steal_pointer(&self->signer));
  stop_lookup(self, TRUE);
  stop_ping(self);
  self->factory_session = FALSE;
  self->lock_reason = LOCK_NONE;
  gboolean interactive = self->lookup_interactive;
  self->lookup_interactive = FALSE;
  if (!self->active_npub) return;
  if (self->active_backend == GH_SIGNER_BACKEND_GROTTO) {
    if (self->bus)
      self->signer = gh_signer_new(self->bus, self->active_npub, NULL);
    return;
  }
  self->remote_state = GH_REMOTE_SIGNER_LOADING_CREDENTIAL;
  if (self->session_factory) {
    self->factory_session = TRUE;
    activate_remote_session(self, self->session_factory(self->active_npub,
                                                        self->session_factory_data));
    return;
  }
  if (!self->credentials) {
    self->remote_state = GH_REMOTE_SIGNER_ERROR;
    return;
  }
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(self->active_npub);
  if (!pubkey) {
    self->remote_state = GH_REMOTE_SIGNER_ERROR;
    return;
  }
  /* The read may wait on a Keychain/keyring prompt: it has its own
   * cancellable (also cancelled with the generation) and a deadline, and it
   * never holds up listing or selection. */
  self->lookup_cancel = g_cancellable_new();
  CredentialQuery *query = g_new0(CredentialQuery, 1);
  query->self = g_object_ref(self);
  query->generation = self->generation;
  query->cancel = g_object_ref(self->lookup_cancel);
  self->lookup_wait_id = g_timeout_add(self->credential_wait_ms, lookup_waiting, self);
  self->lookup_timeout_id = g_timeout_add(self->credential_timeout_ms, lookup_timed_out, self);
  self->credential_lookups++;
  gh_nip46_credential_store_lookup_full_async(self->credentials, pubkey, interactive,
                                               self->lookup_cancel,
                                               credential_lookup_done, query);
}

/* Returns TRUE when it emitted "changed". */
static gboolean
update_state(GhAccountController *self)
{
  if (!self->settings || (self->pair_reconciliation_pending && self->reconcile_id))
    return FALSE;
  g_autofree gchar *current = g_settings_get_string(self->settings, "current-npub");
  GhSignerBackend backend = backend_from_settings(self->settings);
  GhAccountState state;
  if (!self->listed)
    state = GH_ACCOUNT_STATE_DISCOVERING;
  else if (*current && !self->source_ok[backend])
    state = GH_ACCOUNT_STATE_STORE_UNAVAILABLE;
  else if (*current && !identities_contain(self->identities, backend, current))
    state = GH_ACCOUNT_STATE_SELECTED_MISSING;
  else if (!self->identities)
    state = GH_ACCOUNT_STATE_STORE_UNAVAILABLE;
  else if (self->identities->len == 0)
    state = GH_ACCOUNT_STATE_NO_IDENTITIES;
  else if (!*current)
    state = GH_ACCOUNT_STATE_UNSELECTED;
  else
    state = GH_ACCOUNT_STATE_ACTIVE;

  const gchar *active = state == GH_ACCOUNT_STATE_ACTIVE ? current : NULL;
  gboolean account_changed = self->pair_reconciliation_pending ||
    g_strcmp0(active, self->active_npub) != 0 ||
    (active && backend != self->active_backend);
  if (account_changed) {
    g_free(self->active_npub);
    self->active_npub = g_strdup(active);
    self->active_backend = backend;
    revoke_generation(self, TRUE);
    self->pair_reconciliation_pending = FALSE;
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
append_source(GPtrArray *merged, GPtrArray *source, GhSignerBackend backend)
{
  for (guint i = 0; source && i < source->len; i++) {
    const GhIdentityInfo *item = g_ptr_array_index(source, i);
    if (identities_contain(merged, backend, item->npub)) continue;
    GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
    info->npub = g_strdup(item->npub);
    info->label = g_strdup(item->label);
    info->backend = backend;
    g_ptr_array_add(merged, info);
  }
}

static GhIdentityInfo *
remote_identity(const gchar *npub)
{
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub);
  info->label = g_strdup("Remote signer");
  info->backend = GH_SIGNER_BACKEND_NIP46;
  return info;
}

/* Merges the latest result of each source. A remote (NIP-46) account never
 * waits on the Grotto listing (a D-Bus list that hangs must not hold it up,
 * nostrc-8xfib.1): while the current backend is NIP-46, a successful keyring
 * result alone settles the listing, merged with the last known Grotto
 * result; the Grotto result is merged in when it arrives. */
static void
finish_listing(GhAccountController *self)
{
  gboolean remote_first = self->settings &&
    backend_from_settings(self->settings) == GH_SIGNER_BACKEND_NIP46;
  /* A failed keyring listing still waits for Grotto, as before. */
  if (self->list_pending[1] ||
      (self->list_pending[0] && !(remote_first && self->source_ok[1]))) return;
  g_clear_pointer(&self->identities, g_ptr_array_unref);
  if (self->source_ok[0] || self->source_ok[1] || self->adopted_npub) {
    self->identities = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
    append_source(self->identities, self->grotto_results, GH_SIGNER_BACKEND_GROTTO);
    append_source(self->identities, self->remote_results, GH_SIGNER_BACKEND_NIP46);
    if (self->adopted_npub) {
      if (self->source_ok[1] &&
          identities_contain(self->identities, GH_SIGNER_BACKEND_NIP46, self->adopted_npub))
        g_clear_pointer(&self->adopted_npub, g_free); /* the keyring lists it now */
      else
        g_ptr_array_add(self->identities, remote_identity(self->adopted_npub));
    }
    g_ptr_array_sort(self->identities, compare_identity);
  }
  gboolean was_listed = self->listed;
  gboolean remote_fresh = self->remote_fresh;
  self->remote_fresh = FALSE;
  guint64 before = self->generation;
  self->listed = TRUE;
  gboolean emitted = update_state(self);
  /* A refresh rebinds an unchanged remote account only when its signer is
   * LOCKED or in ERROR (a keyring that was unlocked meanwhile): a live,
   * working session is never torn down by an unrelated refresh. */
  if (was_listed && remote_fresh && self->generation == before && self->active_npub &&
      !self->pair_reconciliation_pending && !self->mode_rebind_id &&
      !g_cancellable_is_cancelled(self->generation_cancel) &&
      self->active_backend == GH_SIGNER_BACKEND_NIP46 && self->source_ok[1] &&
      (self->remote_state == GH_REMOTE_SIGNER_LOCKED ||
       self->remote_state == GH_REMOTE_SIGNER_ERROR) && !self->lookup_cancel &&
      identities_contain(self->identities, GH_SIGNER_BACKEND_NIP46, self->active_npub)) {
    revoke_generation(self, TRUE);
    bind_signer(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  } else if (!emitted) {
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
}

static void
remote_list_thread(GTask *task, gpointer source, gpointer task_data,
                   GCancellable *cancellable)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(source);
  (void)task_data; (void)cancellable;
  GError *error = NULL;
  GPtrArray *items = self->remote_list(self->remote_list_data, &error);
  if (items) g_task_return_pointer(task, items, (GDestroyNotify)g_ptr_array_unref);
  else if (error) g_task_return_error(task, error);
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED,
                               "Remote identity listing failed");
}

static void
remote_fake_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(source);
  guint serial = GPOINTER_TO_UINT(g_task_get_task_data(G_TASK(result)));
  g_autoptr(GError) error = NULL;
  GPtrArray *items = g_task_propagate_pointer(G_TASK(result), &error);
  (void)user_data;
  if (!self->settings || serial != self->list_serial) {
    g_clear_pointer(&items, g_ptr_array_unref);
    return;
  }
  g_clear_pointer(&self->remote_results, g_ptr_array_unref);
  self->remote_results = items;
  self->source_ok[1] = items != NULL;
  self->list_pending[1] = FALSE;
  self->remote_fresh = TRUE;
  finish_listing(self);
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
  if (error) g_message("Groundhog could not list Grotto identities: %s", error->message);
  g_clear_pointer(&self->grotto_results, g_ptr_array_unref);
  self->grotto_results = identities;
  self->source_ok[0] = identities != NULL;
  self->list_pending[0] = FALSE;
  finish_listing(self);
}

typedef struct { GhAccountController *self; guint serial; } RemoteListQuery;

static void
remote_list_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  RemoteListQuery *query = user_data;
  GhAccountController *self = query->self;
  g_autoptr(GError) error = NULL;
  GPtrArray *identities = gh_nip46_credential_store_list_finish(
    GH_NIP46_CREDENTIAL_STORE(source), result, &error);
  if (self->settings && query->serial == self->list_serial) {
    if (error) g_message("Groundhog could not list remote identities: %s", error->message);
    else g_debug("Groundhog listed %u remote identities", identities->len);
    g_clear_pointer(&self->remote_results, g_ptr_array_unref);
    self->remote_results = identities;
    self->source_ok[1] = identities != NULL;
    self->list_pending[1] = FALSE;
    self->remote_fresh = TRUE;
    finish_listing(self);
  } else {
    g_clear_pointer(&identities, g_ptr_array_unref);
  }
  g_object_unref(self);
  g_free(query);
}

void
gh_account_controller_refresh(GhAccountController *self)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  if (!self->settings) return;
  if (self->list_cancel) g_cancellable_cancel(self->list_cancel);
  g_clear_object(&self->list_cancel);
  self->list_cancel = g_cancellable_new();
  self->list_pending[0] = TRUE;
  self->list_pending[1] = self->credentials != NULL || self->remote_list != NULL;
  guint serial = ++self->list_serial;
  if (self->remote_list) {
    GTask *remote = g_task_new(self, self->list_cancel, remote_fake_done, NULL);
    g_task_set_task_data(remote, GUINT_TO_POINTER(serial), NULL);
    g_task_run_in_thread(remote, remote_list_thread);
    g_object_unref(remote);
  } else if (!self->credentials) {
    self->source_ok[1] = FALSE;
    g_clear_pointer(&self->remote_results, g_ptr_array_unref);
  } else {
    RemoteListQuery *query = g_new0(RemoteListQuery, 1);
    query->self = g_object_ref(self);
    query->serial = serial;
    gh_nip46_credential_store_list_async(self->credentials, self->list_cancel,
                                          remote_list_done, query);
  }
  GTask *task = g_task_new(self, self->list_cancel, list_done, NULL);
  g_task_set_source_tag(task, gh_account_controller_refresh);
  g_task_set_task_data(task, GUINT_TO_POINTER(serial), NULL);
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

static gboolean
reconcile_pair_idle(gpointer data)
{
  GhAccountController *self = data;
  self->reconcile_id = 0;
  (void)update_state(self);
  return G_SOURCE_REMOVE;
}

static gboolean
rebind_remote_after_mode_idle(gpointer data)
{
  GhAccountController *self = data;
  self->mode_rebind_id = 0;
  (void)update_state(self);
  if (self->settings && !self->pair_reconciliation_pending &&
      self->generation == self->mode_rebind_generation &&
      self->active_npub && self->active_backend == GH_SIGNER_BACKEND_NIP46) {
    revoke_generation(self, TRUE);
    bind_signer(self);
    g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  }
  return G_SOURCE_REMOVE;
}

static void
on_settings_changed(GSettings *settings, const gchar *key, gpointer user_data)
{
  GhAccountController *self = user_data;
  (void)settings;
  if (g_strcmp0(key, "current-npub") == 0 ||
      g_strcmp0(key, "current-backend") == 0) {
    if (self->own_pair_write) return;
    g_autofree gchar *npub = g_settings_get_string(self->settings, "current-npub");
    GhSignerBackend backend = backend_from_settings(self->settings);
    if (!self->pair_reconciliation_pending &&
        ((!*npub && !self->active_npub) ||
         (self->state == GH_ACCOUNT_STATE_ACTIVE &&
          g_strcmp0(npub, self->active_npub) == 0 &&
          backend == self->active_backend)))
      return;
    if (!self->pair_reconciliation_pending) {
      self->pair_reconciliation_pending = TRUE;
      if (self->generation_cancel) g_cancellable_cancel(self->generation_cancel);
      if (self->remote_session) {
        gh_nip46_session_cancel(self->remote_session);
        g_clear_object(&self->remote_session);
      }
      gh_signer_free(g_steal_pointer(&self->signer));
    }
    if (!self->reconcile_id)
      self->reconcile_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
        reconcile_pair_idle, g_object_ref(self), g_object_unref);
  } else if (g_strcmp0(key, "network-mode") == 0 &&
             self->active_npub && self->active_backend == GH_SIGNER_BACKEND_NIP46) {
    if (!self->mode_rebind_id) {
      self->mode_rebind_generation = self->generation;
      if (self->generation_cancel) g_cancellable_cancel(self->generation_cancel);
      if (self->remote_session) gh_nip46_session_cancel(self->remote_session);
      gh_signer_free(g_steal_pointer(&self->signer));
      set_remote_state(self, GH_REMOTE_SIGNER_CONNECTING);
      self->mode_rebind_id = g_idle_add_full(G_PRIORITY_DEFAULT_IDLE,
        rebind_remote_after_mode_idle, g_object_ref(self), g_object_unref);
    }
  }
}

/* Delay/apply is permanent for a GSettings object. Use a separate writer so
 * the app-wide reader stays in immediate-write mode for preferences. */
static GSettings *
transaction_writer(GSettings *settings)
{
  GSettingsSchema *schema = NULL;
  GSettingsBackend *backend = NULL;
  gchar *path = NULL;
  g_object_get(settings, "settings-schema", &schema, "backend", &backend,
               "path", &path, NULL);
  GSettings *writer = g_settings_new_full(schema, backend, path);
  g_settings_schema_unref(schema);
  g_clear_object(&backend);
  g_free(path);
  return writer;
}

static void
migrate_backend_settings(GSettings *settings)
{
  if (g_settings_get_int(settings, "backend-migration-version") >= 1) return;
  g_autofree gchar *npub = g_settings_get_string(settings, "current-npub");
  g_autoptr(GSettings) writer = transaction_writer(settings);
  g_settings_delay(writer);
  if (*npub) g_settings_set_string(writer, "current-backend", "grotto");
  g_settings_set_string(writer, "signer-method", "auto");
  g_settings_set_int(writer, "backend-migration-version", 1);
  g_settings_apply(writer);
}

static GhAccountController *
new_with_sources(GSettings *settings, GDBusConnection *bus, GhAccountListFunc list,
                 gpointer list_data, GhNip46CredentialStore *credentials,
                 GhAccountListFunc remote_list, gpointer remote_list_data)
{
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  g_return_val_if_fail(!bus || G_IS_DBUS_CONNECTION(bus), NULL);
  g_return_val_if_fail(list != NULL, NULL);
  g_return_val_if_fail(!credentials || GH_IS_NIP46_CREDENTIAL_STORE(credentials), NULL);
  migrate_backend_settings(settings); /* before signals and first discovery */
  GhAccountController *self = g_object_new(GH_TYPE_ACCOUNT_CONTROLLER, NULL);
  self->settings = g_object_ref(settings);
  self->list = list;
  self->list_data = list_data;
  self->credentials = credentials ? g_object_ref(credentials) : NULL;
  self->remote_list = remote_list;
  self->remote_list_data = remote_list_data;
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
gh_account_controller_new_full_with_credentials(GSettings *settings,
                               GDBusConnection *bus, GhAccountListFunc list,
                               gpointer list_data, GhNip46CredentialStore *credentials)
{
  return new_with_sources(settings, bus, list, list_data, credentials, NULL, NULL);
}

GhAccountController *
gh_account_controller_new_full_with_remote_list(GSettings *settings,
                               GDBusConnection *bus, GhAccountListFunc grotto_list,
                               gpointer grotto_data, GhAccountListFunc remote_list,
                               gpointer remote_data)
{
  g_return_val_if_fail(remote_list != NULL, NULL);
  return new_with_sources(settings, bus, grotto_list, grotto_data, NULL,
                          remote_list, remote_data);
}

void
gh_account_controller_set_session_factory_for_test(GhAccountController *self,
                                                   GhAccountSessionFactory factory,
                                                   gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  g_return_if_fail(self->remote_list != NULL && self->credentials == NULL);
  g_return_if_fail(self->active_npub == NULL);
  self->session_factory = factory;
  self->session_factory_data = user_data;
}

GhAccountController *
gh_account_controller_new_full(GSettings *settings, GDBusConnection *bus,
                               GhAccountListFunc list, gpointer list_data)
{
  return gh_account_controller_new_full_with_credentials(settings, bus, list,
                                                           list_data, NULL);
}

GhNip46CredentialStore *
gh_account_controller_get_credentials(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), NULL);
  return self->credentials;
}

GhAccountController *
gh_account_controller_new_with_credentials(GSettings *settings,
                                    GDBusConnection *bus,
                                    GhNip46CredentialStore *credentials)
{
  return gh_account_controller_new_full_with_credentials(settings, bus,
    default_list, NULL, credentials);
}

GhAccountController *
gh_account_controller_new(GSettings *settings, GDBusConnection *bus)
{
  return gh_account_controller_new_full(settings, bus, default_list, NULL);
}

gboolean
gh_account_controller_select_backend(GhAccountController *self,
                                     GhSignerBackend backend, const gchar *npub,
                                     GError **error)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  if (!self->settings) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                        "Groundhog account controller is shut down");
    return FALSE;
  }
  if (backend != GH_SIGNER_BACKEND_GROTTO && backend != GH_SIGNER_BACKEND_NIP46) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Unknown signer backend");
    return FALSE;
  }
  g_autofree gchar *pubkey = npub && *npub ? gh_identity_pubkey_hex(npub) : NULL;
  if (npub && *npub && !pubkey) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Selected identity is invalid");
    return FALSE;
  }
  if (npub && *npub && !identities_contain(self->identities, backend, npub)) {
    g_set_error_literal(error, G_IO_ERROR,
                        self->identities ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_NOT_INITIALIZED,
                        self->identities ? "Selected account and signer are not listed" :
                                           "Signer identities are not available");
    return FALSE;
  }
  g_autoptr(GSettings) writer = transaction_writer(self->settings);
  g_settings_delay(writer);
  gboolean saved = TRUE;
  if (npub && *npub)
    saved = g_settings_set_string(writer, "current-backend", backend_name(backend));
  if (saved)
    saved = g_settings_set_string(writer, "current-npub", npub ? npub : "");
  if (!saved) {
    g_settings_revert(writer);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Groundhog could not save the selected account");
    return FALSE;
  }
  self->own_pair_write = TRUE;
  g_settings_apply(writer);
  self->own_pair_write = FALSE;
  g_clear_handle_id(&self->reconcile_id, g_source_remove);
  (void)update_state(self); /* one pair, one generation */
  return TRUE;
}

gboolean
gh_account_controller_adopt_remote(GhAccountController *self, const gchar *npub,
                                   GhNip46Session *session, GError **error)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  g_return_val_if_fail(GH_IS_NIP46_SESSION(session), FALSE);
  if (!self->settings) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED,
                        "Groundhog account controller is shut down");
    return FALSE;
  }
  g_autofree gchar *pubkey = npub ? gh_identity_pubkey_hex(npub) : NULL;
  if (!pubkey || !gh_nip46_session_get_remote_pubkey(session)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "The paired remote signer is not usable");
    return FALSE;
  }
  g_autoptr(GSettings) writer = transaction_writer(self->settings);
  g_settings_delay(writer);
  if (!g_settings_set_string(writer, "current-backend", backend_name(GH_SIGNER_BACKEND_NIP46)) ||
      !g_settings_set_string(writer, "current-npub", npub)) {
    g_settings_revert(writer);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Groundhog could not save the selected account");
    return FALSE;
  }
  self->own_pair_write = TRUE;
  g_settings_apply(writer);
  self->own_pair_write = FALSE;
  g_clear_handle_id(&self->reconcile_id, g_source_remove);
  self->pair_reconciliation_pending = FALSE;

  /* Listed at once, whatever the keyring listing says yet. */
  g_free(self->adopted_npub);
  self->adopted_npub = g_strdup(npub);
  if (!self->identities)
    self->identities = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  if (!identities_contain(self->identities, GH_SIGNER_BACKEND_NIP46, npub)) {
    g_ptr_array_add(self->identities, remote_identity(npub));
    g_ptr_array_sort(self->identities, compare_identity);
  }
  self->source_ok[1] = TRUE;
  self->listed = TRUE;

  /* One new generation bound to the live session: no keyring lookup, no
   * reconnect (gnostr hands its session to the signer service the same way). */
  if (self->remote_session) {
    gh_nip46_session_cancel(self->remote_session);
    g_clear_object(&self->remote_session);
  }
  gh_signer_free(g_steal_pointer(&self->signer));
  g_free(self->active_npub);
  self->active_npub = g_strdup(npub);
  self->active_backend = GH_SIGNER_BACKEND_NIP46;
  revoke_generation(self, TRUE);
  self->factory_session = FALSE;
  self->lock_reason = LOCK_NONE;
  self->lookup_interactive = FALSE;
  self->state = GH_ACCOUNT_STATE_ACTIVE;
  self->remote_state = GH_REMOTE_SIGNER_CONNECTING;
  activate_remote_session(self, g_object_ref(session));
  stop_ping(self); /* the signer has just answered get_public_key */
  if (self->remote_session) self->remote_state = GH_REMOTE_SIGNER_READY;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

gboolean
gh_account_controller_unlock_remote(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  if (!self->settings || !self->active_npub || !self->credentials ||
      self->active_backend != GH_SIGNER_BACKEND_NIP46 ||
      (self->remote_state != GH_REMOTE_SIGNER_LOCKED &&
       self->remote_state != GH_REMOTE_SIGNER_ERROR))
    return FALSE;
  revoke_generation(self, TRUE);
  self->lookup_interactive = TRUE;
  bind_signer(self);
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
  return TRUE;
}

guint
gh_account_controller_get_credential_lookups_for_test(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), 0);
  return self->credential_lookups;
}

void
gh_account_controller_set_ping_for_test(GhAccountController *self,
                                        gboolean ping_factory_sessions, guint timeout_ms)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  self->ping_factory_sessions = ping_factory_sessions;
  if (timeout_ms) self->ping_timeout_ms = timeout_ms;
}

gboolean
gh_account_controller_select(GhAccountController *self, const gchar *npub,
                             GError **error)
{
  return gh_account_controller_select_backend(self, GH_SIGNER_BACKEND_GROTTO,
                                               npub, error);
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

GhSignerBackend
gh_account_controller_get_active_backend(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), GH_SIGNER_BACKEND_GROTTO);
  return self->active_backend;
}

GhRemoteSignerState
gh_account_controller_get_remote_state(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), GH_REMOTE_SIGNER_ERROR);
  return self->remote_state;
}

gboolean
gh_account_controller_is_remote_storage_ready(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  return self->active_backend == GH_SIGNER_BACKEND_NIP46 && self->remote_storage_ready;
}

void
gh_account_controller_set_remote_storage_ready(GhAccountController *self,
                                                guint64 generation, gboolean ready)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  if (!self->settings || !self->active_npub ||
      self->active_backend != GH_SIGNER_BACKEND_NIP46 ||
      generation != self->generation || self->remote_storage_ready == ready)
    return;
  self->remote_storage_ready = ready;
  if (ready) {
    self->remote_gate_cancel = g_cancellable_new();
  } else {
    self->remote_gate_epoch++;
    if (self->remote_gate_cancel) g_cancellable_cancel(self->remote_gate_cancel);
    g_clear_object(&self->remote_gate_cancel);
  }
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

void
gh_account_controller_set_credential_timeouts_for_test(GhAccountController *self,
                                                       guint wait_ms, guint timeout_ms)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  self->credential_wait_ms = wait_ms;
  self->credential_timeout_ms = MAX(timeout_ms, wait_ms);
}

const gchar *
gh_account_controller_describe_remote_lock(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), NULL);
  if (self->remote_state != GH_REMOTE_SIGNER_LOCKED) return NULL;
#ifdef __APPLE__
  switch (self->lock_reason) {
  case LOCK_WAITING:
    return "Waiting for Keychain access — allow Groundhog in the macOS prompt";
  case LOCK_TIMED_OUT:
    return "Keychain access was not granted. Allow Groundhog in the macOS prompt, then reselect the account";
  default:
    return "Unlock your Keychain to use the remote signer";
  }
#else
  switch (self->lock_reason) {
  case LOCK_WAITING:
    return "Unlock your keyring — Groundhog is waiting for it";
  case LOCK_TIMED_OUT:
    return "Unlock your keyring, then reselect the account. The keyring did not answer";
  default:
    return "Unlock your keyring to use the remote signer";
  }
#endif
}

gboolean
gh_account_controller_is_listing(GhAccountController *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), FALSE);
  return self->list_pending[0] || self->list_pending[1];
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
  return self->generation_cancel &&
         !g_cancellable_is_cancelled(self->generation_cancel) &&
         generation == self->generation;
}

typedef struct {
  GTask *task;
  guint64 generation;
  gboolean sign;
  GCancellable *cancel;
  GCancellable *caller;
  GCancellable *generation_cancel;
  GCancellable *gate_cancel;
  guint64 gate_epoch;
  gboolean remote;
  gulong caller_handler;
  gulong generation_handler;
  gulong gate_handler;
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
  if (!gh_account_controller_is_current(self, call->generation) || !self->signer ||
      (call->remote && (!self->remote_storage_ready ||
                        call->gate_epoch != self->remote_gate_epoch))) {
    g_free(value);
    g_task_return_new_error(call->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                            "Account signer generation was revoked");
  } else if (g_cancellable_is_cancelled(call->cancel)) {
    g_free(value);
    g_task_return_new_error(call->task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_CANCELLED,
                            "Signer operation was cancelled");
  } else if (error) {
    if (self->active_backend == GH_SIGNER_BACKEND_NIP46 &&
        g_error_matches(error, GH_SIGNER_ERROR, GH_SIGNER_ERROR_KEY_MISMATCH)) {
      set_remote_state(self, GH_REMOTE_SIGNER_ERROR);
      if (self->remote_session) gh_nip46_session_cancel(self->remote_session);
    }
    g_task_return_error(call->task, g_steal_pointer(&error));
  } else {
    g_task_return_pointer(call->task, value, g_free);
  }
  if (call->caller_handler)
    g_cancellable_disconnect(call->caller, call->caller_handler);
  if (call->generation_handler)
    g_cancellable_disconnect(call->generation_cancel, call->generation_handler);
  if (call->gate_handler)
    g_cancellable_disconnect(call->gate_cancel, call->gate_handler);
  g_clear_object(&call->caller);
  g_clear_object(&call->generation_cancel);
  g_clear_object(&call->gate_cancel);
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
  if (!self->signer || !self->settings ||
      (self->active_backend == GH_SIGNER_BACKEND_NIP46 &&
       (!self->remote_storage_ready ||
        (self->remote_state != GH_REMOTE_SIGNER_READY &&
         self->remote_state != GH_REMOTE_SIGNER_OFFLINE)))) {
    g_task_return_new_error(task, GH_SIGNER_ERROR, GH_SIGNER_ERROR_UNAVAILABLE,
                            self->active_backend == GH_SIGNER_BACKEND_NIP46 &&
                              !self->remote_storage_ready ?
                              "Encrypted storage is required for a remote signer" :
                            self->active_backend == GH_SIGNER_BACKEND_NIP46 ?
                              "Remote signer is not ready" :
                              "No signer is bound to the active account");
    g_object_unref(task);
    return NULL;
  }
  SignerCall *call = g_new0(SignerCall, 1);
  call->task = task;
  call->generation = self->generation;
  call->remote = self->active_backend == GH_SIGNER_BACKEND_NIP46;
  call->gate_epoch = self->remote_gate_epoch;
  call->cancel = g_cancellable_new();
  if (call->remote) {
    call->gate_cancel = g_object_ref(self->remote_gate_cancel);
    call->gate_handler = g_cancellable_connect(call->gate_cancel,
      G_CALLBACK(cancel_signer_call), call->cancel, NULL);
  }
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

/* W33: NIP-04 decryption of older DMs (read-only). */
void
gh_account_controller_nip04_decrypt_async(GhAccountController *self,
                                          const gchar *ciphertext, const gchar *peer,
                                          GCancellable *cancellable,
                                          GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(self));
  SignerCall *call = new_signer_call(self, gh_account_controller_nip04_decrypt_async,
                                    cancellable, callback, user_data);
  if (!call) return;
  gh_signer_nip04_decrypt_async(self->signer, ciphertext, peer, call->cancel,
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
                       tag == gh_account_controller_nip44_decrypt_async ||
                       tag == gh_account_controller_nip04_decrypt_async, NULL);
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
    return NULL; /* the account can send */
  }
}

gchar *
gh_account_controller_describe_limits(GhAccountController *self,
                                      gboolean network_available)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(self), NULL);
  if (self->state != GH_ACCOUNT_STATE_ACTIVE)
    return gh_account_describe_limits(self->state, self->availability, "auto",
                                      network_available);
  if (self->active_backend == GH_SIGNER_BACKEND_GROTTO)
    return gh_account_describe_limits(self->state, self->availability, "auto",
                                      network_available);
  if (!self->remote_storage_ready)
    return g_strdup("Read-only: unlock or repair encrypted storage before using your remote signer");
  if (!network_available)
    return g_strdup("Offline: stored messages remain available; remote signing needs a connection");
  switch (self->remote_state) {
  case GH_REMOTE_SIGNER_LOADING_CREDENTIAL:
    return g_strdup("Read-only: loading remote signer credentials");
  case GH_REMOTE_SIGNER_CONNECTING:
    return g_strdup("Read-only: connecting to your remote signer");
  case GH_REMOTE_SIGNER_OFFLINE:
    /* Keep the composer and retry path available. The session queues until
     * a relay returns; a successful RPC is what clears timeout-derived
     * OFFLINE, so refusing calls here would strand the account forever. */
    return NULL;
  case GH_REMOTE_SIGNER_LOCKED:
    return g_strdup_printf("Read-only: %s", gh_account_controller_describe_remote_lock(self));
  case GH_REMOTE_SIGNER_ERROR:
    return g_strdup("Read-only: remote signer needs repair or re-pairing");
  case GH_REMOTE_SIGNER_READY:
  default:
    return NULL;
  }
}

static void
gh_account_controller_dispose(GObject *object)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(object);
  if (self->settings) {
    g_clear_handle_id(&self->reconcile_id, g_source_remove);
    g_clear_handle_id(&self->mode_rebind_id, g_source_remove);
    g_signal_handlers_disconnect_by_data(self->settings, self);
    g_clear_object(&self->settings);
    revoke_generation(self, FALSE);
    if (self->remote_session) {
      gh_nip46_session_cancel(self->remote_session);
      g_clear_object(&self->remote_session);
    }
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
  g_clear_object(&self->credentials);
  g_clear_object(&self->remote_gate_cancel);
  G_OBJECT_CLASS(gh_account_controller_parent_class)->dispose(object);
}

static void
gh_account_controller_finalize(GObject *object)
{
  GhAccountController *self = GH_ACCOUNT_CONTROLLER(object);
  g_clear_object(&self->signer_cancel);
  g_clear_pointer(&self->identities, g_ptr_array_unref);
  g_clear_pointer(&self->grotto_results, g_ptr_array_unref);
  g_clear_pointer(&self->remote_results, g_ptr_array_unref);
  g_free(self->active_npub);
  g_free(self->adopted_npub);
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
  signals[SIGNAL_AUTH_URL] = g_signal_new("auth-url", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, g_signal_accumulator_true_handled, NULL, NULL,
    G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
}

static void
gh_account_controller_init(GhAccountController *self)
{
  self->state = GH_ACCOUNT_STATE_DISCOVERING;
  self->generation = 1;
  self->credential_wait_ms = CREDENTIAL_WAIT_MS;
  self->credential_timeout_ms = CREDENTIAL_TIMEOUT_MS;
  self->ping_timeout_ms = PING_TIMEOUT_MS;
  self->generation_cancel = g_cancellable_new();
  self->signer_cancel = g_cancellable_new();
}
