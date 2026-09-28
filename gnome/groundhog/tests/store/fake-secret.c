#include "fake-secret.h"

#include <glib/gstdio.h>

struct _FakeSecret {
  GObject parent_instance;
  GPtrArray *items; /* FakeSecretItem */
  GPtrArray *calls; /* FakeSecretCall */
  GQueue parked;    /* GTask, one reference each */
  guint prompts;
  gboolean available;
  gboolean locked;
  gboolean unlock_accepted;
  gboolean store_fails;
  gboolean ignore_query;
  gboolean ignore_cancel;
  gboolean hold;
};

static void fake_backend_init(GhStoreKeyBackendInterface *iface);

G_DEFINE_TYPE_WITH_CODE(FakeSecret, fake_secret, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GH_TYPE_STORE_KEY_BACKEND, fake_backend_init))

typedef struct {
  FakeSecretOp op;
  GhStoreKeyFlags flags;
  GHashTable *attributes;
  gchar *label;
  GBytes *secret;
} FakeRequest;

static GHashTable *
copy_attributes(GHashTable *attributes)
{
  GHashTable *copy = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, attributes);
  while (g_hash_table_iter_next(&iter, &key, &value))
    g_hash_table_insert(copy, g_strdup(key), g_strdup(value));
  return copy;
}

static void
item_free(FakeSecretItem *item)
{
  g_hash_table_unref(item->attributes);
  g_free(item->label);
  g_bytes_unref(item->secret);
  g_free(item);
}

static void
call_free(FakeSecretCall *call)
{
  g_hash_table_unref(call->attributes);
  g_free(call->label);
  g_free(call);
}

static void
request_free(FakeRequest *request)
{
  g_hash_table_unref(request->attributes);
  g_free(request->label);
  g_clear_pointer(&request->secret, g_bytes_unref);
  g_free(request);
}

static void
fake_secret_finalize(GObject *object)
{
  FakeSecret *self = FAKE_SECRET(object);
  g_assert_true(g_queue_is_empty(&self->parked)); /* parked tasks reference self */
  g_ptr_array_unref(self->items);
  g_ptr_array_unref(self->calls);
  G_OBJECT_CLASS(fake_secret_parent_class)->finalize(object);
}

static void
fake_secret_class_init(FakeSecretClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = fake_secret_finalize;
}

static void
fake_secret_init(FakeSecret *self)
{
  self->items = g_ptr_array_new_with_free_func((GDestroyNotify)item_free);
  self->calls = g_ptr_array_new_with_free_func((GDestroyNotify)call_free);
  g_queue_init(&self->parked);
  self->available = TRUE;
  self->unlock_accepted = TRUE;
}

FakeSecret *
fake_secret_new(void)
{
  return g_object_new(FAKE_TYPE_SECRET, NULL);
}

static gboolean
item_matches(FakeSecret *self, const FakeSecretItem *item, GHashTable *query)
{
  if (self->ignore_query)
    return TRUE;
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, query);
  while (g_hash_table_iter_next(&iter, &key, &value))
    if (g_strcmp0(g_hash_table_lookup(item->attributes, key), value) != 0)
      return FALSE;
  return TRUE;
}

static void
prompt_unlock(FakeSecret *self, const FakeRequest *request)
{
  if (!self->locked || !(request->flags & GH_STORE_KEY_FLAGS_INTERACTIVE))
    return;
  self->prompts++;
  if (self->unlock_accepted)
    self->locked = FALSE;
}

static void
return_error(GTask *task, GhStoreKeyError code, const gchar *message)
{
  g_task_return_new_error(task, GH_STORE_KEY_ERROR, code, "%s", message);
}

/* Complete one operation against the current state, like the service would
 * when the D-Bus call arrives. Consumes the task reference. */
