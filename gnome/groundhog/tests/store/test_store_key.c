/* GhStoreKey unit tests (charter §9.2 KC-1…KC-5 at the key-custody layer)
 * against the in-process FakeSecret backend, plus the production libsecret
 * backend with no session bus at all (KC-4).
 *
 * Hermetic by construction: this binary starts no process and never
 * establishes a D-Bus connection, so it cannot leave a daemon behind or race
 * GDBus's connection teardown (which, on macOS, fails the next select() with
 * EBADF and aborts a test). The "bus present, no Secret Service" case and KC-6 (a
 * real gnome-keyring) live in test_store_key_keyring.c. */
#include "fake-secret.h"

#include <glib/gstdio.h>
#include <string.h>

#define ACCOUNT_A "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90"
#define ACCOUNT_B "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0"
#define STORE_ID_1 "6f1c2a3b-4d5e-4f60-8a9b-0c1d2e3f4a5b"
#define STORE_ID_2 "7a2d3b4c-5e6f-4a71-9b0c-1d2e3f4a5b6c"

static const guint8 KEY_BYTES[GH_STORE_KEY_SIZE] = {
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
  0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

static gchar *xdg_root; /* every XDG dir of this process points below it */

static void
assert_key_error(const GhTestKeyResult *result, gint code)
{
  g_assert_error(result->error, GH_STORE_KEY_ERROR, code);
  g_assert_null(result->key);
  g_assert_null(result->store_id);
  g_assert_false(result->created);
  /* Error text may reach logs: it never names the account. */
  g_assert_null(strstr(result->error->message, ACCOUNT_A));
  g_assert_null(strstr(result->error->message, ACCOUNT_B));
}

static void
assert_good_key(const GhTestKeyResult *result)
{
  g_assert_no_error(result->error);
  g_assert_nonnull(result->key);
  g_assert_cmpuint(g_bytes_get_size(result->key), ==, GH_STORE_KEY_SIZE);
  g_assert_true(g_uuid_string_is_valid(result->store_id));
}

static GhStoreKey *
new_store_key(FakeSecret **out_fake)
{
  *out_fake = fake_secret_new();
  return gh_store_key_new(GH_STORE_KEY_BACKEND(*out_fake));
}

static FakeSecretCall *
call_at(FakeSecret *fake, guint i)
{
  GPtrArray *calls = fake_secret_calls(fake);
  g_assert_cmpuint(i, <, calls->len);
  return g_ptr_array_index(calls, i);
}

static void
test_schema_contract(void)
{
  g_assert_cmpstr(GH_STORE_KEY_SCHEMA_NAME, ==, "org.nostr.Groundhog.StoreKey");
  g_assert_cmpstr(GH_STORE_KEY_ATTR_ACCOUNT, ==, "account");
  g_assert_cmpstr(GH_STORE_KEY_ATTR_STORE_ID, ==, "store-id");
  g_assert_cmpstr(GH_STORE_KEY_ATTR_VERSION, ==, "version");
  g_assert_cmpstr(GH_STORE_KEY_VERSION, ==, "1");
  g_assert_cmpstr(GH_STORE_KEY_LABEL, ==, "Groundhog message storage key");
  g_assert_cmpint(GH_STORE_KEY_SIZE, ==, 32);
}

/* KC-1: the item uses the §3.4 schema and attributes and the secret is 32
 * random bytes; a second open reuses it. */
static void
test_first_open(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);

  GhTestKeyResult first = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&first);
  g_assert_true(first.created);
  static const guint8 zero[GH_STORE_KEY_SIZE] = { 0 };
  g_assert_true(memcmp(g_bytes_get_data(first.key, NULL), zero, sizeof zero) != 0);

  GPtrArray *items = fake_secret_items(fake);
  g_assert_cmpuint(items->len, ==, 1);
  FakeSecretItem *item = g_ptr_array_index(items, 0);
  g_assert_cmpuint(g_hash_table_size(item->attributes), ==, 3);
  g_assert_cmpstr(g_hash_table_lookup(item->attributes, "account"), ==, ACCOUNT_A);
  g_assert_cmpstr(g_hash_table_lookup(item->attributes, "store-id"), ==, first.store_id);
  g_assert_cmpstr(g_hash_table_lookup(item->attributes, "version"), ==, "1");
  g_assert_cmpstr(item->label, ==, "Groundhog message storage key");
  g_assert_true(g_bytes_equal(item->secret, first.key));

  /* One search, then one store; neither may prompt. */
  g_assert_cmpuint(fake_secret_calls(fake)->len, ==, 2);
  g_assert_cmpint(call_at(fake, 0)->op, ==, FAKE_SECRET_SEARCH);
  g_assert_cmpuint(g_hash_table_size(call_at(fake, 0)->attributes), ==, 1);
  g_assert_cmpstr(g_hash_table_lookup(call_at(fake, 0)->attributes, "account"), ==, ACCOUNT_A);
  g_assert_cmpint(call_at(fake, 1)->op, ==, FAKE_SECRET_STORE);
  g_assert_cmpint(call_at(fake, 0)->flags, ==, GH_STORE_KEY_FLAGS_NONE);
  g_assert_cmpint(call_at(fake, 1)->flags, ==, GH_STORE_KEY_FLAGS_NONE);

  GhTestKeyResult again = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&again);
  g_assert_false(again.created);
  g_assert_true(g_bytes_equal(again.key, first.key));
  g_assert_cmpstr(again.store_id, ==, first.store_id);
  g_assert_cmpuint(fake_secret_calls(fake)->len, ==, 3); /* a search, no second store */

  GhTestKeyResult looked_up = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&looked_up);
  g_assert_true(g_bytes_equal(looked_up.key, first.key));
  g_assert_cmpstr(looked_up.store_id, ==, first.store_id);
  g_assert_cmpuint(fake_secret_items(fake)->len, ==, 1);

  gh_test_key_result_clear(&first);
  gh_test_key_result_clear(&again);
  gh_test_key_result_clear(&looked_up);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* KC-1/KC-5: each account gets its own random key and store id; a recreated
 * key is new; the account is canonicalized to lowercase. */
