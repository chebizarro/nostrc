#include "gnostr-list-gnostr-adapter.h"
#include "gnostr-signer-bridge.h"
#include "nostr_relay.h"
#include "nostr_subscription.h"
#include "nostr-filter.h"
#include "nostr-tag.h"
#include "signer_proxy.h"
#include <string.h>

/* Gnostr has no account database. This adapter intentionally retains session
 * state only; the account-scoped service, never a global pool, owns its sockets. */
typedef struct { GHashTable *states; char *account; guint64 generation; } AppAdapter;
static gboolean app_valid(gpointer data, const char *account, guint64 generation) {
  AppAdapter *a = data; return a->generation == generation && !strcmp(a->account, account);
}
static char *state_key(int kind, const char *id) { return g_strdup_printf("%d:%s", kind, id); }
static char *app_load(gpointer data, int kind, const char *id, GError **error) {
  (void)error; AppAdapter *a = data; char *key = state_key(kind, id);
  char *state = g_strdup(g_hash_table_lookup(a->states, key)); g_free(key); return state;
}
static gboolean app_commit(gpointer data, int kind, const char *id, const char *json, GError **error) {
  (void)error; AppAdapter *a = data;
  g_hash_table_replace(a->states, state_key(kind, id), g_strdup(json)); return TRUE;
}
static void app_free(gpointer data) {
  AppAdapter *a = data; g_hash_table_unref(a->states); g_free(a->account); g_free(a);
}