static void
fake_run(GTask *task)
{
  FakeSecret *self = g_task_get_source_object(task);
  FakeRequest *request = g_task_get_task_data(task);
  if (!self->ignore_cancel && g_task_return_error_if_cancelled(task)) {
    g_object_unref(task);
    return;
  }
  if (!self->available) {
    return_error(task, GH_STORE_KEY_ERROR_UNAVAILABLE, "fake: no Secret Service");
    g_object_unref(task);
    return;
  }
  prompt_unlock(self, request);
  switch (request->op) {
  case FAKE_SECRET_SEARCH: {
    GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gh_store_key_item_free);
    for (guint i = 0; i < self->items->len; i++) {
      FakeSecretItem *item = g_ptr_array_index(self->items, i);
      if (!item_matches(self, item, request->attributes))
        continue;
      GBytes *secret = NULL;
      if (!self->locked) {
        gsize len = 0;
        gconstpointer data = g_bytes_get_data(item->secret, &len);
        secret = gh_store_key_secret_new(data, len);
      }
      g_ptr_array_add(out, gh_store_key_item_new(copy_attributes(item->attributes),
                                                 self->locked, secret));
    }
    if (out->len == 0 && self->locked) {
      g_ptr_array_unref(out);
      return_error(task, GH_STORE_KEY_ERROR_LOCKED, "fake: keyring locked");
    } else {
      g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
    }
    break;
  }
  case FAKE_SECRET_STORE: {
    if (self->store_fails) {
      return_error(task, GH_STORE_KEY_ERROR_FAILED, "fake: store refused");
      break;
    }
    if (self->locked) {
      return_error(task, GH_STORE_KEY_ERROR_LOCKED, "fake: keyring locked");
      break;
    }
    FakeSecretItem *item = g_new0(FakeSecretItem, 1);
    item->attributes = copy_attributes(request->attributes);
    item->label = g_strdup(request->label);
    gsize len = 0;
    gconstpointer data = g_bytes_get_data(request->secret, &len);
    item->secret = gh_store_key_secret_new(data, len);
    g_ptr_array_add(self->items, item);
    g_task_return_boolean(task, TRUE);
    break;
  }
  case FAKE_SECRET_CLEAR: {
    if (self->locked) {
      return_error(task, GH_STORE_KEY_ERROR_LOCKED, "fake: keyring locked");
      break;
    }
    for (guint i = self->items->len; i > 0; i--)
      if (item_matches(self, g_ptr_array_index(self->items, i - 1), request->attributes))
        g_ptr_array_remove_index(self->items, i - 1);
    g_task_return_boolean(task, TRUE);
    break;
  }
  }
  g_object_unref(task);
}

static gboolean
fake_run_idle(gpointer data)
{
  fake_run(data);
  return G_SOURCE_REMOVE;
}

static void
fake_submit(GhStoreKeyBackend *backend, FakeSecretOp op, GHashTable *attributes,
            const gchar *label, GBytes *secret, GhStoreKeyFlags flags,
            GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  FakeSecret *self = FAKE_SECRET(backend);
  FakeSecretCall *call = g_new0(FakeSecretCall, 1);
  call->op = op;
  call->flags = flags;
  call->attributes = copy_attributes(attributes);
  call->label = g_strdup(label);
  g_ptr_array_add(self->calls, call);

  FakeRequest *request = g_new0(FakeRequest, 1);
  request->op = op;
  request->flags = flags;
  request->attributes = copy_attributes(attributes);
  request->label = g_strdup(label);
  request->secret = secret ? g_bytes_ref(secret) : NULL;
  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_check_cancellable(task, !self->ignore_cancel);
  g_task_set_task_data(task, request, (GDestroyNotify)request_free);
  if (self->hold)
    g_queue_push_tail(&self->parked, task);
  else
    g_idle_add(fake_run_idle, task);
}

static void
fake_search_async(GhStoreKeyBackend *self, GHashTable *attributes, GhStoreKeyFlags flags,
                  GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  fake_submit(self, FAKE_SECRET_SEARCH, attributes, NULL, NULL, flags, cancellable, callback,
              user_data);
}