static void
test_keys_are_random_and_per_account(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);

  GhTestKeyResult a = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  GhTestKeyResult b = gh_test_lookup_or_create(store_key, ACCOUNT_B, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&a);
  assert_good_key(&b);
  g_assert_true(a.created && b.created);
  g_assert_false(g_bytes_equal(a.key, b.key));
  g_assert_cmpstr(a.store_id, !=, b.store_id);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 1);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_B), ==, 1);

  gchar *upper = g_ascii_strup(ACCOUNT_A, -1);
  GhTestKeyResult a_upper = gh_test_lookup(store_key, upper, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&a_upper);
  g_assert_true(g_bytes_equal(a_upper.key, a.key));
  g_assert_cmpstr(g_hash_table_lookup(call_at(fake, fake_secret_calls(fake)->len - 1)->attributes,
                                      "account"), ==, ACCOUNT_A);

  g_assert_true(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL));
  GhTestKeyResult a2 = gh_test_lookup_or_create(store_key, upper, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&a2);
  g_assert_true(a2.created);
  g_assert_false(g_bytes_equal(a2.key, a.key));
  g_assert_cmpstr(a2.store_id, !=, a.store_id);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 1);

  GhTestKeyResult b_again = gh_test_lookup(store_key, ACCOUNT_B, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&b_again);
  g_assert_true(g_bytes_equal(b_again.key, b.key));
  g_assert_cmpstr(b_again.store_id, ==, b.store_id);

  g_free(upper);
  gh_test_key_result_clear(&a);
  gh_test_key_result_clear(&b);
  gh_test_key_result_clear(&a_upper);
  gh_test_key_result_clear(&a2);
  gh_test_key_result_clear(&b_again);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* KC-1: a failed item store returns no key (so no database can be created)
 * and leaves nothing behind. */
static void
test_failed_store_returns_nothing(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  fake_secret_set_store_fails(fake, TRUE);

  GhTestKeyResult result = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&result, GH_STORE_KEY_ERROR_FAILED);
  g_assert_cmpuint(fake_secret_items(fake)->len, ==, 0);

  fake_secret_set_store_fails(fake, FALSE);
  GhTestKeyResult after = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&after, GH_STORE_KEY_ERROR_NOT_FOUND);

  gh_test_key_result_clear(&result);
  gh_test_key_result_clear(&after);
  g_object_unref(store_key);
  g_object_unref(fake);
}

