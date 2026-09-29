#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* memmem, dladdr, dlinfo, RTLD_DEFAULT */
#endif

#include "gh-store.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sodium.h>
#include <sqlite3.h>

#if defined(__GLIBC__)
#include <link.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#ifdef GH_STORE_TEST_HOOKS
#include "crash-harness.h"
#endif

/* Two builds share this file:
 *  - test-groundhog-store: the store compiled with GH_STORE_TEST_HOOKS (cut
 *    points, uid override); the full ST suite.
 *  - test-groundhog-store-sqlite (GH_STORE_TEST_LINK_SET): the production
 *    library in the groundhog executable's link set; ST-4 only.
 * Every test runs with isolated XDG directories and umask 022; nothing here
 * sleeps: crash tests SIGKILL at named cut points and time is a fake GhClock. */

#define ACCOUNT_A     "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e"
#define ACCOUNT_A_DIR "14565eb9af2bf616134ee37cfc13fc87" /* computed independently */
#define ACCOUNT_B     "3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d"
#define ACCOUNT_B_DIR "59b1273317e0cd907eeb523147af8b52"
#define PEER          "82341f882b6eabcd2ba7f1ef90aad961cf074af15b9ef44a09f9d2a8fbfbe6a2"
#define T0            ((gint64) 1790000000)

/* ---- Fake key provider (the Secret Service backend is G03's) ------------------ */

typedef struct {
  guint8 key[GH_STORE_KEY_SIZE];
  gchar *store_id;
} FakeItem;

typedef struct {
  GHashTable *items; /* account hex -> FakeItem */
  gboolean unavailable;
  gboolean locked;       /* only a prompting lookup gets through */
  gboolean fail_store;
  gboolean fail_destroy;
  guint n_lookup;
  guint n_store;
  guint n_destroy;
  GhStoreKeyLookupFlags last_flags;
} FakeKeys;

static void
fake_item_free(gpointer data)
{
  FakeItem *item = data;
  g_free(item->store_id);
  g_free(item);
}

static FakeKeys *
fake_keys_new(void)
{
  FakeKeys *keys = g_new0(FakeKeys, 1);
  keys->items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, fake_item_free);
  return keys;
}

static void
fake_keys_free(FakeKeys *keys)
{
  g_hash_table_unref(keys->items);
  g_free(keys);
}
G_DEFINE_AUTOPTR_CLEANUP_FUNC(FakeKeys, fake_keys_free)

static gboolean
fake_lookup(const gchar *account, GhStoreKeyLookupFlags flags, guint8 *key,
            gchar **store_id, gpointer user_data, GCancellable *cancellable,
            GError **error)
{
  FakeKeys *keys = user_data;
  (void) cancellable;
  keys->n_lookup++;
  keys->last_flags = flags;
  if (keys->unavailable) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_UNAVAILABLE, "No Secret Service");
    return FALSE;
  }
  if (keys->locked && !(flags & GH_STORE_KEY_LOOKUP_ALLOW_PROMPT)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_LOCKED, "The keyring is locked");
    return FALSE;
  }
  FakeItem *item = g_hash_table_lookup(keys->items, account);
  if (!item) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_KEY_MISSING, "No store key item");
    return FALSE;
  }
  memcpy(key, item->key, GH_STORE_KEY_SIZE);
  *store_id = g_strdup(item->store_id);
  return TRUE;
}

static gboolean
fake_store(const gchar *account, const gchar *store_id, const guint8 *key,
           gpointer user_data, GCancellable *cancellable, GError **error)
{
  FakeKeys *keys = user_data;
  (void) cancellable;
  keys->n_store++;
  if (keys->unavailable || keys->fail_store) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "The keyring refused the item");
    return FALSE;
  }
  FakeItem *item = g_new0(FakeItem, 1);
  memcpy(item->key, key, GH_STORE_KEY_SIZE);
  item->store_id = g_strdup(store_id);
  g_hash_table_replace(keys->items, g_strdup(account), item);
  return TRUE;
}

static gboolean
fake_destroy(const gchar *account, gpointer user_data, GCancellable *cancellable,
             GError **error)
{
  FakeKeys *keys = user_data;
  (void) cancellable;
  keys->n_destroy++;
  if (keys->fail_destroy) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_LOCKED, "The keyring is locked");
    return FALSE;
  }
  g_hash_table_remove(keys->items, account);
  return TRUE;
}

static const GhStoreKeyProvider fake_provider = { fake_lookup, fake_store, fake_destroy };

/* ---- Shared helpers ------------------------------------------------------------ */

static GhStoreConfig
config_for(FakeKeys *keys, const gchar *account, GhClock *clock)
{
  GhStoreConfig config = { NULL, account, &fake_provider, keys, clock };
  return config;
}

static GhStore *
open_ok(FakeKeys *keys, const gchar *account, GhClock *clock, GhStoreOpenFlags flags)
{
  GhStoreConfig config = config_for(keys, account, clock);
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open(&config, flags, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

typedef struct {
  gchar *rumor;
  gchar *wrap;
  gchar *raw;
  GhStoreMessage message;
} TestMessage;

static const gchar *const test_participants[] = { PEER, ACCOUNT_A, NULL };

/* An incoming NIP-17 message; ids derive from the seeds. */
static void
test_message_init(TestMessage *t, const gchar *conversation, const gchar *seed,
                  const gchar *wrap_seed, gint64 created_at, const gchar *body)
{
  g_autofree gchar *wrap = g_strconcat(wrap_seed ? wrap_seed : seed, "/wrap", NULL);
  t->rumor = hex_of(seed);
  t->wrap = hex_of(wrap);
  t->raw = g_strdup_printf("{\"id\":\"%s\",\"kind\":14,\"content\":\"%s\"}", t->rumor,
                           body ? body : "");
  t->message = (GhStoreMessage) {
    .backend = GH_STORE_BACKEND_NIP17,
    .backend_key = conversation,
    .backend_msg_id = t->rumor,
    .wrap_id = t->wrap,
    .sender_pubkey = PEER,
    .kind = 14,
    .created_at = created_at,
    .direction = GH_STORE_DIRECTION_IN,
    .body = body,
    .raw_json = t->raw,
    .participants = test_participants,
    .unread = TRUE,
  };
}

static void
test_message_clear(TestMessage *t)
{
  g_free(t->rumor);
  g_free(t->wrap);
  g_free(t->raw);
}

static GhStoreAdmitResult
admit_ok(GhStore *store, const GhStoreMessage *message)
{
  GhStoreAdmitResult result = GH_STORE_ADMIT_DUPLICATE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_admit(store, message, &result, NULL, &error));
  g_assert_no_error(error);
  return result;
}

static GhStoreAdmitResult
admit_text(GhStore *store, const gchar *conversation, const gchar *seed,
           gint64 created_at, const gchar *body, gint64 expires_at)
{
  TestMessage t;
  test_message_init(&t, conversation, seed, NULL, created_at, body);
  t.message.expires_at = expires_at;
  GhStoreAdmitResult result = admit_ok(store, &t.message);
  test_message_clear(&t);
  return result;
}

/* ---- ST-4: one SQLite implementation, SQLCipher ------------------------------------ */

typedef struct {
  const gchar *object; /* skip this object when listing others */
  GString *others;
} SqliteImages;

static void
note_sqlite_image(SqliteImages *images, const gchar *name, void *handle)
{
  void *symbol = dlsym(handle, "sqlite3_open_v2");
  Dl_info info;
  if (symbol && dladdr(symbol, &info) && info.dli_fname &&
      g_strcmp0(info.dli_fname, name) == 0 && g_strcmp0(name, images->object) != 0)
    g_string_append_printf(images->others, "%s%s", images->others->len ? ", " : "", name);
}

#if defined(__GLIBC__)
static int
note_elf_object(struct dl_phdr_info *info, size_t size, void *data)
{
  (void) size;
  if (!info->dlpi_name || !*info->dlpi_name)
    return 0;
  void *handle = dlopen(info->dlpi_name, RTLD_LAZY | RTLD_NOLOAD);
  if (handle) {
    note_sqlite_image(data, info->dlpi_name, handle);
    dlclose(handle);
  }
  return 0;
}
#endif

/* Other loaded objects that define their own sqlite3_open_v2 (informational:
 * on ELF they are shadowed by the global scope, on Mach-O they are private to
 * the images that linked them). */
static gchar *
describe_other_sqlite_images(const gchar *object)
{
  SqliteImages images = { object, g_string_new(NULL) };
#if defined(__GLIBC__)
  dl_iterate_phdr(note_elf_object, &images);
#elif defined(__APPLE__)
  for (uint32_t i = 0; i < _dyld_image_count(); i++) {
    const char *name = _dyld_get_image_name(i);
    void *handle = name ? dlopen(name, RTLD_LAZY | RTLD_NOLOAD) : NULL;
    if (handle) {
      note_sqlite_image(&images, name, handle);
      dlclose(handle);
    }
  }
#endif
  if (images.others->len == 0)
    g_string_append(images.others, "none");
  return g_string_free(images.others, FALSE);
}

static void
test_st4_single_sqlite(void)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_check_sqlite(&error));
  g_assert_no_error(error);
  g_autofree gchar *description = gh_store_describe_sqlite();
  g_assert_true(g_str_has_prefix(description, "SQLCipher "));

  /* This binary's own references, and (ELF) the global scope, agree. */
  Dl_info open_info, key_info;
  g_assert_true(dladdr((const void *) sqlite3_open_v2, &open_info));
  g_assert_true(dladdr((const void *) sqlite3_key, &key_info));
  g_assert_true(open_info.dli_fbase == key_info.dli_fbase);
#if defined(__ELF__)
  const char *names[] = { "sqlite3_open_v2", "sqlite3_prepare_v2", "sqlite3_step",
                          "sqlite3_key", "sqlite3_key_v2" };
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    Dl_info info;
    void *address = dlsym(RTLD_DEFAULT, names[i]);
    g_assert_nonnull(address);
    g_assert_true(dladdr(address, &info));
    g_assert_true(info.dli_fbase == open_info.dli_fbase);
  }
#endif

  sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_prepare_v2(db, "PRAGMA cipher_version", -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  g_assert_nonnull(sqlite3_column_text(stmt, 0));
  g_assert_cmpstr((const char *) sqlite3_column_text(stmt, 0), !=, "");
  sqlite3_finalize(stmt);
  sqlite3_close(db);

  g_autofree gchar *others = describe_other_sqlite_images(open_info.dli_fname);
  g_test_message("ST-4: %s; other SQLite images mapped: %s", description, others);
}

#ifdef GH_STORE_TEST_LINK_SET
/* ---- ST-4 in the real executable's link set ------------------------------------------ */

static void
test_st4_store_round_trip(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(store, "link-set", "round-trip", T0, "hello", 0), ==,
                  GH_STORE_ADMIT_STORED);
  gh_store_close(store);
  store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE);
  g_assert_cmpint(admit_text(store, "link-set", "round-trip", T0, "hello", 0), ==,
                  GH_STORE_ADMIT_DUPLICATE);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_forget(store, NULL, &error));
  g_assert_no_error(error);
}

#if defined(__GLIBC__)
/* Path of an installed shared library, found the way the loader finds it. */
static gchar *
library_path(const gchar *soname)
{
  void *handle = dlopen(soname, RTLD_LAZY | RTLD_LOCAL);
  if (!handle)
    return NULL;
  struct link_map *map = NULL;
  g_assert_cmpint(dlinfo(handle, RTLD_DI_LINKMAP, &map), ==, 0);
  gchar *path = g_strdup(map->l_name);
  dlclose(handle);
  return path;
}

/* Position of the first entry whose basename starts with @prefix in the
 * loader's global scope ("scope 0" of the main program), or -1. */
static gint
scope_position(GBytes *loader_output, const gchar *prefix, gint *out_length)
{
  gsize len = 0;
  const gchar *data = g_bytes_get_data(loader_output, &len);
  g_autofree gchar *text = g_strndup(data, len);
  const gchar *scope0 = strstr(text, " scope 0:");
  g_assert_nonnull(scope0);
  const gchar *end = strchr(scope0, '\n');
  g_autofree gchar *line = end ? g_strndup(scope0, (gsize) (end - scope0)) : g_strdup(scope0);
  g_auto(GStrv) entries = g_strsplit_set(line + strlen(" scope 0:"), " \t", -1);
  gint found = -1, position = 0;
  for (guint i = 0; entries[i]; i++) {
    if (!*entries[i])
      continue;
    g_autofree gchar *base = g_path_get_basename(entries[i]);
    if (found < 0 && g_str_has_prefix(base, prefix))
      found = position;
    position++;
  }
  if (out_length)
    *out_length = position;
  return found;
}

static GBytes *
run_with_env(const gchar *const *argv, const gchar *const *env, gint *exit_status)
{
  g_autoptr(GSubprocessLauncher) launcher =
    g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
  g_subprocess_launcher_unsetenv(launcher, "LD_PRELOAD");
  for (guint i = 0; env && env[i]; i += 2)
    g_subprocess_launcher_setenv(launcher, env[i], env[i + 1], TRUE);
  g_autoptr(GError) error = NULL;
  g_autoptr(GSubprocess) process = g_subprocess_launcher_spawnv(launcher, argv, &error);
  g_assert_no_error(error);
  GBytes *err = NULL;
  g_assert_true(g_subprocess_communicate(process, NULL, NULL, NULL, &err, &error));
  g_assert_no_error(error);
  g_assert_true(g_subprocess_get_if_exited(process));
  *exit_status = g_subprocess_get_exit_status(process);
  return err;
}

static gboolean
basename_has_prefix(const gchar *path, const gchar *prefix)
{
  g_autofree gchar *base = g_path_get_basename(path);
  return g_str_has_prefix(base, prefix);
}
#endif

/* The shipped groundhog binary itself: glibc's loader reports the global
 * lookup scope and every symbol binding (LD_BIND_NOW resolves all of them at
 * startup). SQLCipher must be in the scope ahead of any libsqlite3, and every
 * sqlite3_* binding must land in SQLCipher. */
