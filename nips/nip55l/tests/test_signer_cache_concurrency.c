/* Exercise the actual cache publisher used by StoreKey/CreateProfile and
 * the actual ClearKey/read/sign paths. Compile the core without a custody
 * backend: these tests never access a keychain or session Secret Service.
 * The copy/mutation barriers are absent from shipping objects. */
#define NIP55L_CACHE_TEST_HOOKS
#include "../src/core/signer_ops.c"
#include <glib.h>
#include <errno.h>

static const char OLD_KEY[] = "0000000000000000000000000000000000000000000000000000000000000001";
static const char NEW_KEY[] = "0000000000000000000000000000000000000000000000000000000000000002";
typedef enum { READ_NPUB, READ_PUBLIC, READ_SIGN } ReadOp;
typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  gboolean copying, mutation_attempted, release;
  gboolean clear;
  ReadOp op;
  const char *old_npub, *new_npub;
  char *result;
  int rc;
} Race;

static void copy_barrier(void *data) {
  Race *r = data;
  pthread_mutex_lock(&r->mutex);
  r->copying = TRUE;
  pthread_cond_broadcast(&r->cond);
  while (!r->release) pthread_cond_wait(&r->cond, &r->mutex);
  pthread_mutex_unlock(&r->mutex);
}
static void mutation_barrier(void *data) {
  Race *r = data;
  /* Counterfactual: an unprotected reader permits eviction here. This
   * assertion fails deterministically if copy-under-lock is removed. */
  g_assert_cmpint(pthread_mutex_trylock(&signer_cache_lock), ==, EBUSY);
  pthread_mutex_lock(&r->mutex);
  r->mutation_attempted = TRUE;
  pthread_cond_broadcast(&r->cond);
  pthread_mutex_unlock(&r->mutex);
}
static gpointer read_cache(gpointer data) {
  Race *r = data;
  if (r->op == READ_NPUB)
    r->rc = nostr_nip55l_resolve_npub(r->old_npub, &r->result);
  else if (r->op == READ_PUBLIC)
    r->rc = nostr_nip55l_get_public_key(&r->result);
  else
    r->rc = nostr_nip55l_sign_event_json(
        "{\"kind\":1,\"created_at\":1,\"tags\":[],\"content\":\"cache ownership\"}",
        r->old_npub, "cache-test", &r->result);
  return NULL;
}
static gpointer mutate_cache(gpointer data) {
  Race *r = data;
  if (r->clear)
    g_assert_cmpint(nostr_nip55l_clear_key(r->old_npub), ==, NOSTR_SIGNER_ERROR_NOT_FOUND);
  else
    signer_cache_set(NEW_KEY, r->new_npub);
  return NULL;
}
static void await_flag(Race *r, gboolean *flag) {
  struct timespec bound;
  g_assert_cmpint(clock_gettime(CLOCK_REALTIME, &bound), ==, 0);
  bound.tv_sec += 20;
  pthread_mutex_lock(&r->mutex);
  while (!*flag) g_assert_cmpint(pthread_cond_timedwait(&r->cond, &r->mutex, &bound), ==, 0);
  pthread_mutex_unlock(&r->mutex);
}
static void race_read_mutation(gboolean clear, ReadOp op) {
  char *old_npub = NULL, *new_npub = NULL;
  g_assert_cmpint(sk_hex_to_npub(OLD_KEY, &old_npub), ==, 0);
  g_assert_cmpint(sk_hex_to_npub(NEW_KEY, &new_npub), ==, 0);
  signer_cache_set(OLD_KEY, old_npub);
  Race r = {.mutex=PTHREAD_MUTEX_INITIALIZER, .cond=PTHREAD_COND_INITIALIZER,
            .clear=clear, .op=op, .old_npub=old_npub, .new_npub=new_npub};
  cache_copy_hook = copy_barrier; cache_mutation_hook = mutation_barrier; cache_hook_data = &r;
  pthread_t reader, writer;
  g_assert_cmpint(pthread_create(&reader, NULL, read_cache, &r), ==, 0);
  await_flag(&r, &r.copying);
  g_assert_cmpint(pthread_create(&writer, NULL, mutate_cache, &r), ==, 0);
  await_flag(&r, &r.mutation_attempted);
  pthread_mutex_lock(&r.mutex); r.release = TRUE; pthread_cond_broadcast(&r.cond); pthread_mutex_unlock(&r.mutex);
  g_assert_cmpint(pthread_join(reader, NULL), ==, 0); g_assert_cmpint(pthread_join(writer, NULL), ==, 0);
  cache_copy_hook = NULL; cache_mutation_hook = NULL; cache_hook_data = NULL;
  g_assert_cmpint(r.rc, ==, 0); g_assert_nonnull(r.result);
  if (op == READ_SIGN) {
    NostrEvent *event = nostr_event_new();
    g_assert_cmpint(nostr_event_deserialize(event, r.result), ==, 0);
    g_assert_true(nostr_event_check_signature(event));
    char *pubkey = nostr_key_get_public(OLD_KEY);
    g_assert_cmpstr(nostr_event_get_pubkey(event), ==, pubkey);
    free(pubkey); nostr_event_free(event);
  } else g_assert_cmpstr(r.result, ==, old_npub);
  char *active = NULL;
  if (clear) {
    g_assert_cmpint(nostr_nip55l_get_public_key(&active), ==, NOSTR_SIGNER_ERROR_NOT_FOUND);
    g_assert_null(active);
  } else {
    g_assert_cmpint(nostr_nip55l_get_public_key(&active), ==, 0);
    g_assert_cmpstr(active, ==, new_npub);
  }
  free(active); free(r.result); free(old_npub); free(new_npub);
  signer_cache_evict(NULL);
  pthread_mutex_destroy(&r.mutex); pthread_cond_destroy(&r.cond);
}
static void test_clear_resolve(void) { race_read_mutation(TRUE, READ_NPUB); }
static void test_store_resolve(void) { race_read_mutation(FALSE, READ_NPUB); }
static void test_clear_public(void) { race_read_mutation(TRUE, READ_PUBLIC); }
static void test_store_public(void) { race_read_mutation(FALSE, READ_PUBLIC); }
static void test_clear_sign(void) { race_read_mutation(TRUE, READ_SIGN); }
static void test_store_sign(void) { race_read_mutation(FALSE, READ_SIGN); }
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_unsetenv("NOSTR_SIGNER_SECKEY_HEX"); g_unsetenv("NOSTR_SIGNER_NSEC");
  g_test_add_func("/cache/clear-resolve", test_clear_resolve);
  g_test_add_func("/cache/store-resolve", test_store_resolve);
  g_test_add_func("/cache/clear-public", test_clear_public);
  g_test_add_func("/cache/store-public", test_store_public);
  g_test_add_func("/cache/clear-sign", test_clear_sign);
  g_test_add_func("/cache/store-sign", test_store_sign);
  return g_test_run();
}
