#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* dladdr() and RTLD_DEFAULT on glibc */
#endif

#include "gh-store.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <sodium.h>
#include <sqlite3.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#endif

/* Refuse to compile against plain SQLite's header: sqlite3_key_v2() below is
 * only declared by SQLCipher's sqlite3.h with SQLITE_HAS_CODEC. */
#ifndef SQLITE_HAS_CODEC
#error "GhStore must be built against SQLCipher's sqlite3.h with SQLITE_HAS_CODEC"
#endif

G_DEFINE_QUARK(gh-store-error-quark, gh_store_error)

#define STORE_BUSY_TIMEOUT_MS        5000
#define STORE_JOURNAL_SIZE_LIMIT     (4 * 1024 * 1024)
#define STORE_SQL_LENGTH_LIMIT       (1024 * 1024)
#define STORE_CHECKPOINT_INTERVAL_US (60 * G_USEC_PER_SEC)
#define STORE_OPEN_FLAGS \
  (SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_FULLMUTEX | \
   SQLITE_OPEN_PRIVATECACHE | SQLITE_OPEN_EXRESCODE)

static const gchar account_dir_domain[] = "groundhog/v1/account-dir";
static const gchar *const store_sidecars[] = { "-wal", "-shm", "-journal" };

struct _GhStore {
  struct sqlite3 *db;
  gchar *account_pubkey;
  gchar *store_id;
  gchar *data_dir;        /* canonical; NULL when ephemeral */
  gchar *dir;             /* account directory; NULL when ephemeral */
  gchar *db_path;         /* NULL when ephemeral */
  gchar *cipher_version;
  GhStoreKeyProvider keys;
  gpointer keys_data;
  GhClock *clock;
  guint depth;            /* 0 = autocommit; 1 = BEGIN; >1 = savepoints */
  const gchar *txn_label; /* cut-point prefix of the outermost transaction */
  gboolean read_only;     /* failed quick_check, opened with ALLOW_CORRUPT */
  gboolean ephemeral;
  gboolean registered;    /* holds the in-process registry entry for dir */
  gboolean checkpointed_once;
  gint64 last_checkpoint; /* monotonic us of the last purge checkpoint */
  gint64 default_disappearing; /* disappearing_s of conversations created from now on */
  GhStoreExpiryFunc expiry_func; /* told about each message stored with an expiry */
  gpointer expiry_data;
  GDestroyNotify expiry_destroy;
};

/* ---- Test hooks (charter H8) ------------------------------------------------ */

#ifdef GH_STORE_TEST_HOOKS
static const gchar *const store_cut_points[] = {
  "create:begin", "create:schema", "create:meta",
  "create:before-commit", "create:after-commit",
  "migrate:before-commit", "migrate:after-commit",
  "txn:before-commit", "txn:after-commit",
  "admit:seen-wrap", "admit:seen-message", "admit:conversation", "admit:message",
  "admit:conversation-updated", "admit:participants",
  "admit:before-commit", "admit:after-commit",
  "enqueue:outbox", "enqueue:message", "enqueue:seen", "enqueue:draft",
  "enqueue:before-commit", "enqueue:after-commit",
  "seal:event", "seal:targets", "seal:state", "seal:before-commit", "seal:after-commit",
  "outcome:before-commit", "outcome:after-commit",
  "outbox:before-commit", "outbox:after-commit",
  "purge:expired", "purge:retention", "purge:conversations",
  "purge:before-commit", "purge:after-commit", "purge:checkpoint",
  "forget:outbox", "forget:messages", "forget:conversation",
  "forget:before-commit", "forget:after-commit",
  NULL
};

static gchar *armed_cut;
static guint armed_nth;
static guint armed_hits;
static gint64 test_uid = -1;

void
gh_store_test_crash_at(const gchar *cut_point, guint nth)
{
  g_clear_pointer(&armed_cut, g_free);
  armed_hits = 0;
  armed_nth = MAX(nth, 1);
  if (!cut_point)
    return;
  if (!g_strv_contains(store_cut_points, cut_point))
    g_error("Unknown GhStore cut point '%s'", cut_point);
  armed_cut = g_strdup(cut_point);
}

GStrv
gh_store_test_list_cut_points(const gchar *prefix)
{
  GPtrArray *names = g_ptr_array_new();
  for (guint i = 0; store_cut_points[i]; i++)
    if (!prefix || g_str_has_prefix(store_cut_points[i], prefix))
      g_ptr_array_add(names, g_strdup(store_cut_points[i]));
  g_ptr_array_add(names, NULL);
  return (GStrv) g_ptr_array_free(names, FALSE);
}

void
gh_store_test_set_uid(gint64 uid)
{
  test_uid = uid;
}

static void
store_cut(const gchar *label, const gchar *step)
{
  if (!armed_cut || !label)
    return;
  gsize n = strlen(label);
  if (strncmp(armed_cut, label, n) != 0 || armed_cut[n] != ':' ||
      strcmp(armed_cut + n + 1, step) != 0)
    return;
  if (++armed_hits < armed_nth)
    return;
  /* A crash, not an exit: no unwinding, no close, no buffered output. */
  kill(getpid(), SIGKILL);
  _exit(137);
}
#define STORE_CUT(label, step) store_cut((label), (step))

static uid_t
store_uid(void)
{
  return test_uid >= 0 ? (uid_t) test_uid : geteuid();
}
#else
#define STORE_CUT(label, step) ((void) 0)

static uid_t
store_uid(void)
{
  return geteuid();
}
#endif

/* ---- Validation -------------------------------------------------------------- */

static gboolean
is_lower_hex(const gchar *s, gsize min_len, gsize max_len)
{
  if (!s)
    return FALSE;
  gsize n = 0;
  for (; s[n]; n++) {
    if (n >= max_len)
      return FALSE;
    if (!((s[n] >= '0' && s[n] <= '9') || (s[n] >= 'a' && s[n] <= 'f')))
      return FALSE;
  }
  return n >= min_len;
}

static gboolean
check_hex(const gchar *what, const gchar *value, gsize min_len, gsize max_len,
          gboolean optional, GError **error)
{
  if (!value && optional)
    return TRUE;
  if (is_lower_hex(value, min_len, max_len))
    return TRUE;
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
              "%s must be %s lowercase hex", what,
              min_len == max_len ? "fixed-length" : "bounded");
  return FALSE;
}

static gboolean
check_text(const gchar *what, const gchar *value, gsize max_len, gboolean optional,
           GError **error)
{
  if (!value) {
    if (optional)
      return TRUE;
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "%s is required", what);
    return FALSE;
  }
  gsize len = strnlen(value, max_len + 1);
  if (len > max_len) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                "%s exceeds %" G_GSIZE_FORMAT " bytes", what, max_len);
    return FALSE;
  }
  if (!optional && len == 0) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "%s is empty", what);
    return FALSE;
  }
  if (!g_utf8_validate(value, (gssize) len, NULL)) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "%s is not valid UTF-8", what);
    return FALSE;
  }
  return TRUE;
}

static gboolean
check_account(const gchar *account_pubkey, GError **error)
{
  return check_hex("The account public key", account_pubkey, 64, 64, FALSE, error);
}

static gboolean
check_provider(const GhStoreKeyProvider *keys, GError **error)
{
  if (keys && keys->lookup && keys->store && keys->destroy)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                      "A complete store key provider is required");
  return FALSE;
}

static gboolean
check_backend(GhStoreBackend backend, GError **error)
{
  if (backend >= GH_STORE_BACKEND_NIP17 && backend <= GH_STORE_BACKEND_MLS)
    return TRUE;
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Unknown backend %d", backend);
  return FALSE;
}

static gboolean
check_nonnegative(const gchar *what, gint64 value, GError **error)
{
  if (value >= 0)
    return TRUE;
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "%s is negative", what);
  return FALSE;
}

/* Wrap and rumor ids are event ids; other namespaces allow bounded hex. */
static gboolean
check_seen_id(GhStoreSeenNs ns, const gchar *id, GError **error)
{
  switch (ns) {
  case GH_STORE_SEEN_WRAP:
  case GH_STORE_SEEN_RUMOR:
  case GH_STORE_SEEN_NIP29_EVENT:
  case GH_STORE_SEEN_REJECTED_WRAP:
    return check_hex("A seen event id", id, 64, 64, FALSE, error);
  case GH_STORE_SEEN_MLS_MESSAGE:
    return check_hex("A seen MLS message id", id, 1, GH_STORE_MAX_ID, FALSE, error);
  default:
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Unknown seen namespace %d", ns);
    return FALSE;
  }
}

static GhStoreSeenNs
seen_ns_for_backend(GhStoreBackend backend)
{
  switch (backend) {
  case GH_STORE_BACKEND_NIP29:
    return GH_STORE_SEEN_NIP29_EVENT;
  case GH_STORE_BACKEND_MLS:
    return GH_STORE_SEEN_MLS_MESSAGE;
  case GH_STORE_BACKEND_NIP17:
  default:
    return GH_STORE_SEEN_RUMOR;
  }
}

/* ---- Layout and file safety (§3.2) --------------------------------------------- */

gchar *
gh_store_account_dir_name(const gchar *account_pubkey)
{
  g_return_val_if_fail(is_lower_hex(account_pubkey, 64, 64), NULL);

  guint8 raw[32];
  for (guint i = 0; i < sizeof raw; i++)
    raw[i] = (guint8) ((g_ascii_xdigit_value(account_pubkey[2 * i]) << 4) |
                       g_ascii_xdigit_value(account_pubkey[2 * i + 1]));
  GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
  g_checksum_update(checksum, (const guchar *) account_dir_domain,
                    sizeof account_dir_domain - 1);
  g_checksum_update(checksum, raw, sizeof raw);
  gchar *name = g_strndup(g_checksum_get_string(checksum), 32);
  g_checksum_free(checksum);
  return name;
}

/* Canonical data directory: symlinks in the user's own data home are
 * resolved once, so SQLITE_OPEN_NOFOLLOW only has to reject links below it. */
static gchar *
resolve_data_dir(const gchar *data_dir, gboolean create, GError **error)
{
  const gchar *dir = data_dir ? data_dir : g_get_user_data_dir();
  if (!dir || !g_path_is_absolute(dir)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The data directory must be an absolute path");
    return NULL;
  }
  if (create && g_mkdir_with_parents(dir, 0700) != 0) {
    int saved = errno;
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Cannot create the data directory: %s", g_strerror(saved));
    return NULL;
  }
  char *real = realpath(dir, NULL);
  if (!real) {
    int saved = errno;
    g_set_error(error, GH_STORE_ERROR,
                saved == ENOENT ? GH_STORE_ERROR_NOT_FOUND : GH_STORE_ERROR_FAILED,
                "Cannot resolve the data directory: %s", g_strerror(saved));
    return NULL;
  }
  gchar *resolved = g_strdup(real);
  free(real);
  return resolved;
}

typedef struct {
  gchar *root;     /* <data>/groundhog */
  gchar *accounts; /* <root>/accounts */
  gchar *name;     /* account directory name */
  gchar *dir;      /* <accounts>/<name> */
  gchar *db;       /* <dir>/store.db */
} StoreLayout;

static void
layout_init(StoreLayout *layout, const gchar *data_dir, const gchar *account_pubkey)
{
  layout->root = g_build_filename(data_dir, "groundhog", NULL);
  layout->accounts = g_build_filename(layout->root, "accounts", NULL);
  layout->name = gh_store_account_dir_name(account_pubkey);
  layout->dir = g_build_filename(layout->accounts, layout->name, NULL);
  layout->db = g_build_filename(layout->dir, "store.db", NULL);
}

static void
layout_clear(StoreLayout *layout)
{
  g_clear_pointer(&layout->root, g_free);
  g_clear_pointer(&layout->accounts, g_free);
  g_clear_pointer(&layout->name, g_free);
  g_clear_pointer(&layout->dir, g_free);
  g_clear_pointer(&layout->db, g_free);
}

gchar *
gh_store_account_dir_path(const gchar *data_dir, const gchar *account_pubkey,
                          GError **error)
{
  if (!check_account(account_pubkey, error))
    return NULL;
  g_autofree gchar *resolved = resolve_data_dir(data_dir, FALSE, error);
  if (!resolved)
    return NULL;
  StoreLayout layout = { 0 };
  layout_init(&layout, resolved, account_pubkey);
  gchar *dir = g_steal_pointer(&layout.dir);
  layout_clear(&layout);
  return dir;
}

typedef enum { NODE_DIRECTORY, NODE_FILE } NodeKind;

/* lstat-based check of one component we own: never a symlink, the right type,
 * ours, and inaccessible to group/other. Unsafe nodes are refused, not fixed:
 * they may have been planted. *exists is FALSE (and TRUE returned) if absent. */
static gboolean
check_private_node(const gchar *path, NodeKind kind, gboolean *exists, GError **error)
{
  struct stat st;
  *exists = FALSE;
  if (lstat(path, &st) != 0) {
    int saved = errno;
    if (saved == ENOENT)
      return TRUE;
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Cannot inspect %s: %s", path, g_strerror(saved));
    return FALSE;
  }
  *exists = TRUE;
  if (S_ISLNK(st.st_mode)) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS,
                "%s is a symbolic link; refusing to use it", path);
    return FALSE;
  }
  if (kind == NODE_DIRECTORY ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS,
                "%s is not a %s; refusing to use it", path,
                kind == NODE_DIRECTORY ? "directory" : "regular file");
    return FALSE;
  }
  if (st.st_uid != store_uid()) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS,
                "%s is owned by another user; refusing to use it", path);
    return FALSE;
  }
  if ((st.st_mode & 077) != 0) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS,
                "%s is accessible to other users (mode %04o); refusing to use it",
                path, (unsigned) (st.st_mode & 07777));
    return FALSE;
  }
  return TRUE;
}

/* Verifies every existing component; *db_exists reports store.db. */
static gboolean
layout_inspect(const StoreLayout *layout, gboolean *db_exists, GError **error)
{
  const gchar *dirs[] = { layout->root, layout->accounts, layout->dir };
  gboolean exists = FALSE;
  *db_exists = FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
    if (!check_private_node(dirs[i], NODE_DIRECTORY, &exists, error))
      return FALSE;
    if (!exists)
      return TRUE;
  }
  if (!check_private_node(layout->db, NODE_FILE, db_exists, error))
    return FALSE;
  for (guint i = 0; i < G_N_ELEMENTS(store_sidecars); i++) {
    g_autofree gchar *path = g_strconcat(layout->db, store_sidecars[i], NULL);
    if (!check_private_node(path, NODE_FILE, &exists, error))
      return FALSE;
  }
  return TRUE;
}