static void
test_st4_groundhog_loader_scope(void)
{
#if defined(__GLIBC__)
  const gchar *argv[] = { GROUNDHOG_EXECUTABLE, "--version", NULL };
  const gchar *env[] = { "LD_DEBUG", "scopes,bindings", "LD_BIND_NOW", "1", NULL };
  gint status = -1;
  g_autoptr(GBytes) err = run_with_env(argv, env, &status);
  g_assert_cmpint(status, ==, 0);

  gsize len = 0;
  const gchar *data = g_bytes_get_data(err, &len);
  g_autofree gchar *text = g_strndup(data, len);
  g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
  guint n_bindings = 0;
  for (guint i = 0; lines[i]; i++) {
    const gchar *binding = strstr(lines[i], "binding file ");
    const gchar *symbol = binding ? strstr(binding, ": normal symbol `sqlite3_") : NULL;
    if (!symbol)
      continue;
    const gchar *to = strstr(binding, " to ");
    g_assert_nonnull(to);
    const gchar *end = strstr(to + 4, " [");
    g_assert_nonnull(end);
    g_autofree gchar *target = g_strndup(to + 4, (gsize) (end - (to + 4)));
    if (!basename_has_prefix(target, "libsqlcipher"))
      g_error("groundhog binds %s to %s, not SQLCipher",
              symbol + strlen(": normal symbol `"), target);
    n_bindings++;
  }
  gint position = 0;
  gint sqlcipher = scope_position(err, "libsqlcipher.so", &position);
  gint sqlite = scope_position(err, "libsqlite3.so", NULL);
  g_assert_cmpint(sqlcipher, >=, 0);
  if (sqlite >= 0)
    g_assert_cmpint(sqlcipher, <, sqlite);
  g_test_message("ST-4 groundhog: SQLCipher is #%d of %d in the global scope; plain "
                 "libsqlite3 %s; %u sqlite3_* bindings, all to SQLCipher",
                 sqlcipher, position, sqlite >= 0 ? "is shadowed behind it" : "is not loaded",
                 n_bindings);
#else
  g_test_skip("The glibc loader scope check needs LD_DEBUG; Mach-O binds each image "
              "to the library it linked");
#endif
}

/* Mutation check for the guard: preloading plain libsqlite3 ahead of SQLCipher
 * (what a wrong link order does) must make every store open fail closed. */
static void
test_st4_preloaded_sqlite_refused(void)
{
#if defined(__GLIBC__)
  g_autofree gchar *plain = library_path("libsqlite3.so.0");
  if (!plain) {
    g_test_skip("Plain libsqlite3.so.0 is not installed");
    return;
  }
  g_autofree gchar *self = g_file_read_link("/proc/self/exe", NULL);
  g_assert_nonnull(self);

  const gchar *argv[] = { self, "--st4-probe", NULL };
  gint status = -1;
  g_autoptr(GBytes) clean = run_with_env(argv, NULL, &status);
  g_assert_cmpint(status, ==, 0);
  const gchar *env[] = { "LD_PRELOAD", plain, NULL };
  g_autoptr(GBytes) err = run_with_env(argv, env, &status);
  gsize len = 0;
  const gchar *data = g_bytes_get_data(err, &len);
  g_autofree gchar *text = g_strndup(data, len);
  g_test_message("ST-4 mutation with LD_PRELOAD=%s: %s", plain, text);
  g_assert_cmpint(status, ==, 3);
  g_assert_nonnull(strstr(text, "resolves to"));
  g_assert_nonnull(strstr(text, plain));
#else
  g_test_skip("LD_PRELOAD interposition is an ELF concern");
#endif
}

/* The situation the charter guards against, before any Groundhog code links
 * libsoup: libsoup-3 (and through it plain libsqlite3) loaded into a process
 * that also links SQLCipher. libsqlite3 must land behind SQLCipher in the
 * global scope, and the guard and a real store round trip must still pass. */
static void
test_st4_libsoup_shadowed(void)
{
#if defined(__GLIBC__)
  g_autofree gchar *soup = library_path("libsoup-3.0.so.0");
  if (!soup) {
    g_test_skip("libsoup-3 is not installed");
    return;
  }
  g_autofree gchar *self = g_file_read_link("/proc/self/exe", NULL);
  const gchar *argv[] = { self, "--st4-probe", NULL };
  const gchar *env[] = { "LD_PRELOAD", soup, "LD_DEBUG", "scopes", NULL };
  gint status = -1;
  gint length = 0;
  g_autoptr(GBytes) err = run_with_env(argv, env, &status);
  g_assert_cmpint(status, ==, 0);
  gint sqlcipher = scope_position(err, "libsqlcipher.so", &length);
  gint sqlite = scope_position(err, "libsqlite3.so", NULL);
  gint libsoup = scope_position(err, "libsoup-3.0.so", NULL);
  g_assert_cmpint(libsoup, >=, 0);
  g_assert_cmpint(sqlite, >=, 0);
  g_assert_cmpint(sqlcipher, >=, 0);
  g_assert_cmpint(sqlcipher, <, sqlite);
  g_test_message("ST-4 with libsoup-3 loaded: libsqlite3 is #%d, shadowed behind SQLCipher "
                 "#%d of %d; guard and store round trip pass", sqlite, sqlcipher, length);
#else
  g_test_skip("Global-scope shadowing is an ELF concern");
#endif
}
#endif /* GH_STORE_TEST_LINK_SET */

#ifndef GH_STORE_TEST_LINK_SET
#ifndef GH_STORE_TEST_HOOKS
#error "The full store suite needs the store's test hooks (GH_STORE_TEST_HOOKS)"
#endif

/* ---- Full-suite helpers ------------------------------------------------------------ */

static FakeItem *
fake_item(FakeKeys *keys, const gchar *account)
{
  return g_hash_table_lookup(keys->items, account);
}

/* An item left by an earlier run: the keyring outlives a crashed process. */
static FakeItem *
fake_seed(FakeKeys *keys, const gchar *account)
{
  FakeItem *item = g_new0(FakeItem, 1);
  for (guint i = 0; i < GH_STORE_KEY_SIZE; i++)
    item->key[i] = (guint8) g_random_int();
  item->store_id = g_uuid_string_random();
  g_hash_table_replace(keys->items, g_strdup(account), item);
  return item;
}

static GError *
open_error(FakeKeys *keys, const gchar *account, GhClock *clock, GhStoreOpenFlags flags,
           gint code)
{
  GhStoreConfig config = config_for(keys, account, clock);
  GError *error = NULL;
  GhStore *store = gh_store_open(&config, flags, NULL, &error);
  if (store)
    g_error("Expected store error %d, but the store opened", code);
  g_assert_error(error, GH_STORE_ERROR, code);
  return error;
}

#define OPEN_FAILS(keys, account, clock, flags, code) \
  g_error_free(open_error((keys), (account), (clock), (flags), (code)))

static gchar *hex_printf(const gchar *format, ...) G_GNUC_PRINTF(1, 2);

static gchar *
hex_printf(const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_autofree gchar *seed = g_strdup_vprintf(format, args);
  va_end(args);
  return hex_of(seed);
}

static gint64 sql_int(GhStore *store, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gint64
sql_int(GhStore *store, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_autofree gchar *sql = g_strdup_vprintf(format, args);
  va_end(args);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL) != SQLITE_OK)
    g_error("%s: %s", sql, sqlite3_errmsg(gh_store_get_db(store)));
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gint64 value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

static gchar *sql_text(GhStore *store, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gchar *
sql_text(GhStore *store, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_autofree gchar *sql = g_strdup_vprintf(format, args);
  va_end(args);
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL) != SQLITE_OK)
    g_error("%s: %s", sql, sqlite3_errmsg(gh_store_get_db(store)));
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  const unsigned char *text = sqlite3_column_text(stmt, 0);
  gchar *value = text ? g_strdup((const gchar *) text) : NULL;
  sqlite3_finalize(stmt);
  return value;
}

static gboolean
is_seen(GhStore *store, GhStoreSeenNs ns, const gchar *id)
{
  gboolean seen = FALSE;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_seen_contains(store, ns, id, &seen, &error));
  g_assert_no_error(error);
  return seen;
}

/* The conversation id, or -1 when there is none. */
static gint64
find_conversation(GhStore *store, const gchar *key)
{
  gint64 id = 0;
  g_autoptr(GError) error = NULL;
  if (gh_store_find_conversation(store, GH_STORE_BACKEND_NIP17, key, &id, &error))
    return id;
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  return -1;
}

static gint64
ensure_conversation(GhStore *store, const gchar *key)
{
  gint64 id = 0;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_ensure_conversation(store, GH_STORE_BACKEND_NIP17, key,
                                             GH_STORE_REQUEST_ACCEPTED, &id, &error));
  g_assert_no_error(error);
  return id;
}

static gchar *
draft_of(GhStore *store, gint64 conversation)
{
  gchar *draft = NULL;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_get_draft(store, conversation, &draft, &error));
  g_assert_no_error(error);
  return draft;
}

/* Queues an outgoing message; returns the outbox id, or 0 with @error set. */
static gint64
enqueue_text(GhStore *store, gint64 conversation, const gchar *seed, const gchar *body,
             GError **error)
{
  g_autofree gchar *op_hex = hex_printf("op/%s", seed);
  g_autofree gchar *op = g_strndup(op_hex, 32);
  g_autofree gchar *rumor = hex_of(seed);
  g_autofree gchar *raw = g_strdup_printf("{\"id\":\"%s\",\"kind\":14,\"content\":\"%s\"}",
                                          rumor, body ? body : "");
  GhStoreOutgoing outgoing = { conversation, op, rumor, ACCOUNT_A, 14, T0, body, raw, NULL, 0 };
  gint64 outbox = 0;
  if (!gh_store_enqueue(store, &outgoing, &outbox, NULL, error))
    return 0;
  return outbox;
}

static const gchar *const recipient_urls[] = { "wss://inbox-a.example", "wss://inbox-b.example", NULL };
static const gchar *const own_urls[] = { "wss://own-1.example", "wss://own-2.example", NULL };

/* Two recipient wraps and a delayed self wrap, two relays each. */
static gboolean
seal_three(GhStore *store, gint64 outbox, GError **error)
{
  g_autofree gchar *id1 = hex_of("event/1");
  g_autofree gchar *id2 = hex_of("event/2");
  g_autofree gchar *id3 = hex_of("event/3");
  g_autofree gchar *json1 = g_strdup_printf("{\"id\":\"%s\",\"kind\":1059}", id1);
  g_autofree gchar *json2 = g_strdup_printf("{\"id\":\"%s\",\"kind\":1059}", id2);
  g_autofree gchar *json3 = g_strdup_printf("{\"id\":\"%s\",\"kind\":1059}", id3);
  GhStoreSealedEvent events[] = {
    { GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP, PEER, id1, json1, 0, recipient_urls },
    { GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP, ACCOUNT_B, id2, json2, 0, recipient_urls },
    { GH_STORE_OUTBOX_ROLE_SELF_WRAP, ACCOUNT_A, id3, json3, T0 + 30, own_urls },
  };
  return gh_store_seal(store, outbox, events, G_N_ELEMENTS(events), error);
}

static void
assert_integrity(GhStore *store)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_check_integrity(store, TRUE, &error));
  g_assert_no_error(error);
}

/* Every test message here is unread, so the counter must equal the stored
 * incoming messages of each conversation. */
static void
assert_unread_consistent(GhStore *store)
{
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM conversations c WHERE c.unread_count != "
                                 "(SELECT count(*) FROM messages m WHERE m.conversation_id = c.id "
                                 "AND m.direction = 0)"), ==, 0);
}

static GBytes *
file_bytes(const gchar *path)
{
  gchar *data = NULL;
  gsize len = 0;
  if (!g_file_get_contents(path, &data, &len, NULL))
    return NULL;
  return g_bytes_new_take(data, len);
}

static gboolean
bytes_contain(GBytes *bytes, const void *needle, gsize needle_len)
{
  gsize len = 0;
  const guint8 *data = g_bytes_get_data(bytes, &len);
  return needle_len <= len && memmem(data, len, needle, needle_len) != NULL;
}

static gchar *
file_sha256(const gchar *path)
{
  g_autoptr(GBytes) bytes = file_bytes(path);
  g_assert_nonnull(bytes);
  return g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, bytes);
}

static guint
path_mode(const gchar *path)
{
  struct stat st;
  g_assert_cmpint(lstat(path, &st), ==, 0);
  return st.st_mode & 07777;
}

static gboolean
path_exists(const gchar *path)
{
  struct stat st;
  return lstat(path, &st) == 0;
}

static gint64
path_size(const gchar *path)
{
  struct stat st;
  return lstat(path, &st) == 0 ? (gint64) st.st_size : -1;
}

static gint
compare_names(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *) a, *(const gchar *const *) b);
}

static gchar *
dir_listing(const gchar *path)
{
  g_autoptr(GDir) dir = g_dir_open(path, 0, NULL);
  g_assert_nonnull(dir);
  g_autoptr(GPtrArray) names = g_ptr_array_new_with_free_func(g_free);
  const gchar *name;
  while ((name = g_dir_read_name(dir)))
    g_ptr_array_add(names, g_strdup(name));
  g_ptr_array_sort(names, compare_names);
  g_ptr_array_add(names, NULL);
  return g_strjoinv(",", (gchar **) names->pdata);
}

static gchar *
account_dir(const gchar *account)
{
  g_autoptr(GError) error = NULL;
  gchar *dir = gh_store_account_dir_path(NULL, account, &error);
  g_assert_no_error(error);
  return dir;
}

static gchar *
db_path_of(const gchar *account)
{
  g_autofree gchar *dir = account_dir(account);
  return g_build_filename(dir, "store.db", NULL);
}

static gchar *
groundhog_root(void)
{
  return g_build_filename(g_get_user_data_dir(), "groundhog", NULL);
}

/* ---- Layout -------------------------------------------------------------------------- */

