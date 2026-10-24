/* nostrc-p15n5.3: listing remote credentials must never read secrets (a read
 * can wait on a Keychain access or keyring unlock prompt), and a secret read
 * parked on such a prompt must neither block listing nor its caller once
 * cancelled. Uses an injected backend that records each query. */
#include "gh-nip46-credentials-private.h"
#include <gio/gio.h>

static const gchar *account = "1111111111111111111111111111111111111111111111111111111111111111";

typedef struct {
  GhNip46CredentialBackend base;
  GMutex lock;
  GCond cond;
  guint attribute_searches;
  guint secret_searches;
  gboolean block_secrets;
  gboolean blocked;   /* a secret read is parked */
  gboolean release;
} FakeBackend;

static GPtrArray *
fake_search(GhNip46CredentialBackend *backend, GObject *prepared, const gchar *acct,
            gboolean interactive, gboolean load_secrets, GCancellable *cancellable,
            GError **error)
{
  (void)prepared; (void)acct; (void)interactive; (void)cancellable; (void)error;
  FakeBackend *self = (FakeBackend *)backend;
  g_mutex_lock(&self->lock);
  if (load_secrets) {
    self->secret_searches++;
    if (self->block_secrets) {
      /* A prompt nobody answers: only the test can release it. */
      self->blocked = TRUE;
      g_cond_broadcast(&self->cond);
      while (!self->release) g_cond_wait(&self->cond, &self->lock);
    }
  } else {
    self->attribute_searches++;
  }
  g_mutex_unlock(&self->lock);
  GPtrArray *items = g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip46_credential_item_free);
  GhNip46CredentialItem *item = g_new0(GhNip46CredentialItem, 1);
  item->account = g_strdup(account);
  item->version = g_strdup(GH_NIP46_CREDENTIAL_VERSION);
  item->label = g_strdup(GH_NIP46_CREDENTIAL_LABEL);
  item->attributes_valid = TRUE;
  item->locked = !load_secrets; /* no secret was read */
  g_ptr_array_add(items, item);
  return items;
}

static gboolean fake_write(GhNip46CredentialBackend *b, GObject *p, const gchar *a, GBytes *s,
                           gboolean i, GError **e) { (void)b; (void)p; (void)a; (void)s; (void)i; (void)e; return TRUE; }
static gboolean fake_remove(GhNip46CredentialBackend *b, GObject *p, const gchar *a, gboolean i,
                            GError **e) { (void)b; (void)p; (void)a; (void)i; (void)e; return TRUE; }
static void fake_free(GhNip46CredentialBackend *b) { (void)b; /* owned by the test */ }

static void
fake_init(FakeBackend *fake)
{
  *fake = (FakeBackend){0};
  g_mutex_init(&fake->lock);
  g_cond_init(&fake->cond);
  fake->base.search = fake_search;
  fake->base.write = fake_write;
  fake->base.remove = fake_remove;
  fake->base.free = fake_free;
}

typedef struct {
  GPtrArray *list; GhNip46Credential *credential; GError *error; gboolean ok; gboolean done;
  guint calls; /* completion callbacks */
} Result;