static GPtrArray *
fake_search_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_assert_true(g_task_is_valid(result, self));
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
fake_store_async(GhStoreKeyBackend *self, GHashTable *attributes, const gchar *label,
                 GBytes *secret, GhStoreKeyFlags flags, GCancellable *cancellable,
                 GAsyncReadyCallback callback, gpointer user_data)
{
  fake_submit(self, FAKE_SECRET_STORE, attributes, label, secret, flags, cancellable, callback,
              user_data);
}

static gboolean
fake_boolean_finish(GhStoreKeyBackend *self, GAsyncResult *result, GError **error)
{
  g_assert_true(g_task_is_valid(result, self));
  return g_task_propagate_boolean(G_TASK(result), error);
}

static void
fake_clear_async(GhStoreKeyBackend *self, GHashTable *attributes, GhStoreKeyFlags flags,
                 GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data)
{
  fake_submit(self, FAKE_SECRET_CLEAR, attributes, NULL, NULL, flags, cancellable, callback,
              user_data);
}

static void
fake_backend_init(GhStoreKeyBackendInterface *iface)
{
  iface->search_async = fake_search_async;
  iface->search_finish = fake_search_finish;
  iface->store_async = fake_store_async;
  iface->store_finish = fake_boolean_finish;
  iface->clear_async = fake_clear_async;
  iface->clear_finish = fake_boolean_finish;
}

void fake_secret_set_available(FakeSecret *self, gboolean available) { self->available = available; }
void fake_secret_set_locked(FakeSecret *self, gboolean locked) { self->locked = locked; }
gboolean fake_secret_get_locked(FakeSecret *self) { return self->locked; }
void fake_secret_set_unlock_accepted(FakeSecret *self, gboolean accepted) { self->unlock_accepted = accepted; }
void fake_secret_set_store_fails(FakeSecret *self, gboolean fails) { self->store_fails = fails; }
void fake_secret_set_ignore_query(FakeSecret *self, gboolean ignore) { self->ignore_query = ignore; }
void fake_secret_set_ignore_cancel(FakeSecret *self, gboolean ignore) { self->ignore_cancel = ignore; }
void fake_secret_set_hold(FakeSecret *self, gboolean hold) { self->hold = hold; }
guint fake_secret_pending(FakeSecret *self) { return self->parked.length; }
GPtrArray *fake_secret_items(FakeSecret *self) { return self->items; }
GPtrArray *fake_secret_calls(FakeSecret *self) { return self->calls; }
guint fake_secret_prompts(FakeSecret *self) { return self->prompts; }

void
fake_secret_clear_calls(FakeSecret *self)
{
  g_ptr_array_set_size(self->calls, 0);
}

guint
fake_secret_pending_for(FakeSecret *self, const gchar *account)
{
  guint n = 0;
  for (GList *l = self->parked.head; l; l = l->next) {
    FakeRequest *request = g_task_get_task_data(l->data);
    if (g_strcmp0(g_hash_table_lookup(request->attributes, GH_STORE_KEY_ATTR_ACCOUNT),
                  account) == 0)
      n++;
  }
  return n;
}

gboolean
fake_secret_release(FakeSecret *self)
{
  GTask *task = g_queue_pop_head(&self->parked);
  if (!task)
    return FALSE;
  fake_run(task);
  return TRUE;
}

void
fake_secret_add_item(FakeSecret *self, const gchar *label, gconstpointer secret,
                     gsize secret_len, ...)
{
  FakeSecretItem *item = g_new0(FakeSecretItem, 1);
  item->attributes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  item->label = g_strdup(label);
  item->secret = gh_store_key_secret_new(secret, secret_len);
  va_list args;
  va_start(args, secret_len);
  const gchar *name;
  while ((name = va_arg(args, const gchar *))) {
    const gchar *value = va_arg(args, const gchar *);
    g_hash_table_insert(item->attributes, g_strdup(name), g_strdup(value));
  }
  va_end(args);
  g_ptr_array_add(self->items, item);
}

guint
fake_secret_count(FakeSecret *self, const gchar *account)
{
  guint n = 0;
  for (guint i = 0; i < self->items->len; i++) {
    FakeSecretItem *item = g_ptr_array_index(self->items, i);
    if (g_strcmp0(g_hash_table_lookup(item->attributes, GH_STORE_KEY_ATTR_ACCOUNT), account) == 0)
      n++;
  }
  return n;
}