static void
assert_no_interactive_calls(FakeSecret *fake)
{
  GPtrArray *calls = fake_secret_calls(fake);
  g_assert_cmpuint(calls->len, >, 0);
  for (guint i = 0; i < calls->len; i++)
    g_assert_cmpint(call_at(fake, i)->flags & GH_STORE_KEY_FLAGS_INTERACTIVE, ==, 0);
  g_assert_cmpuint(fake_secret_prompts(fake), ==, 0);
}

/* KC-2: with the keyring locked, background calls never prompt, report
 * LOCKED, create nothing and destroy nothing. */
static void
test_locked_background(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  GhTestKeyResult created = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&created);
  fake_secret_set_locked(fake, TRUE);
  fake_secret_clear_calls(fake);

  GhTestKeyResult lookup = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&lookup, GH_STORE_KEY_ERROR_LOCKED);
  GhTestKeyResult open = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&open, GH_STORE_KEY_ERROR_LOCKED);
  GhTestKeyResult fresh = gh_test_lookup_or_create(store_key, ACCOUNT_B, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&fresh, GH_STORE_KEY_ERROR_LOCKED);
  GError *error = NULL;
  g_assert_false(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, &error));
  g_assert_error(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_LOCKED);
  g_clear_error(&error);

  assert_no_interactive_calls(fake);
  g_assert_true(fake_secret_get_locked(fake));
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 1);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_B), ==, 0);
  for (guint i = 0; i < fake_secret_calls(fake)->len; i++)
    g_assert_cmpint(call_at(fake, i)->op, !=, FAKE_SECRET_STORE);

  gh_test_key_result_clear(&created);
  gh_test_key_result_clear(&lookup);
  gh_test_key_result_clear(&open);
  gh_test_key_result_clear(&fresh);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* KC-2 interactive half: a user-present call may prompt once; a declined
 * prompt stays LOCKED, an accepted one returns the key. */
static void
test_locked_interactive(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  GhTestKeyResult created = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&created);
  fake_secret_set_locked(fake, TRUE);

  fake_secret_set_unlock_accepted(fake, FALSE);
  GhTestKeyResult declined = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_INTERACTIVE, NULL);
  assert_key_error(&declined, GH_STORE_KEY_ERROR_LOCKED);
  g_assert_cmpuint(fake_secret_prompts(fake), ==, 1);

  fake_secret_set_unlock_accepted(fake, TRUE);
  GhTestKeyResult accepted = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_INTERACTIVE, NULL);
  assert_good_key(&accepted);
  g_assert_true(g_bytes_equal(accepted.key, created.key));
  g_assert_cmpuint(fake_secret_prompts(fake), ==, 2);

  /* A first open while locked may prompt too, then stores. */
  fake_secret_set_locked(fake, TRUE);
  GhTestKeyResult b = gh_test_lookup_or_create(store_key, ACCOUNT_B, GH_STORE_KEY_FLAGS_INTERACTIVE, NULL);
  assert_good_key(&b);
  g_assert_true(b.created);
  g_assert_cmpuint(fake_secret_prompts(fake), ==, 3);

  gh_test_key_result_clear(&created);
  gh_test_key_result_clear(&declined);
  gh_test_key_result_clear(&accepted);
  gh_test_key_result_clear(&b);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* KC-3: a missing item is NOT_FOUND from lookup (STORE_KEY_MISSING), which
 * never creates one; destroying nothing succeeds. */
static void
test_missing_item(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  GhTestKeyResult result = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&result, GH_STORE_KEY_ERROR_NOT_FOUND);
  g_assert_cmpuint(fake_secret_calls(fake)->len, ==, 1);
  g_assert_cmpint(call_at(fake, 0)->op, ==, FAKE_SECRET_SEARCH);
  g_assert_cmpuint(fake_secret_items(fake)->len, ==, 0);

  GError *error = NULL;
  g_assert_true(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, &error));
  g_assert_no_error(error);

  gh_test_key_result_clear(&result);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* KC-4: without a Secret Service every operation is UNAVAILABLE; nothing is
 * kept anywhere else. */