static gboolean
ensure_private_dir(const gchar *path, GError **error)
{
  if (g_mkdir(path, 0700) != 0 && errno != EEXIST) {
    int saved = errno;
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Cannot create %s: %s", path, g_strerror(saved));
    return FALSE;
  }
  gboolean exists = FALSE;
  if (!check_private_node(path, NODE_DIRECTORY, &exists, error))
    return FALSE;
  if (!exists) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED, "%s vanished", path);
    return FALSE;
  }
  return TRUE;
}

/* A -wal left behind by a deleted database must never be replayed into a new
 * one with the same name; the sidecars were verified as ours already. */
static gboolean
remove_stale_sidecars(const StoreLayout *layout, GError **error)
{
  for (guint i = 0; i < G_N_ELEMENTS(store_sidecars); i++) {
    g_autofree gchar *path = g_strconcat(layout->db, store_sidecars[i], NULL);
    if (g_unlink(path) != 0 && errno != ENOENT) {
      int saved = errno;
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                  "Cannot remove stale %s: %s", path, g_strerror(saved));
      return FALSE;
    }
  }
  return TRUE;
}

/* store.db is created by us, 0600, never through a link; SQLite then opens
 * the existing (empty) file and gives -wal/-shm the same mode. */
static gboolean
precreate_db(const gchar *path, GError **error)
{
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0) {
    int saved = errno;
    g_set_error(error, GH_STORE_ERROR,
                saved == EEXIST ? GH_STORE_ERROR_BUSY : GH_STORE_ERROR_FAILED,
                "Cannot create %s: %s", path, g_strerror(saved));
    return FALSE;
  }
  struct stat st;
  gboolean ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 077) == 0;
  close(fd);
  if (!ok) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_PERMISSIONS,
                "%s was not created private", path);
    g_unlink(path);
  }
  return ok;
}

/* ---- In-process registry: one open handle per account store ---------------- */

static GMutex registry_lock;
static GHashTable *registry;

static gboolean
registry_claim(const gchar *dir, GError **error)
{
  g_mutex_lock(&registry_lock);
  if (!registry)
    registry = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  gboolean busy = g_hash_table_contains(registry, dir);
  if (!busy)
    g_hash_table_add(registry, g_strdup(dir));
  g_mutex_unlock(&registry_lock);
  if (busy)
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_BUSY,
                        "This account's store is already open in this process");
  return !busy;
}

static void
registry_release(const gchar *dir)
{
  g_mutex_lock(&registry_lock);
  if (registry)
    g_hash_table_remove(registry, dir);
  g_mutex_unlock(&registry_lock);
}


/* ---- Single-SQLite guard (ST-4) --------------------------------------------------- */

typedef struct {
  const char *name;
  const void *address; /* as bound for this code */
} SqliteEntry;

#define SQLITE_ENTRY(fn) { #fn, (const void *) (fn) }

/* Every SQLite entry point GhStore calls, plus the SQLCipher-only ones. */
static const SqliteEntry sqlite_entries[] = {
  SQLITE_ENTRY(sqlite3_open_v2), SQLITE_ENTRY(sqlite3_key), SQLITE_ENTRY(sqlite3_key_v2),
  SQLITE_ENTRY(sqlite3_close), SQLITE_ENTRY(sqlite3_close_v2),
  SQLITE_ENTRY(sqlite3_prepare_v2), SQLITE_ENTRY(sqlite3_step), SQLITE_ENTRY(sqlite3_reset),
  SQLITE_ENTRY(sqlite3_clear_bindings), SQLITE_ENTRY(sqlite3_finalize),
  SQLITE_ENTRY(sqlite3_exec), SQLITE_ENTRY(sqlite3_free),
  SQLITE_ENTRY(sqlite3_bind_int64), SQLITE_ENTRY(sqlite3_bind_text),
  SQLITE_ENTRY(sqlite3_bind_null), SQLITE_ENTRY(sqlite3_bind_parameter_count),
  SQLITE_ENTRY(sqlite3_column_int64),
  SQLITE_ENTRY(sqlite3_column_text), SQLITE_ENTRY(sqlite3_column_type),
  SQLITE_ENTRY(sqlite3_changes), SQLITE_ENTRY(sqlite3_last_insert_rowid),
  SQLITE_ENTRY(sqlite3_errmsg), SQLITE_ENTRY(sqlite3_errstr),
  SQLITE_ENTRY(sqlite3_extended_errcode), SQLITE_ENTRY(sqlite3_extended_result_codes),
  SQLITE_ENTRY(sqlite3_busy_timeout), SQLITE_ENTRY(sqlite3_limit),
  SQLITE_ENTRY(sqlite3_db_config), SQLITE_ENTRY(sqlite3_get_autocommit),
  SQLITE_ENTRY(sqlite3_wal_checkpoint_v2), SQLITE_ENTRY(sqlite3_threadsafe),
  SQLITE_ENTRY(sqlite3_compileoption_get), SQLITE_ENTRY(sqlite3_compileoption_used),
  SQLITE_ENTRY(sqlite3_libversion), SQLITE_ENTRY(sqlite3_libversion_number),
};

typedef struct {
  gboolean ok;
  gchar *message;        /* failure reason */
  gchar *object;         /* path of the SQLCipher object */
  gchar *cipher_version;
} SqliteGuard;

static gboolean
guard_fail(SqliteGuard *guard, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gboolean
guard_fail(SqliteGuard *guard, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_autofree gchar *reason = g_strdup_vprintf(format, args);
  va_end(args);
  guard->ok = FALSE;
  guard->message = g_strdup_printf(
    "Refusing to open the encrypted store: %s (exactly one SQLite "
    "implementation, SQLCipher, must be bound in this process)", reason);
  return FALSE;
}

static gboolean
guard_bindings(SqliteGuard *guard)
{
  const void *base = NULL;
  const char *base_name = NULL;
  const char *base_object = NULL;

  for (guint i = 0; i < G_N_ELEMENTS(sqlite_entries); i++) {
    const void *address = sqlite_entries[i].address;
#if defined(__ELF__)
    /* The global scope binds every default-visibility reference in the
     * process, ours and every other library's (libsoup, libmarmot, ...). */
    address = dlsym(RTLD_DEFAULT, sqlite_entries[i].name);
    if (!address)
      return guard_fail(guard, "%s is not bound in the global scope",
                        sqlite_entries[i].name);
#endif
    Dl_info info;
    if (!dladdr(address, &info) || !info.dli_fbase || !info.dli_fname)
      return guard_fail(guard, "cannot locate the object that defines %s",
                        sqlite_entries[i].name);
    if (!base) {
      base = info.dli_fbase;
      base_name = sqlite_entries[i].name;
      base_object = info.dli_fname;
    } else if (info.dli_fbase != base) {
      return guard_fail(guard, "%s resolves to %s but %s resolves to %s",
                        base_name, base_object, sqlite_entries[i].name,
                        info.dli_fname);
    }
  }
  guard->object = g_strdup(base_object);

#if defined(__APPLE__)
  /* Two-level namespace: each image binds to the library it linked, so
   * system frameworks may use their own libsqlite3 privately without
   * affecting ours; a forced flat namespace would allow interposition. */
  const struct mach_header *executable = _dyld_get_image_header(0);
  if (!executable || !(executable->flags & MH_TWOLEVEL))
    return guard_fail(guard, "the executable does not use two-level namespace binding");
  if (g_getenv("DYLD_FORCE_FLAT_NAMESPACE"))
    return guard_fail(guard, "DYLD_FORCE_FLAT_NAMESPACE is set");
#elif !defined(__ELF__)
  return guard_fail(guard, "this object format is not supported by the guard");
#endif
  return TRUE;
}

static gpointer
sqlite_guard_run(gpointer data)
{
  (void) data;
  SqliteGuard *guard = g_new0(SqliteGuard, 1);

  /* No SQLite call may happen before the bindings are proven consistent:
   * mixing two implementations' handles is undefined behaviour. */
  if (!guard_bindings(guard))
    return guard;
  if (!sqlite3_compileoption_used("HAS_CODEC")) {
    guard_fail(guard, "%s was not built with SQLCipher's codec", guard->object);
    return guard;
  }
  if (sqlite3_libversion_number() < 3031000) {
    guard_fail(guard, "SQLite %s is too old for SQLITE_OPEN_NOFOLLOW", sqlite3_libversion());
    return guard;
  }
  if (!sqlite3_threadsafe()) {
    guard_fail(guard, "%s was built without thread safety", guard->object);
    return guard;
  }
  for (int i = 0; sqlite3_compileoption_get(i); i++) {
    if (strcmp(sqlite3_compileoption_get(i), "TEMP_STORE=0") == 0) {
      guard_fail(guard, "%s was built with SQLITE_TEMP_STORE=0, so temporary files "
                 "cannot be kept in memory", guard->object);
      return guard;
    }
  }

  struct sqlite3 *db = NULL;
  sqlite3_stmt *stmt = NULL;
  if (sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL) == SQLITE_OK &&
      sqlite3_prepare_v2(db, "PRAGMA cipher_version", -1, &stmt, NULL) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_text(stmt, 0))
    guard->cipher_version = g_strdup((const gchar *) sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  if (!guard->cipher_version || !*guard->cipher_version) {
    guard_fail(guard, "PRAGMA cipher_version is empty, so %s is not SQLCipher",
               guard->object);
    return guard;
  }
  guard->ok = TRUE;
  return guard;
}

static const SqliteGuard *
sqlite_guard(void)
{
  static GOnce once = G_ONCE_INIT;
  return g_once(&once, sqlite_guard_run, NULL);
}

gboolean
gh_store_check_sqlite(GError **error)
{
  const SqliteGuard *guard = sqlite_guard();
  if (guard->ok)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NO_CIPHER, guard->message);
  return FALSE;
}

gchar *
gh_store_describe_sqlite(void)
{
  const SqliteGuard *guard = sqlite_guard();
  if (!guard->ok)
    return g_strdup(guard->message);
  return g_strdup_printf("SQLCipher %s (SQLite %s) from %s", guard->cipher_version,
                         sqlite3_libversion(), guard->object);
}

/* ---- Errors and statements ------------------------------------------------------ */

gboolean
gh_store_set_sqlite_error(GhStore *store, gint rc, const gchar *what, GError **error)
{
  GhStoreError code;
  switch (rc & 0xff) {
  case SQLITE_FULL:
    code = GH_STORE_ERROR_FULL;
    break;
  case SQLITE_BUSY:
  case SQLITE_LOCKED:
    code = GH_STORE_ERROR_BUSY;
    break;
  case SQLITE_CORRUPT:
  case SQLITE_NOTADB:
    code = GH_STORE_ERROR_CORRUPT;
    break;
  case SQLITE_CONSTRAINT:
  case SQLITE_TOOBIG:
  case SQLITE_MISMATCH:
  case SQLITE_RANGE:
    code = GH_STORE_ERROR_INVALID;
    break;
  case SQLITE_READONLY:
    code = store && store->read_only ? GH_STORE_ERROR_CORRUPT : GH_STORE_ERROR_PERMISSIONS;
    break;
  case SQLITE_PERM:
  case SQLITE_AUTH:
    code = GH_STORE_ERROR_PERMISSIONS;
    break;
  case SQLITE_CANTOPEN:
    code = rc == SQLITE_CANTOPEN_SYMLINK ? GH_STORE_ERROR_PERMISSIONS : GH_STORE_ERROR_FAILED;
    break;
  default:
    code = GH_STORE_ERROR_FAILED;
    break;
  }
  const char *detail = store && store->db && sqlite3_extended_errcode(store->db) == rc
                         ? sqlite3_errmsg(store->db) : sqlite3_errstr(rc);
  g_set_error(error, GH_STORE_ERROR, code, "%s: %s", what ? what : "Store", detail);
  return FALSE;
}

/* SQLite may abort a whole transaction on its own (disk full, I/O error). If
 * that happened, nothing may run as if the transaction were still open: each
 * statement would otherwise autocommit a fragment of an atomic operation. */
static gboolean
store_txn_alive(GhStore *store, GError **error)
{
  if (store->depth == 0 || !sqlite3_get_autocommit(store->db))
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                      "The store transaction was rolled back by an earlier error");
  return FALSE;
}

static gboolean
store_writable(GhStore *store, GError **error)
{
  if (!store->read_only)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                      "The store failed its integrity check and is read-only");
  return FALSE;
}

/* Transaction control statements; no liveness check. */
static gboolean
store_exec_control(GhStore *store, const char *sql, GError **error)
{
  int rc = sqlite3_exec(store->db, sql, NULL, NULL, NULL);
  if (rc == SQLITE_OK)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, sql, error);
}

gboolean
gh_store_exec(GhStore *store, const gchar *sql, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(sql != NULL, FALSE);
  if (!store_txn_alive(store, error))
    return FALSE;
  int rc = sqlite3_exec(store->db, sql, NULL, NULL, NULL);
  if (rc == SQLITE_OK)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Running a store statement", error);
}

static sqlite3_stmt *
store_prepare(GhStore *store, const char *sql, GError **error)
{
  if (!store_txn_alive(store, error))
    return NULL;
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
  if (rc == SQLITE_OK)
    return stmt;
  sqlite3_finalize(stmt);
  gh_store_set_sqlite_error(store, rc, "Preparing a store statement", error);
  return NULL;
}

/* Steps a statement that returns no rows. */
static gboolean
store_step_done(GhStore *store, sqlite3_stmt *stmt, const gchar *what, GError **error)
{
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE)
    return TRUE;
  if (rc == SQLITE_ROW) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED, "%s: unexpected row", what);
    return FALSE;
  }
  return gh_store_set_sqlite_error(store, rc, what, error);
}

/* Steps once; *has_row tells whether a row is available. */
static gboolean
store_step_row(GhStore *store, sqlite3_stmt *stmt, gboolean *has_row,
               const gchar *what, GError **error)
{
  int rc = sqlite3_step(stmt);
  *has_row = rc == SQLITE_ROW;
  if (rc == SQLITE_ROW || rc == SQLITE_DONE)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, what, error);
}

#define BIND(expr)                                                              \
  G_STMT_START {                                                                \
    int bind_rc_ = (expr);                                                      \
    if (bind_rc_ != SQLITE_OK) {                                                \
      gh_store_set_sqlite_error(store, bind_rc_, "Binding a store value", error); \
      goto fail;                                                                \
    }                                                                           \
  } G_STMT_END

static int
bind_text(sqlite3_stmt *stmt, int index, const gchar *text)
{
  if (!text)
    return sqlite3_bind_null(stmt, index);
  return sqlite3_bind_text(stmt, index, text, -1, SQLITE_STATIC);
}

/* 0 means "none" for optional times and ids. */
static int
bind_int64_or_null(sqlite3_stmt *stmt, int index, gint64 value)
{
  if (value == 0)
    return sqlite3_bind_null(stmt, index);
  return sqlite3_bind_int64(stmt, index, value);
}