static void
test_layout_account_dir(void)
{
  g_autofree gchar *a = gh_store_account_dir_name(ACCOUNT_A);
  g_autofree gchar *b = gh_store_account_dir_name(ACCOUNT_B);
  g_assert_cmpstr(a, ==, ACCOUNT_A_DIR);
  g_assert_cmpstr(b, ==, ACCOUNT_B_DIR);
  g_assert_null(strstr(ACCOUNT_A, a));

  g_autoptr(GError) error = NULL;
  g_assert_null(gh_store_account_dir_path(NULL, "ABCDEF", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_null(gh_store_account_dir_path("relative/data", ACCOUNT_A, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
}

/* ---- ST-1 encrypted file ------------------------------------------------------------------ */

static void
assert_no_plaintext(const gchar *path, const gchar *canary, const FakeItem *item)
{
  g_autoptr(GBytes) bytes = file_bytes(path);
  g_assert_nonnull(bytes);
  g_assert_cmpuint(g_bytes_get_size(bytes), >, 0);
  gchar hex[2 * GH_STORE_KEY_SIZE + 1];
  for (guint i = 0; i < GH_STORE_KEY_SIZE; i++)
    g_snprintf(hex + 2 * i, 3, "%02x", item->key[i]);
  g_autofree gchar *upper = g_ascii_strup(hex, -1);

  g_assert_false(bytes_contain(bytes, canary, strlen(canary)));
  g_assert_false(bytes_contain(bytes, ACCOUNT_A, 64));
  g_assert_false(bytes_contain(bytes, PEER, 64));
  g_assert_false(bytes_contain(bytes, item->store_id, strlen(item->store_id)));
  g_assert_false(bytes_contain(bytes, item->key, GH_STORE_KEY_SIZE));
  g_assert_false(bytes_contain(bytes, hex, 2 * GH_STORE_KEY_SIZE));
  g_assert_false(bytes_contain(bytes, upper, 2 * GH_STORE_KEY_SIZE));
  g_assert_false(bytes_contain(bytes, "SQLite format 3", 15));
}

static void
test_st1_encrypted_file(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, clock, GH_STORE_OPEN_CREATE);
  g_autofree gchar *nonce = gh_store_new_op_id();
  g_autofree gchar *canary = g_strdup_printf("GROUNDHOG-CANARY-%s", nonce);
  g_autofree gchar *conversation = g_strdup_printf("%s,%s", PEER, ACCOUNT_A);

  for (guint i = 0; i < 40; i++) {
    g_autofree gchar *seed = g_strdup_printf("st1/%u", i);
    g_autofree gchar *body = g_strdup_printf("%s message %u", canary, i);
    TestMessage t;
    test_message_init(&t, conversation, seed, NULL, T0 - 100 + i, body);
    t.message.title = canary;
    g_assert_cmpint(admit_ok(store, &t.message), ==, GH_STORE_ADMIT_STORED);
    test_message_clear(&t);
  }
  gint64 id = find_conversation(store, conversation);
  g_autofree gchar *draft = g_strdup_printf("draft %s", canary);
  g_assert_true(gh_store_set_draft(store, id, draft, &error));
  g_assert_no_error(error);
  g_autofree gchar *reply = g_strdup_printf("reply %s", canary);
  gint64 outbox = enqueue_text(store, id, "st1/out", reply, &error);
  g_assert_no_error(error);
  g_assert_true(seal_three(store, outbox, &error));
  g_assert_no_error(error);

  g_autofree gchar *path = g_strdup(gh_store_get_path(store));
  g_autofree gchar *wal = g_strconcat(path, "-wal", NULL);
  g_autofree gchar *shm = g_strconcat(path, "-shm", NULL);
  /* While open, the new pages are still in the WAL. */
  g_assert_cmpint(path_size(wal), >, 0);
  g_assert_true(path_exists(shm));
  const FakeItem *item = fake_item(keys, ACCOUNT_A);
  g_autoptr(GBytes) header = file_bytes(path);
  g_assert_cmpuint(g_bytes_get_size(header), >=, 16);
  g_assert_cmpint(memcmp(g_bytes_get_data(header, NULL), "SQLite format 3", 16), !=, 0);
  assert_no_plaintext(path, canary, item);
  assert_no_plaintext(wal, canary, item);
  assert_no_plaintext(shm, canary, item);

  gh_store_close(store);
  g_assert_false(path_exists(wal));
  assert_no_plaintext(path, canary, item);
}

static void
test_st1_unreadable_without_key(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(store, "c", "secret", T0, "GROUNDHOG-CANARY-no-key", 0), ==,
                  GH_STORE_ADMIT_STORED);
  g_autofree gchar *path = g_strdup(gh_store_get_path(store));
  gh_store_close(store);
  const FakeItem *item = fake_item(keys, ACCOUNT_A);
  gchar hex[2 * GH_STORE_KEY_SIZE + 1];
  for (guint i = 0; i < GH_STORE_KEY_SIZE; i++)
    g_snprintf(hex + 2 * i, 3, "%02x", item->key[i]);

  sqlite3 *db = NULL;
  g_assert_cmpint(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", NULL, NULL, NULL) & 0xff,
                  ==, SQLITE_NOTADB);
  sqlite3_close(db);

  /* The same 32 bytes as a passphrase (SQLCipher's PBKDF2 path) do not open it. */
  g_assert_cmpint(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_key(db, item->key, GH_STORE_KEY_SIZE), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_exec(db, "SELECT count(*) FROM sqlite_master", NULL, NULL, NULL) & 0xff,
                  ==, SQLITE_NOTADB);
  sqlite3_close(db);

  /* The charter's literal raw-key statement opens it: the key is used raw. */
  g_autofree gchar *pragma = g_strdup_printf("PRAGMA key = \"x'%s'\"", hex);
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_exec(db, pragma, NULL, NULL, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key = 'account_pubkey'",
                                     -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  g_assert_cmpstr((const char *) sqlite3_column_text(stmt, 0), ==, ACCOUNT_A);
  sqlite3_finalize(stmt);
  sqlite3_close(db);
}

/* ---- ST-2 wrong key ----------------------------------------------------------------------- */

static void
test_st2_wrong_key(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(store, "c", "m", T0, "hello", 0), ==, GH_STORE_ADMIT_STORED);
  gh_store_close(store);

  g_autofree gchar *dir = account_dir(ACCOUNT_A);
  g_autofree gchar *path = db_path_of(ACCOUNT_A);
  g_autofree gchar *sha = file_sha256(path);
  g_autofree gchar *listing = dir_listing(dir);
  FakeItem *item = fake_item(keys, ACCOUNT_A);
  guint8 saved[GH_STORE_KEY_SIZE];
  memcpy(saved, item->key, sizeof saved);
  item->key[7] ^= 0x5a;

  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_KEY);
  g_autofree gchar *sha_after = file_sha256(path);
  g_autofree gchar *listing_after = dir_listing(dir);
  g_assert_cmpstr(sha_after, ==, sha);
  g_assert_cmpstr(listing_after, ==, listing);

  memcpy(item->key, saved, sizeof saved);
  gh_store_close(open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE));
}

/* ---- ST-3 modes and ownership ------------------------------------------------------------ */

static void
test_st3_modes(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(store, "c", "m", T0, "hello", 0), ==, GH_STORE_ADMIT_STORED);
  const gchar *dir = gh_store_get_dir(store);
  const gchar *path = gh_store_get_path(store);
  g_autofree gchar *accounts = g_path_get_dirname(dir);
  g_autofree gchar *root = g_path_get_dirname(accounts);
  g_autofree gchar *wal = g_strconcat(path, "-wal", NULL);
  g_autofree gchar *shm = g_strconcat(path, "-shm", NULL);
  g_autofree gchar *name = g_path_get_basename(dir);

  g_assert_cmpstr(name, ==, ACCOUNT_A_DIR);
  g_assert_cmpuint(path_mode(root), ==, 0700);
  g_assert_cmpuint(path_mode(accounts), ==, 0700);
  g_assert_cmpuint(path_mode(dir), ==, 0700);
  g_assert_cmpuint(path_mode(path), ==, 0600);
  g_assert_cmpuint(path_mode(wal), ==, 0600);
  g_assert_cmpuint(path_mode(shm), ==, 0600);
  gh_store_close(store);
}

static void
test_st3_refuse_unsafe(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  gh_store_close(open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE));
  g_autofree gchar *dir = account_dir(ACCOUNT_A);
  g_autofree gchar *path = g_build_filename(dir, "store.db", NULL);
  g_autofree gchar *wal = g_strconcat(path, "-wal", NULL);
  g_autofree gchar *real_db = g_strconcat(path, ".real", NULL);
  g_autofree gchar *real_dir = g_strconcat(dir, ".real", NULL);

  /* A group-readable store is refused and left exactly as found. */
  g_assert_cmpint(g_chmod(path, 0640), ==, 0);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_PERMISSIONS);
  g_assert_cmpuint(path_mode(path), ==, 0640);
  g_assert_cmpint(g_chmod(path, 0600), ==, 0);

  /* A world-readable WAL beside it. */
  g_assert_true(g_file_set_contents(wal, "", 0, NULL));
  g_assert_cmpint(g_chmod(wal, 0644), ==, 0);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_PERMISSIONS);
  g_assert_cmpuint(path_mode(wal), ==, 0644);
  g_assert_cmpint(g_unlink(wal), ==, 0);

  /* A group-accessible account directory. */
  g_assert_cmpint(g_chmod(dir, 0750), ==, 0);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_PERMISSIONS);
  g_assert_cmpuint(path_mode(dir), ==, 0750);
  g_assert_cmpint(g_chmod(dir, 0700), ==, 0);

  /* store.db replaced by a symbolic link. */
  g_assert_cmpint(g_rename(path, real_db), ==, 0);
  g_assert_cmpint(symlink(real_db, path), ==, 0);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_PERMISSIONS);
  g_assert_true(g_file_test(path, G_FILE_TEST_IS_SYMLINK));
  g_assert_cmpint(g_unlink(path), ==, 0);
  g_assert_cmpint(g_rename(real_db, path), ==, 0);

  /* The account directory replaced by a symbolic link. */
  g_assert_cmpint(g_rename(dir, real_dir), ==, 0);
  g_assert_cmpint(symlink(real_dir, dir), ==, 0);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_PERMISSIONS);
  g_assert_true(g_file_test(dir, G_FILE_TEST_IS_SYMLINK));
  g_assert_cmpint(g_unlink(dir), ==, 0);
  g_assert_cmpint(g_rename(real_dir, dir), ==, 0);

  /* Owned by another user: the hook changes which uid counts as ours. */
  gh_store_test_set_uid((gint64) geteuid() + 1);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_PERMISSIONS);
  gh_store_test_set_uid(-1);

  gh_store_close(open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE));
}

/* ---- ST-5 pragma readback ------------------------------------------------------------------ */

static void
test_st5_pragmas(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  sqlite3 *db = gh_store_get_db(store);
  g_autofree gchar *mode = sql_text(store, "PRAGMA journal_mode");
  g_autofree gchar *cipher = sql_text(store, "PRAGMA cipher_version");

  g_assert_cmpstr(mode, ==, "wal");
  g_assert_cmpint(sql_int(store, "PRAGMA synchronous"), ==, 2);
  g_assert_cmpint(sql_int(store, "PRAGMA secure_delete"), ==, 1);
  g_assert_cmpint(sql_int(store, "PRAGMA temp_store"), ==, 2);
  g_assert_cmpint(sql_int(store, "PRAGMA foreign_keys"), ==, 1);
  g_assert_cmpint(sql_int(store, "PRAGMA trusted_schema"), ==, 0);
  g_assert_nonnull(cipher);
  g_assert_cmpstr(cipher, !=, "");
  g_assert_cmpstr(cipher, ==, gh_store_get_cipher_version(store));
  g_assert_cmpint(sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, -1), ==, 0);
  g_assert_cmpint(sqlite3_limit(db, SQLITE_LIMIT_LENGTH, -1), ==, GH_STORE_MAX_VALUE_SIZE);
  /* No second (possibly plaintext) database can be attached. */
  g_assert_cmpint(sqlite3_exec(db, "ATTACH DATABASE ':memory:' AS other", NULL, NULL, NULL),
                  !=, SQLITE_OK);
  g_test_message("ST-5: SQLCipher %s, journal_mode=%s synchronous=2 secure_delete=1 "
                 "temp_store=2 foreign_keys=1", cipher, mode);
  gh_store_close(store);
}

/* ---- ST-6 crash harness (H8) ---------------------------------------------------------------- */

typedef struct {
  FakeKeys *keys;
  const gchar *account;
  GhClock *clock;
  gint64 outbox;
} CrashScript;

static void
expect_killed(const gchar *cut, guint nth, GhCrashScript script, gpointer data)
{
  GhCrashOutcome outcome = gh_crash_harness_run(cut, nth, script, data);
  if (outcome != GH_CRASH_KILLED)
    g_error("Cut point %s#%u: %s", cut, nth, gh_crash_outcome_to_string(outcome));
}

static void
script_create(gpointer data)
{
  CrashScript *s = data;
  gh_store_close(open_ok(s->keys, s->account, s->clock, GH_STORE_OPEN_CREATE));
}

static void
test_st6_crash_create(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("create:");
  g_assert_cmpuint(g_strv_length(cuts), ==, 5);
  for (guint i = 0; cuts[i]; i++) {
    g_autofree gchar *account = hex_printf("st6/%s", cuts[i]);
    g_autoptr(FakeKeys) keys = fake_keys_new();
    FakeItem *item = fake_seed(keys, account);
    CrashScript script = { keys, account, NULL, 0 };
    expect_killed(cuts[i], 1, script_create, &script);

    /* The next open completes an interrupted creation. */
    GhStore *store = open_ok(keys, account, NULL, GH_STORE_OPEN_NONE);
    assert_integrity(store);
    g_autofree gchar *meta_account = sql_text(store, "SELECT value FROM meta WHERE key = 'account_pubkey'");
    g_autofree gchar *meta_id = sql_text(store, "SELECT value FROM meta WHERE key = 'store_id'");
    g_assert_cmpint(sql_int(store, "PRAGMA user_version"), ==, GH_STORE_SCHEMA_VERSION);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM schema_migrations"), ==,
                    GH_STORE_SCHEMA_VERSION);
    g_assert_cmpstr(meta_account, ==, account);
    g_assert_cmpstr(meta_id, ==, item->store_id);
    g_assert_cmpstr(gh_store_get_store_id(store), ==, item->store_id);
    gh_store_close(store);
  }
}