static void
test_unavailable(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  fake_secret_set_available(fake, FALSE);
  for (guint interactive = 0; interactive < 2; interactive++) {
    GhStoreKeyFlags flags = interactive ? GH_STORE_KEY_FLAGS_INTERACTIVE : GH_STORE_KEY_FLAGS_NONE;
    GhTestKeyResult lookup = gh_test_lookup(store_key, ACCOUNT_A, flags, NULL);
    assert_key_error(&lookup, GH_STORE_KEY_ERROR_UNAVAILABLE);
    GhTestKeyResult open = gh_test_lookup_or_create(store_key, ACCOUNT_A, flags, NULL);
    assert_key_error(&open, GH_STORE_KEY_ERROR_UNAVAILABLE);
    GError *error = NULL;
    g_assert_false(gh_test_destroy(store_key, ACCOUNT_A, flags, &error));
    g_assert_error(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_UNAVAILABLE);
    g_clear_error(&error);
    gh_test_key_result_clear(&lookup);
    gh_test_key_result_clear(&open);
  }
  g_assert_cmpuint(fake_secret_items(fake)->len, ==, 0);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* KC-5 (key layer): a service returning another account's item is refused,
 * and a first open does not paper over it. */
static void
test_foreign_item_refused(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  fake_secret_set_ignore_query(fake, TRUE);
  fake_secret_add_item(fake, GH_STORE_KEY_LABEL, KEY_BYTES, sizeof KEY_BYTES,
                       "account", ACCOUNT_B, "store-id", STORE_ID_1, "version", "1", NULL);

  GhTestKeyResult lookup = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&lookup, GH_STORE_KEY_ERROR_INVALID);
  GhTestKeyResult open = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_key_error(&open, GH_STORE_KEY_ERROR_INVALID);
  g_assert_cmpuint(fake_secret_items(fake)->len, ==, 1);

  gh_test_key_result_clear(&lookup);
  gh_test_key_result_clear(&open);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* The returned store id is the item's, so the store can compare it with
 * meta.store_id (KC-5). */
static void
test_existing_item_store_id(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  fake_secret_add_item(fake, GH_STORE_KEY_LABEL, KEY_BYTES, sizeof KEY_BYTES,
                       "account", ACCOUNT_A, "store-id", STORE_ID_1, "version", "1", NULL);
  GhTestKeyResult result = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
  assert_good_key(&result);
  g_assert_false(result.created);
  g_assert_cmpstr(result.store_id, ==, STORE_ID_1);
  g_assert_cmpmem(g_bytes_get_data(result.key, NULL), GH_STORE_KEY_SIZE, KEY_BYTES, sizeof KEY_BYTES);
  gh_test_key_result_clear(&result);
  g_object_unref(store_key);
  g_object_unref(fake);
}

typedef struct {
  const gchar *name;
  gsize secret_len;
  const gchar *store_id; /* NULL: attribute absent */
  const gchar *version;  /* NULL: attribute absent */
  const gchar *second;   /* version of a second item of the account, or NULL */
  gint code;
} BadItemCase;

static void
test_unusable_items(void)
{
  static const BadItemCase cases[] = {
    { "short secret", 16, STORE_ID_1, "1", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "long secret", 33, STORE_ID_1, "1", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "empty secret", 0, STORE_ID_1, "1", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "no store id", 32, NULL, "1", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "bad store id", 32, "not-a-uuid", "1", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "no version", 32, STORE_ID_1, NULL, NULL, GH_STORE_KEY_ERROR_INVALID },
    { "version 0", 32, STORE_ID_1, "0", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "garbled version", 32, STORE_ID_1, "1a", NULL, GH_STORE_KEY_ERROR_INVALID },
    { "version 2", 32, STORE_ID_1, "2", NULL, GH_STORE_KEY_ERROR_NEWER_VERSION },
    { "version 10", 32, STORE_ID_1, "10", NULL, GH_STORE_KEY_ERROR_NEWER_VERSION },
    { "two items", 32, STORE_ID_1, "1", "1", GH_STORE_KEY_ERROR_INVALID },
    /* An interrupted migration by a newer build: never looks destroyable. */
    { "newer beside current", 32, STORE_ID_1, "1", "2", GH_STORE_KEY_ERROR_NEWER_VERSION },
  };
  guint8 secret[64] = { 0 };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    const BadItemCase *c = &cases[i];
    g_test_message("case: %s", c->name);
    FakeSecret *fake;
    GhStoreKey *store_key = new_store_key(&fake);
    fake_secret_add_item(fake, GH_STORE_KEY_LABEL, secret, c->secret_len, "account", ACCOUNT_A,
                         NULL);
    FakeSecretItem *item = g_ptr_array_index(fake_secret_items(fake), 0);
    if (c->store_id)
      g_hash_table_insert(item->attributes, g_strdup("store-id"), g_strdup(c->store_id));
    if (c->version)
      g_hash_table_insert(item->attributes, g_strdup("version"), g_strdup(c->version));
    if (c->second)
      fake_secret_add_item(fake, GH_STORE_KEY_LABEL, secret, 32, "account", ACCOUNT_A,
                           "store-id", STORE_ID_2, "version", c->second, NULL);
    guint seeded = fake_secret_items(fake)->len;

    GhTestKeyResult lookup = gh_test_lookup(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
    assert_key_error(&lookup, c->code);
    GhTestKeyResult open = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL);
    assert_key_error(&open, c->code);
    g_assert_cmpuint(fake_secret_items(fake)->len, ==, seeded);
    for (guint j = 0; j < fake_secret_calls(fake)->len; j++)
      g_assert_cmpint(call_at(fake, j)->op, ==, FAKE_SECRET_SEARCH);

    /* Forget still removes whatever is there. */
    g_assert_true(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL));
    g_assert_cmpuint(fake_secret_items(fake)->len, ==, 0);

    gh_test_key_result_clear(&lookup);
    gh_test_key_result_clear(&open);
    g_object_unref(store_key);
    g_object_unref(fake);
  }
}