static gboolean
store_query_int64(GhStore *store, const char *sql, gint64 *out, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store, sql, error);
  if (!stmt)
    return FALSE;
  gboolean has_row = FALSE;
  gboolean ok = store_step_row(store, stmt, &has_row, sql, error);
  if (ok)
    *out = has_row ? sqlite3_column_int64(stmt, 0) : 0;
  sqlite3_finalize(stmt);
  return ok;
}

/* *out is NULL when there is no row or the value is NULL. */
static gboolean
store_query_text(GhStore *store, const char *sql, gchar **out, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store, sql, error);
  if (!stmt)
    return FALSE;
  gboolean has_row = FALSE;
  gboolean ok = store_step_row(store, stmt, &has_row, sql, error);
  *out = ok && has_row && sqlite3_column_text(stmt, 0)
           ? g_strdup((const gchar *) sqlite3_column_text(stmt, 0)) : NULL;
  sqlite3_finalize(stmt);
  return ok;
}

/* ---- Transactions -------------------------------------------------------------- */

static gboolean
store_begin(GhStore *store, const gchar *label, GError **error)
{
  if (!store_writable(store, error))
    return FALSE;
  if (store->depth == 0) {
    if (!store_exec_control(store, "BEGIN IMMEDIATE", error))
      return FALSE;
    store->txn_label = label;
  } else {
    if (!store_txn_alive(store, error))
      return FALSE;
    g_autofree gchar *sql = g_strdup_printf("SAVEPOINT gh_sp_%u", store->depth);
    if (!store_exec_control(store, sql, error))
      return FALSE;
  }
  store->depth++;
  return TRUE;
}

static void
store_rollback(GhStore *store)
{
  if (store->depth == 0)
    return;
  gboolean alive = !sqlite3_get_autocommit(store->db);
  if (store->depth == 1) {
    if (alive)
      (void) sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
    store->depth = 0;
    store->txn_label = NULL;
    return;
  }
  if (alive) {
    guint level = store->depth - 1;
    g_autofree gchar *sql =
      g_strdup_printf("ROLLBACK TO gh_sp_%u; RELEASE gh_sp_%u", level, level);
    /* On failure the outer levels see a dead transaction and fail too. */
    (void) sqlite3_exec(store->db, sql, NULL, NULL, NULL);
  }
  store->depth--;
}

static gboolean
store_commit(GhStore *store, GError **error)
{
  g_return_val_if_fail(store->depth > 0, FALSE);
  if (!store_txn_alive(store, error)) {
    store_rollback(store);
    return FALSE;
  }
  if (store->depth > 1) {
    g_autofree gchar *sql = g_strdup_printf("RELEASE gh_sp_%u", store->depth - 1);
    if (!store_exec_control(store, sql, error)) {
      store_rollback(store);
      return FALSE;
    }
    store->depth--;
    return TRUE;
  }
  const gchar *label = store->txn_label;
  STORE_CUT(label, "before-commit");
  if (!store_exec_control(store, "COMMIT", error)) {
    /* BUSY leaves the transaction open, FULL/IOERR may already have undone
     * it; either way nothing of it may survive. */
    if (!sqlite3_get_autocommit(store->db))
      (void) sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
    store->depth = 0;
    store->txn_label = NULL;
    return FALSE;
  }
  store->depth = 0;
  store->txn_label = NULL;
  STORE_CUT(label, "after-commit");
  (void) label;
  return TRUE;
}

gboolean
gh_store_begin(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return store_begin(store, "txn", error);
}

gboolean
gh_store_commit(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(store->depth > 0, FALSE);
  return store_commit(store, error);
}

void
gh_store_rollback(GhStore *store)
{
  g_return_if_fail(store != NULL);
  store_rollback(store);
}

guint
gh_store_get_transaction_depth(GhStore *store)
{
  g_return_val_if_fail(store != NULL, 0);
  return store->depth;
}

gboolean
gh_store_transaction(GhStore *store, GhStoreTransactionFunc func, gpointer user_data,
                     GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(func != NULL, FALSE);
  if (!store_begin(store, "txn", error))
    return FALSE;
  if (!func(store, user_data, error)) {
    store_rollback(store);
    return FALSE;
  }
  return store_commit(store, error);
}

/* ---- Open ------------------------------------------------------------------------ */

/* Connection settings and the pragma sequence of §3.5. The key has already
 * been applied (file stores) before this runs. */
static gboolean
store_configure(GhStore *store, gboolean file_backed, GError **error)
{
  struct sqlite3 *db = store->db;
  sqlite3_extended_result_codes(db, 1);
  sqlite3_busy_timeout(db, STORE_BUSY_TIMEOUT_MS);
  sqlite3_limit(db, SQLITE_LIMIT_ATTACHED, 0);
  sqlite3_limit(db, SQLITE_LIMIT_LENGTH, GH_STORE_MAX_VALUE_SIZE);
  sqlite3_limit(db, SQLITE_LIMIT_SQL_LENGTH, STORE_SQL_LENGTH_LIMIT);
  sqlite3_db_config(db, SQLITE_DBCONFIG_DEFENSIVE, 1, (int *) NULL);
  sqlite3_db_config(db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, (int *) NULL);
  sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, (int *) NULL);

  /* Right after keying (§3.4): SQLCipher zeroes every SQLite allocation on
   * free and mlock()s it where RLIMIT_MEMLOCK allows, so its own copy of the
   * key, the decrypted pages and statement buffers do not linger in freed
   * heap or reach swap. Process-wide; once on, it stays on ("OFF" is
   * ignored). Read back below: fail closed, like every other setting. */
  if (!gh_store_exec(store, "PRAGMA cipher_memory_security = ON", error))
    return FALSE;
  if (!store_query_text(store, "PRAGMA cipher_version", &store->cipher_version, error))
    return FALSE;
  if (!store->cipher_version || !*store->cipher_version) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NO_CIPHER,
                        "PRAGMA cipher_version is empty: the store would not be encrypted");
    return FALSE;
  }
  /* Every SQLCipher 4 has it (4.0.0 added it); an older one answers
   * nothing, which reads as 0. */
  gint64 memory_security = -1;
  if (!store_query_int64(store, "PRAGMA cipher_memory_security", &memory_security, error))
    return FALSE;
  if (memory_security != 1) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NO_CIPHER,
                "SQLCipher %s cannot guard its memory (PRAGMA cipher_memory_security is %"
                G_GINT64_FORMAT "); SQLCipher 4 is required", store->cipher_version,
                memory_security);
    return FALSE;
  }

  if (file_backed) {
    /* Verify the key with a read before any statement can write. */
    gint64 n_objects = 0;
    GError *local = NULL;
    if (!store_query_int64(store, "SELECT count(*) FROM sqlite_master", &n_objects, &local)) {
      if (g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT)) {
        g_clear_error(&local);
        g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_KEY,
                            "The store key does not open this store (wrong key, "
                            "or not a Groundhog store)");
      } else {
        g_propagate_error(error, local);
      }
      return FALSE;
    }
    g_autofree gchar *mode = NULL;
    if (!store_query_text(store, "PRAGMA journal_mode=WAL", &mode, error))
      return FALSE;
    if (!mode || g_ascii_strcasecmp(mode, "wal") != 0) {
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                  "The store cannot use write-ahead logging (journal mode %s)",
                  mode ? mode : "unknown");
      return FALSE;
    }
    if (!gh_store_exec(store, "PRAGMA synchronous=FULL", error))
      return FALSE;
  }
  if (!gh_store_exec(store,
                     "PRAGMA foreign_keys=ON;"
                     "PRAGMA secure_delete=ON;"
                     "PRAGMA temp_store=MEMORY;"
                     "PRAGMA trusted_schema=OFF;", error))
    return FALSE;
  if (file_backed) {
    g_autofree gchar *limit =
      g_strdup_printf("PRAGMA journal_size_limit=%d", STORE_JOURNAL_SIZE_LIMIT);
    if (!gh_store_exec(store, limit, error))
      return FALSE;
  }

  /* Fail closed if a setting did not take (ST-5 reads the same values). */
  static const struct { const char *sql; gint64 expected; gboolean file_only; } readback[] = {
    { "PRAGMA foreign_keys", 1, FALSE },
    { "PRAGMA secure_delete", 1, FALSE },
    { "PRAGMA temp_store", 2, FALSE },
    { "PRAGMA synchronous", 2, TRUE },
  };
  for (guint i = 0; i < G_N_ELEMENTS(readback); i++) {
    if (readback[i].file_only && !file_backed)
      continue;
    gint64 value = -1;
    if (!store_query_int64(store, readback[i].sql, &value, error))
      return FALSE;
    if (value != readback[i].expected) {
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                  "%s is %" G_GINT64_FORMAT ", expected %" G_GINT64_FORMAT,
                  readback[i].sql, value, readback[i].expected);
      return FALSE;
    }
  }
  return TRUE;
}

/* Opens store.db and applies the raw key before anything else touches it.
 * The hex form is wiped at once, and so is @key when it is our own buffer
 * (@owned); a caller's key (possibly read-only guarded memory) is only read. */
static gboolean
store_open_keyed(GhStore *store, const guint8 *key, gboolean owned, GError **error)
{
  int rc = sqlite3_open_v2(store->db_path, &store->db, STORE_OPEN_FLAGS, NULL);
  if (rc != SQLITE_OK) {
    if (owned)
      sodium_memzero((guint8 *) key, GH_STORE_KEY_SIZE);
    return gh_store_set_sqlite_error(store, rc, "Opening the store", error);
  }

  /* "x'<64 hex>'": SQLCipher's raw-key form (no passphrase KDF). This is the
   * call PRAGMA key = "x'...'" makes, minus the key in SQL statement text. */
  enum { SPEC_LEN = 2 + 2 * GH_STORE_KEY_SIZE + 1 };
  char *spec = sodium_malloc(SPEC_LEN + 1);
  if (!spec) {
    if (owned)
      sodium_memzero((guint8 *) key, GH_STORE_KEY_SIZE);
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "Cannot allocate protected memory for the store key");
    return FALSE;
  }
  spec[0] = 'x';
  spec[1] = '\'';
  sodium_bin2hex(spec + 2, 2 * GH_STORE_KEY_SIZE + 1, key, GH_STORE_KEY_SIZE);
  spec[SPEC_LEN - 1] = '\'';
  spec[SPEC_LEN] = '\0';
  rc = sqlite3_key_v2(store->db, "main", spec, SPEC_LEN);
  sodium_free(spec); /* zeroes before freeing */
  if (owned)
    sodium_memzero((guint8 *) key, GH_STORE_KEY_SIZE);
  if (rc != SQLITE_OK)
    return gh_store_set_sqlite_error(store, rc, "Keying the store", error);
  return store_configure(store, TRUE, error);
}

static gboolean
store_migrate(GhStore *store, gint64 from_version, const gchar *label, GError **error)
{
  gsize n = 0;
  const GhStoreMigration *migrations = gh_store_schema_get_migrations(&n);
  const gint64 now = gh_clock_get_unix(store->clock);
  for (gsize i = 0; i < n; i++) {
    if (migrations[i].version <= from_version)
      continue;
    if (!store_begin(store, label, error))
      return FALSE;
    sqlite3_stmt *stmt = NULL;
    g_autofree gchar *version =
      g_strdup_printf("PRAGMA user_version = %d", migrations[i].version);
    if (!gh_store_exec(store, migrations[i].sql, error))
      goto fail;
    stmt = store_prepare(store, "INSERT INTO schema_migrations (version, applied_at, "
                                "description) VALUES (?1, ?2, ?3)", error);
    if (!stmt)
      goto fail;
    BIND(sqlite3_bind_int64(stmt, 1, migrations[i].version));
    BIND(sqlite3_bind_int64(stmt, 2, now));
    BIND(bind_text(stmt, 3, migrations[i].description));
    if (!store_step_done(store, stmt, "Recording a schema migration", error))
      goto fail;
    g_clear_pointer(&stmt, sqlite3_finalize);
    if (!gh_store_exec(store, version, error))
      goto fail;
    if (!store_commit(store, error))
      return FALSE;
    continue;
  fail:
    sqlite3_finalize(stmt);
    store_rollback(store);
    return FALSE;
  }
  return TRUE;
}

static gboolean
store_set_meta(GhStore *store, const gchar *key, const gchar *value, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store,
    "INSERT INTO meta (key, value) VALUES (?1, ?2)", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, key));
  BIND(bind_text(stmt, 2, value));
  gboolean ok = store_step_done(store, stmt, "Writing store metadata", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean
store_get_meta(GhStore *store, const gchar *key, gchar **value, GError **error)
{
  *value = NULL;
  sqlite3_stmt *stmt = store_prepare(store, "SELECT value FROM meta WHERE key = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, key));
  gboolean has_row = FALSE;
  gboolean ok = store_step_row(store, stmt, &has_row, "Reading store metadata", error);
  if (ok && has_row && sqlite3_column_text(stmt, 0))
    *value = g_strdup((const gchar *) sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Schema v1 and the identity rows, atomically: a crash leaves either an
 * empty (user_version 0) store, which the next open completes, or all of it. */
static gboolean
store_create_schema(GhStore *store, GError **error)
{
  g_autofree gchar *created_at =
    g_strdup_printf("%" G_GINT64_FORMAT, gh_clock_get_unix(store->clock));
  if (!store_begin(store, "create", error))
    return FALSE;
  STORE_CUT("create", "begin");
  if (!store_migrate(store, 0, "create", error))
    goto fail;
  STORE_CUT("create", "schema");
  if (!store_set_meta(store, "account_pubkey", store->account_pubkey, error) ||
      !store_set_meta(store, "store_id", store->store_id, error) ||
      !store_set_meta(store, "created_at", created_at, error))
    goto fail;
  STORE_CUT("create", "meta");
  return store_commit(store, error);
fail:
  store_rollback(store);
  return FALSE;
}

gboolean
gh_store_check_integrity(GhStore *store, gboolean full, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_autofree gchar *result = NULL;
  GError *local = NULL;
  if (!store_query_text(store, full ? "PRAGMA integrity_check(1)" : "PRAGMA quick_check(1)",
                        &result, &local)) {
    /* Only damage is corruption; busy, I/O or disk-full stay what they are. */
    if (g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT)) {
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                  "The store failed its integrity check: %s", local->message);
      g_error_free(local);
    } else {
      g_propagate_prefixed_error(error, local, "Checking the store's integrity: ");
    }
    return FALSE;
  }
  if (result && strcmp(result, "ok") == 0)
    return TRUE;
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
              "The store failed its integrity check: %s", result ? result : "no result");
  return FALSE;
}

/* The migration log must match user_version, and meta must name this
 * account and (for file stores) this key item's store id. */
static gboolean
store_verify_identity(GhStore *store, gint64 version, GError **error)
{
  gint64 recorded = 0;
  if (!store_query_int64(store, "SELECT max(version) FROM schema_migrations", &recorded, error))
    return FALSE;
  if (recorded != version) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                "The store's schema version (%" G_GINT64_FORMAT ") does not match its "
                "migration log (%" G_GINT64_FORMAT ")", version, recorded);
    return FALSE;
  }
  g_autofree gchar *account = NULL;
  g_autofree gchar *store_id = NULL;
  if (!store_get_meta(store, "account_pubkey", &account, error) ||
      !store_get_meta(store, "store_id", &store_id, error))
    return FALSE;
  if (g_strcmp0(account, store->account_pubkey) != 0 ||
      (!store->ephemeral && g_strcmp0(store_id, store->store_id) != 0)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FOREIGN,
                        "This store belongs to another account or key item");
    return FALSE;
  }
  return TRUE;
}