typedef struct {
  gatomicrefcount refs;
  GWeakRef service;
  GNostrRelay *relay;
  GNostrSubscription *sub;
  GCancellable *cancel;
  char *account, *identifier;
  int kind;
  guint64 generation, serial;
  gboolean closed, connected_once;
  GSource *retry;
  guint failures;
} AppRead;
static AppRead *read_ref(AppRead *r) { g_atomic_ref_count_inc(&r->refs); return r; }
static void read_unref(gpointer data) {
  AppRead *r = data; if (!g_atomic_ref_count_dec(&r->refs)) return;
  g_weak_ref_clear(&r->service); g_clear_object(&r->relay); g_clear_object(&r->sub); g_clear_object(&r->cancel);
  g_free(r->account); g_free(r->identifier); g_free(r);
}
static void read_notify(AppRead *r, GNostrListReadNotification type, const char *payload) {
  if (r->closed) return;
  GNostrListService *service = g_weak_ref_get(&r->service);
  if (service) { gnostr_list_service_notify(service, r->serial, r->kind, r->identifier, gnostr_relay_get_url(r->relay), type, payload); g_object_unref(service); }
}
static void read_event(GNostrSubscription *sub, const char *json, AppRead *r) { (void)sub; read_notify(r, GNOSTR_LIST_EVENT, json); }
static void read_eose(GNostrSubscription *sub, AppRead *r) { (void)sub; read_notify(r, GNOSTR_LIST_EOSE, NULL); }
static void read_closed(GNostrSubscription *sub, const char *reason, AppRead *r) { (void)sub; (void)reason; read_notify(r, GNOSTR_LIST_CLOSED, NULL); }
static void read_auth(GNostrRelay *relay, const char *challenge, AppRead *r) {
  (void)relay; (void)challenge;
  /* No blanket account AUTH permission: an AUTH-only source is explicitly
   * incomplete. The application can change its relay plan or retry. */
  read_notify(r, GNOSTR_LIST_AUTH_REQUIRED, NULL);
}
static void read_clear_sub(AppRead *r) {
  if (!r->sub) return;
  g_signal_handlers_disconnect_by_data(r->sub, r); gnostr_subscription_close(r->sub); g_clear_object(&r->sub);
}
static void read_subscribe(AppRead *r) {
  if (r->closed || g_cancellable_is_cancelled(r->cancel)) return;
  GNostrListService *s = g_weak_ref_get(&r->service);
  if (!s || gnostr_list_service_get_read_serial(s) != r->serial) { g_clear_object(&s); return; }
  g_object_unref(s);
  read_clear_sub(r);
  NostrFilter *f = nostr_filter_new(); nostr_filter_set_kinds(f, &r->kind, 1);
  const char *authors[] = {r->account}; nostr_filter_set_authors(f, authors, 1); nostr_filter_set_limit(f, 1);
  if (nostr_nip51_schema_requires_identifier(nostr_nip51_schema_lookup(r->kind))) {
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("d", r->identifier, NULL)); nostr_filter_set_tags(f, tags);
  }
  NostrFilters *filters = nostr_filters_new(); nostr_filters_add(filters, f); nostr_filter_free(f);
  r->sub = gnostr_subscription_new(r->relay, filters);
  if (!r->sub) { nostr_filters_free(filters); read_notify(r, GNOSTR_LIST_ERROR, NULL); return; }
  g_signal_connect(r->sub, "event", G_CALLBACK(read_event), r);
  g_signal_connect(r->sub, "eose", G_CALLBACK(read_eose), r);
  g_signal_connect(r->sub, "closed", G_CALLBACK(read_closed), r);
  GError *error = NULL;
  if (!gnostr_subscription_fire(r->sub, &error)) read_notify(r, GNOSTR_LIST_ERROR, NULL);
  g_clear_error(&error);
}
static void read_connected(GObject *source, GAsyncResult *res, gpointer data) {
  AppRead *r = data; GError *error = NULL;
  gboolean ok = gnostr_relay_connect_finish(GNOSTR_RELAY(source), res, &error);
  if (!r->closed) {
    if (ok) { r->failures = 0; r->connected_once = TRUE; read_notify(r, GNOSTR_LIST_RECONNECTED, NULL); read_subscribe(r); }
    else read_notify(r, GNOSTR_LIST_ERROR, NULL);
  }
  g_clear_error(&error); read_unref(r);
}
static gboolean read_retry(gpointer data) {
  AppRead *r = data; g_clear_pointer(&r->retry, g_source_unref);
  if (!r->closed && !g_cancellable_is_cancelled(r->cancel)) gnostr_relay_connect_async(r->relay, r->cancel, read_connected, read_ref(r));
  return G_SOURCE_REMOVE;
}
static void read_state(GNostrRelay *relay, int old_state, int state, AppRead *r) {
  (void)relay; (void)old_state;
  if (r->closed) return;
  if (state == GNOSTR_RELAY_STATE_DISCONNECTED || state == GNOSTR_RELAY_STATE_ERROR) {
    read_notify(r, GNOSTR_LIST_DISCONNECTED, NULL); read_clear_sub(r);
    if (!r->retry) {
      r->retry = g_timeout_source_new_seconds(MIN(1u << MIN(r->failures++, 6u), 60u));
      g_source_set_callback(r->retry, read_retry, read_ref(r), read_unref);
      g_source_attach(r->retry, g_main_context_get_thread_default());
    }
  }
}
static gpointer app_open(gpointer data, GNostrListService *service, const char *account, guint64 generation,
                         guint64 serial, const GNostrListReadPlanEntry *entry, GError **error) {
  (void)data; (void)error;
  GPtrArray *reads = g_ptr_array_new();
  for (guint i = 0; entry->sources && entry->sources[i]; i++) {
    AppRead *r = g_new0(AppRead, 1); g_atomic_ref_count_init(&r->refs); g_weak_ref_init(&r->service, service);
    r->account = g_strdup(account); r->generation = generation; r->serial = serial;
    r->kind = entry->kind; r->identifier = g_strdup(entry->identifier); r->cancel = g_cancellable_new();
    r->relay = gnostr_relay_new(entry->sources[i]); g_ptr_array_add(reads, r);
    g_signal_connect(r->relay, "auth-challenge", G_CALLBACK(read_auth), r);
    g_signal_connect(r->relay, "state-changed", G_CALLBACK(read_state), r);
    gnostr_relay_connect_async(r->relay, r->cancel, read_connected, read_ref(r));
  }
  return reads;
}
static void app_close(gpointer data, gpointer handle) {
  (void)data; GPtrArray *reads = handle;
  for (guint i = 0; i < reads->len; i++) {
    AppRead *r = reads->pdata[i]; r->closed = TRUE; g_cancellable_cancel(r->cancel);
    if (r->retry) { g_source_destroy(r->retry); g_clear_pointer(&r->retry, g_source_unref); }
    read_clear_sub(r); g_signal_handlers_disconnect_by_data(r->relay, r); gnostr_relay_disconnect(r->relay); read_unref(r);
  }
  g_ptr_array_unref(reads);
}
static void crypto_done(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data; GError *error = NULL; char *text = NULL;
  int method = GPOINTER_TO_INT(g_task_get_task_data(task)); gboolean ok;
  if (method == 0) ok = nostr_org_nostr_signer_call_nip44_encrypt_finish(NOSTR_ORG_NOSTR_SIGNER(source), &text, result, &error);
  else if (method == 1) ok = nostr_org_nostr_signer_call_nip44_decrypt_finish(NOSTR_ORG_NOSTR_SIGNER(source), &text, result, &error);
  else ok = nostr_org_nostr_signer_call_nip04_decrypt_finish(NOSTR_ORG_NOSTR_SIGNER(source), &text, result, &error);
  if (ok && text) g_task_return_pointer(task, text, g_free);
  else { g_free(text); if (error) g_task_return_error(task, error); else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Signer crypto returned no result"); }
  g_object_unref(task);
}
static void crypto_start(const char *account, const char *input, int method, GCancellable *cancel, GAsyncReadyCallback cb, gpointer user_data) {
  GTask *task = g_task_new(NULL, cancel, cb, user_data); GError *error = NULL;
  NostrOrgNostrSigner *proxy = gnostr_signer_bridge_proxy_get(&error);
  if (!proxy) { if (error) g_task_return_error(task, error); else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Gnostr signer bridge unavailable"); g_object_unref(task); return; }
  g_task_set_task_data(task, GINT_TO_POINTER(method), NULL);
  if (method == 0) nostr_org_nostr_signer_call_nip44_encrypt(proxy, input, account, account, cancel, crypto_done, task);
  else if (method == 1) nostr_org_nostr_signer_call_nip44_decrypt(proxy, input, account, account, cancel, crypto_done, task);
  else nostr_org_nostr_signer_call_nip04_decrypt(proxy, input, account, account, cancel, crypto_done, task);
}
static void app_encrypt(gpointer data, const char *account, const char *input, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) { (void)data; crypto_start(account, input, 0, cancel, cb, u); }
static void app_decrypt(gpointer data, const char *account, const char *input, gboolean legacy, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) { (void)data; crypto_start(account, input, legacy ? 2 : 1, cancel, cb, u); }
static char *app_crypto_finish(gpointer data, GAsyncResult *res, GError **error) { (void)data; return g_task_propagate_pointer(G_TASK(res), error); }
static void app_sign(gpointer data, const char *json, GCancellable *cancel, GAsyncReadyCallback cb, gpointer u) { (void)data; gnostr_signer_bridge_sign_event_async(json, cancel, cb, u); }
static char *app_sign_finish(gpointer data, GAsyncResult *res, GError **error) { (void)data; char *json = NULL; gnostr_signer_bridge_sign_event_finish(res, &json, error); return json; }

typedef struct {
  GTask *task;
  GNostrRelay *relay;
  NostrEvent *event;
  GSource *deadline;
  GCancellable *cancel;
  gulong cancel_id;
  char *id;
  gboolean done;
} AppPublish;
static void pub_free(gpointer data) {
  AppPublish *p = data;
  g_clear_object(&p->relay); nostr_event_free(p->event); g_clear_object(&p->cancel); g_free(p->id); g_free(p);
}
static void pub_finish(AppPublish *p, GNostrListPublishOutcome outcome) {
  if (p->done) return; p->done = TRUE;
  g_signal_handlers_disconnect_by_data(p->relay, p);
  if (p->deadline) { g_source_destroy(p->deadline); g_clear_pointer(&p->deadline, g_source_unref); }
  /* Cancellation is marshalled before finishing; disconnect never runs inside
   * its own GCancellable handler. */
  if (p->cancel_id) { g_cancellable_disconnect(p->cancel, p->cancel_id); p->cancel_id = 0; }
  gnostr_relay_disconnect(p->relay); g_task_return_int(p->task, outcome); g_clear_object(&p->task);
}
static void pub_ok(GNostrRelay *relay, const char *id, gboolean accepted, const char *reason, AppPublish *p) {
  (void)relay; if (g_strcmp0(id, p->id)) return;
  GNostrListPublishOutcome outcome = accepted ? GNOSTR_LIST_ACCEPTED : GNOSTR_LIST_REJECTED;
  if (reason && g_str_has_prefix(reason, "duplicate:")) outcome = GNOSTR_LIST_DUPLICATE;
  if (!accepted && reason && g_str_has_prefix(reason, "auth-required:")) outcome = GNOSTR_LIST_AUTH_FAILURE;
  pub_finish(p, outcome);
}
static void pub_auth(GNostrRelay *relay, const char *challenge, AppPublish *p) { (void)relay; (void)challenge; pub_finish(p, GNOSTR_LIST_AUTH_FAILURE); }
static void pub_error(GNostrRelay *relay, GError *error, AppPublish *p) { (void)relay; (void)error; pub_finish(p, GNOSTR_LIST_TRANSPORT_FAILURE); }
static gboolean pub_timeout(gpointer data) { pub_finish(data, GNOSTR_LIST_TRANSPORT_FAILURE); return G_SOURCE_REMOVE; }
static gboolean pub_cancel_dispatch(gpointer data) { GTask *task = data; AppPublish *p = g_task_get_task_data(task); pub_finish(p, GNOSTR_LIST_CANCELLED); return G_SOURCE_REMOVE; }
static void pub_cancel(GCancellable *cancel, gpointer data) {
  (void)cancel; GTask *task = data; GSource *idle = g_idle_source_new();
  g_source_set_callback(idle, pub_cancel_dispatch, g_object_ref(task), g_object_unref);
  g_source_attach(idle, g_task_get_context(task)); g_source_unref(idle);
}
static void pub_connected(GObject *source, GAsyncResult *res, gpointer data) {
  GTask *task = data; AppPublish *p = g_task_get_task_data(task); GError *error = NULL;
  gboolean ok = gnostr_relay_connect_finish(GNOSTR_RELAY(source), res, &error);
  if (!p->done) {
    if (!ok || g_cancellable_is_cancelled(p->cancel) || !gnostr_relay_publish(p->relay, p->event, &error)) pub_finish(p, GNOSTR_LIST_TRANSPORT_FAILURE);
  }
  g_clear_error(&error); g_object_unref(task);
}
typedef struct { GTask *task; GVariantBuilder outcomes; guint remaining; } PublishBatch;
typedef struct { PublishBatch *batch; char *url; } PublishTarget;
static void pub_target_done(GObject *source, GAsyncResult *res, gpointer data) {
  (void)source; PublishTarget *target = data; PublishBatch *b = target->batch; GError *error = NULL;
  gint outcome = g_task_propagate_int(G_TASK(res), &error); if (error) outcome = GNOSTR_LIST_CANCELLED;
  g_clear_error(&error); g_variant_builder_add(&b->outcomes, "{si}", target->url, outcome); g_free(target->url); g_free(target);
  if (!--b->remaining) { g_task_return_pointer(b->task, g_variant_ref_sink(g_variant_builder_end(&b->outcomes)), (GDestroyNotify)g_variant_unref); g_object_unref(b->task); g_free(b); }
}
static void app_publish(gpointer data, const char *json, const char *const *targets, const char *purpose,
                        GCancellable *cancel, GAsyncReadyCallback cb, gpointer user_data) {
  (void)data; (void)purpose;
  PublishBatch *b = g_new0(PublishBatch, 1); b->task = g_task_new(NULL, cancel, cb, user_data);
  b->remaining = g_strv_length((char **)targets); g_variant_builder_init(&b->outcomes, G_VARIANT_TYPE("a{si}"));
  if (!b->remaining) { g_task_return_new_error(b->task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "No list publication targets"); g_variant_builder_clear(&b->outcomes); g_object_unref(b->task); g_free(b); return; }
  for (guint i = 0; targets[i]; i++) {
    AppPublish *p = g_new0(AppPublish, 1); PublishTarget *t = g_new0(PublishTarget, 1); t->batch = b; t->url = g_strdup(targets[i]);
    p->task = g_task_new(NULL, cancel, pub_target_done, t); g_task_set_task_data(p->task, p, pub_free);
    p->relay = gnostr_relay_new(targets[i]); p->event = nostr_event_new(); nostr_event_deserialize_signed(p->event, json, NULL); p->id = nostr_event_get_id(p->event);
    p->cancel = cancel ? g_object_ref(cancel) : g_cancellable_new();
    g_signal_connect(p->relay, "ok", G_CALLBACK(pub_ok), p); g_signal_connect(p->relay, "auth-challenge", G_CALLBACK(pub_auth), p); g_signal_connect(p->relay, "error", G_CALLBACK(pub_error), p);
    p->deadline = g_timeout_source_new_seconds(120); g_source_set_callback(p->deadline, pub_timeout, p, NULL); g_source_attach(p->deadline, g_task_get_context(p->task));
    p->cancel_id = g_cancellable_connect(p->cancel, G_CALLBACK(pub_cancel), p->task, NULL);
    gnostr_relay_connect_async(p->relay, p->cancel, pub_connected, g_object_ref(p->task));
  }
}
static GVariant *app_publish_finish(gpointer data, GAsyncResult *res, GError **error) { (void)data; return g_task_propagate_pointer(G_TASK(res), error); }
GNostrListService *list_gnostr_service_new(const char *account, guint64 generation) {
  AppAdapter *data = g_new0(AppAdapter, 1); data->account = g_strdup(account); data->generation = generation;
  data->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GNostrListAdapter a = {.generation_valid = app_valid, .load = app_load, .commit = app_commit,
    .open_subscription = app_open, .close_subscription = app_close,
    .encrypt_async = app_encrypt, .encrypt_finish = app_crypto_finish, .decrypt_async = app_decrypt, .decrypt_finish = app_crypto_finish,
    .sign_async = app_sign, .sign_finish = app_sign_finish, .publish_async = app_publish, .publish_finish = app_publish_finish};
  GNostrListService *s = gnostr_list_service_new(account, generation, &a, data, app_free);
  if (!s) app_free(data); return s;
}