static void
script_admit(gpointer data)
{
  CrashScript *s = data;
  GhStore *store = open_ok(s->keys, s->account, s->clock, GH_STORE_OPEN_NONE);
  admit_text(store, "conversation-2", "B", T0, "second", 0);
  gh_store_close(store);
}

static void
test_st6_crash_admit(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("admit:");
  g_autofree gchar *rumor_a = hex_of("A");
  g_autofree gchar *wrap_a = hex_of("A/wrap");
  g_autofree gchar *rumor_b = hex_of("B");
  g_autofree gchar *wrap_b = hex_of("B/wrap");
  g_assert_cmpuint(g_strv_length(cuts), ==, 8);
  for (guint i = 0; cuts[i]; i++) {
    g_autofree gchar *account = hex_printf("st6/%s", cuts[i]);
    g_autoptr(FakeKeys) keys = fake_keys_new();
    GhStore *store = open_ok(keys, account, NULL, GH_STORE_OPEN_CREATE);
    g_assert_cmpint(admit_text(store, "conversation-1", "A", T0 - 10, "first", 0), ==,
                    GH_STORE_ADMIT_STORED);
    gh_store_close(store);

    CrashScript script = { keys, account, NULL, 0 };
    expect_killed(cuts[i], 1, script_admit, &script);

    store = open_ok(keys, account, NULL, GH_STORE_OPEN_NONE);
    assert_integrity(store);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'",
                            rumor_a), ==, 1);
    g_assert_true(is_seen(store, GH_STORE_SEEN_WRAP, wrap_a));
    g_assert_true(is_seen(store, GH_STORE_SEEN_RUMOR, rumor_a));
    /* B is all or nothing: message, both seen keys and its new conversation. */
    const gboolean has_b =
      sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'", rumor_b) == 1;
    g_assert_cmpint(has_b, ==, g_str_has_suffix(cuts[i], ":after-commit"));
    g_assert_cmpint(is_seen(store, GH_STORE_SEEN_RUMOR, rumor_b), ==, has_b);
    g_assert_cmpint(is_seen(store, GH_STORE_SEEN_WRAP, wrap_b), ==, has_b);
    g_assert_cmpint(find_conversation(store, "conversation-2") > 0, ==, has_b);
    assert_unread_consistent(store);
    /* Backfill brings B again: it ends up stored exactly once. */
    g_assert_cmpint(admit_text(store, "conversation-2", "B", T0, "second", 0), ==,
                    has_b ? GH_STORE_ADMIT_DUPLICATE : GH_STORE_ADMIT_STORED);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'",
                            rumor_b), ==, 1);
    gh_store_close(store);
  }
}

static void
script_enqueue(gpointer data)
{
  CrashScript *s = data;
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(s->keys, s->account, s->clock, GH_STORE_OPEN_NONE);
  g_assert_cmpint(enqueue_text(store, find_conversation(store, "conversation"), "outgoing",
                               "hello", &error), >, 0);
  gh_store_close(store);
}

static void
test_st6_crash_enqueue(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("enqueue:");
  g_autofree gchar *rumor = hex_of("outgoing");
  g_assert_cmpuint(g_strv_length(cuts), ==, 6);
  for (guint i = 0; cuts[i]; i++) {
    g_autofree gchar *account = hex_printf("st6/%s", cuts[i]);
    g_autoptr(FakeKeys) keys = fake_keys_new();
    GhStore *store = open_ok(keys, account, NULL, GH_STORE_OPEN_CREATE);
    gint64 conversation = ensure_conversation(store, "conversation");
    g_assert_true(gh_store_set_draft(store, conversation, "unsent draft", NULL));
    gh_store_close(store);

    CrashScript script = { keys, account, NULL, 0 };
    expect_killed(cuts[i], 1, script_enqueue, &script);

    store = open_ok(keys, account, NULL, GH_STORE_OPEN_NONE);
    assert_integrity(store);
    const gboolean committed = g_str_has_suffix(cuts[i], ":after-commit");
    g_autofree gchar *draft = draft_of(store, conversation);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox"), ==, committed);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, committed);
    g_assert_cmpint(is_seen(store, GH_STORE_SEEN_RUMOR, rumor), ==, committed);
    g_assert_cmpstr(draft, ==, committed ? NULL : "unsent draft");
    gh_store_close(store);
  }
}

static void
script_seal(gpointer data)
{
  CrashScript *s = data;
  GhStore *store = open_ok(s->keys, s->account, s->clock, GH_STORE_OPEN_NONE);
  g_assert_true(seal_three(store, s->outbox, NULL));
  gh_store_close(store);
}

static void
test_st6_crash_seal(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("seal:");
  g_assert_cmpuint(g_strv_length(cuts), ==, 5);
  for (guint i = 0; cuts[i]; i++) {
    const gboolean per_event = g_str_equal(cuts[i], "seal:event") ||
                               g_str_equal(cuts[i], "seal:targets");
    for (guint nth = 1; nth <= (per_event ? 3u : 1u); nth++) {
      g_autofree gchar *account = hex_printf("st6/%s/%u", cuts[i], nth);
      g_autoptr(FakeKeys) keys = fake_keys_new();
      g_autoptr(GError) error = NULL;
      GhStore *store = open_ok(keys, account, NULL, GH_STORE_OPEN_CREATE);
      gint64 conversation = ensure_conversation(store, "conversation");
      gint64 outbox = enqueue_text(store, conversation, "outgoing", "hello", &error);
      g_assert_no_error(error);
      gh_store_close(store);

      CrashScript script = { keys, account, NULL, outbox };
      expect_killed(cuts[i], nth, script_seal, &script);

      store = open_ok(keys, account, NULL, GH_STORE_OPEN_NONE);
      assert_integrity(store);
      const gboolean sealed = g_str_has_suffix(cuts[i], ":after-commit");
      g_assert_cmpint(sql_int(store, "SELECT state FROM outbox WHERE id = %" G_GINT64_FORMAT,
                              outbox), ==,
                      sealed ? GH_STORE_OUTBOX_SEALED : GH_STORE_OUTBOX_QUEUED);
      g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_events WHERE outbox_id = %"
                                     G_GINT64_FORMAT, outbox), ==, sealed ? 3 : 0);
      g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_targets t JOIN outbox_events e "
                                     "ON e.id = t.outbox_event_id WHERE e.outbox_id = %"
                                     G_GINT64_FORMAT, outbox), ==, sealed ? 6 : 0);
      g_autofree gchar *ids = sql_text(store,
        "SELECT group_concat(event_id, ',') FROM (SELECT event_id FROM outbox_events "
        "WHERE outbox_id = %" G_GINT64_FORMAT " ORDER BY id)", outbox);

      /* Never re-seal: a sealed message keeps its events; otherwise seal once. */
      gboolean resealed = seal_three(store, outbox, &error);
      if (sealed) {
        g_assert_false(resealed);
        g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
        g_clear_error(&error);
      } else {
        g_assert_no_error(error);
        g_assert_true(resealed);
      }
      g_autofree gchar *ids_after = sql_text(store,
        "SELECT group_concat(event_id, ',') FROM (SELECT event_id FROM outbox_events "
        "WHERE outbox_id = %" G_GINT64_FORMAT " ORDER BY id)", outbox);
      if (sealed)
        g_assert_cmpstr(ids_after, ==, ids);
      g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_events WHERE outbox_id = %"
                                     G_GINT64_FORMAT, outbox), ==, 3);
      gh_store_close(store);
    }
  }
}

static void
script_purge(gpointer data)
{
  CrashScript *s = data;
  GhStorePurgeStats stats;
  GhStore *store = open_ok(s->keys, s->account, s->clock, GH_STORE_OPEN_NONE);
  g_assert_true(gh_store_purge(store, 0, &stats, NULL));
  gh_store_close(store);
}

static void
test_st6_crash_purge(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("purge:");
  const gchar *seeds[] = { "e1", "e2", "e3", "keep" };
  g_assert_cmpuint(g_strv_length(cuts), ==, 6);
  for (guint i = 0; cuts[i]; i++) {
    g_autofree gchar *account = hex_printf("st6/%s", cuts[i]);
    g_autoptr(FakeKeys) keys = fake_keys_new();
    g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
    g_autoptr(GError) error = NULL;
    GhStore *store = open_ok(keys, account, clock, GH_STORE_OPEN_CREATE);
    for (guint j = 0; j < G_N_ELEMENTS(seeds); j++)
      g_assert_cmpint(admit_text(store, "conversation", seeds[j], T0 - 10, "text",
                                 j < 3 ? T0 + 100 : 0), ==, GH_STORE_ADMIT_STORED);
    gh_store_close(store);
    gh_clock_fake_advance(clock, 200 * G_USEC_PER_SEC);

    CrashScript script = { keys, account, clock, 0 };
    expect_killed(cuts[i], 1, script_purge, &script);

    store = open_ok(keys, account, clock, GH_STORE_OPEN_NONE);
    assert_integrity(store);
    const gboolean purged = g_str_has_suffix(cuts[i], ":after-commit") ||
                            g_str_equal(cuts[i], "purge:checkpoint");
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages WHERE expires_at IS NOT NULL"),
                    ==, purged ? 0 : 3);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, purged ? 1 : 4);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM seen WHERE ns = %d", GH_STORE_SEEN_RUMOR),
                    ==, 4);
    assert_unread_consistent(store);

    GhStorePurgeStats stats;
    g_assert_true(gh_store_purge(store, 0, &stats, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(stats.n_expired, ==, purged ? 0 : 3);
    if (!purged) {
      g_autofree gchar *wal = g_strconcat(gh_store_get_path(store), "-wal", NULL);
      g_assert_true(stats.checkpointed);
      g_assert_cmpint(path_size(wal), ==, 0);
    }
    /* Another relay's copy of an expired message cannot resurrect it (EX-6). */
    TestMessage copy;
    test_message_init(&copy, "conversation", "e1", "e1/relay-2", T0 - 10, "text");
    copy.message.expires_at = T0 + 100;
    g_assert_cmpint(admit_ok(store, &copy.message), ==, GH_STORE_ADMIT_DUPLICATE);
    test_message_clear(&copy);
    gh_store_close(store);
  }
}

static void
script_forget(gpointer data)
{
  CrashScript *s = data;
  GhStore *store = open_ok(s->keys, s->account, s->clock, GH_STORE_OPEN_NONE);
  g_assert_true(gh_store_forget_conversation(store, find_conversation(store, "conversation"), NULL));
  gh_store_close(store);
}

static void
test_st6_crash_forget(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("forget:");
  g_assert_cmpuint(g_strv_length(cuts), ==, 5);
  for (guint i = 0; cuts[i]; i++) {
    g_autofree gchar *account = hex_printf("st6/%s", cuts[i]);
    g_autoptr(FakeKeys) keys = fake_keys_new();
    g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
    g_autoptr(GError) error = NULL;
    GhStore *store = open_ok(keys, account, clock, GH_STORE_OPEN_CREATE);
    g_assert_cmpint(admit_text(store, "conversation", "A", T0 - 20, "a", 0), ==, GH_STORE_ADMIT_STORED);
    g_assert_cmpint(admit_text(store, "conversation", "B", T0 - 10, "b", 0), ==, GH_STORE_ADMIT_STORED);
    g_assert_cmpint(enqueue_text(store, find_conversation(store, "conversation"), "out", "c",
                                 &error), >, 0);
    gh_store_close(store);
    gh_clock_fake_advance(clock, 10 * G_USEC_PER_SEC);

    CrashScript script = { keys, account, clock, 0 };
    expect_killed(cuts[i], 1, script_forget, &script);

    store = open_ok(keys, account, clock, GH_STORE_OPEN_NONE);
    assert_integrity(store);
    const gboolean forgotten = g_str_has_suffix(cuts[i], ":after-commit");
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, forgotten ? 0 : 3);
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox"), ==, forgotten ? 0 : 1);
    g_assert_cmpint(sql_int(store, "SELECT forgotten_before FROM conversations"), ==,
                    forgotten ? T0 + 10 : 0);
    gh_store_close(store);
  }
}

/* ---- ST-7 idempotent admission ------------------------------------------------------------ */

static void
test_st7_idempotent_admission(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  TestMessage first, second;
  test_message_init(&first, "conversation", "rumor", "relay-1", T0, "hello");
  test_message_init(&second, "conversation", "rumor", "relay-2", T0, "hello");
  g_assert_cmpstr(first.rumor, ==, second.rumor);
  g_assert_cmpstr(first.wrap, !=, second.wrap);

  g_assert_false(is_seen(store, GH_STORE_SEEN_WRAP, first.wrap));
  g_assert_cmpint(admit_ok(store, &first.message), ==, GH_STORE_ADMIT_STORED);
  g_assert_cmpint(admit_ok(store, &second.message), ==, GH_STORE_ADMIT_DUPLICATE);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 2);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM seen WHERE ns = 2"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT unread_count FROM conversations"), ==, 1);
  g_assert_cmpint(admit_ok(store, &first.message), ==, GH_STORE_ADMIT_DUPLICATE);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM seen WHERE ns = 1"), ==, 2);
  g_assert_true(is_seen(store, GH_STORE_SEEN_WRAP, second.wrap));

  /* Our own message coming back from our inbox as a self-copy. */
  gint64 conversation = find_conversation(store, "conversation");
  gint64 outbox = enqueue_text(store, conversation, "mine", "sent by me", &error);
  g_assert_no_error(error);
  TestMessage self;
  test_message_init(&self, "conversation", "mine", "self-copy", T0, "sent by me");
  self.message.sender_pubkey = ACCOUNT_A;
  self.message.direction = GH_STORE_DIRECTION_OUT;
  self.message.unread = FALSE;
  g_assert_cmpint(admit_ok(store, &self.message), ==, GH_STORE_ADMIT_DUPLICATE);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'",
                          self.rumor), ==, 1);

  /* Enqueue is idempotent on its op_id. */
  g_assert_cmpint(enqueue_text(store, conversation, "mine", "sent by me", &error), ==, outbox);
  g_assert_no_error(error);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox"), ==, 1);

  test_message_clear(&first);
  test_message_clear(&second);
  test_message_clear(&self);
  gh_store_close(store);
}