static void
test_invalid_account(void)
{
  static const gchar *bad[] = {
    "",
    "abc",
    /* 63 and 65 characters */
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f9",
    "a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f900",
    "g1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6d7e8f90",
    "npub1sg6plzptd64u62a878hep2kev88swjh3tw00gjsfl8f237lmu63q0uf63m",
  };
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    GhTestKeyResult lookup = gh_test_lookup(store_key, bad[i], GH_STORE_KEY_FLAGS_NONE, NULL);
    g_assert_error(lookup.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_null(lookup.key);
    GhTestKeyResult open = gh_test_lookup_or_create(store_key, bad[i], GH_STORE_KEY_FLAGS_NONE, NULL);
    g_assert_error(open.error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    GError *error = NULL;
    g_assert_false(gh_test_destroy(store_key, bad[i], GH_STORE_KEY_FLAGS_NONE, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    gh_test_key_result_clear(&lookup);
    gh_test_key_result_clear(&open);
  }
  g_assert_cmpuint(fake_secret_calls(fake)->len, ==, 0);
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* Forget removes every item of the account (any store id or version) and no
 * other account's; a locked keyring needs the user. */
static void
test_destroy(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  fake_secret_add_item(fake, GH_STORE_KEY_LABEL, KEY_BYTES, 32, "account", ACCOUNT_A,
                       "store-id", STORE_ID_1, "version", "1", NULL);
  fake_secret_add_item(fake, GH_STORE_KEY_LABEL, KEY_BYTES, 32, "account", ACCOUNT_A,
                       "store-id", STORE_ID_2, "version", "2", NULL);
  fake_secret_add_item(fake, GH_STORE_KEY_LABEL, KEY_BYTES, 32, "account", ACCOUNT_B,
                       "store-id", STORE_ID_1, "version", "1", NULL);

  fake_secret_set_locked(fake, TRUE);
  fake_secret_set_unlock_accepted(fake, FALSE);
  GError *error = NULL;
  g_assert_false(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_INTERACTIVE, &error));
  g_assert_error(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_LOCKED);
  g_clear_error(&error);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 2);

  fake_secret_set_unlock_accepted(fake, TRUE);
  g_assert_true(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_INTERACTIVE, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 0);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_B), ==, 1);
  GPtrArray *calls = fake_secret_calls(fake);
  FakeSecretCall *last = g_ptr_array_index(calls, calls->len - 1);
  g_assert_cmpint(last->op, ==, FAKE_SECRET_CLEAR);
  g_assert_cmpuint(g_hash_table_size(last->attributes), ==, 1);
  g_assert_cmpstr(g_hash_table_lookup(last->attributes, "account"), ==, ACCOUNT_A);

  g_assert_true(gh_test_destroy(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL));
  g_object_unref(store_key);
  g_object_unref(fake);
}

/* Operations on one account reach the service one at a time and in call
 * order, so concurrent first opens share one key and a destroy queued behind
 * them really removes it; other accounts are not held up. */