/* Version gate, integrity, creation or identity check, migrations. */
static gboolean
store_initialize(GhStore *store, GhStoreOpenFlags flags, GError **error)
{
  gint64 version = 0;
  if (!store_query_int64(store, "PRAGMA user_version", &version, error))
    return FALSE;
  if (version > GH_STORE_SCHEMA_VERSION) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NEWER_SCHEMA,
                "Created by a newer Groundhog (store schema %" G_GINT64_FORMAT
                "; this version supports up to %d)", version, GH_STORE_SCHEMA_VERSION);
    return FALSE;
  }

  /* STORE_CORRUPT: show what is still readable, never write. No integrity
   * scan here: SQLCipher's page-authentication failure is sticky for the
   * connection, so touching a damaged page would make every read fail. */
  if ((flags & GH_STORE_OPEN_ALLOW_CORRUPT) && !store->ephemeral) {
    if (version == 0) {
      g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                          "The store holds no data");
      return FALSE;
    }
    if (!store_verify_identity(store, version, error))
      return FALSE;
    if (version < GH_STORE_SCHEMA_VERSION) {
      g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                          "The store is damaged and too old to be migrated");
      return FALSE;
    }
    store->read_only = TRUE;
    return gh_store_exec(store, "PRAGMA query_only=ON", error);
  }

  if (!store->ephemeral && !gh_store_check_integrity(store, FALSE, error))
    return FALSE;

  if (version == 0) {
    gint64 n_objects = 0;
    if (!store_query_int64(store, "SELECT count(*) FROM sqlite_master", &n_objects, error))
      return FALSE;
    if (n_objects != 0) {
      g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_CORRUPT,
                          "The store has tables but no schema version");
      return FALSE;
    }
    return store_create_schema(store, error);
  }

  if (!store_verify_identity(store, version, error))
    return FALSE;
  if (version < GH_STORE_SCHEMA_VERSION)
    return store_migrate(store, version, "migrate", error);
  return TRUE;
}

static GhStore *
store_new(const gchar *account_pubkey, GhClock *clock)
{
  GhStore *store = g_new0(GhStore, 1);
  store->account_pubkey = g_strdup(account_pubkey);
  store->clock = clock ? gh_clock_ref(clock) : gh_clock_new_system();
  return store;
}

/* Closes the connection and frees; the caller handles the registry. */
static void
store_free(GhStore *store)
{
  if (store->db) {
    if (sqlite3_close(store->db) != SQLITE_OK) {
      g_critical("GhStore closed with unfinalized statements");
      sqlite3_close_v2(store->db);
    }
    store->db = NULL;
  }
  g_free(store->account_pubkey);
  g_free(store->store_id);
  g_free(store->data_dir);
  g_free(store->dir);
  g_free(store->db_path);
  g_free(store->cipher_version);
  if (store->expiry_destroy)
    store->expiry_destroy(store->expiry_data);
  gh_clock_unref(store->clock);
  g_free(store);
}

/* State shared by both open paths between the checks and the file work. */
typedef struct {
  gchar *data_dir;
  StoreLayout layout;
  gboolean claimed;   /* holds the registry entry for layout.dir */
  gboolean db_exists;
} OpenContext;

static void
open_context_clear(OpenContext *ctx)
{
  if (ctx->claimed)
    registry_release(ctx->layout.dir);
  layout_clear(&ctx->layout);
  g_clear_pointer(&ctx->data_dir, g_free);
}

static gboolean
store_no_store_error(GError **error)
{
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                      "There is no store for this account");
  return FALSE;
}

/* Guard, argument checks, data directory, in-process claim and the safety
 * inspection of every existing component. */
static gboolean
open_prepare(const GhStoreConfig *config, gboolean create, OpenContext *ctx,
             GError **error)
{
  if (!gh_store_check_sqlite(error) || !check_account(config->account_pubkey, error))
    return FALSE;
  if (sodium_init() < 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "libsodium failed to initialize");
    return FALSE;
  }
  GError *local = NULL;
  ctx->data_dir = resolve_data_dir(config->data_dir, create, &local);
  if (!ctx->data_dir) {
    if (!g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_propagate_error(error, local);
      return FALSE;
    }
    g_clear_error(&local);
    return store_no_store_error(error);
  }
  layout_init(&ctx->layout, ctx->data_dir, config->account_pubkey);
  if (!registry_claim(ctx->layout.dir, error))
    return FALSE;
  ctx->claimed = TRUE;
  return layout_inspect(&ctx->layout, &ctx->db_exists, error);
}

/* Creates the layout if needed, opens and keys the file and initializes the
 * schema. On success the store owns the registry claim. */
static GhStore *
open_finish(const GhStoreConfig *config, OpenContext *ctx, const guint8 *key,
            gboolean owned_key, const gchar *store_id, GhStoreOpenFlags flags,
            GError **error)
{
  if (!ctx->db_exists &&
      (!ensure_private_dir(ctx->layout.root, error) ||
       !ensure_private_dir(ctx->layout.accounts, error) ||
       !ensure_private_dir(ctx->layout.dir, error) ||
       !remove_stale_sidecars(&ctx->layout, error) ||
       !precreate_db(ctx->layout.db, error)))
    return NULL;

  GhStore *store = store_new(config->account_pubkey, config->clock);
  store->store_id = g_strdup(store_id);
  store->data_dir = g_strdup(ctx->data_dir);
  store->dir = g_strdup(ctx->layout.dir);
  store->db_path = g_strdup(ctx->layout.db);
  if (config->key_provider)
    store->keys = *config->key_provider;
  store->keys_data = config->key_provider_data;
  if (!store_open_keyed(store, key, owned_key, error) ||
      !store_initialize(store, flags, error)) {
    store_free(store);
    return NULL;
  }
  store->registered = TRUE;
  ctx->claimed = FALSE;
  return store;
}

GhStore *
gh_store_open(const GhStoreConfig *config, GhStoreOpenFlags flags,
              GCancellable *cancellable, GError **error)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!check_provider(config->key_provider, error) ||
      g_cancellable_set_error_if_cancelled(cancellable, error))
    return NULL;

  const gboolean create = (flags & GH_STORE_OPEN_CREATE) != 0;
  const GhStoreKeyProvider *keys = config->key_provider;
  OpenContext ctx = { 0 };
  GhStore *store = NULL;
  guint8 *key = NULL;
  g_autofree gchar *store_id = NULL;
  GError *local = NULL;
  if (!open_prepare(config, create, &ctx, error))
    goto out;
  const gboolean db_exists = ctx.db_exists;

  key = sodium_malloc(GH_STORE_KEY_SIZE);
  if (!key) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "Cannot allocate protected memory for the store key");
    goto out;
  }
  GhStoreKeyLookupFlags lookup_flags = (flags & GH_STORE_OPEN_INTERACTIVE)
                                         ? GH_STORE_KEY_LOOKUP_ALLOW_PROMPT
                                         : GH_STORE_KEY_LOOKUP_NONE;
  if (!keys->lookup(config->account_pubkey, lookup_flags, key, &store_id,
                    config->key_provider_data, cancellable, &local)) {
    g_clear_pointer(&store_id, g_free);
    if (!g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_KEY_MISSING) || db_exists) {
      /* KEY_MISSING with a store present leaves the store untouched. */
      g_propagate_error(error, local);
      goto out;
    }
    g_clear_error(&local);
    if (!create) {
      store_no_store_error(error);
      goto out;
    }
    /* First open: the key item must be durable before any file exists. */
    randombytes_buf(key, GH_STORE_KEY_SIZE);
    store_id = g_uuid_string_random();
    if (g_cancellable_set_error_if_cancelled(cancellable, error) ||
        !keys->store(config->account_pubkey, store_id, key, config->key_provider_data,
                     cancellable, error))
      goto out;
  } else if (!store_id || !g_uuid_string_is_valid(store_id)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                        "The store key item has no valid store id");
    goto out;
  } else if (!db_exists && !create) {
    store_no_store_error(error);
    goto out;
  }
  store = open_finish(config, &ctx, key, TRUE, store_id, flags, error);

out:
  if (key)
    sodium_free(key); /* zeroes before freeing */
  open_context_clear(&ctx);
  return store;
}

GhStore *
gh_store_open_with_key(const GhStoreConfig *config, GBytes *key, const gchar *store_id,
                       GhStoreOpenFlags flags, GError **error)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  gsize key_size = 0;
  const guint8 *key_data = key ? g_bytes_get_data(key, &key_size) : NULL;
  if (!key_data || key_size != GH_STORE_KEY_SIZE) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                "The store key must be %d raw bytes", GH_STORE_KEY_SIZE);
    return NULL;
  }
  if (!store_id || !g_uuid_string_is_valid(store_id)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The store id must be a UUID");
    return NULL;
  }
  if (config->key_provider && !check_provider(config->key_provider, error))
    return NULL;

  const gboolean create = (flags & GH_STORE_OPEN_CREATE) != 0;
  OpenContext ctx = { 0 };
  GhStore *store = NULL;
  if (open_prepare(config, create, &ctx, error)) {
    if (!ctx.db_exists && !create)
      store_no_store_error(error);
    else
      store = open_finish(config, &ctx, key_data, FALSE, store_id, flags, error);
  }
  open_context_clear(&ctx);
  return store;
}

gboolean
gh_store_exists(const gchar *data_dir, const gchar *account_pubkey, gboolean *out_exists,
                GError **error)
{
  g_return_val_if_fail(out_exists != NULL, FALSE);
  *out_exists = FALSE;
  if (!check_account(account_pubkey, error))
    return FALSE;
  GError *local = NULL;
  g_autofree gchar *resolved = resolve_data_dir(data_dir, FALSE, &local);
  if (!resolved) {
    if (g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_error_free(local);
      return TRUE;
    }
    g_propagate_error(error, local);
    return FALSE;
  }
  StoreLayout layout = { 0 };
  layout_init(&layout, resolved, account_pubkey);
  gboolean ok = layout_inspect(&layout, out_exists, error);
  layout_clear(&layout);
  return ok;
}

GhStore *
gh_store_open_ephemeral(const gchar *account_pubkey, GhClock *clock, GError **error)
{
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (!gh_store_check_sqlite(error) || !check_account(account_pubkey, error))
    return NULL;

  GhStore *store = store_new(account_pubkey, clock);
  store->ephemeral = TRUE;
  store->store_id = g_uuid_string_random();
  int rc = sqlite3_open_v2(":memory:", &store->db,
                           SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                           SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_EXRESCODE, NULL);
  if (rc != SQLITE_OK) {
    gh_store_set_sqlite_error(store, rc, "Opening the in-memory store", error);
    store_free(store);
    return NULL;
  }
  if (!store_configure(store, FALSE, error) ||
      !store_initialize(store, GH_STORE_OPEN_CREATE, error)) {
    store_free(store);
    return NULL;
  }
  return store;
}

void
gh_store_close(GhStore *store)
{
  if (!store)
    return;
  if (store->depth > 0) {
    g_critical("GhStore closed inside a transaction; rolling it back");
    if (!sqlite3_get_autocommit(store->db))
      (void) sqlite3_exec(store->db, "ROLLBACK", NULL, NULL, NULL);
    store->depth = 0;
  }
  if (store->db && !store->ephemeral && !store->read_only)
    (void) sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
  g_autofree gchar *dir = store->registered ? g_strdup(store->dir) : NULL;
  store_free(store);
  if (dir)
    registry_release(dir);
}

/* ---- Forget (crypto-shred) ------------------------------------------------------- */

/* Removes @name below @parent_fd without ever following a symbolic link: a
 * link is unlinked itself, and directories are opened with O_NOFOLLOW. */
static gboolean
remove_tree_at(int parent_fd, const char *name, GError **error)
{
  int fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    int saved = errno;
    if (saved == ENOENT)
      return TRUE;
    if ((saved == ELOOP || saved == ENOTDIR) &&
        (unlinkat(parent_fd, name, 0) == 0 || errno == ENOENT))
      return TRUE;
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Cannot remove %s: %s", name, g_strerror(saved));
    return FALSE;
  }
  DIR *dir = fdopendir(fd);
  if (!dir) {
    int saved = errno;
    close(fd);
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Cannot list %s: %s", name, g_strerror(saved));
    return FALSE;
  }
  gboolean ok = TRUE;
  struct dirent *entry;
  while (ok && (entry = readdir(dir))) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    struct stat st;
    if (fstatat(dirfd(dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT)
        continue;
      int saved = errno;
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                  "Cannot inspect %s: %s", entry->d_name, g_strerror(saved));
      ok = FALSE;
    } else if (S_ISDIR(st.st_mode)) {
      ok = remove_tree_at(dirfd(dir), entry->d_name, error);
    } else if (unlinkat(dirfd(dir), entry->d_name, 0) != 0 && errno != ENOENT) {
      int saved = errno;
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                  "Cannot remove %s: %s", entry->d_name, g_strerror(saved));
      ok = FALSE;
    }
  }
  closedir(dir);
  if (!ok)
    return FALSE;
  if (unlinkat(parent_fd, name, AT_REMOVEDIR) != 0 && errno != ENOENT) {
    int saved = errno;
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED,
                "Cannot remove %s: %s", name, g_strerror(saved));
    return FALSE;
  }
  return TRUE;
}

/* Opens groundhog/ and accounts/ without following links (a planted link
 * there must never redirect the removal), then removes the account directory. */