/* ---- Synchronous drivers ------------------------------------------------ */

void
gh_test_store_result(GObject *source, GAsyncResult *result, gpointer user_data)
{
  (void)source;
  GAsyncResult **slot = user_data;
  g_assert_null(*slot);
  *slot = g_object_ref(result);
}

static gboolean
on_wait_deadline(gpointer user_data)
{
  *(gboolean *)user_data = TRUE;
  return G_SOURCE_REMOVE;
}

GAsyncResult *
gh_test_wait(GAsyncResult **slot)
{
  gboolean expired = FALSE;
  guint deadline = g_timeout_add_seconds(GH_TEST_WAIT_SECONDS, on_wait_deadline, &expired);
  while (!*slot && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!*slot)
    g_error("no result after %d s: an asynchronous operation never completed",
            GH_TEST_WAIT_SECONDS);
  if (!expired)
    g_source_remove(deadline);
  return *slot;
}

void
gh_test_run_until_idle(void)
{
  while (g_main_context_iteration(NULL, FALSE))
    ;
}

void
gh_test_key_result_clear(GhTestKeyResult *result)
{
  g_clear_pointer(&result->key, g_bytes_unref);
  g_clear_pointer(&result->store_id, g_free);
  g_clear_error(&result->error);
  result->created = FALSE;
}

GhTestKeyResult
gh_test_lookup(GhStoreKey *store_key, const gchar *account, GhStoreKeyFlags flags,
               GCancellable *cancellable)
{
  GAsyncResult *async = NULL;
  gh_store_key_lookup_async(store_key, account, flags, cancellable, gh_test_store_result, &async);
  GhTestKeyResult result = { 0 };
  result.key = gh_store_key_lookup_finish(store_key, gh_test_wait(&async), &result.store_id,
                                          &result.error);
  g_object_unref(async);
  return result;
}

GhTestKeyResult
gh_test_lookup_or_create(GhStoreKey *store_key, const gchar *account, GhStoreKeyFlags flags,
                         GCancellable *cancellable)
{
  GAsyncResult *async = NULL;
  gh_store_key_lookup_or_create_async(store_key, account, flags, cancellable,
                                      gh_test_store_result, &async);
  GhTestKeyResult result = { 0 };
  result.key = gh_store_key_lookup_or_create_finish(store_key, gh_test_wait(&async),
                                                    &result.store_id, &result.created,
                                                    &result.error);
  g_object_unref(async);
  return result;
}

gboolean
gh_test_destroy(GhStoreKey *store_key, const gchar *account, GhStoreKeyFlags flags,
                GError **error)
{
  GAsyncResult *async = NULL;
  gh_store_key_destroy_async(store_key, account, flags, NULL, gh_test_store_result, &async);
  gboolean ok = gh_store_key_destroy_finish(store_key, gh_test_wait(&async), error);
  g_object_unref(async);
  return ok;
}

static gboolean
is_real_directory(const gchar *path)
{
  return g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK);
}

guint
gh_test_count_files(const gchar *path)
{
  if (!is_real_directory(path)) {
    g_printerr("file: %s\n", path);
    return 1;
  }
  guint n = 0;
  GDir *dir = g_dir_open(path, 0, NULL);
  g_assert_nonnull(dir);
  const gchar *name;
  while ((name = g_dir_read_name(dir))) {
    gchar *child = g_build_filename(path, name, NULL);
    n += gh_test_count_files(child);
    g_free(child);
  }
  g_dir_close(dir);
  return n;
}

void
gh_test_remove_tree(const gchar *path)
{
  if (is_real_directory(path)) {
    GDir *dir = g_dir_open(path, 0, NULL);
    if (dir) {
      const gchar *name;
      while ((name = g_dir_read_name(dir))) {
        gchar *child = g_build_filename(path, name, NULL);
        gh_test_remove_tree(child);
        g_free(child);
      }
      g_dir_close(dir);
    }
  }
  g_remove(path);
}