static void
test_per_account_order(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);
  fake_secret_set_hold(fake, TRUE);

  GAsyncResult *r1 = NULL, *r2 = NULL, *r3 = NULL, *r4 = NULL, *r5 = NULL;
  gh_store_key_lookup_or_create_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL,
                                      gh_test_store_result, &r1);
  gh_store_key_lookup_or_create_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL,
                                      gh_test_store_result, &r2);
  gh_store_key_destroy_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL,
                             gh_test_store_result, &r3);
  gh_store_key_lookup_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL,
                            gh_test_store_result, &r4);
  gh_store_key_lookup_or_create_async(store_key, ACCOUNT_B, GH_STORE_KEY_FLAGS_NONE, NULL,
                                      gh_test_store_result, &r5);
  gh_test_run_until_idle();
  g_assert_cmpuint(fake_secret_pending_for(fake, ACCOUNT_A), ==, 1);
  g_assert_cmpuint(fake_secret_pending_for(fake, ACCOUNT_B), ==, 1);

  while (fake_secret_pending(fake) > 0) {
    g_assert_cmpuint(fake_secret_pending_for(fake, ACCOUNT_A), <=, 1);
    g_assert_cmpuint(fake_secret_pending_for(fake, ACCOUNT_B), <=, 1);
    g_assert_true(fake_secret_release(fake));
    gh_test_run_until_idle();
  }
  g_assert_nonnull(r1);
  g_assert_nonnull(r2);
  g_assert_nonnull(r3);
  g_assert_nonnull(r4);
  g_assert_nonnull(r5);

  GhTestKeyResult first = { 0 }, second = { 0 }, after = { 0 }, other = { 0 };
  first.key = gh_store_key_lookup_or_create_finish(store_key, r1, &first.store_id, &first.created,
                                                   &first.error);
  second.key = gh_store_key_lookup_or_create_finish(store_key, r2, &second.store_id,
                                                    &second.created, &second.error);
  GError *error = NULL;
  g_assert_true(gh_store_key_destroy_finish(store_key, r3, &error));
  g_assert_no_error(error);
  after.key = gh_store_key_lookup_finish(store_key, r4, &after.store_id, &after.error);
  other.key = gh_store_key_lookup_or_create_finish(store_key, r5, &other.store_id, &other.created,
                                                   &other.error);
  assert_good_key(&first);
  assert_good_key(&second);
  g_assert_true(first.created);
  g_assert_false(second.created);
  g_assert_true(g_bytes_equal(first.key, second.key));
  g_assert_cmpstr(first.store_id, ==, second.store_id);
  assert_key_error(&after, GH_STORE_KEY_ERROR_NOT_FOUND);
  assert_good_key(&other);
  g_assert_true(other.created);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 0);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_B), ==, 1);

  /* Account A's calls, in order: search+store, search, clear, search. */
  static const FakeSecretOp expected[] = {
    FAKE_SECRET_SEARCH, FAKE_SECRET_STORE, FAKE_SECRET_SEARCH, FAKE_SECRET_CLEAR,
    FAKE_SECRET_SEARCH,
  };
  guint n = 0;
  for (guint i = 0; i < fake_secret_calls(fake)->len; i++) {
    FakeSecretCall *call = call_at(fake, i);
    if (g_strcmp0(g_hash_table_lookup(call->attributes, "account"), ACCOUNT_A) != 0)
      continue;
    g_assert_cmpuint(n, <, G_N_ELEMENTS(expected));
    g_assert_cmpint(call->op, ==, expected[n++]);
  }
  g_assert_cmpuint(n, ==, G_N_ELEMENTS(expected));

  GAsyncResult *results[] = { r1, r2, r3, r4, r5 };
  for (guint i = 0; i < G_N_ELEMENTS(results); i++)
    g_object_unref(results[i]);
  gh_test_key_result_clear(&first);
  gh_test_key_result_clear(&second);
  gh_test_key_result_clear(&after);
  gh_test_key_result_clear(&other);
  g_object_unref(store_key);
  g_object_unref(fake);
}