static gboolean
remove_account_dir(const StoreLayout *layout, GError **error)
{
  int root = open(layout->root, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (root < 0) {
    int saved = errno;
    if (saved == ENOENT)
      return TRUE;
    g_set_error(error, GH_STORE_ERROR,
                saved == ELOOP || saved == ENOTDIR ? GH_STORE_ERROR_PERMISSIONS
                                                   : GH_STORE_ERROR_FAILED,
                "Cannot open %s: %s", layout->root, g_strerror(saved));
    return FALSE;
  }
  int accounts = openat(root, "accounts", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  int saved = errno;
  close(root);
  if (accounts < 0) {
    if (saved == ENOENT)
      return TRUE;
    g_set_error(error, GH_STORE_ERROR,
                saved == ELOOP || saved == ENOTDIR ? GH_STORE_ERROR_PERMISSIONS
                                                   : GH_STORE_ERROR_FAILED,
                "Cannot open %s: %s", layout->accounts, g_strerror(saved));
    return FALSE;
  }
  gboolean ok = remove_tree_at(accounts, layout->name, error);
  close(accounts);
  return ok;
}

/* §3.8: destroy the key item first (crypto-shred whatever the unlink
 * misses), then unlink the directory even if that failed. */
static gboolean
forget_files(const StoreLayout *layout, const gchar *account_pubkey,
             const GhStoreKeyProvider *keys, gpointer keys_data,
             GCancellable *cancellable, GError **error)
{
  GError *key_error = NULL;
  GError *dir_error = NULL;
  gboolean key_ok = keys->destroy(account_pubkey, keys_data, cancellable, &key_error);
  gboolean dir_ok = remove_account_dir(layout, &dir_error);
  if (!key_ok) {
    if (dir_error)
      g_prefix_error(&key_error, "%s; ", dir_error->message);
    g_prefix_error(&key_error, "The store key could not be destroyed: ");
    g_propagate_error(error, key_error);
    g_clear_error(&dir_error);
    return FALSE;
  }
  if (!dir_ok) {
    g_propagate_error(error, dir_error);
    return FALSE;
  }
  return TRUE;
}

gboolean
gh_store_forget(GhStore *store, GCancellable *cancellable, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (store->ephemeral) {
    gh_store_close(store);
    return TRUE;
  }
  GhStoreKeyProvider keys = store->keys;
  gpointer keys_data = store->keys_data;
  g_autofree gchar *account = g_strdup(store->account_pubkey);
  g_autofree gchar *data_dir = g_strdup(store->data_dir);
  /* Keep the in-process claim until the files are gone, so no open can
   * slip in between the close and the unlink. */
  const gboolean claimed = store->registered;
  store->registered = FALSE;
  gh_store_close(store);

  StoreLayout layout = { 0 };
  layout_init(&layout, data_dir, account);
  /* Without a provider the caller destroyed the key item already. */
  gboolean ok = keys.destroy
                  ? forget_files(&layout, account, &keys, keys_data, cancellable, error)
                  : remove_account_dir(&layout, error);
  if (claimed)
    registry_release(layout.dir);
  layout_clear(&layout);
  return ok;
}

gboolean
gh_store_delete_files(const gchar *data_dir, const gchar *account_pubkey, GError **error)
{
  if (!check_account(account_pubkey, error))
    return FALSE;
  GError *local = NULL;
  g_autofree gchar *resolved = resolve_data_dir(data_dir, FALSE, &local);
  if (!resolved) {
    if (g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_error_free(local);
      return TRUE;
    }
    g_propagate_error(error, local);
    return FALSE;
  }
  StoreLayout layout = { 0 };
  layout_init(&layout, resolved, account_pubkey);
  gboolean ok = FALSE;
  /* Held for the whole removal: an open for this account now fails BUSY. */
  if (!registry_claim(layout.dir, NULL)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_BUSY,
                        "Close this account's store before deleting it");
  } else {
    ok = remove_account_dir(&layout, error);
    registry_release(layout.dir);
  }
  layout_clear(&layout);
  return ok;
}

gboolean
gh_store_forget_account(const GhStoreConfig *config, GCancellable *cancellable,
                        GError **error)
{
  g_return_val_if_fail(config != NULL, FALSE);
  if (!check_account(config->account_pubkey, error) ||
      !check_provider(config->key_provider, error))
    return FALSE;

  GError *local = NULL;
  g_autofree gchar *data_dir = resolve_data_dir(config->data_dir, FALSE, &local);
  if (!data_dir) {
    if (!g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND)) {
      g_propagate_error(error, local);
      return FALSE;
    }
    /* No data directory: only the key item can exist. */
    g_clear_error(&local);
    return config->key_provider->destroy(config->account_pubkey,
                                         config->key_provider_data, cancellable, error);
  }

  StoreLayout layout = { 0 };
  layout_init(&layout, data_dir, config->account_pubkey);
  gboolean ok = FALSE;
  /* Held for the whole forget: an open for this account now fails BUSY. */
  if (!registry_claim(layout.dir, NULL)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_BUSY,
                        "Close this account's store before forgetting it");
  } else {
    ok = forget_files(&layout, config->account_pubkey, config->key_provider,
                      config->key_provider_data, cancellable, error);
    registry_release(layout.dir);
  }
  layout_clear(&layout);
  return ok;
}

/* ---- Accessors ------------------------------------------------------------------- */

const gchar *
gh_store_get_account_pubkey(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->account_pubkey;
}

const gchar *
gh_store_get_store_id(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->store_id;
}

const gchar *
gh_store_get_dir(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->dir;
}

const gchar *
gh_store_get_path(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->db_path;
}

const gchar *
gh_store_get_cipher_version(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->cipher_version;
}

GhClock *
gh_store_get_clock(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->clock;
}

gboolean
gh_store_is_read_only(GhStore *store)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return store->read_only;
}

gboolean
gh_store_is_ephemeral(GhStore *store)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return store->ephemeral;
}

struct sqlite3 *
gh_store_get_db(GhStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->db;
}

gboolean
gh_store_checkpoint(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (store->ephemeral)
    return TRUE;
  if (store->depth > 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "Cannot checkpoint inside a transaction");
    return FALSE;
  }
  int rc = sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
  if (rc == SQLITE_OK)
    return TRUE;
  return gh_store_set_sqlite_error(store, rc, "Truncating the store WAL", error);
}

/* ---- Conversations ------------------------------------------------------------- */

static gboolean
conversation_lookup(GhStore *store, GhStoreBackend backend, const gchar *backend_key,
                    gboolean *found, gint64 *id, gint64 *forgotten_before, GError **error)
{
  *found = FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT id, forgotten_before FROM conversations WHERE backend = ?1 AND backend_key = ?2",
    error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, backend));
  BIND(bind_text(stmt, 2, backend_key));
  gboolean ok = store_step_row(store, stmt, found, "Looking up a conversation", error);
  if (ok && *found) {
    *id = sqlite3_column_int64(stmt, 0);
    if (forgotten_before)
      *forgotten_before = sqlite3_column_int64(stmt, 1);
  }
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean
conversation_insert(GhStore *store, GhStoreBackend backend, const gchar *backend_key,
                    gint64 created_at, gint64 last_activity, GhStoreRequestState state,
                    gint64 *id, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store,
    "INSERT INTO conversations (backend, backend_key, created_at, last_activity, "
    "request_state, disappearing_s) VALUES (?1, ?2, ?3, ?4, ?5, ?6)", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, backend));
  BIND(bind_text(stmt, 2, backend_key));
  BIND(sqlite3_bind_int64(stmt, 3, created_at));
  BIND(sqlite3_bind_int64(stmt, 4, last_activity));
  BIND(sqlite3_bind_int64(stmt, 5, state));
  BIND(sqlite3_bind_int64(stmt, 6, store->default_disappearing));
  gboolean ok = store_step_done(store, stmt, "Creating a conversation", error);
  if (ok)
    *id = sqlite3_last_insert_rowid(store->db);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean
check_request_state(GhStoreRequestState state, GError **error)
{
  if (state >= GH_STORE_REQUEST_ACCEPTED && state <= GH_STORE_REQUEST_BLOCKED)
    return TRUE;
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Unknown request state %d", state);
  return FALSE;
}

gboolean
gh_store_ensure_conversation(GhStore *store, GhStoreBackend backend,
                             const gchar *backend_key,
                             GhStoreRequestState request_state_if_new,
                             gint64 *out_conversation_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_backend(backend, error) ||
      !check_text("The conversation key", backend_key, GH_STORE_MAX_BACKEND_KEY, FALSE, error) ||
      !check_request_state(request_state_if_new, error) || !store_writable(store, error))
    return FALSE;
  gboolean found = FALSE;
  gint64 id = 0;
  if (!conversation_lookup(store, backend, backend_key, &found, &id, NULL, error))
    return FALSE;
  if (!found) {
    const gint64 now = gh_clock_get_unix(store->clock);
    if (!conversation_insert(store, backend, backend_key, now, now, request_state_if_new,
                             &id, error))
      return FALSE;
  }
  if (out_conversation_id)
    *out_conversation_id = id;
  return TRUE;
}

gboolean
gh_store_find_conversation(GhStore *store, GhStoreBackend backend,
                           const gchar *backend_key, gint64 *out_conversation_id,
                           GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_backend(backend, error) ||
      !check_text("The conversation key", backend_key, GH_STORE_MAX_BACKEND_KEY, FALSE, error))
    return FALSE;
  gboolean found = FALSE;
  gint64 id = 0;
  if (!conversation_lookup(store, backend, backend_key, &found, &id, NULL, error))
    return FALSE;
  if (!found) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "No such conversation");
    return FALSE;
  }
  if (out_conversation_id)
    *out_conversation_id = id;
  return TRUE;
}