/* ---- ST-9 forget conversation ----------------------------------------------------------------- */

static void
test_st9_forget_conversation(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, clock, GH_STORE_OPEN_CREATE);
  const gchar *key = "conversation";
  g_assert_cmpint(admit_text(store, key, "m1", T0 - 100, "one", 0), ==, GH_STORE_ADMIT_STORED);
  g_assert_cmpint(admit_text(store, key, "m2", T0 - 50, "two", 0), ==, GH_STORE_ADMIT_STORED);
  gint64 id = find_conversation(store, key);
  gint64 outbox = enqueue_text(store, id, "out", "reply", &error);
  g_assert_no_error(error);
  g_assert_true(seal_three(store, outbox, &error));
  g_assert_no_error(error);
  g_assert_true(gh_store_set_draft(store, id, "draft", &error));
  g_autofree gchar *pin = g_strdup_printf(
    "UPDATE conversations SET pinned_rank = 1, request_state = 2 WHERE id = %" G_GINT64_FORMAT, id);
  g_assert_true(gh_store_exec(store, pin, &error));

  gh_clock_fake_advance(clock, 10 * G_USEC_PER_SEC);
  g_assert_true(gh_store_forget_conversation(store, id, &error));
  g_assert_no_error(error);
  g_autofree gchar *draft = draft_of(store, id);
  g_autofree gchar *wal = g_strconcat(gh_store_get_path(store), "-wal", NULL);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_events"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_targets"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM participants"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT forgotten_before FROM conversations"), ==, T0 + 10);
  g_assert_cmpint(sql_int(store, "SELECT unread_count FROM conversations"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT pinned_rank IS NULL FROM conversations"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT request_state FROM conversations"), ==,
                  GH_STORE_REQUEST_BLOCKED);
  g_assert_null(draft);
  g_assert_cmpint(path_size(wal), ==, 0);

  /* Backfill: another relay's copy of m1, and an older message never seen. */
  TestMessage copy;
  test_message_init(&copy, key, "m1", "m1/relay-2", T0 - 100, "one");
  g_assert_cmpint(admit_ok(store, &copy.message), ==, GH_STORE_ADMIT_DUPLICATE);
  test_message_clear(&copy);
  g_autofree gchar *m3 = hex_of("m3");
  g_assert_cmpint(admit_text(store, key, "m3", T0 - 1000, "old", 0), ==, GH_STORE_ADMIT_FORGOTTEN);
  g_assert_true(is_seen(store, GH_STORE_SEEN_RUMOR, m3));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 0);

  /* A newer message starts the conversation fresh, with only itself. */
  g_autofree gchar *m4 = hex_of("m4");
  g_assert_cmpint(admit_text(store, key, "m4", T0 + 20, "new", 0), ==, GH_STORE_ADMIT_STORED);
  g_assert_cmpint(find_conversation(store, key), ==, id);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages WHERE conversation_id = %"
                                 G_GINT64_FORMAT, id), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'", m4),
                  ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT unread_count FROM conversations"), ==, 1);

  g_assert_false(gh_store_forget_conversation(store, 999999, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  gh_store_close(store);
}

/* ---- ST-10 disk full ------------------------------------------------------------------------ */

/* No free page and no growth allowed: the next page allocation is SQLITE_FULL. */
static void
limit_growth(GhStore *store)
{
  g_assert_cmpint(sql_int(store, "PRAGMA freelist_count"), ==, 0);
  gint64 pages = sql_int(store, "PRAGMA page_count");
  g_assert_cmpint(sql_int(store, "PRAGMA max_page_count = %" G_GINT64_FORMAT, pages), ==, pages);
}

static void
lift_limit(GhStore *store)
{
  g_assert_cmpint(sql_int(store, "PRAGMA max_page_count = 1000000"), >=, 1000000);
}

static void
test_st10_disk_full_admission(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(store, "conversation", "first", T0, "hi", 0), ==, GH_STORE_ADMIT_STORED);
  limit_growth(store);

  g_autofree gchar *big = g_strnfill(200 * 1024, 'x');
  TestMessage t;
  test_message_init(&t, "conversation", "big", NULL, T0 + 1, big);
  GhStoreAdmitResult result = GH_STORE_ADMIT_STORED;
  g_assert_false(gh_store_admit(store, &t.message, &result, NULL, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL);
  g_clear_error(&error);
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 0);
  /* Not seen-recorded, so the wrap is fetched and admitted again later. */
  g_assert_false(is_seen(store, GH_STORE_SEEN_WRAP, t.wrap));
  g_assert_false(is_seen(store, GH_STORE_SEEN_RUMOR, t.rumor));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT unread_count FROM conversations"), ==, 1);

  lift_limit(store);
  g_assert_cmpint(admit_ok(store, &t.message), ==, GH_STORE_ADMIT_STORED);
  g_assert_true(is_seen(store, GH_STORE_SEEN_WRAP, t.wrap));
  g_assert_true(is_seen(store, GH_STORE_SEEN_RUMOR, t.rumor));
  test_message_clear(&t);
  gh_store_close(store);
}

static void
test_st10_disk_full_enqueue(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  gint64 conversation = ensure_conversation(store, "conversation");
  g_assert_true(gh_store_set_draft(store, conversation, "keep me", &error));
  limit_growth(store);

  g_autofree gchar *big = g_strnfill(200 * 1024, 'y');
  g_autofree gchar *rumor = hex_of("big");
  g_assert_cmpint(enqueue_text(store, conversation, "big", big, &error), ==, 0);
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL);
  g_clear_error(&error);
  g_autofree gchar *kept = draft_of(store, conversation);
  g_assert_cmpstr(kept, ==, "keep me");
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 0);
  g_assert_false(is_seen(store, GH_STORE_SEEN_RUMOR, rumor));

  lift_limit(store);
  g_assert_cmpint(enqueue_text(store, conversation, "big", big, &error), >, 0);
  g_assert_no_error(error);
  g_autofree gchar *cleared = draft_of(store, conversation);
  g_assert_null(cleared);
  gh_store_close(store);
}

/* ---- ST-11 schema version ------------------------------------------------------------------- */

static void
test_st11_schema_version(void)
{
  static const gchar *const tables[] = {
    "conversations", "contacts", "cursors", "directory", "media", "messages", "meta",
    "mls_groups", "mls_kv", "mls_snapshots", "nip29_groups", "outbox", "outbox_events",
    "outbox_targets", "participants", "schema_migrations", "seen",
  };
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  const gchar *objects_sql =
    "SELECT group_concat(name, ',') FROM (SELECT name FROM sqlite_master ORDER BY name)";
  g_autofree gchar *objects = sql_text(store, "%s", objects_sql);
  g_assert_cmpint(sql_int(store, "PRAGMA user_version"), ==, GH_STORE_SCHEMA_VERSION);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM schema_migrations WHERE version = 1"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM schema_migrations"), ==,
                  GH_STORE_SCHEMA_VERSION);
  for (guint i = 0; i < G_N_ELEMENTS(tables); i++)
    g_assert_cmpint(sql_int(store, "SELECT count(*) FROM sqlite_master WHERE type = 'table' "
                                   "AND name = '%s'", tables[i]), ==, 1);
  gh_store_close(store);

  /* Reopening is idempotent: nothing is created or migrated again. */
  store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_autofree gchar *objects_again = sql_text(store, "%s", objects_sql);
  g_assert_cmpstr(objects_again, ==, objects);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM schema_migrations"), ==,
                  GH_STORE_SCHEMA_VERSION);
  g_autofree gchar *newer_version =
    g_strdup_printf("PRAGMA user_version = %d", GH_STORE_SCHEMA_VERSION + 1);
  g_assert_true(gh_store_exec(store, newer_version, &error));
  g_assert_no_error(error);
  g_autofree gchar *path = g_strdup(gh_store_get_path(store));
  gh_store_close(store);

  g_autofree gchar *sha = file_sha256(path);
  GError *newer = open_error(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE,
                             GH_STORE_ERROR_NEWER_SCHEMA);
  g_assert_nonnull(strstr(newer->message, "Created by a newer Groundhog"));
  g_error_free(newer);
  g_autofree gchar *sha_after = file_sha256(path);
  g_assert_cmpstr(sha_after, ==, sha);
}

/* ---- Key custody seam (KC-1, KC-2, KC-3, KC-4, KC-5 at the store level) --------------------- */

static void
test_key_first_open(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autofree gchar *root = groundhog_root();
  keys->fail_store = TRUE;
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE, GH_STORE_ERROR_FAILED);
  g_assert_cmpuint(keys->n_store, ==, 1);
  /* The key item must be durable before any file exists. */
  g_assert_false(path_exists(root));

  keys->fail_store = FALSE;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  const FakeItem *item = fake_item(keys, ACCOUNT_A);
  static const guint8 zeros[GH_STORE_KEY_SIZE] = { 0 };
  g_assert_nonnull(item);
  g_assert_true(g_uuid_string_is_valid(item->store_id));
  g_assert_cmpstr(item->store_id, ==, gh_store_get_store_id(store));
  g_assert_cmpint(memcmp(item->key, zeros, GH_STORE_KEY_SIZE), !=, 0);
  gh_store_close(store);

  /* Without CREATE nothing is created for an unknown account. */
  guint n_store = keys->n_store;
  g_autofree gchar *b_dir = g_build_filename(root, "accounts", ACCOUNT_B_DIR, NULL);
  OPEN_FAILS(keys, ACCOUNT_B, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_NOT_FOUND);
  g_assert_false(path_exists(b_dir));
  g_assert_cmpuint(keys->n_store, ==, n_store);
}

static void
test_key_missing_store_untouched(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(store, "c", "m", T0, "hello", 0), ==, GH_STORE_ADMIT_STORED);
  gh_store_close(store);
  g_autofree gchar *dir = account_dir(ACCOUNT_A);
  g_autofree gchar *path = db_path_of(ACCOUNT_A);
  g_autofree gchar *sha = file_sha256(path);
  g_autofree gchar *listing = dir_listing(dir);

  /* Even with CREATE, a store whose key is gone is never replaced. */
  g_hash_table_remove(keys->items, ACCOUNT_A);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE, GH_STORE_ERROR_KEY_MISSING);
  g_autofree gchar *sha_after = file_sha256(path);
  g_autofree gchar *listing_after = dir_listing(dir);
  g_assert_cmpstr(sha_after, ==, sha);
  g_assert_cmpstr(listing_after, ==, listing);

  /* "Start Fresh" crypto-shreds the directory; a new store starts empty. */
  GhStoreConfig config = config_for(keys, ACCOUNT_A, NULL);
  g_assert_true(gh_store_forget_account(&config, NULL, &error));
  g_assert_no_error(error);
  g_assert_false(path_exists(dir));
  store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 0);
  gh_store_close(store);
}

static void
test_key_locked_and_unavailable(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autofree gchar *root = groundhog_root();
  keys->locked = TRUE;
  /* Background: no prompt is requested and nothing is written. */
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE, GH_STORE_ERROR_LOCKED);
  g_assert_cmpint(keys->last_flags, ==, GH_STORE_KEY_LOOKUP_NONE);
  g_assert_false(path_exists(root));
  /* Interactive: the lookup may prompt. */
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE | GH_STORE_OPEN_INTERACTIVE);
  g_assert_cmpint(keys->last_flags, ==, GH_STORE_KEY_LOOKUP_ALLOW_PROMPT);
  gh_store_close(store);

  keys->locked = FALSE;
  keys->unavailable = TRUE;
  g_autofree gchar *b_dir = g_build_filename(root, "accounts", ACCOUNT_B_DIR, NULL);
  OPEN_FAILS(keys, ACCOUNT_B, NULL, GH_STORE_OPEN_CREATE, GH_STORE_ERROR_UNAVAILABLE);
  g_assert_false(path_exists(b_dir));
}

static void
test_key_foreign(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  gh_store_close(open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE));
  FakeItem *item = fake_item(keys, ACCOUNT_A);

  /* The key opens the file, but the item's store id is not this store's. */
  g_autofree gchar *saved_id = item->store_id;
  item->store_id = g_uuid_string_random();
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_FOREIGN);
  g_free(item->store_id);
  item->store_id = g_steal_pointer(&saved_id);

  /* A's store copied under B's directory, with B's item holding A's key. */
  g_autofree gchar *a_path = db_path_of(ACCOUNT_A);
  g_autofree gchar *b_dir = account_dir(ACCOUNT_B);
  g_autofree gchar *b_path = g_build_filename(b_dir, "store.db", NULL);
  g_autoptr(GBytes) bytes = file_bytes(a_path);
  g_assert_cmpint(g_mkdir(b_dir, 0700), ==, 0);
  g_assert_true(g_file_set_contents(b_path, g_bytes_get_data(bytes, NULL),
                                    (gssize) g_bytes_get_size(bytes), NULL));
  g_assert_cmpint(g_chmod(b_path, 0600), ==, 0);
  FakeItem *b = g_new0(FakeItem, 1);
  memcpy(b->key, item->key, GH_STORE_KEY_SIZE);
  b->store_id = g_strdup(item->store_id);
  g_hash_table_replace(keys->items, g_strdup(ACCOUNT_B), b);
  OPEN_FAILS(keys, ACCOUNT_B, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_FOREIGN);
}

/* ---- Forget account (crypto-shred) --------------------------------------------------------------- */

