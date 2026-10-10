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
fake_search(GhNip46CredentialBackend *backend, const gchar *acct, gboolean interactive,
            gboolean load_secrets, GCancellable *cancellable, GError **error)
{
  (void)acct; (void)interactive; (void)cancellable; (void)error;
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

static gboolean fake_write(GhNip46CredentialBackend *b, const gchar *a, GBytes *s,
                           gboolean i, GError **e) { (void)b; (void)a; (void)s; (void)i; (void)e; return TRUE; }
static gboolean fake_remove(GhNip46CredentialBackend *b, const gchar *a, gboolean i,
                            GError **e) { (void)b; (void)a; (void)i; (void)e; return TRUE; }
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

typedef struct { GPtrArray *list; GhNip46Credential *credential; GError *error; gboolean done; } Result;

static void
on_list(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->list = gh_nip46_credential_store_list_finish(GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
}

static void
on_lookup(GObject *source, GAsyncResult *result, gpointer data)
{
  Result *out = data;
  out->credential = gh_nip46_credential_store_lookup_finish(GH_NIP46_CREDENTIAL_STORE(source), result, &out->error);
  out->done = TRUE;
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

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nip46-credentials/list/attributes-only", test_list_reads_attributes_only);
  g_test_add_func("/nip46-credentials/list/blocked-lookup", test_blocked_lookup_does_not_block_list);
  return g_test_run();
}