gboolean
gh_store_set_draft(GhStore *store, gint64 conversation_id, const gchar *draft,
                   GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (draft && !*draft)
    draft = NULL;
  if (!check_text("The draft", draft, GH_STORE_MAX_DRAFT, TRUE, error) ||
      !store_writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "UPDATE conversations SET draft = ?1 WHERE id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, draft));
  BIND(sqlite3_bind_int64(stmt, 2, conversation_id));
  gboolean ok = store_step_done(store, stmt, "Saving a draft", error);
  sqlite3_finalize(stmt);
  if (ok && sqlite3_changes(store->db) == 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    return FALSE;
  }
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_get_draft(GhStore *store, gint64 conversation_id, gchar **out_draft,
                   GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(out_draft != NULL, FALSE);
  *out_draft = NULL;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT draft FROM conversations WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  gboolean ok = store_step_row(store, stmt, &has_row, "Reading a draft", error);
  if (ok && !has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    ok = FALSE;
  } else if (ok && sqlite3_column_text(stmt, 0)) {
    *out_draft = g_strdup((const gchar *) sqlite3_column_text(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* ---- Disappearing messages (G07) -------------------------------------------------- */

static gboolean
check_disappearing(gint64 seconds, GError **error)
{
  if (seconds >= 0 && seconds <= GH_STORE_MAX_DISAPPEARING)
    return TRUE;
  g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
              "A disappearing timer is 0 to %d seconds", GH_STORE_MAX_DISAPPEARING);
  return FALSE;
}

void
gh_store_set_default_disappearing(GhStore *store, gint64 seconds)
{
  g_return_if_fail(store != NULL);
  g_return_if_fail(seconds >= 0 && seconds <= GH_STORE_MAX_DISAPPEARING);
  store->default_disappearing = seconds;
}

gboolean
gh_store_get_disappearing(GhStore *store, gint64 conversation_id, gint64 *out_seconds,
                          GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(out_seconds != NULL, FALSE);
  *out_seconds = 0;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT disappearing_s FROM conversations WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  gboolean has_row = FALSE;
  gboolean ok = store_step_row(store, stmt, &has_row, "Reading a disappearing timer", error);
  if (ok && !has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    ok = FALSE;
  } else if (ok) {
    *out_seconds = sqlite3_column_int64(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_set_disappearing(GhStore *store, gint64 conversation_id, gint64 seconds,
                          GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_disappearing(seconds, error) || !store_writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "UPDATE conversations SET disappearing_s = ?1 WHERE id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, seconds));
  BIND(sqlite3_bind_int64(stmt, 2, conversation_id));
  gboolean ok = store_step_done(store, stmt, "Saving a disappearing timer", error);
  sqlite3_finalize(stmt);
  if (ok && sqlite3_changes(store->db) == 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    return FALSE;
  }
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

void
gh_store_set_expiry_notify(GhStore *store, GhStoreExpiryFunc func, gpointer user_data,
                           GDestroyNotify destroy)
{
  g_return_if_fail(store != NULL);
  GDestroyNotify old_destroy = store->expiry_destroy;
  gpointer old_data = store->expiry_data;
  store->expiry_func = func;
  store->expiry_data = user_data;
  store->expiry_destroy = destroy;
  if (old_destroy)
    old_destroy(old_data);
}

/* A message that expires at expires_at (0: never) was just stored. */
static void
store_notify_expiry(GhStore *store, gint64 expires_at)
{
  if (expires_at > 0 && store->expiry_func)
    store->expiry_func(expires_at, store->expiry_data);
}

/* ---- Cursors ---------------------------------------------------------------------- */

static gboolean
check_cursor_key(const gchar *scope, const gchar *relay_url, GError **error)
{
  if (!check_text("The cursor scope", scope, GH_STORE_MAX_CURSOR_SCOPE, FALSE, error))
    return FALSE;
  if (!relay_url) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "The cursor relay URL is required (\"\" for the whole scope)");
    return FALSE;
  }
  return check_text("The cursor relay URL", relay_url, GH_STORE_MAX_URL, TRUE, error);
}

gboolean
gh_store_get_cursor(GhStore *store, const gchar *scope, const gchar *relay_url,
                    gint64 *out_since, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(out_since != NULL, FALSE);
  *out_since = 0;
  if (!check_cursor_key(scope, relay_url, error))
    return FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT since FROM cursors WHERE scope = ?1 AND relay_url = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, scope));
  BIND(bind_text(stmt, 2, relay_url));
  gboolean has_row = FALSE;
  gboolean ok = store_step_row(store, stmt, &has_row, "Reading a cursor", error);
  if (ok && has_row)
    *out_since = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_set_cursor(GhStore *store, const gchar *scope, const gchar *relay_url,
                    gint64 since, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_cursor_key(scope, relay_url, error))
    return FALSE;
  if (since < 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "A cursor cannot be negative");
    return FALSE;
  }
  if (!store_writable(store, error))
    return FALSE;
  sqlite3_stmt *stmt = store_prepare(store, since > 0
    ? "INSERT INTO cursors (scope, relay_url, since) VALUES (?1, ?2, ?3) "
      "ON CONFLICT (scope, relay_url) DO UPDATE SET since = excluded.since"
    : "DELETE FROM cursors WHERE scope = ?1 AND relay_url = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, scope));
  BIND(bind_text(stmt, 2, relay_url));
  if (since > 0)
    BIND(sqlite3_bind_int64(stmt, 3, since));
  gboolean ok = store_step_done(store, stmt, "Saving a cursor", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* ---- Seen set --------------------------------------------------------------------- */

static gboolean
seen_insert(GhStore *store, GhStoreSeenNs ns, const gchar *id, gint64 now,
            gboolean *inserted, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store,
    "INSERT INTO seen (ns, id, first_seen) VALUES (?1, ?2, ?3) "
    "ON CONFLICT (ns, id) DO NOTHING", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, ns));
  BIND(bind_text(stmt, 2, id));
  BIND(sqlite3_bind_int64(stmt, 3, now));
  gboolean ok = store_step_done(store, stmt, "Recording a seen id", error);
  if (ok && inserted)
    *inserted = sqlite3_changes(store->db) > 0;
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_seen_contains(GhStore *store, GhStoreSeenNs ns, const gchar *id,
                       gboolean *out_seen, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(out_seen != NULL, FALSE);
  *out_seen = FALSE;
  if (!check_seen_id(ns, id, error))
    return FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT 1 FROM seen WHERE ns = ?1 AND id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, ns));
  BIND(bind_text(stmt, 2, id));
  gboolean ok = store_step_row(store, stmt, out_seen, "Checking a seen id", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_seen_add(GhStore *store, GhStoreSeenNs ns, const gchar *id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_seen_id(ns, id, error) || !store_writable(store, error))
    return FALSE;
  return seen_insert(store, ns, id, gh_clock_get_unix(store->clock), NULL, error);
}

/* ---- T-admit ------------------------------------------------------------------------ */

static gboolean
check_message(const GhStoreMessage *m, GError **error)
{
  if (!check_backend(m->backend, error) ||
      !check_text("The conversation key", m->backend_key, GH_STORE_MAX_BACKEND_KEY, FALSE, error) ||
      !check_hex("The message id", m->backend_msg_id, 1, GH_STORE_MAX_ID, FALSE, error) ||
      !check_hex("The wrap id", m->wrap_id, 64, 64, TRUE, error) ||
      !check_hex("The sender public key", m->sender_pubkey, 64, 64, FALSE, error) ||
      !check_nonnegative("The kind", m->kind, error) ||
      !check_nonnegative("created_at", m->created_at, error) ||
      !check_nonnegative("received_at", m->received_at, error) ||
      !check_nonnegative("expires_at", m->expires_at, error) ||
      !check_text("The body", m->body, GH_STORE_MAX_BODY, TRUE, error) ||
      !check_text("The raw event", m->raw_json, GH_STORE_MAX_EVENT_JSON, FALSE, error) ||
      !check_hex("The reply id", m->reply_to, 1, GH_STORE_MAX_ID, TRUE, error) ||
      !check_text("The title", m->title, GH_STORE_MAX_TITLE, TRUE, error) ||
      !check_request_state(m->request_state, error))
    return FALSE;
  if (m->wrap_id && m->backend != GH_STORE_BACKEND_NIP17) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "Only NIP-17 messages have a wrap id");
    return FALSE;
  }
  if (m->direction != GH_STORE_DIRECTION_IN && m->direction != GH_STORE_DIRECTION_OUT) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Unknown direction");
    return FALSE;
  }
  /* NIP-17 rumor and NIP-29 event ids are event ids; MLS ids are bounded hex. */
  if (m->backend != GH_STORE_BACKEND_MLS &&
      !check_hex("The event id", m->backend_msg_id, 64, 64, FALSE, error))
    return FALSE;
  guint n = 0;
  for (; m->participants && m->participants[n]; n++) {
    if (n >= GH_STORE_MAX_PARTICIPANTS) {
      g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                          "Too many participants");
      return FALSE;
    }
    if (!check_hex("A participant public key", m->participants[n], 64, 64, FALSE, error))
      return FALSE;
  }
  return TRUE;
}

static gboolean
admit_locked(GhStore *store, const GhStoreMessage *m, gint64 now,
             GhStoreAdmitResult *result, gint64 *message_id, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  gboolean fresh = FALSE;
  gboolean found = FALSE;
  gint64 conversation_id = 0;
  gint64 forgotten_before = 0;

  if (m->wrap_id && !seen_insert(store, GH_STORE_SEEN_WRAP, m->wrap_id, now, NULL, error))
    return FALSE;
  STORE_CUT("admit", "seen-wrap");
  if (!seen_insert(store, seen_ns_for_backend(m->backend), m->backend_msg_id, now,
                   &fresh, error))
    return FALSE;
  STORE_CUT("admit", "seen-message");
  /* Seen before (stored, purged or forgotten): never store it again. */
  if (!fresh) {
    *result = GH_STORE_ADMIT_DUPLICATE;
    return TRUE;
  }
  if (m->expires_at > 0 && m->expires_at <= now) {
    *result = GH_STORE_ADMIT_EXPIRED;
    return TRUE;
  }
  if (!conversation_lookup(store, m->backend, m->backend_key, &found, &conversation_id,
                           &forgotten_before, error))
    return FALSE;
  if (found && m->created_at < forgotten_before) {
    *result = GH_STORE_ADMIT_FORGOTTEN;
    return TRUE;
  }
  if (!found && !conversation_insert(store, m->backend, m->backend_key, now, m->created_at,
                                     m->request_state, &conversation_id, error))
    return FALSE;
  STORE_CUT("admit", "conversation");

  stmt = store_prepare(store,
    "INSERT INTO messages (conversation_id, backend_msg_id, sender_pubkey, kind, "
    "created_at, received_at, direction, body, raw_json, reply_to, expires_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11) "
    "ON CONFLICT (conversation_id, backend_msg_id) DO NOTHING", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, m->backend_msg_id));
  BIND(bind_text(stmt, 3, m->sender_pubkey));
  BIND(sqlite3_bind_int64(stmt, 4, m->kind));
  BIND(sqlite3_bind_int64(stmt, 5, m->created_at));
  BIND(sqlite3_bind_int64(stmt, 6, m->received_at ? m->received_at : now));
  BIND(sqlite3_bind_int64(stmt, 7, m->direction));
  BIND(bind_text(stmt, 8, m->body));
  BIND(bind_text(stmt, 9, m->raw_json));
  BIND(bind_text(stmt, 10, m->reply_to));
  BIND(bind_int64_or_null(stmt, 11, m->expires_at));
  if (!store_step_done(store, stmt, "Storing a message", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (sqlite3_changes(store->db) == 0) {
    /* Stored before its seen row existed (e.g. imported); now consistent. */
    *result = GH_STORE_ADMIT_DUPLICATE;
    return TRUE;
  }
  *message_id = sqlite3_last_insert_rowid(store->db);
  STORE_CUT("admit", "message");

  /* The title follows the newest stored message (this one is already in),
   * however late older ones arrive through backfill. */
  stmt = store_prepare(store,
    "UPDATE conversations SET unread_count = unread_count + ?1, "
    "title = CASE WHEN ?2 IS NOT NULL AND ?3 >= (SELECT max(created_at) FROM messages "
    "WHERE conversation_id = ?4) THEN ?2 ELSE title END, "
    "last_activity = MAX(last_activity, ?3) WHERE id = ?4", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1,
                          m->unread && m->direction == GH_STORE_DIRECTION_IN ? 1 : 0));
  BIND(bind_text(stmt, 2, m->title));
  BIND(sqlite3_bind_int64(stmt, 3, m->created_at));
  BIND(sqlite3_bind_int64(stmt, 4, conversation_id));
  if (!store_step_done(store, stmt, "Updating a conversation", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  STORE_CUT("admit", "conversation-updated");

  if (m->participants && m->participants[0]) {
    stmt = store_prepare(store,
      "INSERT INTO participants (conversation_id, pubkey) VALUES (?1, ?2) "
      "ON CONFLICT (conversation_id, pubkey) DO NOTHING", error);
    if (!stmt)
      return FALSE;
    for (guint i = 0; m->participants[i]; i++) {
      sqlite3_reset(stmt);
      BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
      BIND(bind_text(stmt, 2, m->participants[i]));
      if (!store_step_done(store, stmt, "Recording a participant", error))
        goto fail;
    }
    g_clear_pointer(&stmt, sqlite3_finalize);
  }
  STORE_CUT("admit", "participants");
  *result = GH_STORE_ADMIT_STORED;
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_admit(GhStore *store, const GhStoreMessage *message,
               GhStoreAdmitResult *out_result, gint64 *out_message_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(message != NULL, FALSE);
  GhStoreAdmitResult result = GH_STORE_ADMIT_DUPLICATE;
  gint64 message_id = 0;
  if (out_result)
    *out_result = result;
  if (out_message_id)
    *out_message_id = 0;
  if (!check_message(message, error))
    return FALSE;

  const gint64 now = gh_clock_get_unix(store->clock);
  if (!store_begin(store, "admit", error))
    return FALSE;
  if (!admit_locked(store, message, now, &result, &message_id, error)) {
    store_rollback(store);
    return FALSE;
  }
  if (!store_commit(store, error))
    return FALSE;
  if (result == GH_STORE_ADMIT_STORED)
    store_notify_expiry(store, message->expires_at);
  if (out_result)
    *out_result = result;
  if (out_message_id)
    *out_message_id = message_id;
  return TRUE;
}

/* ---- T-enqueue ------------------------------------------------------------------------ */

gchar *
gh_store_new_op_id(void)
{
  guint8 raw[16];
  gchar hex[2 * sizeof raw + 1];
  if (sodium_init() < 0)
    g_error("libsodium failed to initialize");
  randombytes_buf(raw, sizeof raw);
  sodium_bin2hex(hex, sizeof hex, raw, sizeof raw);
  return g_strdup(hex);
}

static gboolean
check_outgoing(const GhStoreOutgoing *o, GError **error)
{
  if (o->conversation_id <= 0) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                        "A conversation id is required");
    return FALSE;
  }
  return check_hex("The operation id", o->op_id, 32, 32, FALSE, error) &&
         check_hex("The message id", o->backend_msg_id, 1, GH_STORE_MAX_ID, FALSE, error) &&
         check_hex("The sender public key", o->sender_pubkey, 64, 64, FALSE, error) &&
         check_nonnegative("The kind", o->kind, error) &&
         check_nonnegative("created_at", o->created_at, error) &&
         check_nonnegative("expires_at", o->expires_at, error) &&
         check_text("The body", o->body, GH_STORE_MAX_BODY, TRUE, error) &&
         check_text("The rumor", o->rumor_json, GH_STORE_MAX_EVENT_JSON, FALSE, error) &&
         check_hex("The reply id", o->reply_to, 1, GH_STORE_MAX_ID, TRUE, error);
}

static gboolean
enqueue_locked(GhStore *store, const GhStoreOutgoing *o, gint64 now,
               gint64 *outbox_id, gint64 *message_id, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  gboolean has_row = FALSE;
  gint64 backend = 0;

  /* Idempotent on op_id: a repeat after a crash or retry changes nothing. */
  stmt = store_prepare(store,
    "SELECT o.id, (SELECT m.id FROM messages m WHERE m.outbox_id = o.id) "
    "FROM outbox o WHERE o.op_id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(bind_text(stmt, 1, o->op_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up an operation", error))
    goto fail;
  if (has_row) {
    *outbox_id = sqlite3_column_int64(stmt, 0);
    *message_id = sqlite3_column_int64(stmt, 1);
    sqlite3_finalize(stmt);
    return TRUE;
  }
  g_clear_pointer(&stmt, sqlite3_finalize);

  stmt = store_prepare(store, "SELECT backend FROM conversations WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, o->conversation_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up a conversation", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    goto fail;
  }
  backend = sqlite3_column_int64(stmt, 0);
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (backend != GH_STORE_BACKEND_MLS &&
      !check_hex("The event id", o->backend_msg_id, 64, 64, FALSE, error))
    return FALSE;

  stmt = store_prepare(store,
    "INSERT INTO outbox (conversation_id, op_id, backend, state, rumor_json, created_at) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6)", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, o->conversation_id));
  BIND(bind_text(stmt, 2, o->op_id));
  BIND(sqlite3_bind_int64(stmt, 3, backend));
  BIND(sqlite3_bind_int64(stmt, 4, GH_STORE_OUTBOX_QUEUED));
  BIND(bind_text(stmt, 5, o->rumor_json));
  BIND(sqlite3_bind_int64(stmt, 6, now));
  if (!store_step_done(store, stmt, "Queueing a message", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  *outbox_id = sqlite3_last_insert_rowid(store->db);
  STORE_CUT("enqueue", "outbox");

  stmt = store_prepare(store,
    "INSERT INTO messages (conversation_id, backend_msg_id, sender_pubkey, kind, "
    "created_at, received_at, direction, body, raw_json, reply_to, expires_at, outbox_id) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, o->conversation_id));
  BIND(bind_text(stmt, 2, o->backend_msg_id));
  BIND(bind_text(stmt, 3, o->sender_pubkey));
  BIND(sqlite3_bind_int64(stmt, 4, o->kind));
  BIND(sqlite3_bind_int64(stmt, 5, o->created_at));
  BIND(sqlite3_bind_int64(stmt, 6, now));
  BIND(sqlite3_bind_int64(stmt, 7, GH_STORE_DIRECTION_OUT));
  BIND(bind_text(stmt, 8, o->body));
  BIND(bind_text(stmt, 9, o->rumor_json));
  BIND(bind_text(stmt, 10, o->reply_to));
  BIND(bind_int64_or_null(stmt, 11, o->expires_at));
  BIND(sqlite3_bind_int64(stmt, 12, *outbox_id));
  if (!store_step_done(store, stmt, "Storing an outgoing message", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  *message_id = sqlite3_last_insert_rowid(store->db);
  STORE_CUT("enqueue", "message");

  /* The self-copy that comes back from our inbox is then a duplicate. */
  if (!seen_insert(store, seen_ns_for_backend((GhStoreBackend) backend), o->backend_msg_id,
                   now, NULL, error))
    return FALSE;
  STORE_CUT("enqueue", "seen");

  stmt = store_prepare(store,
    "UPDATE conversations SET draft = NULL, last_activity = MAX(last_activity, ?1) "
    "WHERE id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, o->created_at));
  BIND(sqlite3_bind_int64(stmt, 2, o->conversation_id));
  if (!store_step_done(store, stmt, "Clearing a draft", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  STORE_CUT("enqueue", "draft");
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_enqueue(GhStore *store, const GhStoreOutgoing *outgoing, gint64 *out_outbox_id,
                 gint64 *out_message_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(outgoing != NULL, FALSE);
  gint64 outbox_id = 0;
  gint64 message_id = 0;
  if (!check_outgoing(outgoing, error))
    return FALSE;
  const gint64 now = gh_clock_get_unix(store->clock);
  if (!store_begin(store, "enqueue", error))
    return FALSE;
  if (!enqueue_locked(store, outgoing, now, &outbox_id, &message_id, error)) {
    store_rollback(store);
    return FALSE;
  }
  if (!store_commit(store, error))
    return FALSE;
  store_notify_expiry(store, outgoing->expires_at);
  if (out_outbox_id)
    *out_outbox_id = outbox_id;
  if (out_message_id)
    *out_message_id = message_id;
  return TRUE;
}

/* ---- T-seal ---------------------------------------------------------------------------- */

static gboolean
check_sealed_events(const GhStoreSealedEvent *events, gsize n_events, GError **error)
{
  if (!events || n_events == 0 || n_events > GH_STORE_MAX_SEALED_EVENTS) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                "Sealing needs 1 to %d events", GH_STORE_MAX_SEALED_EVENTS);
    return FALSE;
  }
  for (gsize i = 0; i < n_events; i++) {
    const GhStoreSealedEvent *e = &events[i];
    if (e->role < GH_STORE_OUTBOX_ROLE_RECIPIENT_WRAP ||
        e->role > GH_STORE_OUTBOX_ROLE_WELCOME_WRAP) {
      g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Unknown outbox role %d", e->role);
      return FALSE;
    }
    if (!check_hex("A target public key", e->target_pubkey, 64, 64, TRUE, error) ||
        !check_hex("An event id", e->event_id, 64, 64, FALSE, error) ||
        !check_text("A signed event", e->event_json, GH_STORE_MAX_EVENT_JSON, FALSE, error) ||
        !check_nonnegative("not_before", e->not_before, error))
      return FALSE;
    guint n = 0;
    for (; e->relay_urls && e->relay_urls[n]; n++) {
      if (n >= GH_STORE_MAX_TARGETS_PER_EVENT) {
        g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID,
                            "Too many relay targets for one event");
        return FALSE;
      }
      if (!check_text("A relay URL", e->relay_urls[n], GH_STORE_MAX_URL, FALSE, error))
        return FALSE;
    }
  }
  return TRUE;
}

static gboolean
seal_locked(GhStore *store, gint64 outbox_id, const GhStoreSealedEvent *events,
            gsize n_events, GError **error)
{
  sqlite3_stmt *stmt = NULL;
  sqlite3_stmt *targets = NULL;
  gboolean has_row = FALSE;
  gint64 state = 0;

  stmt = store_prepare(store, "SELECT state FROM outbox WHERE id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, outbox_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up an outbox entry", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such outbox entry");
    goto fail;
  }
  state = sqlite3_column_int64(stmt, 0);
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (state != GH_STORE_OUTBOX_QUEUED && state != GH_STORE_OUTBOX_SEALING) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "This message is already sealed: its stored events are "
                        "republished, never re-sealed");
    return FALSE;
  }

  stmt = store_prepare(store,
    "INSERT INTO outbox_events (outbox_id, role, target_pubkey, event_id, event_json, "
    "not_before) VALUES (?1, ?2, ?3, ?4, ?5, ?6)", error);
  targets = stmt ? store_prepare(store,
    "INSERT INTO outbox_targets (outbox_event_id, relay_url) VALUES (?1, ?2) "
    "ON CONFLICT (outbox_event_id, relay_url) DO NOTHING", error) : NULL;
  if (!stmt || !targets)
    goto fail;
  for (gsize i = 0; i < n_events; i++) {
    const GhStoreSealedEvent *e = &events[i];
    sqlite3_reset(stmt);
    BIND(sqlite3_bind_int64(stmt, 1, outbox_id));
    BIND(sqlite3_bind_int64(stmt, 2, e->role));
    BIND(bind_text(stmt, 3, e->target_pubkey));
    BIND(bind_text(stmt, 4, e->event_id));
    BIND(bind_text(stmt, 5, e->event_json));
    BIND(sqlite3_bind_int64(stmt, 6, e->not_before));
    if (!store_step_done(store, stmt, "Storing a signed event", error))
      goto fail;
    gint64 event_row = sqlite3_last_insert_rowid(store->db);
    STORE_CUT("seal", "event");
    for (guint j = 0; e->relay_urls && e->relay_urls[j]; j++) {
      sqlite3_reset(targets);
      BIND(sqlite3_bind_int64(targets, 1, event_row));
      BIND(bind_text(targets, 2, e->relay_urls[j]));
      if (!store_step_done(store, targets, "Storing a relay target", error))
        goto fail;
    }
    STORE_CUT("seal", "targets");
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  g_clear_pointer(&targets, sqlite3_finalize);

  stmt = store_prepare(store, "UPDATE outbox SET state = ?1 WHERE id = ?2", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, GH_STORE_OUTBOX_SEALED));
  BIND(sqlite3_bind_int64(stmt, 2, outbox_id));
  if (!store_step_done(store, stmt, "Marking a message sealed", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  STORE_CUT("seal", "state");
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  sqlite3_finalize(targets);
  return FALSE;
}

gboolean
gh_store_seal(GhStore *store, gint64 outbox_id, const GhStoreSealedEvent *events,
              gsize n_events, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  if (!check_sealed_events(events, n_events, error))
    return FALSE;
  if (!store_begin(store, "seal", error))
    return FALSE;
  if (!seal_locked(store, outbox_id, events, n_events, error)) {
    store_rollback(store);
    return FALSE;
  }
  return store_commit(store, error);
}

/* ---- T-outcome ------------------------------------------------------------------------- */

/* Relay-controlled text: made valid UTF-8 and cut on a character boundary. */
static gchar *
bounded_relay_text(const gchar *text)
{
  if (!text)
    return NULL;
  gsize len = strnlen(text, 4 * GH_STORE_MAX_OK_MESSAGE);
  g_autofree gchar *valid = g_utf8_make_valid(text, (gssize) len);
  if (strlen(valid) <= GH_STORE_MAX_OK_MESSAGE)
    return g_steal_pointer(&valid);
  const gchar *end = valid + GH_STORE_MAX_OK_MESSAGE;
  while (end > valid && (((guchar) *end) & 0xc0) == 0x80)
    end--;
  return g_strndup(valid, (gsize) (end - valid));
}

gboolean
gh_store_record_outcome(GhStore *store, gint64 outbox_event_id,
                        const GhStoreTargetOutcome *outcome, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(outcome != NULL, FALSE);
  if (!check_text("A relay URL", outcome->relay_url, GH_STORE_MAX_URL, FALSE, error))
    return FALSE;
  g_autofree gchar *message = bounded_relay_text(outcome->ok_message);
  sqlite3_stmt *stmt = NULL;
  if (!store_begin(store, "outcome", error))
    return FALSE;
  stmt = store_prepare(store,
    "INSERT INTO outbox_targets (outbox_event_id, relay_url, outcome, ok_prefix, "
    "ok_message, attempts, last_attempt_at) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7) "
    "ON CONFLICT (outbox_event_id, relay_url) DO UPDATE SET "
    "outcome = excluded.outcome, ok_prefix = excluded.ok_prefix, "
    "ok_message = excluded.ok_message, attempts = attempts + excluded.attempts, "
    "last_attempt_at = coalesce(excluded.last_attempt_at, last_attempt_at)", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, outbox_event_id));
  BIND(bind_text(stmt, 2, outcome->relay_url));
  BIND(sqlite3_bind_int64(stmt, 3, outcome->outcome));
  BIND(outcome->ok_prefix < 0 ? sqlite3_bind_null(stmt, 4)
                              : sqlite3_bind_int64(stmt, 4, outcome->ok_prefix));
  BIND(bind_text(stmt, 5, message));
  BIND(sqlite3_bind_int64(stmt, 6, outcome->count_attempt ? 1 : 0));
  /* A cancelled publish (switch/quit) is not an attempt: time unchanged. */
  BIND(bind_int64_or_null(stmt, 7,
                          outcome->count_attempt ? gh_clock_get_unix(store->clock) : 0));
  if (!store_step_done(store, stmt, "Recording a relay outcome", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  return store_commit(store, error);

fail:
  sqlite3_finalize(stmt);
  store_rollback(store);
  return FALSE;
}

/* ---- Outbox engine (G06) --------------------------------------------------------------- */

static gboolean store_exec_id(GhStore *store, const char *sql, gint64 id, gint64 value,
                              GError **error);

static gchar *
column_text_dup(sqlite3_stmt *stmt, int column)
{
  const unsigned char *text = sqlite3_column_text(stmt, column);
  return text ? g_strdup((const gchar *) text) : NULL;
}

static void
outbox_target_free(gpointer data)
{
  GhStoreOutboxTarget *target = data;
  g_free(target->relay_url);
  g_free(target->ok_message);
  g_free(target);
}

static void
outbox_event_free(gpointer data)
{
  GhStoreOutboxEvent *event = data;
  g_free(event->target_pubkey);
  g_free(event->event_id);
  g_free(event->event_json);
  g_ptr_array_unref(event->targets);
  g_free(event);
}

void
gh_store_outbox_entry_free(GhStoreOutboxEntry *entry)
{
  if (!entry)
    return;
  g_free(entry->op_id);
  g_free(entry->rumor_json);
  g_free(entry->last_error);
  g_ptr_array_unref(entry->events);
  g_free(entry);
}

GArray *
gh_store_outbox_list_unfinished(GhStore *store, GhStoreBackend backend, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT id FROM outbox WHERE backend = ?3 AND state NOT IN (?1, ?2) "
    "ORDER BY created_at, id", error);
  if (!stmt)
    return NULL;
  GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
  BIND(sqlite3_bind_int64(stmt, 1, GH_STORE_OUTBOX_SETTLED));
  BIND(sqlite3_bind_int64(stmt, 2, GH_STORE_OUTBOX_CANCELLED));
  BIND(sqlite3_bind_int64(stmt, 3, backend));
  for (;;) {
    gboolean has_row = FALSE;
    if (!store_step_row(store, stmt, &has_row, "Listing the outbox", error))
      goto fail;
    if (!has_row)
      break;
    gint64 id = sqlite3_column_int64(stmt, 0);
    g_array_append_val(ids, id);
  }
  sqlite3_finalize(stmt);
  return ids;

fail:
  sqlite3_finalize(stmt);
  g_array_unref(ids);
  return NULL;
}

static gboolean
outbox_load_targets(GhStore *store, GhStoreOutboxEvent *event, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT relay_url, outcome, ok_prefix, ok_message, attempts, last_attempt_at "
    "FROM outbox_targets WHERE outbox_event_id = ?1 ORDER BY relay_url", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, event->id));
  for (;;) {
    gboolean has_row = FALSE;
    if (!store_step_row(store, stmt, &has_row, "Loading relay targets", error))
      goto fail;
    if (!has_row)
      break;
    GhStoreOutboxTarget *target = g_new0(GhStoreOutboxTarget, 1);
    target->relay_url = column_text_dup(stmt, 0);
    target->outcome = sqlite3_column_int(stmt, 1);
    target->ok_prefix = sqlite3_column_type(stmt, 2) == SQLITE_NULL
                          ? -1 : sqlite3_column_int(stmt, 2);
    target->ok_message = column_text_dup(stmt, 3);
    target->attempts = (guint) MAX(sqlite3_column_int64(stmt, 4), 0);
    target->last_attempt_at = sqlite3_column_int64(stmt, 5);
    g_ptr_array_add(event->targets, target);
  }
  sqlite3_finalize(stmt);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

GhStoreOutboxEntry *
gh_store_outbox_load(GhStore *store, gint64 outbox_id, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  gboolean has_row = FALSE;
  GhStoreOutboxEntry *entry = NULL;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT o.conversation_id, o.op_id, o.backend, o.state, o.rumor_json, o.created_at, "
    "o.next_attempt_at, o.attempts, o.last_error, "
    "(SELECT m.id FROM messages m WHERE m.outbox_id = o.id) "
    "FROM outbox o WHERE o.id = ?1", error);
  if (!stmt)
    return NULL;
  BIND(sqlite3_bind_int64(stmt, 1, outbox_id));
  if (!store_step_row(store, stmt, &has_row, "Loading an outbox entry", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such outbox entry");
    goto fail;
  }
  entry = g_new0(GhStoreOutboxEntry, 1);
  entry->id = outbox_id;
  entry->conversation_id = sqlite3_column_int64(stmt, 0);
  entry->op_id = column_text_dup(stmt, 1);
  entry->backend = (GhStoreBackend) sqlite3_column_int(stmt, 2);
  entry->state = (GhStoreOutboxState) sqlite3_column_int(stmt, 3);
  entry->rumor_json = column_text_dup(stmt, 4);
  entry->created_at = sqlite3_column_int64(stmt, 5);
  entry->next_attempt_at = sqlite3_column_int64(stmt, 6);
  entry->attempts = (guint) MAX(sqlite3_column_int64(stmt, 7), 0);
  entry->last_error = column_text_dup(stmt, 8);
  entry->message_id = sqlite3_column_int64(stmt, 9);
  entry->events = g_ptr_array_new_with_free_func(outbox_event_free);
  g_clear_pointer(&stmt, sqlite3_finalize);

  stmt = store_prepare(store,
    "SELECT id, role, target_pubkey, event_id, event_json, not_before "
    "FROM outbox_events WHERE outbox_id = ?1 ORDER BY id", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, outbox_id));
  for (;;) {
    if (!store_step_row(store, stmt, &has_row, "Loading stored events", error))
      goto fail;
    if (!has_row)
      break;
    GhStoreOutboxEvent *event = g_new0(GhStoreOutboxEvent, 1);
    event->id = sqlite3_column_int64(stmt, 0);
    event->role = (GhStoreOutboxRole) sqlite3_column_int(stmt, 1);
    event->target_pubkey = column_text_dup(stmt, 2);
    event->event_id = column_text_dup(stmt, 3);
    event->event_json = column_text_dup(stmt, 4);
    event->not_before = sqlite3_column_int64(stmt, 5);
    event->targets = g_ptr_array_new_with_free_func(outbox_target_free);
    g_ptr_array_add(entry->events, event);
  }
  g_clear_pointer(&stmt, sqlite3_finalize);
  for (guint i = 0; i < entry->events->len; i++)
    if (!outbox_load_targets(store, g_ptr_array_index(entry->events, i), error))
      goto fail;
  return entry;

fail:
  sqlite3_finalize(stmt);
  gh_store_outbox_entry_free(entry);
  return NULL;
}

gboolean
gh_store_outbox_find_by_message(GhStore *store, gint64 message_id, gint64 *out_outbox_id,
                                GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  gboolean has_row = FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT outbox_id FROM messages WHERE id = ?1 AND outbox_id IS NOT NULL", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, message_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up a message's outbox entry", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "The message has no outbox entry");
    goto fail;
  }
  if (out_outbox_id)
    *out_outbox_id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_outbox_find_by_rumor(GhStore *store, gint64 conversation_id,
                              const gchar *backend_msg_id, gint64 *out_outbox_id,
                              GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(backend_msg_id != NULL, FALSE);
  gboolean has_row = FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT outbox_id FROM messages WHERE conversation_id = ?1 AND backend_msg_id = ?2 "
    "AND direction = 1 AND outbox_id IS NOT NULL", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  BIND(bind_text(stmt, 2, backend_msg_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up a message's outbox entry", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND,
                        "The message has no outbox entry");
    goto fail;
  }
  if (out_outbox_id)
    *out_outbox_id = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* The entry's state and whether it has stored events; NOT_FOUND if absent. */
static gboolean
outbox_state_locked(GhStore *store, gint64 outbox_id, gint64 *state, gboolean *sealed,
                    GError **error)
{
  gboolean has_row = FALSE;
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT state, EXISTS (SELECT 1 FROM outbox_events e WHERE e.outbox_id = o.id) "
    "FROM outbox o WHERE o.id = ?1", error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, outbox_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up an outbox entry", error))
    goto fail;
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such outbox entry");
    goto fail;
  }
  *state = sqlite3_column_int64(stmt, 0);
  *sealed = sqlite3_column_int(stmt, 1) != 0;
  sqlite3_finalize(stmt);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

static gboolean
outbox_transition_allowed(gint64 from, GhStoreOutboxState to, gboolean sealed, GError **error)
{
  const gchar *why = NULL;
  if (from == GH_STORE_OUTBOX_CANCELLED)
    why = "A cancelled message stays cancelled";
  else if (to == GH_STORE_OUTBOX_SEALED)
    why = "Only sealing (T-seal) marks a message sealed";
  else if (sealed && (to == GH_STORE_OUTBOX_QUEUED || to == GH_STORE_OUTBOX_SEALING))
    why = "This message is already sealed: its stored events are republished, never re-sealed";
  else if (!sealed && (to == GH_STORE_OUTBOX_PUBLISHING || to == GH_STORE_OUTBOX_WAITING_RETRY ||
                       to == GH_STORE_OUTBOX_SETTLED))
    why = "This message has no stored events to publish";
  if (!why)
    return TRUE;
  g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE, why);
  return FALSE;
}

gboolean
gh_store_outbox_update(GhStore *store, gint64 outbox_id, const GhStoreOutboxUpdate *update,
                       GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(update != NULL, FALSE);
  if (update->state < GH_STORE_OUTBOX_QUEUED || update->state > GH_STORE_OUTBOX_CANCELLED) {
    g_set_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID, "Unknown outbox state %d",
                update->state);
    return FALSE;
  }
  if (!check_nonnegative("next_attempt_at", update->next_attempt_at, error) ||
      !check_text("The outbox reason", update->last_error, GH_STORE_MAX_OK_MESSAGE, TRUE, error))
    return FALSE;
  sqlite3_stmt *stmt = NULL;
  gint64 state = 0;
  gboolean sealed = FALSE;
  if (!store_begin(store, "outbox", error))
    return FALSE;
  if (!outbox_state_locked(store, outbox_id, &state, &sealed, error) ||
      !outbox_transition_allowed(state, update->state, sealed, error))
    goto fail;
  stmt = store_prepare(store,
    "UPDATE outbox SET state = ?1, next_attempt_at = ?2, attempts = attempts + ?3, "
    "last_error = ?4 WHERE id = ?5", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, update->state));
  BIND(bind_int64_or_null(stmt, 2, update->next_attempt_at));
  BIND(sqlite3_bind_int64(stmt, 3, update->count_attempt ? 1 : 0));
  BIND(bind_text(stmt, 4, update->last_error));
  BIND(sqlite3_bind_int64(stmt, 5, outbox_id));
  if (!store_step_done(store, stmt, "Recording an outbox transition", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  return store_commit(store, error);

fail:
  sqlite3_finalize(stmt);
  store_rollback(store);
  return FALSE;
}

gboolean
gh_store_outbox_delete(GhStore *store, gint64 outbox_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  gint64 state = 0;
  gboolean sealed = FALSE;
  if (!store_begin(store, "outbox", error))
    return FALSE;
  if (!outbox_state_locked(store, outbox_id, &state, &sealed, error) ||
      !store_exec_id(store, "DELETE FROM messages WHERE outbox_id = ?1", outbox_id, 0, error) ||
      !store_exec_id(store, "DELETE FROM outbox WHERE id = ?1", outbox_id, 0, error)) {
    store_rollback(store);
    return FALSE;
  }
  if (!store_commit(store, error))
    return FALSE;
  /* The deleted text (body, rumor, signed wraps) leaves the WAL now. */
  if (store->depth == 0 && !store->ephemeral)
    (void) sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
  return TRUE;
}

/* ---- T-purge ---------------------------------------------------------------------------- */

static gboolean
store_delete_count(GhStore *store, const char *sql, gint64 value, guint *count,
                   GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store, sql, error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, value));
  gboolean ok = store_step_done(store, stmt, "Purging messages", error);
  if (ok)
    *count = (guint) sqlite3_changes(store->db);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* The messages one purge deletes, with ?1 = now and ?2 = the retention
 * cutoff (0: none). */
#define PURGE_DOOMED(m) \
  "((" m ".expires_at IS NOT NULL AND " m ".expires_at <= ?1) OR " \
  "(?2 > 0 AND " m ".received_at < ?2))"

static gboolean
purge_exec(GhStore *store, const char *sql, gint64 now, gint64 cutoff, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store, sql, error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, now));
  BIND(sqlite3_bind_int64(stmt, 2, cutoff));
  gboolean ok = store_step_done(store, stmt, "Purging messages", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

void
gh_store_purged_message_free(GhStorePurgedMessage *message)
{
  if (!message)
    return;
  g_free(message->backend_key);
  g_free(message->backend_msg_id);
  g_free(message);
}

static gboolean
purge_collect(GhStore *store, gint64 now, gint64 cutoff, GPtrArray *purged, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store,
    "SELECT c.backend, c.backend_key, m.backend_msg_id "
    "FROM messages m JOIN conversations c ON c.id = m.conversation_id WHERE "
    PURGE_DOOMED("m"), error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, now));
  BIND(sqlite3_bind_int64(stmt, 2, cutoff));
  gboolean has_row = FALSE;
  while (TRUE) {
    if (!store_step_row(store, stmt, &has_row, "Listing messages to purge", error))
      goto fail;
    if (!has_row)
      break;
    GhStorePurgedMessage *message = g_new0(GhStorePurgedMessage, 1);
    message->backend = (GhStoreBackend) sqlite3_column_int64(stmt, 0);
    message->backend_key = g_strdup((const gchar *) sqlite3_column_text(stmt, 1));
    message->backend_msg_id = g_strdup((const gchar *) sqlite3_column_text(stmt, 2));
    g_ptr_array_add(purged, message);
  }
  sqlite3_finalize(stmt);
  return TRUE;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

/* Before the rows go: a read marker on a doomed message moves back to the
 * newest surviving message at or before it (none: nothing is read), which
 * leaves the same surviving messages after it; then each touched room's
 * unread count becomes its surviving messages from others after the marker.
 * A marker whose row was already gone keeps the clamp below. */
static gboolean
purge_read_state(GhStore *store, gint64 now, gint64 cutoff, GError **error)
{
  return purge_exec(store,
           "UPDATE conversations SET last_read_msg = ("
           "SELECT k.id FROM messages k, messages r WHERE r.id = conversations.last_read_msg "
           "AND k.conversation_id = conversations.id AND NOT " PURGE_DOOMED("k") " "
           "AND (k.created_at, k.backend_msg_id) <= (r.created_at, r.backend_msg_id) "
           "ORDER BY k.created_at DESC, k.backend_msg_id DESC LIMIT 1) "
           "WHERE last_read_msg IN (SELECT m.id FROM messages m WHERE " PURGE_DOOMED("m") ")",
           now, cutoff, error) &&
         purge_exec(store,
           "UPDATE conversations SET unread_count = ("
           "SELECT count(*) FROM messages m WHERE m.conversation_id = conversations.id "
           "AND m.direction = 0 AND NOT " PURGE_DOOMED("m") " "
           "AND (conversations.last_read_msg IS NULL OR (m.created_at, m.backend_msg_id) > "
           "(SELECT r.created_at, r.backend_msg_id FROM messages r "
           "WHERE r.id = conversations.last_read_msg))) "
           "WHERE id IN (SELECT d.conversation_id FROM messages d WHERE " PURGE_DOOMED("d") ") "
           "AND (last_read_msg IS NULL OR "
           "EXISTS (SELECT 1 FROM messages r WHERE r.id = conversations.last_read_msg))",
           now, cutoff, error);
}

gboolean
gh_store_purge(GhStore *store, gint64 retention_cutoff, GhStorePurgeStats *out_stats,
               GError **error)
{
  return gh_store_purge_full(store, retention_cutoff, NULL, out_stats, error);
}

gboolean
gh_store_purge_full(GhStore *store, gint64 retention_cutoff, GPtrArray **out_purged,
                    GhStorePurgeStats *out_stats, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  GhStorePurgeStats stats = { 0 };
  if (out_stats)
    *out_stats = stats;
  if (out_purged)
    *out_purged = NULL;
  if (!check_nonnegative("The retention cutoff", retention_cutoff, error))
    return FALSE;

  const gint64 now = gh_clock_get_unix(store->clock);
  guint n_outbox = 0;
  g_autoptr(GPtrArray) purged = out_purged
    ? g_ptr_array_new_with_free_func((GDestroyNotify) gh_store_purged_message_free) : NULL;
  if (!store_begin(store, "purge", error))
    return FALSE;
  if ((purged && !purge_collect(store, now, retention_cutoff, purged, error)) ||
      !purge_read_state(store, now, retention_cutoff, error))
    goto fail;
  /* An outgoing message's text is also in its outbox row (rumor_json) and
   * its signed wraps: those go first, with the message. */
  if (!store_delete_count(store,
        "DELETE FROM outbox WHERE id IN (SELECT outbox_id FROM messages WHERE "
        "outbox_id IS NOT NULL AND expires_at IS NOT NULL AND expires_at <= ?1)",
        now, &n_outbox, error) ||
      !store_delete_count(store,
        "DELETE FROM messages WHERE expires_at IS NOT NULL AND expires_at <= ?1",
        now, &stats.n_expired, error))
    goto fail;
  stats.n_outbox += n_outbox;
  STORE_CUT("purge", "expired");
  if (retention_cutoff > 0 &&
      (!store_delete_count(store,
         "DELETE FROM outbox WHERE id IN (SELECT outbox_id FROM messages WHERE "
         "outbox_id IS NOT NULL AND received_at < ?1)",
         retention_cutoff, &n_outbox, error) ||
       !store_delete_count(store, "DELETE FROM messages WHERE received_at < ?1",
                           retention_cutoff, &stats.n_retention, error)))
    goto fail;
  if (retention_cutoff > 0)
    stats.n_outbox += n_outbox;
  STORE_CUT("purge", "retention");
  if (stats.n_expired + stats.n_retention > 0 &&
      !gh_store_exec(store,
        "UPDATE conversations SET unread_count = MIN(unread_count, "
        "(SELECT count(*) FROM messages m WHERE m.conversation_id = conversations.id "
        "AND m.direction = 0)) WHERE unread_count > 0", error))
    goto fail;
  STORE_CUT("purge", "conversations");
  if (!store_query_int64(store,
        "SELECT min(expires_at) FROM messages WHERE expires_at IS NOT NULL",
        &stats.next_expires_at, error))
    goto fail;
  if (!store_commit(store, error))
    return FALSE;
  if (out_purged)
    *out_purged = g_steal_pointer(&purged);

  /* Expired content must leave the WAL too, at most once a minute; inside a
   * caller's transaction that has to wait for its commit. */
  if (stats.n_expired + stats.n_retention > 0 && store->depth > 0 && !store->ephemeral)
    stats.checkpoint_deferred = TRUE;
  if (stats.n_expired + stats.n_retention > 0 && store->depth == 0 && !store->ephemeral) {
    const gint64 mono = gh_clock_get_monotonic_time(store->clock);
    if (store->checkpointed_once &&
        mono - store->last_checkpoint < STORE_CHECKPOINT_INTERVAL_US) {
      stats.checkpoint_deferred = TRUE;
    } else {
      int rc = sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE,
                                         NULL, NULL);
      stats.checkpointed = rc == SQLITE_OK;
      stats.checkpoint_deferred = rc != SQLITE_OK;
      if (stats.checkpointed) {
        store->checkpointed_once = TRUE;
        store->last_checkpoint = mono;
      }
    }
    STORE_CUT("purge", "checkpoint");
  }
  if (out_stats)
    *out_stats = stats;
  return TRUE;

fail:
  store_rollback(store);
  return FALSE;
}