static void
test_forget_account(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *a = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  GhStore *b = open_ok(keys, ACCOUNT_B, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(admit_text(a, "c", "a1", T0, "for a", 0), ==, GH_STORE_ADMIT_STORED);
  g_assert_cmpint(admit_text(b, "c", "b1", T0, "for b", 0), ==, GH_STORE_ADMIT_STORED);
  g_autofree gchar *a_dir = g_strdup(gh_store_get_dir(a));
  g_autofree gchar *b_dir = g_strdup(gh_store_get_dir(b));
  GhStoreConfig a_config = config_for(keys, ACCOUNT_A, NULL);
  GhStoreConfig b_config = config_for(keys, ACCOUNT_B, NULL);

  /* An open store must be closed first. */
  g_assert_false(gh_store_forget_account(&b_config, NULL, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_BUSY);
  g_clear_error(&error);
  g_assert_true(path_exists(b_dir));

  /* Forget A: the key item goes first, then the directory; B is untouched. */
  g_assert_true(gh_store_forget(a, NULL, &error));
  g_assert_no_error(error);
  g_assert_false(path_exists(a_dir));
  g_assert_null(fake_item(keys, ACCOUNT_A));
  g_assert_cmpuint(keys->n_destroy, ==, 1);
  g_assert_true(path_exists(b_dir));
  g_assert_cmpint(admit_text(b, "c", "b2", T0, "still here", 0), ==, GH_STORE_ADMIT_STORED);
  gh_store_close(b);

  /* A failed key deletion is reported, and the files are still removed. */
  keys->fail_destroy = TRUE;
  g_assert_false(gh_store_forget_account(&b_config, NULL, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_LOCKED);
  g_clear_error(&error);
  g_assert_false(path_exists(b_dir));
  g_assert_nonnull(fake_item(keys, ACCOUNT_B));
  keys->fail_destroy = FALSE;

  /* A planted symbolic link is removed, never followed. */
  gh_store_close(open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE));
  g_autofree gchar *outside = g_build_filename(g_get_user_data_dir(), "outside", NULL);
  g_autofree gchar *keep = g_build_filename(outside, "keep", NULL);
  g_autofree gchar *moved = g_strconcat(a_dir, ".moved", NULL);
  g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
  g_assert_true(g_file_set_contents(keep, "keep", -1, NULL));
  g_assert_cmpint(g_rename(a_dir, moved), ==, 0);
  g_assert_cmpint(symlink(outside, a_dir), ==, 0);
  g_assert_true(gh_store_forget_account(&a_config, NULL, &error));
  g_assert_no_error(error);
  g_assert_false(path_exists(a_dir));
  g_assert_true(path_exists(keep));
  g_assert_true(path_exists(moved));
}

static void
free_guarded(gpointer data)
{
  sodium_free(data); /* restores write access, wipes, unmaps */
}

/* A key as G03's GhStoreKey hands it over: guarded memory made read-only, so
 * any write by the store (a wipe, say) would crash the test. */
static GBytes *
readonly_key(void)
{
  g_assert_cmpint(sodium_init(), >=, 0);
  guint8 *data = sodium_malloc(GH_STORE_KEY_SIZE);
  g_assert_nonnull(data);
  randombytes_buf(data, GH_STORE_KEY_SIZE);
  g_assert_cmpint(sodium_mprotect_readonly(data), ==, 0);
  return g_bytes_new_with_free_func(data, GH_STORE_KEY_SIZE, free_guarded, data);
}

/* The path G04 takes with G03's asynchronous key custody. */
static void
test_open_with_key(void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GBytes) key = readonly_key();
  g_autoptr(GBytes) other_key = readonly_key();
  g_autoptr(GBytes) literal = g_bytes_new_static("x'00'", 5);
  g_autofree gchar *store_id = g_uuid_string_random();
  g_autofree gchar *other_id = g_uuid_string_random();
  GhStoreConfig config = { NULL, ACCOUNT_A, NULL, NULL, NULL };
  gboolean exists = TRUE;

  g_assert_true(gh_store_exists(NULL, ACCOUNT_A, &exists, &error));
  g_assert_false(exists);
  g_assert_null(gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_NONE, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);

  GhStore *store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  g_assert_cmpstr(gh_store_get_store_id(store), ==, store_id);
  g_assert_cmpint(admit_text(store, "c", "m", T0, "hello", 0), ==, GH_STORE_ADMIT_STORED);
  g_assert_true(gh_store_exists(NULL, ACCOUNT_A, &exists, &error));
  g_assert_true(exists);
  g_assert_false(gh_store_delete_files(NULL, ACCOUNT_A, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_BUSY);
  g_clear_error(&error);
  g_autofree gchar *dir = g_strdup(gh_store_get_dir(store));
  gh_store_close(store);

  /* exists() applies the same safety checks as an open. */
  g_assert_cmpint(g_chmod(dir, 0755), ==, 0);
  g_assert_false(gh_store_exists(NULL, ACCOUNT_A, &exists, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS);
  g_clear_error(&error);
  g_assert_cmpint(g_chmod(dir, 0700), ==, 0);

  store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_NONE, &error);
  g_assert_no_error(error);
  g_assert_cmpint(admit_text(store, "c", "m", T0, "hello", 0), ==, GH_STORE_ADMIT_DUPLICATE);
  gh_store_close(store);
  g_assert_null(gh_store_open_with_key(&config, key, other_id, GH_STORE_OPEN_NONE, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FOREIGN);
  g_clear_error(&error);
  g_assert_null(gh_store_open_with_key(&config, other_key, store_id, GH_STORE_OPEN_NONE, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_KEY);
  g_clear_error(&error);
  /* Only the raw key is accepted, not the x'...' literal or any other size. */
  g_assert_null(gh_store_open_with_key(&config, literal, store_id, GH_STORE_OPEN_CREATE, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);

  /* Forget without a provider: the caller destroyed the key item first. */
  store = gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_NONE, &error);
  g_assert_no_error(error);
  g_assert_true(gh_store_forget(store, NULL, &error));
  g_assert_no_error(error);
  g_assert_false(path_exists(dir));
  g_assert_true(gh_store_exists(NULL, ACCOUNT_A, &exists, &error));
  g_assert_false(exists);
  g_assert_true(gh_store_delete_files(NULL, ACCOUNT_A, &error));
  g_assert_no_error(error);
}

static void
test_open_twice(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_BUSY);
  gh_store_close(store);
  gh_store_close(open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE));
}

/* ---- No plaintext temporary files: a recording VFS sees every file SQLite opens ------------ */

typedef struct {
  sqlite3_vfs vfs; /* first: SQLite passes &vfs back to us */
  sqlite3_vfs *real;
  guint n_temp;
  GPtrArray *paths;
} ShimVfs;

static ShimVfs shim;

static int
shim_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *out_flags)
{
  ShimVfs *self = (ShimVfs *) vfs;
  const int temporary = SQLITE_OPEN_TEMP_DB | SQLITE_OPEN_TEMP_JOURNAL |
                        SQLITE_OPEN_SUBJOURNAL | SQLITE_OPEN_TRANSIENT_DB;
  if (!name || (flags & temporary))
    self->n_temp++;
  else
    g_ptr_array_add(self->paths, g_strdup(name));
  return self->real->xOpen(self->real, name, file, flags, out_flags);
}

static void
shim_install(void)
{
  memset(&shim, 0, sizeof shim);
  shim.real = sqlite3_vfs_find(NULL);
  g_assert_nonnull(shim.real);
  shim.vfs = *shim.real;
  shim.vfs.zName = "groundhog-test-shim";
  shim.vfs.pNext = NULL;
  shim.vfs.xOpen = shim_open;
  shim.paths = g_ptr_array_new_with_free_func(g_free);
  g_assert_cmpint(sqlite3_vfs_register(&shim.vfs, 1), ==, SQLITE_OK);
}

static void
shim_uninstall(void)
{
  sqlite3_vfs_unregister(&shim.vfs);
  g_assert_cmpint(sqlite3_vfs_register(shim.real, 1), ==, SQLITE_OK);
  g_ptr_array_unref(shim.paths);
}

#define BULK_ROWS "20000"
#define BULK_CTE "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < " BULK_ROWS ") "

static guint
drain_sort(sqlite3 *db, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  guint rows = 0;
  g_assert_cmpint(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL), ==, SQLITE_OK);
  while (sqlite3_step(stmt) == SQLITE_ROW)
    rows++;
  g_assert_cmpint(sqlite3_finalize(stmt), ==, SQLITE_OK);
  return rows;
}

static void
test_no_temp_files(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  shim_install();
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  gint64 conversation = ensure_conversation(store, "bulk");
  g_autofree gchar *fill = g_strdup_printf(
    BULK_CTE "INSERT INTO messages (conversation_id, backend_msg_id, sender_pubkey, kind, "
    "created_at, received_at, direction, body, raw_json) SELECT %" G_GINT64_FORMAT ", "
    "printf('%%064x', i), '%s', 14, i, i, 0, hex(randomblob(150)), '{}' FROM n",
    conversation, PEER);

  /* ~6 MB of bodies: a sort that exceeds the page cache and a statement
   * journal far above SQLite's in-memory spill threshold. */
  g_assert_true(gh_store_begin(store, &error));
  g_assert_true(gh_store_exec(store, fill, &error));
  g_assert_no_error(error);
  g_assert_true(gh_store_commit(store, &error));
  g_assert_cmpuint(drain_sort(gh_store_get_db(store), "SELECT body FROM messages ORDER BY body"),
                   ==, 20000);
  g_assert_true(gh_store_begin(store, &error));
  g_assert_true(gh_store_exec(store, "UPDATE messages SET body = body || 'x'", &error));
  g_assert_no_error(error);
  g_assert_true(gh_store_commit(store, &error));

  g_assert_cmpuint(shim.n_temp, ==, 0);
  g_assert_cmpuint(shim.paths->len, >, 0);
  for (guint i = 0; i < shim.paths->len; i++)
    g_assert_true(g_str_has_prefix(g_ptr_array_index(shim.paths, i), gh_store_get_dir(store)));
  gh_store_close(store);

  /* Control: the same work with temp_store=FILE opens temporary files, so the
   * shim would have seen any the store opened. */
  guint before = shim.n_temp;
  g_autofree gchar *scratch = g_build_filename(g_get_user_data_dir(), "scratch.db", NULL);
  sqlite3 *db = NULL;
  g_assert_cmpint(sqlite3_open_v2(scratch, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL),
                  ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_exec(db, "PRAGMA temp_store=FILE; PRAGMA cache_size=-256; "
                                   "CREATE TABLE t (x TEXT); " BULK_CTE
                                   "INSERT INTO t SELECT hex(randomblob(150)) FROM n;",
                               NULL, NULL, NULL), ==, SQLITE_OK);
  g_assert_cmpuint(drain_sort(db, "SELECT x FROM t ORDER BY x"), ==, 20000);
  sqlite3_close(db);
  g_assert_cmpuint(shim.n_temp, >, before);
  shim_uninstall();
}

/* ---- In-memory store (KC-4 "Continue Without Saving Messages") ----------------------------- */

static void
test_ephemeral_store(void)
{
  g_autoptr(GError) error = NULL;
  g_autofree gchar *root = groundhog_root();
  shim_install();
  GhStore *store = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  g_assert_true(gh_store_is_ephemeral(store));
  g_assert_null(gh_store_get_path(store));
  g_assert_cmpint(admit_text(store, "c", "m", T0, "GROUNDHOG-CANARY-ephemeral", 0), ==,
                  GH_STORE_ADMIT_STORED);
  g_assert_cmpint(admit_text(store, "c", "m", T0, "GROUNDHOG-CANARY-ephemeral", 0), ==,
                  GH_STORE_ADMIT_DUPLICATE);
  gint64 outbox = enqueue_text(store, find_conversation(store, "c"), "out", "reply", &error);
  g_assert_no_error(error);
  g_assert_true(seal_three(store, outbox, &error));
  gh_store_close(store);
  /* Nothing touched the file system at all. */
  g_assert_cmpuint(shim.n_temp, ==, 0);
  g_assert_cmpuint(shim.paths->len, ==, 0);
  shim_uninstall();
  g_assert_false(path_exists(root));

  store = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error);
  g_assert_true(gh_store_forget(store, NULL, &error));
  g_assert_no_error(error);
}

/* ---- Cursors (G04: the inbox checkpoint lives in the store) ------------------------------------ */

static gint64
cursor_of(GhStore *store, const gchar *scope, const gchar *url)
{
  g_autoptr(GError) error = NULL;
  gint64 since = -1;
  g_assert_true(gh_store_get_cursor(store, scope, url, &since, &error));
  g_assert_no_error(error);
  return since;
}