static void
test_cancelled(void)
{
  FakeSecret *fake;
  GhStoreKey *store_key = new_store_key(&fake);

  GCancellable *before = g_cancellable_new();
  g_cancellable_cancel(before);
  GhTestKeyResult early = gh_test_lookup_or_create(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, before);
  g_assert_error(early.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_null(early.key);
  g_assert_cmpuint(fake_secret_calls(fake)->len, ==, 0);

  /* Cancelled while queued behind another operation on the account. */
  fake_secret_set_hold(fake, TRUE);
  GCancellable *queued = g_cancellable_new();
  GAsyncResult *r1 = NULL, *r2 = NULL;
  gh_store_key_lookup_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, NULL,
                            gh_test_store_result, &r1);
  gh_store_key_lookup_or_create_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, queued,
                                      gh_test_store_result, &r2);
  gh_test_run_until_idle();
  g_assert_cmpuint(fake_secret_pending(fake), ==, 1);
  g_cancellable_cancel(queued);
  /* It completes at once, while the operation ahead of it (think: an open
   * unlock prompt) is still pending. */
  GhTestKeyResult missing = { 0 }, cancelled = { 0 };
  cancelled.key = gh_store_key_lookup_or_create_finish(store_key, gh_test_wait(&r2), NULL, NULL,
                                                       &cancelled.error);
  g_assert_null(r1);
  g_assert_cmpuint(fake_secret_pending(fake), ==, 1);
  while (fake_secret_release(fake))
    gh_test_run_until_idle();
  missing.key = gh_store_key_lookup_finish(store_key, gh_test_wait(&r1), NULL, &missing.error);
  g_assert_error(missing.error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_NOT_FOUND);
  g_assert_error(cancelled.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
  g_assert_cmpuint(fake_secret_calls(fake)->len, ==, 1);
  g_assert_cmpuint(fake_secret_items(fake)->len, ==, 0);

  /* Cancelled after the service already stored the item: the key is still
   * returned, not wiped while the keyring keeps it. */
  fake_secret_set_ignore_cancel(fake, TRUE);
  GCancellable *late = g_cancellable_new();
  GAsyncResult *r3 = NULL;
  gh_store_key_lookup_or_create_async(store_key, ACCOUNT_A, GH_STORE_KEY_FLAGS_NONE, late,
                                      gh_test_store_result, &r3);
  g_assert_true(fake_secret_release(fake)); /* search: nothing */
  gh_test_run_until_idle();
  g_assert_cmpuint(fake_secret_pending(fake), ==, 1); /* the store, in flight */
  g_cancellable_cancel(late);
  g_assert_true(fake_secret_release(fake));
  GhTestKeyResult stored = { 0 };
  stored.key = gh_store_key_lookup_or_create_finish(store_key, gh_test_wait(&r3), &stored.store_id,
                                                    &stored.created, &stored.error);
  assert_good_key(&stored);
  g_assert_true(stored.created);
  g_assert_cmpuint(fake_secret_count(fake, ACCOUNT_A), ==, 1);
  gh_test_key_result_clear(&stored);
  g_object_unref(r3);
  g_object_unref(late);

  g_object_unref(r1);
  g_object_unref(r2);
  g_object_unref(before);
  g_object_unref(queued);
  gh_test_key_result_clear(&early);
  gh_test_key_result_clear(&missing);
  gh_test_key_result_clear(&cancelled);
  g_object_unref(store_key);
  g_object_unref(fake);
}

static void
test_secret_bytes(void)
{
  guint8 source[4] = { 1, 2, 3, 4 };
  GBytes *secret = gh_store_key_secret_new(source, sizeof source);
  source[0] = 9;
  gsize size = 0;
  const guint8 *data = g_bytes_get_data(secret, &size);
  g_assert_cmpuint(size, ==, 4);
  g_assert_cmpuint(data[0], ==, 1);
  g_bytes_unref(secret);

  GBytes *empty = gh_store_key_secret_new(NULL, 0);
  g_assert_cmpuint(g_bytes_get_size(empty), ==, 0);
  g_bytes_unref(empty);
}

static void
test_sqlcipher_key(void)
{
  GBytes *key = gh_store_key_secret_new(KEY_BYTES, sizeof KEY_BYTES);
  GBytes *literal = gh_store_key_dup_sqlcipher_key(key);
  g_assert_nonnull(literal);
  gsize size = 0;
  const gchar *text = g_bytes_get_data(literal, &size);
  g_assert_cmpuint(size, ==, 67);
  g_assert_cmpint(text[size], ==, '\0');
  g_assert_cmpstr(text, ==,
                  "x'000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f'");
  g_bytes_unref(literal);
  g_bytes_unref(key);

  GBytes *short_key = gh_store_key_secret_new(KEY_BYTES, 31);
  g_assert_null(gh_store_key_dup_sqlcipher_key(short_key));
  g_bytes_unref(short_key);
}