/* ---- Forget conversation ------------------------------------------------------------------ */

static gboolean
store_exec_id(GhStore *store, const char *sql, gint64 id, gint64 value, GError **error)
{
  sqlite3_stmt *stmt = store_prepare(store, sql, error);
  if (!stmt)
    return FALSE;
  BIND(sqlite3_bind_int64(stmt, 1, id));
  if (sqlite3_bind_parameter_count(stmt) >= 2)
    BIND(sqlite3_bind_int64(stmt, 2, value));
  gboolean ok = store_step_done(store, stmt, "Forgetting a conversation", error);
  sqlite3_finalize(stmt);
  return ok;
fail:
  sqlite3_finalize(stmt);
  return FALSE;
}

gboolean
gh_store_forget_conversation(GhStore *store, gint64 conversation_id, GError **error)
{
  g_return_val_if_fail(store != NULL, FALSE);
  sqlite3_stmt *stmt = NULL;
  gboolean has_row = FALSE;
  const gint64 now = gh_clock_get_unix(store->clock);

  if (!store_begin(store, "forget", error))
    return FALSE;
  stmt = store_prepare(store, "SELECT 1 FROM conversations WHERE id = ?1", error);
  if (!stmt)
    goto fail;
  BIND(sqlite3_bind_int64(stmt, 1, conversation_id));
  if (!store_step_row(store, stmt, &has_row, "Looking up a conversation", error))
    goto fail;
  g_clear_pointer(&stmt, sqlite3_finalize);
  if (!has_row) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND, "No such conversation");
    goto fail;
  }
  /* Unsettled sends are cancelled by deleting their rows (cascades to the
   * stored events and targets). */
  if (!store_exec_id(store, "DELETE FROM outbox WHERE conversation_id = ?1",
                     conversation_id, 0, error))
    goto fail;
  STORE_CUT("forget", "outbox");
  if (!store_exec_id(store, "DELETE FROM messages WHERE conversation_id = ?1",
                     conversation_id, 0, error) ||
      !store_exec_id(store, "DELETE FROM participants WHERE conversation_id = ?1",
                     conversation_id, 0, error))
    goto fail;
  STORE_CUT("forget", "messages");
  if (!store_exec_id(store,
        "UPDATE conversations SET forgotten_before = MAX(forgotten_before, ?2), "
        "title = NULL, draft = NULL, unread_count = 0, last_read_msg = NULL, "
        "pinned_rank = NULL, last_activity = 0 WHERE id = ?1",
        conversation_id, now, error))
    goto fail;
  STORE_CUT("forget", "conversation");
  if (!store_commit(store, error))
    return FALSE;
  /* The deleted content leaves the WAL now, not at the next purge. */
  if (store->depth == 0 && !store->ephemeral)
    (void) sqlite3_wal_checkpoint_v2(store->db, NULL, SQLITE_CHECKPOINT_TRUNCATE, NULL, NULL);
  return TRUE;

fail:
  sqlite3_finalize(stmt);
  store_rollback(store);
  return FALSE;
}