static void
test_cursors(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_assert_cmpint(cursor_of(store, "nip17-inbox", ""), ==, 0);
  g_assert_true(gh_store_set_cursor(store, "nip17-inbox", "", T0, &error));
  g_assert_true(gh_store_set_cursor(store, "nip17-inbox", "wss://a.test.invalid", T0 + 5, &error));
  g_assert_true(gh_store_set_cursor(store, "nip17-inbox", "", T0 + 1, &error));
  g_assert_no_error(error);
  gh_store_close(store);

  /* Durable, per (scope, relay), replaced in place. */
  store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE);
  g_assert_cmpint(cursor_of(store, "nip17-inbox", ""), ==, T0 + 1);
  g_assert_cmpint(cursor_of(store, "nip17-inbox", "wss://a.test.invalid"), ==, T0 + 5);
  g_assert_cmpint(cursor_of(store, "other", ""), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM cursors"), ==, 2);
  /* 0 deletes. */
  g_assert_true(gh_store_set_cursor(store, "nip17-inbox", "wss://a.test.invalid", 0, &error));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM cursors"), ==, 1);

  /* Bounds. */
  gint64 since = 0;
  g_autofree gchar *long_scope = g_strnfill(GH_STORE_MAX_CURSOR_SCOPE + 1, 's');
  g_autofree gchar *long_url = g_strnfill(GH_STORE_MAX_URL + 1, 'u');
  g_assert_false(gh_store_set_cursor(store, long_scope, "", T0, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_set_cursor(store, "", "", T0, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_set_cursor(store, "s", NULL, T0, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_get_cursor(store, "s", long_url, &since, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_set_cursor(store, "s", "", -1, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  gh_store_close(store);

  /* The in-memory store keeps cursors like the others, and nothing after close. */
  store = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error);
  g_assert_no_error(error);
  g_assert_true(gh_store_set_cursor(store, "nip17-inbox", "", T0, &error));
  g_assert_cmpint(cursor_of(store, "nip17-inbox", ""), ==, T0);
  gh_store_close(store);
}

/* ---- Transactions ------------------------------------------------------------------------------ */

static gboolean
failing_transaction(GhStore *store, gpointer data, GError **error)
{
  g_assert_true(gh_store_set_draft(store, *(gint64 *) data, "never saved", NULL));
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "scripted failure");
  return FALSE;
}

static void
test_transactions(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  gint64 conversation = ensure_conversation(store, "conversation");
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 0);

  g_assert_true(gh_store_begin(store, &error));
  g_assert_true(gh_store_set_draft(store, conversation, "outer", &error));
  g_assert_true(gh_store_begin(store, &error));
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 2);
  g_assert_true(gh_store_set_draft(store, conversation, "inner", &error));
  gh_store_rollback(store);
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 1);
  g_autofree gchar *after_inner = draft_of(store, conversation);
  g_assert_cmpstr(after_inner, ==, "outer");
  g_assert_true(gh_store_commit(store, &error));
  g_assert_no_error(error);

  g_assert_false(gh_store_transaction(store, failing_transaction, &conversation, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
  g_clear_error(&error);
  g_autofree gchar *after_failure = draft_of(store, conversation);
  g_assert_cmpstr(after_failure, ==, "outer");
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 0);

  /* A T-* operation inside a caller's transaction is part of it (T-mls). */
  g_autofree gchar *rumor = hex_of("inside");
  g_assert_true(gh_store_begin(store, &error));
  g_assert_cmpint(admit_text(store, "conversation", "inside", T0, "x", 0), ==, GH_STORE_ADMIT_STORED);
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 1);
  gh_store_rollback(store);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 0);
  g_assert_false(is_seen(store, GH_STORE_SEEN_RUMOR, rumor));
  g_assert_true(gh_store_begin(store, &error));
  g_assert_cmpint(admit_text(store, "conversation", "inside", T0, "x", 0), ==, GH_STORE_ADMIT_STORED);
  g_assert_true(gh_store_commit(store, &error));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 1);
  gh_store_close(store);
}

/* ---- Conversation title ------------------------------------------------------------------------ */

/* The title is the subject of the newest message, whatever order backfill
 * delivers them in and even if the conversation was opened before. */
static void
test_title_follows_newest(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  GhStore *store = open_ok(keys, ACCOUNT_A, clock, GH_STORE_OPEN_CREATE);
  ensure_conversation(store, "c");
  const struct { const gchar *seed; gint64 created_at; const gchar *title; const gchar *expect; } steps[] = {
    { "t1", T0 - 100, "Trip", "Trip" },
    { "t2", T0 - 200, "Older", "Trip" },
    { "t3", T0 - 50, NULL, "Trip" },
    { "t4", T0 - 40, "Plans", "Plans" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(steps); i++) {
    TestMessage t;
    test_message_init(&t, "c", steps[i].seed, NULL, steps[i].created_at, "hi");
    t.message.title = steps[i].title;
    g_assert_cmpint(admit_ok(store, &t.message), ==, GH_STORE_ADMIT_STORED);
    test_message_clear(&t);
    g_autofree gchar *title = sql_text(store, "SELECT title FROM conversations");
    g_assert_cmpstr(title, ==, steps[i].expect);
  }
  gh_store_close(store);
}

/* ---- Integrity failure: read-only (STORE_CORRUPT) --------------------------------------------- */

static void
test_corrupt_read_only(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  for (guint i = 0; i < 60; i++) {
    g_autofree gchar *seed = g_strdup_printf("bulk/%u", i);
    g_autofree gchar *body = g_strnfill(2000, (gchar) ('a' + i % 26));
    g_assert_cmpint(admit_text(store, "c", seed, T0 + i, body, 0), ==, GH_STORE_ADMIT_STORED);
  }
  g_autofree gchar *path = g_strdup(gh_store_get_path(store));
  gh_store_close(store);

  /* Flip one bit in a page past the schema: the page HMAC no longer matches. */
  gint64 size = path_size(path);
  g_assert_cmpint(size, >, 32 * 4096);
  int fd = open(path, O_RDWR);
  g_assert_cmpint(fd, >=, 0);
  const off_t offset = (off_t) (size - 3 * 4096 + 1000);
  guint8 byte = 0;
  g_assert_cmpint(pread(fd, &byte, 1, offset), ==, 1);
  byte ^= 0x40;
  g_assert_cmpint(pwrite(fd, &byte, 1, offset), ==, 1);
  close(fd);

  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_NONE, GH_STORE_ERROR_CORRUPT);
  g_autofree gchar *sha = file_sha256(path);
  /* A damaged store is still only shown if it is this key item's. */
  FakeItem *item = fake_item(keys, ACCOUNT_A);
  g_autofree gchar *saved_id = item->store_id;
  item->store_id = g_uuid_string_random();
  OPEN_FAILS(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_ALLOW_CORRUPT, GH_STORE_ERROR_FOREIGN);
  g_free(item->store_id);
  item->store_id = g_steal_pointer(&saved_id);
  store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_ALLOW_CORRUPT);
  g_assert_true(gh_store_is_read_only(store));
  /* Undamaged data is still readable. */
  g_autofree gchar *meta_account =
    sql_text(store, "SELECT value FROM meta WHERE key = 'account_pubkey'");
  g_assert_cmpstr(meta_account, ==, ACCOUNT_A);
  g_assert_cmpint(sql_int(store, "SELECT created_at FROM messages WHERE id = 1"), ==, T0);
  g_assert_false(gh_store_set_draft(store, 1, "x", &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT);
  g_clear_error(&error);
  TestMessage t;
  test_message_init(&t, "c", "late", NULL, T0, "late");
  g_assert_false(gh_store_admit(store, &t.message, NULL, NULL, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT);
  test_message_clear(&t);
  gh_store_close(store);
  g_autofree gchar *sha_after = file_sha256(path);
  g_assert_cmpstr(sha_after, ==, sha);
}

/* ---- Bounds ----------------------------------------------------------------------------------------- */

static void
expect_admit_invalid(GhStore *store, const GhStoreMessage *message)
{
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_store_admit(store, message, NULL, NULL, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
}

static void
test_bounds(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, NULL, GH_STORE_OPEN_CREATE);
  g_autofree gchar *big_body = g_strnfill(GH_STORE_MAX_BODY + 1, 'b');
  g_autofree gchar *big_json = g_strnfill(GH_STORE_MAX_EVENT_JSON + 1, 'j');
  g_autofree gchar *upper = g_ascii_strup(PEER, -1);
  const gchar *crowd[GH_STORE_MAX_PARTICIPANTS + 2];
  for (guint i = 0; i < GH_STORE_MAX_PARTICIPANTS + 1; i++)
    crowd[i] = PEER;
  crowd[GH_STORE_MAX_PARTICIPANTS + 1] = NULL;
  TestMessage t;
  test_message_init(&t, "bounds", "m", NULL, T0, "ok");
  GhStoreMessage m;

  m = t.message; m.body = big_body; expect_admit_invalid(store, &m);
  m = t.message; m.raw_json = big_json; expect_admit_invalid(store, &m);
  m = t.message; m.raw_json = NULL; expect_admit_invalid(store, &m);
  m = t.message; m.backend_msg_id = upper; expect_admit_invalid(store, &m);
  m = t.message; m.sender_pubkey = "abcd"; expect_admit_invalid(store, &m);
  m = t.message; m.backend = GH_STORE_BACKEND_NIP29; expect_admit_invalid(store, &m);
  m = t.message; m.body = "\xff\xfe"; expect_admit_invalid(store, &m);
  m = t.message; m.participants = crowd; expect_admit_invalid(store, &m);
  m = t.message; m.created_at = -1; expect_admit_invalid(store, &m);
  /* NIP-29 ids are event ids too: exactly 64 hex, like the seen check. */
  m = t.message; m.backend = GH_STORE_BACKEND_NIP29; m.wrap_id = NULL;
  m.backend_msg_id = "0123456789abcdef0123456789abcdef01234567"; expect_admit_invalid(store, &m);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM seen"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM conversations"), ==, 0);
  test_message_clear(&t);

  gint64 conversation = ensure_conversation(store, "bounds");
  g_autofree gchar *rumor = hex_of("out");
  GhStoreOutgoing outgoing = { conversation, "0123456789abcdef0123456789abcde", rumor, ACCOUNT_A,
                               14, T0, "x", "{}", NULL, 0 };
  g_assert_false(gh_store_enqueue(store, &outgoing, NULL, NULL, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_seal(store, 1, NULL, 0, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_autofree gchar *big_draft = g_strnfill(GH_STORE_MAX_DRAFT + 1, 'd');
  g_assert_false(gh_store_set_draft(store, conversation, big_draft, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  gh_store_close(store);
}

/* ---- T-purge: expiry, retention, WAL truncation (store level of EX-3..EX-6) --------------- */

static void
test_purge(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, clock, GH_STORE_OPEN_CREATE);
  g_autofree gchar *wal = g_strconcat(gh_store_get_path(store), "-wal", NULL);
  GhStorePurgeStats stats;
  g_assert_cmpint(admit_text(store, "c", "m1", T0 - 100, "one", T0 + 10), ==, GH_STORE_ADMIT_STORED);
  g_assert_cmpint(admit_text(store, "c", "m2", T0 - 100, "two", T0 + 20), ==, GH_STORE_ADMIT_STORED);
  TestMessage old;
  test_message_init(&old, "c", "m3", NULL, T0 - 40 * 86400, "three");
  old.message.received_at = T0 - 40 * 86400;
  g_assert_cmpint(admit_ok(store, &old.message), ==, GH_STORE_ADMIT_STORED);
  test_message_clear(&old);
  /* A disappearing message we sent: its text is also in the outbox. */
  g_autofree gchar *sent_rumor = hex_of("sent");
  g_autofree gchar *sent_raw = g_strdup_printf("{\"id\":\"%s\"}", sent_rumor);
  GhStoreOutgoing sent = { find_conversation(store, "c"), "00112233445566778899aabbccddeeff",
                           sent_rumor, ACCOUNT_A, 14, T0 - 100, "GROUNDHOG-CANARY-sent",
                           sent_raw, NULL, T0 + 10 };
  gint64 sent_outbox = 0;
  g_assert_true(gh_store_enqueue(store, &sent, &sent_outbox, NULL, &error));
  g_assert_true(seal_three(store, sent_outbox, &error));
  g_assert_no_error(error);

  g_assert_true(gh_store_purge(store, 0, &stats, &error));
  g_assert_cmpuint(stats.n_expired, ==, 0);
  g_assert_cmpint(stats.next_expires_at, ==, T0 + 10);
  g_assert_false(stats.checkpointed);

  gh_clock_fake_advance(clock, 15 * G_USEC_PER_SEC);
  g_assert_true(gh_store_purge(store, 0, &stats, &error));
  g_assert_cmpuint(stats.n_expired, ==, 2);
  g_assert_cmpuint(stats.n_outbox, ==, 1);
  g_assert_true(stats.checkpointed);
  g_assert_cmpint(path_size(wal), ==, 0);
  g_assert_cmpint(stats.next_expires_at, ==, T0 + 20);
  /* The outgoing text and its signed events are gone with the message. */
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_events"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_targets"), ==, 0);

  /* At most one TRUNCATE checkpoint a minute. */
  gh_clock_fake_advance(clock, 10 * G_USEC_PER_SEC);
  g_assert_true(gh_store_purge(store, 0, &stats, &error));
  g_assert_cmpuint(stats.n_expired, ==, 1);
  g_assert_false(stats.checkpointed);
  g_assert_true(stats.checkpoint_deferred);
  g_assert_cmpint(stats.next_expires_at, ==, 0);

  /* Retention purges by received_at. */
  gh_clock_fake_advance(clock, 60 * G_USEC_PER_SEC);
  g_assert_true(gh_store_purge(store, T0 - 30 * 86400, &stats, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(stats.n_retention, ==, 1);
  g_assert_true(stats.checkpointed);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM messages"), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM seen WHERE ns = 2"), ==, 4);
  g_assert_cmpint(sql_int(store, "SELECT unread_count FROM conversations"), ==, 0);

  /* Inside a caller's transaction the WAL truncation waits: reported. */
  g_assert_cmpint(admit_text(store, "c", "m5", T0, "five", gh_clock_get_unix(clock) + 5), ==,
                  GH_STORE_ADMIT_STORED);
  gh_clock_fake_advance(clock, 120 * G_USEC_PER_SEC);
  g_assert_true(gh_store_begin(store, &error));
  g_assert_true(gh_store_purge(store, 0, &stats, &error));
  g_assert_cmpuint(stats.n_expired, ==, 1);
  g_assert_false(stats.checkpointed);
  g_assert_true(stats.checkpoint_deferred);
  g_assert_true(gh_store_commit(store, &error));
  g_assert_no_error(error);

  /* EX-6: another relay's copy of a purged message does not come back. */
  TestMessage copy;
  test_message_init(&copy, "c", "m1", "m1/relay-2", T0 - 100, "one");
  copy.message.expires_at = T0 + 10;
  g_assert_cmpint(admit_ok(store, &copy.message), ==, GH_STORE_ADMIT_DUPLICATE);
  test_message_clear(&copy);

  /* EX-4: expired on arrival is seen-recorded only; no conversation appears. */
  const gint64 now = gh_clock_get_unix(clock);
  g_autofree gchar *late = hex_of("m4");
  g_assert_cmpint(admit_text(store, "fresh", "m4", now - 100, "late", now - 1), ==,
                  GH_STORE_ADMIT_EXPIRED);
  g_assert_cmpint(find_conversation(store, "fresh"), ==, -1);
  g_assert_true(is_seen(store, GH_STORE_SEEN_RUMOR, late));
  gh_store_close(store);
}

/* ---- T-outcome ------------------------------------------------------------------------------------ */

/* One column of the wss://inbox-a.example target row of @event. */
static gint64
target_a(GhStore *store, gint64 event, const gchar *column)
{
  g_autofree gchar *expression = g_strdup(column);
  return sql_int(store, "SELECT %s FROM outbox_targets WHERE outbox_event_id = %" G_GINT64_FORMAT
                        " AND relay_url = 'wss://inbox-a.example'", expression, event);
}

static void
test_outcome(void)
{
  g_autoptr(FakeKeys) keys = fake_keys_new();
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  g_autoptr(GError) error = NULL;
  GhStore *store = open_ok(keys, ACCOUNT_A, clock, GH_STORE_OPEN_CREATE);
  gint64 conversation = ensure_conversation(store, "c");
  gint64 outbox = enqueue_text(store, conversation, "out", "hello", &error);
  g_assert_true(seal_three(store, outbox, &error));
  g_assert_no_error(error);
  gint64 event = sql_int(store, "SELECT id FROM outbox_events WHERE outbox_id = %" G_GINT64_FORMAT
                                " AND role = 0 ORDER BY id LIMIT 1", outbox);

  GhStoreTargetOutcome accepted = { "wss://inbox-a.example", 1, -1, NULL, TRUE };
  g_assert_true(gh_store_record_outcome(store, event, &accepted, &error));
  g_assert_true(gh_store_record_outcome(store, event, &accepted, &error));
  g_assert_no_error(error);
  g_assert_cmpint(target_a(store, event, "attempts"), ==, 2);
  g_assert_cmpint(target_a(store, event, "ok_prefix IS NULL"), ==, 1);
  g_assert_cmpint(target_a(store, event, "last_attempt_at"), ==, T0);
  /* A cancelled publish (switch/quit) is not an attempt. */
  gh_clock_fake_advance(clock, 30 * G_USEC_PER_SEC);
  GhStoreTargetOutcome cancelled = { "wss://inbox-a.example", 5, -1, NULL, FALSE };
  g_assert_true(gh_store_record_outcome(store, event, &cancelled, &error));
  g_assert_cmpint(target_a(store, event, "attempts"), ==, 2);
  g_assert_cmpint(target_a(store, event, "outcome"), ==, 5);
  g_assert_cmpint(target_a(store, event, "last_attempt_at"), ==, T0);

  /* The recipient's 10050 changed after sealing: same wrap, new target. */
  GhStoreTargetOutcome moved = { "wss://inbox-c.example", 2, 3, "rate-limited: slow down", TRUE };
  g_assert_true(gh_store_record_outcome(store, event, &moved, &error));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_targets WHERE outbox_event_id = %"
                                 G_GINT64_FORMAT, event), ==, 3);

  /* Relay text is bounded and made valid UTF-8. */
  GString *noise = g_string_new(NULL);
  for (guint i = 0; i < 3000; i++)
    g_string_append(noise, "\xc3\xa9");
  g_string_append(noise, "\xff");
  GhStoreTargetOutcome noisy = { "wss://inbox-a.example", 2, 1, noise->str, TRUE };
  g_assert_true(gh_store_record_outcome(store, event, &noisy, &error));
  g_string_free(noise, TRUE);
  g_autofree gchar *stored = sql_text(store, "SELECT ok_message FROM outbox_targets WHERE "
                                      "outbox_event_id = %" G_GINT64_FORMAT
                                      " AND relay_url = 'wss://inbox-a.example'", event);
  g_assert_cmpuint(strlen(stored), <=, GH_STORE_MAX_OK_MESSAGE);
  g_assert_cmpuint(strlen(stored), >, GH_STORE_MAX_OK_MESSAGE - 4);
  g_assert_true(g_utf8_validate(stored, -1, NULL));

  /* An unknown event is refused by its foreign key. */
  g_assert_false(gh_store_record_outcome(store, 999999, &accepted, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  gh_store_close(store);
}

/* ---- GhClock (H6) ------------------------------------------------------------------------------------ */

typedef struct {
  const gchar *name;
  GString *log;
  GhClock *clock;
  gint64 origin;
  guint repeats;
  guint notified;
} Tick;

static gboolean
tick(gpointer data)
{
  Tick *t = data;
  g_string_append_printf(t->log, "%s@%" G_GINT64_FORMAT " ", t->name,
                         (gh_clock_get_monotonic_time(t->clock) - t->origin) / 1000);
  if (t->repeats > 0) {
    t->repeats--;
    return G_SOURCE_CONTINUE;
  }
  return G_SOURCE_REMOVE;
}

static void
tick_notify(gpointer data)
{
  ((Tick *) data)->notified++;
}

static void
test_clock_fake(void)
{
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  g_assert_true(gh_clock_is_fake(clock));
  g_assert_cmpint(gh_clock_get_unix(clock), ==, T0);
  const gint64 origin = gh_clock_get_monotonic_time(clock);
  GString *log = g_string_new(NULL);
  Tick a = { "a", log, clock, origin, 0, 0 };
  Tick b = { "b", log, clock, origin, 0, 0 };
  Tick c = { "c", log, clock, origin, 2, 0 };
  gh_clock_timeout_add(clock, 3000, tick, &a, tick_notify);
  gh_clock_timeout_add(clock, 1000, tick, &b, tick_notify);
  gh_clock_timeout_add(clock, 1000, tick, &c, tick_notify);
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(clock), ==, 3);
  g_assert_cmpuint(gh_clock_fake_get_shortest_interval_ms(clock), ==, 1000);
  g_assert_cmpint(gh_clock_fake_get_next_deadline(clock), ==, origin + 1000 * 1000);

  gh_clock_fake_advance(clock, 2500 * 1000);
  g_assert_cmpstr(log->str, ==, "b@1000 c@1000 c@2000 ");
  g_assert_cmpint(gh_clock_get_monotonic_time(clock) - origin, ==, 2500 * 1000);
  gh_clock_fake_advance(clock, 1000 * 1000);
  g_assert_cmpstr(log->str, ==, "b@1000 c@1000 c@2000 a@3000 c@3000 ");
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(clock), ==, 0);
  g_assert_cmpuint(a.notified + b.notified + c.notified, ==, 3);
  g_assert_cmpint(gh_clock_get_unix(clock), ==, T0 + 3);

  Tick d = { "d", log, clock, origin, 0, 0 };
  guint id = gh_clock_timeout_add(clock, 1000, tick, &d, tick_notify);
  g_assert_true(gh_clock_source_remove(clock, id));
  g_assert_false(gh_clock_source_remove(clock, id));
  gh_clock_fake_advance(clock, 5000 * 1000);
  g_assert_cmpuint(d.notified, ==, 1);
  g_assert_null(strstr(log->str, "d@"));
  g_assert_cmpint(gh_clock_fake_get_next_deadline(clock), ==, -1);

  /* A wall-clock step moves neither monotonic time nor timers. */
  const gint64 monotonic = gh_clock_get_monotonic_time(clock);
  gh_clock_fake_set_real_time(clock, (T0 + 86400) * G_USEC_PER_SEC);
  g_assert_cmpint(gh_clock_get_unix(clock), ==, T0 + 86400);
  g_assert_cmpint(gh_clock_get_monotonic_time(clock), ==, monotonic);

  /* Jitter: scripted values first, then a fixed-seed sequence in bounds. */
  gh_clock_fake_push_random(clock, 7);
  gh_clock_fake_push_random(clock, 85);
  g_assert_cmpuint(gh_clock_random_uniform(clock, 5), ==, 2);
  g_assert_cmpint(gh_clock_random_range(clock, 5, 90), ==, 90);
  for (guint i = 0; i < 1000; i++) {
    gint64 value = gh_clock_random_range(clock, 5, 90);
    g_assert_cmpint(value, >=, 5);
    g_assert_cmpint(value, <=, 90);
  }
  g_string_free(log, TRUE);
}