static void
on_list(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->list = gh_nip46_credential_store_list_finish(GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
  out->calls++;
}

static void
on_lookup(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->credential = gh_nip46_credential_store_lookup_finish(GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
  out->calls++;
}

static void
on_store(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->ok = gh_nip46_credential_store_store_finish(GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
  out->calls++;
}

static void
on_delete(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->ok = gh_nip46_credential_store_delete_finish(GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
  out->calls++;
}

static gboolean
expire(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
wait_done(Result *result)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, expire, &expired);
  while (!result->done && !expired) g_main_context_iteration(NULL, TRUE);
  g_assert_false(expired);
  g_source_remove(timer);
}

/* Dispatch everything already pending, so a second completion would show. */
static void
drain(void)
{
  while (g_main_context_iteration(NULL, FALSE)) {}
}

static GhNip46Credential *
test_credential(void)
{
  const gchar *relays[] = { "wss://nos.lol", NULL };
  GError *error = NULL;
  GhNip46Credential *credential = gh_nip46_credential_new(account,
    "3333333333333333333333333333333333333333333333333333333333333333",
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", relays, &error);
  g_assert_no_error(error);
  return credential;
}

static void
test_list_reads_attributes_only(void)
{
  static FakeBackend fake;
  fake_init(&fake);
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_with_backend(&fake.base);
  Result listed = {0};
  gh_nip46_credential_store_list_async(store, NULL, on_list, &listed);
  wait_done(&listed);
  g_assert_no_error(listed.error);
  g_assert_cmpuint(listed.list->len, ==, 1);
  g_assert_cmpuint(fake.attribute_searches, ==, 1);
  g_assert_cmpuint(fake.secret_searches, ==, 0);
  g_ptr_array_unref(listed.list);
  g_object_unref(store);
}

static void
test_blocked_lookup_does_not_block_list(void)
{
  static FakeBackend fake; /* outlives the parked worker */
  fake_init(&fake);
  fake.block_secrets = TRUE;
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_with_backend(&fake.base);
  GCancellable *cancel = g_cancellable_new();
  Result found = {0};
  gh_nip46_credential_store_lookup_async(store, account, cancel, on_lookup, &found);
  g_mutex_lock(&fake.lock);
  while (!fake.blocked) g_cond_wait(&fake.cond, &fake.lock);
  g_mutex_unlock(&fake.lock);

  Result listed = {0};
  gh_nip46_credential_store_list_async(store, NULL, on_list, &listed);
  wait_done(&listed);
  g_assert_no_error(listed.error);
  g_assert_cmpuint(listed.list->len, ==, 1);
  g_assert_cmpuint(fake.secret_searches, ==, 1); /* only the lookup */
  g_ptr_array_unref(listed.list);
  g_assert_false(found.done);

  g_cancellable_cancel(cancel);
  wait_done(&found);
  g_assert_error(found.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(found.credential);
  g_clear_error(&found.error);

  g_mutex_lock(&fake.lock);
  fake.release = TRUE;
  g_cond_broadcast(&fake.cond);
  g_mutex_unlock(&fake.lock);
  g_object_unref(cancel);
  g_object_unref(store); /* the released worker drops the last reference */
}

/* nostrc-0cfsg.36.30: a backend's preparation runs, and completes, on the
 * thread that starts the operation before any worker uses it (the Secret
 * Service backend gets libsecret's default instance there); each worker call
 * receives that object. A refused preparation fails the operation without a
 * worker call. */
typedef struct {
  GhNip46CredentialBackend base;
  GMutex lock;
  GThread *starter;
  GObject *token;
  gboolean refuse;
  guint prepared;     /* prepare_finish() calls */
  guint worker_calls;
  gboolean broken;    /* a worker call broke the contract */
} PrepBackend;

static void
prep_prepare_async(GhNip46CredentialBackend *backend, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  PrepBackend *self = (PrepBackend *)backend;
  g_assert_true(g_thread_self() == self->starter);
  GTask *task = g_task_new(NULL, cancellable, callback, user_data);
  if (self->refuse)
    g_task_return_new_error(task, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE,
                            "Secret Service unavailable");
  else
    g_task_return_pointer(task, g_object_ref(self->token), g_object_unref);
  g_object_unref(task);
}

static GObject *
prep_prepare_finish(GhNip46CredentialBackend *backend, GAsyncResult *result, GError **error)
{
  PrepBackend *self = (PrepBackend *)backend;
  g_assert_true(g_thread_self() == self->starter);
  g_mutex_lock(&self->lock);
  self->prepared++;
  g_mutex_unlock(&self->lock);
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
prep_worker_call(GhNip46CredentialBackend *backend, GObject *prepared)
{
  PrepBackend *self = (PrepBackend *)backend;
  g_mutex_lock(&self->lock);
  if (prepared != self->token || g_thread_self() == self->starter || self->prepared == 0)
    self->broken = TRUE;
  self->worker_calls++;
  g_mutex_unlock(&self->lock);
}

static GPtrArray *
prep_search(GhNip46CredentialBackend *b, GObject *p, const gchar *a, gboolean i, gboolean l,
            GCancellable *c, GError **e)
{
  (void)a; (void)i; (void)l; (void)c; (void)e;
  prep_worker_call(b, p);
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip46_credential_item_free);
}
static gboolean prep_write(GhNip46CredentialBackend *b, GObject *p, const gchar *a, GBytes *s,
                           gboolean i, GError **e) { (void)a; (void)s; (void)i; (void)e; prep_worker_call(b, p); return TRUE; }
static gboolean prep_remove(GhNip46CredentialBackend *b, GObject *p, const gchar *a, gboolean i,
                            GError **e) { (void)a; (void)i; (void)e; prep_worker_call(b, p); return TRUE; }

static void
test_prepared_on_starting_thread(void)
{
  static PrepBackend prep;
  prep = (PrepBackend){0};
  g_mutex_init(&prep.lock);
  prep.starter = g_thread_self();
  prep.token = g_object_new(G_TYPE_OBJECT, NULL);
  prep.base.prepare_async = prep_prepare_async;
  prep.base.prepare_finish = prep_prepare_finish;
  prep.base.search = prep_search;
  prep.base.write = prep_write;
  prep.base.remove = prep_remove;
  prep.base.free = fake_free;
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_with_backend(&prep.base);

  Result found = {0};
  gh_nip46_credential_store_lookup_async(store, account, NULL, on_lookup, &found);
  wait_done(&found);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_NOT_FOUND);
  g_clear_error(&found.error);
  Result listed = {0};
  gh_nip46_credential_store_list_async(store, NULL, on_list, &listed);
  wait_done(&listed);
  g_assert_no_error(listed.error);
  g_assert_cmpuint(listed.list->len, ==, 0);
  g_ptr_array_unref(listed.list);
  GhNip46Credential *credential = test_credential();
  Result saved = {0};
  gh_nip46_credential_store_store_async(store, credential, FALSE, NULL, on_store, &saved);
  wait_done(&saved);
  g_assert_no_error(saved.error);
  g_assert_true(saved.ok);
  gh_nip46_credential_free(credential);
  Result deleted = {0};
  gh_nip46_credential_store_delete_async(store, account, FALSE, NULL, on_delete, &deleted);
  wait_done(&deleted);
  g_assert_no_error(deleted.error);
  g_assert_true(deleted.ok);
  g_mutex_lock(&prep.lock);
  g_assert_cmpuint(prep.prepared, ==, 4);
  g_assert_cmpuint(prep.worker_calls, ==, 6); /* one search each, a write, a remove */
  g_assert_false(prep.broken);
  g_mutex_unlock(&prep.lock);

  prep.refuse = TRUE;
  found = (Result){0};
  gh_nip46_credential_store_lookup_async(store, account, NULL, on_lookup, &found);
  wait_done(&found);
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE);
  g_clear_error(&found.error);
  g_mutex_lock(&prep.lock);
  g_assert_cmpuint(prep.prepared, ==, 5);
  g_assert_cmpuint(prep.worker_calls, ==, 6);
  g_mutex_unlock(&prep.lock);
  /* Nor does a refused preparation reach a mutation. */
  credential = test_credential();
  saved = (Result){0};
  gh_nip46_credential_store_store_async(store, credential, FALSE, NULL, on_store, &saved);
  wait_done(&saved);
  g_assert_error(saved.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_UNAVAILABLE);
  g_assert_false(saved.ok);
  g_clear_error(&saved.error);
  gh_nip46_credential_free(credential);
  g_mutex_lock(&prep.lock);
  g_assert_cmpuint(prep.prepared, ==, 6);
  g_assert_cmpuint(prep.worker_calls, ==, 6);
  g_mutex_unlock(&prep.lock);
  g_object_unref(store);
  g_object_unref(prep.token);
}

/* A preparation the test completes by hand: it stays pending while the test
 * cancels the operation or drops the store, then hands over a fresh object
 * whose finalization (and the backend's release) the test observes. */
typedef struct {
  GhNip46CredentialBackend base;
  GMutex lock;
  GTask *pending;        /* the parked preparation */
  gboolean null_result;  /* breaks the contract: NULL without an error */
  guint worker_calls, writes;
  gboolean freed;        /* the store released its backend */
  gboolean prepared_freed;
} HeldBackend;

static void
held_set(HeldBackend *self, gboolean *flag)
{
  g_mutex_lock(&self->lock);
  *flag = TRUE;
  g_mutex_unlock(&self->lock);
  g_main_context_wakeup(NULL); /* may run on a worker thread */
}

static void
held_prepare_async(GhNip46CredentialBackend *backend, GCancellable *cancellable,
                   GAsyncReadyCallback callback, gpointer user_data)
{
  HeldBackend *self = (HeldBackend *)backend;
  g_assert_null(self->pending);
  self->pending = g_task_new(NULL, cancellable, callback, user_data);
}

static GObject *
held_prepare_finish(GhNip46CredentialBackend *backend, GAsyncResult *result, GError **error)
{
  HeldBackend *self = (HeldBackend *)backend;
  if (self->null_result) {
    GObject *dropped = g_task_propagate_pointer(G_TASK(result), NULL);
    g_clear_object(&dropped);
    return NULL;
  }
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
on_prepared_finalized(gpointer data, GObject *where)
{
  (void)where;
  HeldBackend *self = data;
  held_set(self, &self->prepared_freed);
}

/* Completes the parked preparation; a cancelled one still reports CANCELLED
 * (and frees the object) through GTask's cancellable check. */
static void
held_release(HeldBackend *self)
{
  g_assert_nonnull(self->pending);
  GObject *prepared = g_object_new(G_TYPE_OBJECT, NULL);
  g_object_weak_ref(prepared, on_prepared_finalized, self);
  GTask *task = g_steal_pointer(&self->pending);
  g_task_return_pointer(task, prepared, g_object_unref);
  g_object_unref(task);
}

static GPtrArray *
held_search(GhNip46CredentialBackend *b, GObject *p, const gchar *a, gboolean i, gboolean l,
            GCancellable *c, GError **e)
{
  (void)p; (void)a; (void)i; (void)l; (void)c; (void)e;
  HeldBackend *self = (HeldBackend *)b;
  g_mutex_lock(&self->lock);
  self->worker_calls++;
  g_mutex_unlock(&self->lock);
  return g_ptr_array_new_with_free_func((GDestroyNotify)gh_nip46_credential_item_free);
}

static gboolean
held_write(GhNip46CredentialBackend *b, GObject *p, const gchar *a, GBytes *s, gboolean i, GError **e)
{
  (void)p; (void)a; (void)s; (void)i; (void)e;
  HeldBackend *self = (HeldBackend *)b;
  g_mutex_lock(&self->lock);
  self->worker_calls++;
  self->writes++;
  g_mutex_unlock(&self->lock);
  return TRUE;
}

static gboolean
held_remove(GhNip46CredentialBackend *b, GObject *p, const gchar *a, gboolean i, GError **e)
{
  (void)p; (void)a; (void)i; (void)e;
  HeldBackend *self = (HeldBackend *)b;
  g_mutex_lock(&self->lock);
  self->worker_calls++;
  g_mutex_unlock(&self->lock);
  return TRUE;
}

static void
held_free(GhNip46CredentialBackend *b)
{
  HeldBackend *self = (HeldBackend *)b;
  held_set(self, &self->freed);
}

static void
held_init(HeldBackend *held)
{
  *held = (HeldBackend){0};
  g_mutex_init(&held->lock);
  held->base.prepare_async = held_prepare_async;
  held->base.prepare_finish = held_prepare_finish;
  held->base.search = held_search;
  held->base.write = held_write;
  held->base.remove = held_remove;
  held->base.free = held_free;
}

static void
wait_held(HeldBackend *self, gboolean *flag)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, expire, &expired);
  for (;;) {
    g_mutex_lock(&self->lock);
    gboolean set = *flag;
    g_mutex_unlock(&self->lock);
    if (set || expired) break;
    g_main_context_iteration(NULL, TRUE);
  }
  g_assert_false(expired);
  g_source_remove(timer);
}

static void
assert_held(HeldBackend *self, guint worker_calls, guint writes)
{
  g_mutex_lock(&self->lock);
  g_assert_cmpuint(self->worker_calls, ==, worker_calls);
  g_assert_cmpuint(self->writes, ==, writes);
  g_mutex_unlock(&self->lock);
}

/* Cancelled while its preparation is pending, an operation completes once,
 * cancelled, and never reaches a worker: no mutation. */
static void
test_cancelled_while_preparing(void)
{
  static HeldBackend held;
  held_init(&held);
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_with_backend(&held.base);
  GCancellable *cancel = g_cancellable_new();
  GhNip46Credential *credential = test_credential();
  Result saved = {0};
  gh_nip46_credential_store_store_async(store, credential, FALSE, cancel, on_store, &saved);
  g_cancellable_cancel(cancel);
  drain();
  g_assert_false(saved.done); /* a mutation waits for its preparation */
  held_release(&held);
  wait_done(&saved);
  g_assert_error(saved.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_false(saved.ok);
  g_clear_error(&saved.error);
  wait_held(&held, &held.prepared_freed);
  drain();
  g_assert_cmpuint(saved.calls, ==, 1);
  assert_held(&held, 0, 0);

  held.prepared_freed = FALSE;
  g_cancellable_reset(cancel);
  Result found = {0};
  gh_nip46_credential_store_lookup_async(store, account, cancel, on_lookup, &found);
  g_cancellable_cancel(cancel);
  held_release(&held);
  wait_done(&found);
  g_assert_error(found.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(found.credential);
  g_clear_error(&found.error);
  wait_held(&held, &held.prepared_freed);
  drain();
  g_assert_cmpuint(found.calls, ==, 1);
  assert_held(&held, 0, 0);
  gh_nip46_credential_free(credential);
  g_object_unref(cancel);
  g_object_unref(store);
  wait_held(&held, &held.freed);
}

/* Dropping the store while a preparation is pending: the operation keeps the
 * store and its backend, completes once, and then releases them and the
 * prepared object. */
static void
test_store_dropped_while_preparing(void)
{
  static HeldBackend held;
  held_init(&held);
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_with_backend(&held.base);
  GhNip46Credential *credential = test_credential();
  Result saved = {0};
  gh_nip46_credential_store_store_async(store, credential, FALSE, NULL, on_store, &saved);
  gh_nip46_credential_free(credential);
  g_object_unref(store);
  drain();
  g_mutex_lock(&held.lock);
  g_assert_false(held.freed);
  g_mutex_unlock(&held.lock);
  g_assert_false(saved.done);
  held_release(&held);
  wait_done(&saved);
  g_assert_no_error(saved.error);
  g_assert_true(saved.ok);
  wait_held(&held, &held.freed);
  wait_held(&held, &held.prepared_freed);
  drain();
  g_assert_cmpuint(saved.calls, ==, 1);
  assert_held(&held, 2, 1); /* its search and write */
}

/* A backend breaking the contract (NULL without an error) is reported, and
 * the operation still completes once, FAILED, without a worker. */
static void
test_prepare_without_error(void)
{
  static HeldBackend held;
  held_init(&held);
  held.null_result = TRUE;
  GhNip46CredentialStore *store = gh_nip46_credential_store_new_with_backend(&held.base);
  Result found = {0};
  gh_nip46_credential_store_lookup_async(store, account, NULL, on_lookup, &found);
  g_test_expect_message(NULL, G_LOG_LEVEL_CRITICAL, "*prepare_finish() returned NULL without an error");
  held_release(&held);
  wait_done(&found);
  g_test_assert_expected_messages();
  g_assert_error(found.error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_FAILED);
  g_clear_error(&found.error);
  wait_held(&held, &held.prepared_freed);
  drain();
  g_assert_cmpuint(found.calls, ==, 1);
  assert_held(&held, 0, 0);
  g_object_unref(store);
  wait_held(&held, &held.freed);
}

/* The Secret Service backend called without its prepared service reports the
 * misuse and still sets an error for its caller to complete with. */
static void
test_secret_service_unprepared(void)
{
  GhNip46CredentialBackend *backend = gh_nip46_credentials_secret_service_new();
  GError *error = NULL;
  g_test_expect_message(NULL, G_LOG_LEVEL_CRITICAL, "*should not be reached*");
  g_assert_null(backend->search(backend, NULL, account, FALSE, TRUE, NULL, &error));
  g_test_assert_expected_messages();
  g_assert_error(error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_FAILED);
  g_clear_error(&error);
  GBytes *secret = g_bytes_new_static("{}", 2);
  g_test_expect_message(NULL, G_LOG_LEVEL_CRITICAL, "*should not be reached*");
  g_assert_false(backend->write(backend, NULL, account, secret, FALSE, &error));
  g_test_assert_expected_messages();
  g_assert_error(error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_FAILED);
  g_clear_error(&error);
  g_bytes_unref(secret);
  g_test_expect_message(NULL, G_LOG_LEVEL_CRITICAL, "*should not be reached*");
  g_assert_false(backend->remove(backend, NULL, account, FALSE, &error));
  g_test_assert_expected_messages();
  g_assert_error(error, GH_NIP46_CREDENTIAL_ERROR, GH_NIP46_CREDENTIAL_ERROR_FAILED);
  g_clear_error(&error);
  backend->free(backend);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip46-credentials/list/attributes-only", test_list_reads_attributes_only);
  g_test_add_func("/nip46-credentials/list/blocked-lookup", test_blocked_lookup_does_not_block_list);
  g_test_add_func("/nip46-credentials/prepared-on-starting-thread", test_prepared_on_starting_thread);
  g_test_add_func("/nip46-credentials/cancelled-while-preparing", test_cancelled_while_preparing);
  g_test_add_func("/nip46-credentials/store-dropped-while-preparing", test_store_dropped_while_preparing);
  g_test_add_func("/nip46-credentials/prepare-without-error", test_prepare_without_error);
  g_test_add_func("/nip46-credentials/secret-service-unprepared", test_secret_service_unprepared);
  return g_test_run();
}