/* KC-4 with the production backend: no session bus is reachable (main()
 * points DBUS_SESSION_BUS_ADDRESS at a socket that does not exist), so
 * libsecret cannot reach any Secret Service. Runs last, so the no-files
 * check also covers every fake-backed case before it. */
static void
test_real_backend_no_bus(void)
{
  GhStoreKey *store_key = gh_store_key_new(NULL);
  for (guint interactive = 0; interactive < 2; interactive++) {
    GhStoreKeyFlags flags = interactive ? GH_STORE_KEY_FLAGS_INTERACTIVE : GH_STORE_KEY_FLAGS_NONE;
    GhTestKeyResult open = gh_test_lookup_or_create(store_key, ACCOUNT_A, flags, NULL);
    assert_key_error(&open, GH_STORE_KEY_ERROR_UNAVAILABLE);
    GhTestKeyResult lookup = gh_test_lookup(store_key, ACCOUNT_A, flags, NULL);
    assert_key_error(&lookup, GH_STORE_KEY_ERROR_UNAVAILABLE);
    GError *error = NULL;
    g_assert_false(gh_test_destroy(store_key, ACCOUNT_A, flags, &error));
    g_assert_error(error, GH_STORE_KEY_ERROR, GH_STORE_KEY_ERROR_UNAVAILABLE);
    g_clear_error(&error);
    gh_test_key_result_clear(&open);
    gh_test_key_result_clear(&lookup);
  }
  g_object_unref(store_key);

  /* Never a fallback: nothing was written under any XDG directory. */
  g_assert_cmpuint(gh_test_count_files(xdg_root), ==, 0);
}

int
main(int argc, char **argv)
{
  /* Point every XDG directory at an empty private tree first, so a stray
   * write by the code under test would be caught (and never touch $HOME). */
  xdg_root = g_dir_make_tmp("gh-store-key-XXXXXX", NULL);
  g_assert_nonnull(xdg_root);
  static const gchar *vars[] = { "HOME", "XDG_DATA_HOME", "XDG_CONFIG_HOME", "XDG_CACHE_HOME",
                                 "XDG_STATE_HOME", "XDG_RUNTIME_DIR" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    gchar *dir = g_build_filename(xdg_root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
    g_free(dir);
  }
  /* An address nothing listens on: connecting fails at once, with no
   * daemon to start, wait for or clean up. */
  gchar *no_bus = g_strdup_printf("unix:path=%s/no-session-bus", xdg_root);
  g_setenv("DBUS_SESSION_BUS_ADDRESS", no_bus, TRUE);
  g_free(no_bus);

  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/store-key/schema-contract", test_schema_contract);
  g_test_add_func("/store-key/kc1/first-open", test_first_open);
  g_test_add_func("/store-key/kc1/random-per-account", test_keys_are_random_and_per_account);
  g_test_add_func("/store-key/kc1/failed-store", test_failed_store_returns_nothing);
  g_test_add_func("/store-key/kc2/locked-background", test_locked_background);
  g_test_add_func("/store-key/kc2/locked-interactive", test_locked_interactive);
  g_test_add_func("/store-key/kc3/missing-item", test_missing_item);
  g_test_add_func("/store-key/kc4/unavailable", test_unavailable);
  g_test_add_func("/store-key/kc5/foreign-item", test_foreign_item_refused);
  g_test_add_func("/store-key/kc5/existing-store-id", test_existing_item_store_id);
  g_test_add_func("/store-key/unusable-items", test_unusable_items);
  g_test_add_func("/store-key/invalid-account", test_invalid_account);
  g_test_add_func("/store-key/destroy", test_destroy);
  g_test_add_func("/store-key/per-account-order", test_per_account_order);
  g_test_add_func("/store-key/cancelled", test_cancelled);
  g_test_add_func("/store-key/secret-bytes", test_secret_bytes);
  g_test_add_func("/store-key/sqlcipher-key", test_sqlcipher_key);
  /* Its own root path so GTest runs it after every /store-key case. */
  g_test_add_func("/store-key-libsecret/kc4-no-session-bus", test_real_backend_no_bus);
  int status = g_test_run();
  gh_test_remove_tree(xdg_root);
  g_free(xdg_root);
  return status;
}