static gboolean
set_flag(gpointer data)
{
  *(gboolean *) data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
test_clock_system(void)
{
  g_autoptr(GhClock) clock = gh_clock_new_system();
  g_assert_false(gh_clock_is_fake(clock));
  g_assert_cmpint(ABS(gh_clock_get_real_time(clock) - g_get_real_time()), <, 5 * G_USEC_PER_SEC);
  const gint64 first = gh_clock_get_monotonic_time(clock);
  g_assert_cmpint(gh_clock_get_monotonic_time(clock), >=, first);
  for (guint i = 0; i < 1000; i++) {
    gint64 value = gh_clock_random_range(clock, 5, 90);
    g_assert_cmpint(value, >=, 5);
    g_assert_cmpint(value, <=, 90);
  }
  /* A zero interval is ready at once: no waiting involved. */
  gboolean fired = FALSE;
  gh_clock_timeout_add(clock, 0, set_flag, &fired, NULL);
  while (!fired)
    g_main_context_iteration(NULL, TRUE);
  gboolean never = FALSE;
  guint id = gh_clock_timeout_add(clock, 60 * 1000, set_flag, &never, NULL);
  g_assert_true(gh_clock_source_remove(clock, id));
  g_assert_false(gh_clock_source_remove(clock, id));
  g_assert_false(never);
}

static void
register_full_suite(void)
{
  g_test_add_func("/groundhog/store/layout/account-dir", test_layout_account_dir);
  g_test_add_func("/groundhog/store/st1/encrypted-file", test_st1_encrypted_file);
  g_test_add_func("/groundhog/store/st1/unreadable-without-key", test_st1_unreadable_without_key);
  g_test_add_func("/groundhog/store/st2/wrong-key", test_st2_wrong_key);
  g_test_add_func("/groundhog/store/st3/modes", test_st3_modes);
  g_test_add_func("/groundhog/store/st3/refuse-unsafe", test_st3_refuse_unsafe);
  g_test_add_func("/groundhog/store/st5/pragmas", test_st5_pragmas);
  g_test_add_func("/groundhog/store/st6/crash-create", test_st6_crash_create);
  g_test_add_func("/groundhog/store/st6/crash-admit", test_st6_crash_admit);
  g_test_add_func("/groundhog/store/st6/crash-enqueue", test_st6_crash_enqueue);
  g_test_add_func("/groundhog/store/st6/crash-seal", test_st6_crash_seal);
  g_test_add_func("/groundhog/store/st6/crash-purge", test_st6_crash_purge);
  g_test_add_func("/groundhog/store/st6/crash-forget", test_st6_crash_forget);
  g_test_add_func("/groundhog/store/st7/idempotent-admission", test_st7_idempotent_admission);
  g_test_add_func("/groundhog/store/st9/forget-conversation", test_st9_forget_conversation);
  g_test_add_func("/groundhog/store/st10/disk-full-admission", test_st10_disk_full_admission);
  g_test_add_func("/groundhog/store/st10/disk-full-enqueue", test_st10_disk_full_enqueue);
  g_test_add_func("/groundhog/store/st11/schema-version", test_st11_schema_version);
  g_test_add_func("/groundhog/store/key/first-open", test_key_first_open);
  g_test_add_func("/groundhog/store/key/missing-store-untouched", test_key_missing_store_untouched);
  g_test_add_func("/groundhog/store/key/locked-unavailable", test_key_locked_and_unavailable);
  g_test_add_func("/groundhog/store/key/foreign", test_key_foreign);
  g_test_add_func("/groundhog/store/forget/account", test_forget_account);
  g_test_add_func("/groundhog/store/open-with-key", test_open_with_key);
  g_test_add_func("/groundhog/store/open-twice", test_open_twice);
  g_test_add_func("/groundhog/store/no-temp-files", test_no_temp_files);
  g_test_add_func("/groundhog/store/ephemeral", test_ephemeral_store);
  g_test_add_func("/groundhog/store/cursors", test_cursors);
  g_test_add_func("/groundhog/store/transactions", test_transactions);
  g_test_add_func("/groundhog/store/title-follows-newest", test_title_follows_newest);
  g_test_add_func("/groundhog/store/corrupt-read-only", test_corrupt_read_only);
  g_test_add_func("/groundhog/store/bounds", test_bounds);
  g_test_add_func("/groundhog/store/purge", test_purge);
  g_test_add_func("/groundhog/store/outcome", test_outcome);
  g_test_add_func("/groundhog/clock/fake", test_clock_fake);
  g_test_add_func("/groundhog/clock/system", test_clock_system);
}
#endif /* !GH_STORE_TEST_LINK_SET */

int
main(int argc, char **argv)
{
#ifdef GH_STORE_TEST_LINK_SET
  /* Child mode for the loader checks: the guard, then a real store. */
  if (argc > 1 && strcmp(argv[1], "--st4-probe") == 0) {
    GError *error = NULL;
    GhStore *store = NULL;
    if (gh_store_check_sqlite(&error) &&
        (store = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error)) &&
        admit_text(store, "probe", "probe", T0, "probe", 0) == GH_STORE_ADMIT_STORED) {
      gh_store_close(store);
      return 0;
    }
    g_printerr("%s\n", error ? error->message : "store round trip failed");
    g_clear_error(&error);
    return 3;
  }
#endif
  umask(022);
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);

  g_test_add_func("/groundhog/store/st4/single-sqlite", test_st4_single_sqlite);
#ifdef GH_STORE_TEST_LINK_SET
  g_test_add_func("/groundhog/store/st4/round-trip", test_st4_store_round_trip);
  g_test_add_func("/groundhog/store/st4/groundhog-loader-scope", test_st4_groundhog_loader_scope);
  g_test_add_func("/groundhog/store/st4/preloaded-sqlite-refused", test_st4_preloaded_sqlite_refused);
  g_test_add_func("/groundhog/store/st4/libsoup-shadowed", test_st4_libsoup_shadowed);
#else
  register_full_suite();
#endif
  return g_test_run();
}
