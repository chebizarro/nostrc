#include "gh-store-marmot.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

/* libmarmot keeps a group's MLS state (tree, epoch secrets, ...) in mls_kv
 * under MLS_GROUP_STATE_LABEL, and since libmarmot 0.5.0 the parent state of
 * the last applied Commit (for same-epoch races, nostrc-9ata) under
 * MLS_GROUP_PARENT_LABEL, a local Commit awaiting a relay under
 * MLS_GROUP_PENDING_LABEL and the unsent Welcomes of merged Adds under
 * MLS_GROUP_WELCOMES_LABEL, and since libmarmot 0.12.0 (nostrc-2um6) the
 * group's standalone proposals under MLS_GROUP_PROPOSALS_LABEL and the
 * account's leave request under MLS_GROUP_LEAVING_LABEL, all keyed by the MLS
 * group id (groups.c, commits.c, messages.c, welcome.c, proposals.c). They
 * are the group-scoped labels a snapshot captures and restores (the kept
 * proposals themselves, "mls_group_proposal_slot", are keyed by group id ||
 * epoch || sender and stay out of snapshots like every composite key; a
 * rollback restores the index, whose missing or extra slots read as empty or
 * are pruned). "welcome_signer" (since libmarmot 0.12.0, members.c) is keyed
 * by the group id too, but records a fact of the join (which device the
 * Welcome's sender vouched for), not epoch state: an epoch rollback must not
 * touch it, so it stays out of snapshots. The
 * others are keyed by a public key (kp_slot; kp_life, since libmarmot 0.12.0
 * the account's KeyPackage lifecycle record, nostrc-0bdg), a KeyPackageRef
 * (kp_priv, kp_full), a gift-wrap id (welcome_data) or a nostr_group_id
 * (group_event_created_at, since libmarmot 0.12: the created_at floor of the
 * group's events, which a rolled-back Commit, already published, must not
 * lower) and must never be captured or restored by a group snapshot. The
 * e2e test fails if libmarmot starts using an unclassified label. */
#define MLS_GROUP_STATE_LABEL "mls_group"
#define MLS_GROUP_PARENT_LABEL "mls_group_parent"
#define MLS_GROUP_PENDING_LABEL "mls_group_pending"
#define MLS_GROUP_WELCOMES_LABEL "mls_group_welcomes"
#define MLS_GROUP_PROPOSALS_LABEL "mls_group_proposals"
#define MLS_GROUP_LEAVING_LABEL "mls_group_leaving"

/* mls_snapshots.data: the snapshot format. 1 = row copies in
 * mls_snapshot_rows (schema v2). A rollback refuses any other value. */
static const guint8 snapshot_format = 1;

typedef struct {
  GhStore *store;
  GError *error; /* the first failure since gh_store_marmot_take_error() */
} GhStoreMarmot;

static void ghm_destroy(void *ctx);

/* ---- Errors ---------------------------------------------------------------------
 * Failures are recorded as GhStore errors for gh_store_marmot_take_error().
 * "Not found" results are answers, not failures, and are never recorded. */

static MarmotError
record(GhStoreMarmot *self, MarmotError code, GError *error)
{
  if (!self->error)
    self->error = error;
  else
    g_error_free(error);
  return code;
}

static MarmotError fail(GhStoreMarmot *self, MarmotError code, GhStoreError store_code,
                        const gchar *format, ...) G_GNUC_PRINTF(4, 5);

static MarmotError
fail(GhStoreMarmot *self, MarmotError code, GhStoreError store_code, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  GError *error = g_error_new_valist(GH_STORE_ERROR, store_code, format, args);
  va_end(args);
  return record(self, code, error);
}

#define invalid(self, ...) fail((self), MARMOT_ERR_INVALID_ARG, GH_STORE_ERROR_INVALID, __VA_ARGS__)

static MarmotError
oom(GhStoreMarmot *self)
{
  return fail(self, MARMOT_ERR_MEMORY, GH_STORE_ERROR_FAILED, "Out of memory");
}

/* SQLite result codes keep their GhStore meaning (SQLITE_FULL stays FULL). */
static MarmotError
fail_sqlite(GhStoreMarmot *self, int rc, const gchar *what)
{
  GError *error = NULL;
  gh_store_set_sqlite_error(self->store, rc, what, &error);
  return record(self, (rc & 0xff) == SQLITE_CONSTRAINT ? MARMOT_ERR_STORAGE_CONSTRAINT
                                                        : MARMOT_ERR_STORAGE, error);
}

static MarmotError
fail_store(GhStoreMarmot *self, GError *error)
{
  return record(self, g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID)
                        ? MARMOT_ERR_INVALID_ARG : MARMOT_ERR_STORAGE, error);
}

static MarmotError
corrupt(GhStoreMarmot *self, sqlite3_stmt *stmt, int column)
{
  const char *name = sqlite3_column_name(stmt, column);
  return fail(self, MARMOT_ERR_STORAGE, GH_STORE_ERROR_CORRUPT,
              "The stored MLS value %s has an unexpected type or size", name ? name : "?");
}

/* ---- Transactions and statements ------------------------------------------------ */

static sqlite3 *
ghm_db(GhStoreMarmot *self)
{
  return gh_store_get_db(self->store);
}

/* Writes: their own transaction, or a savepoint of the caller's. */
static MarmotError
txn_begin(GhStoreMarmot *self)
{
  GError *error = NULL;
  if (gh_store_begin(self->store, &error))
    return MARMOT_OK;
  return fail_store(self, error);
}

/* Commits (releases the savepoint) after success; otherwise undoes it all. */
static MarmotError
txn_end(GhStoreMarmot *self, MarmotError result)
{
  if (result != MARMOT_OK) {
    gh_store_rollback(self->store);
    return result;
  }
  GError *error = NULL;
  if (gh_store_commit(self->store, &error))
    return MARMOT_OK;
  return fail_store(self, error);
}

/* Reads must not run on in a caller transaction that SQLite abandoned. */
static MarmotError
check_readable(GhStoreMarmot *self)
{
  if (gh_store_get_transaction_depth(self->store) == 0 || !sqlite3_get_autocommit(ghm_db(self)))
    return MARMOT_OK;
  return fail(self, MARMOT_ERR_STORAGE, GH_STORE_ERROR_FAILED,
              "The store transaction was rolled back by an earlier error");
}

static MarmotError
prepare(GhStoreMarmot *self, const char *sql, sqlite3_stmt **stmt)
{
  *stmt = NULL;
  int rc = sqlite3_prepare_v2(ghm_db(self), sql, -1, stmt, NULL);
  if (rc == SQLITE_OK)
    return MARMOT_OK;
  sqlite3_finalize(*stmt);
  *stmt = NULL;
  return fail_sqlite(self, rc, "Preparing an MLS statement");
}

/* Test builds: the crash harness can cut after any MLS write (H8). */
#ifdef GH_STORE_TEST_HOOKS
#define MLS_CUT(step) gh_store_test_cut("mls", (step))
#else
#define MLS_CUT(step) ((void) 0)
#endif

/* Steps a statement that returns no rows (every write goes through here). */
static MarmotError
step_done(GhStoreMarmot *self, sqlite3_stmt *stmt, const gchar *what)
{
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_DONE) {
    MLS_CUT("write");
    return MARMOT_OK;
  }
  if (rc == SQLITE_ROW)
    return fail(self, MARMOT_ERR_STORAGE, GH_STORE_ERROR_FAILED, "%s: unexpected row", what);
  return fail_sqlite(self, rc, what);
}

/* Steps once; *has_row tells whether a row is available. */
static MarmotError
step_row(GhStoreMarmot *self, sqlite3_stmt *stmt, gboolean *has_row, const gchar *what)
{
  int rc = sqlite3_step(stmt);
  *has_row = rc == SQLITE_ROW;
  if (rc == SQLITE_ROW || rc == SQLITE_DONE)
    return MARMOT_OK;
  return fail_sqlite(self, rc, what);
}

/* Both need `MarmotError err` and an `out:` label that cleans up. */
#define TRY(expr)                                                               \
  G_STMT_START {                                                                \
    err = (expr);                                                               \
    if (err != MARMOT_OK)                                                       \
      goto out;                                                                 \
  } G_STMT_END

#define BIND(expr)                                                              \
  G_STMT_START {                                                                \
    int bind_rc_ = (expr);                                                      \
    if (bind_rc_ != SQLITE_OK) {                                                \
      err = fail_sqlite(self, bind_rc_, "Binding an MLS value");                \
      goto out;                                                                 \
    }                                                                           \
  } G_STMT_END

/* Exact bytes; an empty value is an empty blob, never NULL. */
static int
bind_bytes(sqlite3_stmt *stmt, int index, const void *data, size_t len)
{
  if (len == 0)
    return sqlite3_bind_zeroblob(stmt, index, 0);
  return sqlite3_bind_blob64(stmt, index, data, (sqlite3_uint64) len, SQLITE_STATIC);
}

/* NULL for an absent optional field or an empty list. */
static int
bind_optional_bytes(sqlite3_stmt *stmt, int index, const void *data, size_t len)
{
  if (!data || len == 0)
    return sqlite3_bind_null(stmt, index);
  return sqlite3_bind_blob64(stmt, index, data, (sqlite3_uint64) len, SQLITE_STATIC);
}

static int
bind_text_len(sqlite3_stmt *stmt, int index, const char *text, size_t len)
{
  if (!text)
    return sqlite3_bind_null(stmt, index);
  return sqlite3_bind_text64(stmt, index, text, (sqlite3_uint64) len, SQLITE_STATIC,
                             SQLITE_UTF8);
}

static int
bind_text(sqlite3_stmt *stmt, int index, const char *text)
{
  return bind_text_len(stmt, index, text, text ? strlen(text) : 0);
}

/* A group id that may be absent (welcomes, processed messages). */
static int
bind_optional_gid(sqlite3_stmt *stmt, int index, const MarmotGroupId *gid)
{
  return bind_optional_bytes(stmt, index, gid ? gid->data : NULL, gid ? gid->len : 0);
}

static sqlite3_int64
clamp_size(size_t value)
{
  return value > (size_t) G_MAXINT64 ? G_MAXINT64 : (sqlite3_int64) value;
}

/* ---- Validation (before any SQL) ------------------------------------------------ */

static MarmotError
check_gid(GhStoreMarmot *self, const MarmotGroupId *gid)
{
  if (!gid || gid->len == 0 || !gid->data)
    return invalid(self, "An MLS group id is required");
  if (gid->len > GH_STORE_MARMOT_MAX_GROUP_ID)
    return invalid(self, "An MLS group id is longer than %d bytes", GH_STORE_MARMOT_MAX_GROUP_ID);
  return MARMOT_OK;
}

/* An absent (empty) group id is fine; a present one must be valid. */
static MarmotError
check_optional_gid(GhStoreMarmot *self, const MarmotGroupId *gid)
{
  if (!gid || gid->len == 0)
    return MARMOT_OK;
  return check_gid(self, gid);
}

/* NULL is fine; longer than @max bytes is not. */
static MarmotError
check_text(GhStoreMarmot *self, const gchar *what, const char *text, gsize max)
{
  if (text && strnlen(text, max + 1) > max)
    return invalid(self, "%s is longer than %" G_GSIZE_FORMAT " bytes", what, max);
  return MARMOT_OK;
}

static MarmotError
check_bytes(GhStoreMarmot *self, const gchar *what, const void *data, size_t len, gsize max)
{
  if (len > max)
    return invalid(self, "%s is longer than %" G_GSIZE_FORMAT " bytes", what, max);
  if (len > 0 && !data)
    return invalid(self, "%s is missing", what);
  return MARMOT_OK;
}

static MarmotError
check_admins(GhStoreMarmot *self, const void *pubkeys, size_t count)
{
  if (count > GH_STORE_MARMOT_MAX_ADMINS)
    return invalid(self, "A group has more than %d admins", GH_STORE_MARMOT_MAX_ADMINS);
  if (count > 0 && !pubkeys)
    return invalid(self, "The admin public keys are missing");
  return MARMOT_OK;
}

static MarmotError
check_relays(GhStoreMarmot *self, const char *const *urls, size_t count)
{
  if (count > GH_STORE_MARMOT_MAX_RELAYS)
    return invalid(self, "More than %d relays", GH_STORE_MARMOT_MAX_RELAYS);
  if (count > 0 && !urls)
    return invalid(self, "The relay list is missing");
  for (size_t i = 0; i < count; i++) {
    if (!urls[i])
      return invalid(self, "Relay %" G_GSIZE_FORMAT " is missing", i);
    MarmotError err = check_text(self, "A relay URL", urls[i], GH_STORE_MARMOT_MAX_RELAY_URL);
    if (err != MARMOT_OK)
      return err;
  }
  return MARMOT_OK;
}

static MarmotError
check_label_key(GhStoreMarmot *self, const char *label, const uint8_t *key, size_t key_len)
{
  if (!label || !*label)
    return invalid(self, "An MLS storage label is required");
  MarmotError err = check_text(self, "An MLS storage label", label, GH_STORE_MARMOT_MAX_LABEL);
  if (err != MARMOT_OK)
    return err;
  return check_bytes(self, "An MLS storage key", key, key_len, GH_STORE_MARMOT_MAX_KEY);
}

static MarmotError
check_snapshot(GhStoreMarmot *self, const MarmotGroupId *gid, const char *name)
{
  MarmotError err = check_gid(self, gid);
  if (err != MARMOT_OK)
    return err;
  if (!name || !*name)
    return invalid(self, "A snapshot name is required");
  return check_text(self, "A snapshot name", name, GH_STORE_MARMOT_MAX_SNAPSHOT_NAME);
}

/* Failure reasons are diagnostics: cut, on a character boundary, not refused. */
static size_t
reason_length(const char *reason)
{
  size_t len = strnlen(reason, GH_STORE_MARMOT_MAX_REASON + 1);
  if (len <= GH_STORE_MARMOT_MAX_REASON)
    return len;
  len = GH_STORE_MARMOT_MAX_REASON;
  while (len > 0 && (((guchar) reason[len]) & 0xc0) == 0x80)
    len--;
  return len;
}

/* ---- Reading columns into malloc() memory libmarmot frees ----------------------- */

static void *
grow_array(void *array, size_t *cap, size_t len, size_t elem_size)
{
  if (len < *cap)
    return array;
  size_t new_cap = *cap ? *cap * 2 : 8;
  void *bigger = realloc(array, new_cap * elem_size);
  if (bigger)
    *cap = new_cap;
  return bigger;
}

static MarmotError
read_text(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, char **out)
{
  *out = NULL;
  int type = sqlite3_column_type(stmt, col);
  if (type == SQLITE_NULL)
    return MARMOT_OK;
  if (type != SQLITE_TEXT)
    return corrupt(self, stmt, col);
  const unsigned char *text = sqlite3_column_text(stmt, col);
  int len = sqlite3_column_bytes(stmt, col);
  if (!text)
    return oom(self);
  char *copy = malloc((size_t) len + 1);
  if (!copy)
    return oom(self);
  memcpy(copy, text, (size_t) len);
  copy[len] = '\0';
  *out = copy;
  return MARMOT_OK;
}

/* A blob of exactly @size (> 0) bytes. */
static MarmotError
read_fixed(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, uint8_t *out, size_t size)
{
  if (sqlite3_column_type(stmt, col) != SQLITE_BLOB)
    return corrupt(self, stmt, col);
  const void *data = sqlite3_column_blob(stmt, col);
  if ((size_t) sqlite3_column_bytes(stmt, col) != size)
    return corrupt(self, stmt, col);
  if (!data)
    return oom(self);
  memcpy(out, data, size);
  return MARMOT_OK;
}

/* An optional fixed-size blob (NULL stays NULL). */
static MarmotError
read_fixed_dup(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, size_t size, uint8_t **out)
{
  *out = NULL;
  if (sqlite3_column_type(stmt, col) == SQLITE_NULL)
    return MARMOT_OK;
  uint8_t *copy = malloc(size);
  if (!copy)
    return oom(self);
  MarmotError err = read_fixed(self, stmt, col, copy, size);
  if (err != MARMOT_OK) {
    free(copy);
    return err;
  }
  *out = copy;
  return MARMOT_OK;
}

/* A variable-length blob (mls_kv values). Present but empty is a 1-byte
 * allocation with length 0, so callers can tell it from "missing". */
static MarmotError
read_blob(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, uint8_t **out, size_t *out_len)
{
  *out = NULL;
  *out_len = 0;
  if (sqlite3_column_type(stmt, col) != SQLITE_BLOB)
    return corrupt(self, stmt, col);
  const void *data = sqlite3_column_blob(stmt, col);
  int len = sqlite3_column_bytes(stmt, col);
  if (len > 0 && !data)
    return oom(self);
  uint8_t *copy = malloc(len > 0 ? (size_t) len : 1);
  if (!copy)
    return oom(self);
  if (len > 0)
    memcpy(copy, data, (size_t) len);
  *out = copy;
  *out_len = (size_t) len;
  return MARMOT_OK;
}

/* NULL means no group id. */
static MarmotError
read_gid(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, MarmotGroupId *out)
{
  out->data = NULL;
  out->len = 0;
  int type = sqlite3_column_type(stmt, col);
  if (type == SQLITE_NULL)
    return MARMOT_OK;
  if (type != SQLITE_BLOB)
    return corrupt(self, stmt, col);
  const void *data = sqlite3_column_blob(stmt, col);
  int len = sqlite3_column_bytes(stmt, col);
  if (len <= 0 || len > GH_STORE_MARMOT_MAX_GROUP_ID)
    return corrupt(self, stmt, col);
  if (!data)
    return oom(self);
  *out = marmot_group_id_new(data, (size_t) len);
  return out->data ? MARMOT_OK : oom(self);
}

/* n x 32-byte x-only public keys; NULL for none. */
static MarmotError
read_pubkeys(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, uint8_t (**out)[32],
             size_t *out_count)
{
  *out = NULL;
  *out_count = 0;
  int type = sqlite3_column_type(stmt, col);
  if (type == SQLITE_NULL)
    return MARMOT_OK;
  if (type != SQLITE_BLOB)
    return corrupt(self, stmt, col);
  const void *data = sqlite3_column_blob(stmt, col);
  int len = sqlite3_column_bytes(stmt, col);
  if (len <= 0 || len % 32 != 0 || len / 32 > GH_STORE_MARMOT_MAX_ADMINS)
    return corrupt(self, stmt, col);
  if (!data)
    return oom(self);
  uint8_t (*copy)[32] = malloc((size_t) len);
  if (!copy)
    return oom(self);
  memcpy(copy, data, (size_t) len);
  *out = copy;
  *out_count = (size_t) len / 32;
  return MARMOT_OK;
}

/* ---- Relay lists of welcomes and key packages -------------------------------------
 * Per URL a 4-byte big-endian length, then its bytes; NULL for no relays. The
 * list comes back exactly as given (order, duplicates, any byte but NUL). */

static GByteArray *
relays_encode(const char *const *urls, size_t count)
{
  if (count == 0)
    return NULL;
  GByteArray *buf = g_byte_array_new();
  for (size_t i = 0; i < count; i++) {
    guint32 len = (guint32) strlen(urls[i]);
    const guint8 prefix[4] = { (guint8) (len >> 24), (guint8) (len >> 16),
                               (guint8) (len >> 8), (guint8) len };
    g_byte_array_append(buf, prefix, sizeof prefix);
    g_byte_array_append(buf, (const guint8 *) urls[i], len);
  }
  return buf;
}

static guint32
read_be32(const guint8 *p)
{
  return (guint32) p[0] << 24 | (guint32) p[1] << 16 | (guint32) p[2] << 8 | (guint32) p[3];
}

static void
free_strings(char **strings, size_t count)
{
  if (!strings)
    return;
  for (size_t i = 0; i < count; i++)
    free(strings[i]);
  free(strings);
}

static MarmotError
read_relays(GhStoreMarmot *self, sqlite3_stmt *stmt, int col, char ***out, size_t *out_count)
{
  *out = NULL;
  *out_count = 0;
  int type = sqlite3_column_type(stmt, col);
  if (type == SQLITE_NULL)
    return MARMOT_OK;
  if (type != SQLITE_BLOB)
    return corrupt(self, stmt, col);
  const guint8 *data = sqlite3_column_blob(stmt, col);
  size_t len = (size_t) sqlite3_column_bytes(stmt, col);
  if (len == 0)
    return corrupt(self, stmt, col);
  if (!data)
    return oom(self);

  /* Validate the whole list before allocating anything. */
  size_t n = 0;
  for (size_t pos = 0; pos < len; n++) {
    if (n >= GH_STORE_MARMOT_MAX_RELAYS || len - pos < 4)
      return corrupt(self, stmt, col);
    guint32 url_len = read_be32(data + pos);
    pos += 4;
    if (url_len > GH_STORE_MARMOT_MAX_RELAY_URL || url_len > len - pos ||
        memchr(data + pos, '\0', url_len))
      return corrupt(self, stmt, col);
    pos += url_len;
  }

  char **urls = calloc(n + 1, sizeof *urls);
  if (!urls)
    return oom(self);
  size_t pos = 0;
  for (size_t i = 0; i < n; i++) {
    guint32 url_len = read_be32(data + pos);
    pos += 4;
    urls[i] = malloc((size_t) url_len + 1);
    if (!urls[i]) {
      free_strings(urls, i);
      return oom(self);
    }
    memcpy(urls[i], data + pos, url_len);
    urls[i][url_len] = '\0';
    pos += url_len;
  }
  *out = urls;
  *out_count = n;
  return MARMOT_OK;
}

/* ---- Rows <-> records ------------------------------------------------------------- */

#define GROUP_COLUMNS                                                           \
  "mls_group_id, nostr_group_id, name, description, image_hash, image_key, "  \
  "image_nonce, admin_pubkeys, last_message_id, last_message_at, "            \
  "last_message_processed_at, epoch, state"

#define MESSAGE_COLUMNS                                                         \
  "id, mls_group_id, pubkey, kind, created_at, processed_at, content, "       \
  "tags_json, event_json, wrapper_event_id, epoch, state"

#define WELCOME_COLUMNS                                                         \
  "id, wrapper_event_id, event_json, mls_group_id, nostr_group_id, "          \
  "group_name, group_description, group_image_hash, group_admin_pubkeys, "    \
  "group_relays, welcomer, member_count, state"

#define KEY_PACKAGE_COLUMNS "ref, owner_pubkey, relay_urls, created_at, active"

/* Columns in GROUP_COLUMNS order. */
static MarmotError
group_from_row(GhStoreMarmot *self, sqlite3_stmt *stmt, MarmotGroup **out)
{
  *out = NULL;
  MarmotGroup *g = marmot_group_new();
  if (!g)
    return oom(self);
  MarmotError err = MARMOT_OK;
  TRY(read_gid(self, stmt, 0, &g->mls_group_id));
  TRY(read_fixed(self, stmt, 1, g->nostr_group_id, 32));
  TRY(read_text(self, stmt, 2, &g->name));
  TRY(read_text(self, stmt, 3, &g->description));
  TRY(read_fixed_dup(self, stmt, 4, 32, &g->image_hash));
  TRY(read_fixed_dup(self, stmt, 5, 32, &g->image_key));
  TRY(read_fixed_dup(self, stmt, 6, 12, &g->image_nonce));
  TRY(read_pubkeys(self, stmt, 7, &g->admin_pubkeys, &g->admin_count));
  TRY(read_text(self, stmt, 8, &g->last_message_id));
  g->last_message_at = sqlite3_column_int64(stmt, 9);
  g->last_message_processed_at = sqlite3_column_int64(stmt, 10);
  g->epoch = (uint64_t) sqlite3_column_int64(stmt, 11);
  g->state = (MarmotGroupState) sqlite3_column_int(stmt, 12);
out:
  if (err != MARMOT_OK) {
    marmot_group_free(g);
    return err;
  }
  *out = g;
  return MARMOT_OK;
}

/* Columns in MESSAGE_COLUMNS order. */
static MarmotError
message_from_row(GhStoreMarmot *self, sqlite3_stmt *stmt, MarmotMessage **out)
{
  *out = NULL;
  MarmotMessage *m = marmot_message_new();
  if (!m)
    return oom(self);
  MarmotError err = MARMOT_OK;
  TRY(read_fixed(self, stmt, 0, m->id, 32));
  TRY(read_gid(self, stmt, 1, &m->mls_group_id));
  TRY(read_fixed(self, stmt, 2, m->pubkey, 32));
  m->kind = (uint32_t) sqlite3_column_int64(stmt, 3);
  m->created_at = sqlite3_column_int64(stmt, 4);
  m->processed_at = sqlite3_column_int64(stmt, 5);
  TRY(read_text(self, stmt, 6, &m->content));
  TRY(read_text(self, stmt, 7, &m->tags_json));
  TRY(read_text(self, stmt, 8, &m->event_json));
  TRY(read_fixed(self, stmt, 9, m->wrapper_event_id, 32));
  m->epoch = (uint64_t) sqlite3_column_int64(stmt, 10);
  m->state = (MarmotMessageState) sqlite3_column_int(stmt, 11);
out:
  if (err != MARMOT_OK) {
    marmot_message_free(m);
    return err;
  }
  *out = m;
  return MARMOT_OK;
}

/* Columns in WELCOME_COLUMNS order. */
static MarmotError
welcome_from_row(GhStoreMarmot *self, sqlite3_stmt *stmt, MarmotWelcome **out)
{
  *out = NULL;
  MarmotWelcome *w = marmot_welcome_new();
  if (!w)
    return oom(self);
  MarmotError err = MARMOT_OK;
  TRY(read_fixed(self, stmt, 0, w->id, 32));
  TRY(read_fixed(self, stmt, 1, w->wrapper_event_id, 32));
  TRY(read_text(self, stmt, 2, &w->event_json));
  TRY(read_gid(self, stmt, 3, &w->mls_group_id));
  TRY(read_fixed(self, stmt, 4, w->nostr_group_id, 32));
  TRY(read_text(self, stmt, 5, &w->group_name));
  TRY(read_text(self, stmt, 6, &w->group_description));
  TRY(read_fixed_dup(self, stmt, 7, 32, &w->group_image_hash));
  TRY(read_pubkeys(self, stmt, 8, &w->group_admin_pubkeys, &w->group_admin_count));
  TRY(read_relays(self, stmt, 9, &w->group_relays, &w->group_relay_count));
  TRY(read_fixed(self, stmt, 10, w->welcomer, 32));
  w->member_count = (uint32_t) sqlite3_column_int64(stmt, 11);
  w->state = (MarmotWelcomeState) sqlite3_column_int(stmt, 12);
out:
  if (err != MARMOT_OK) {
    marmot_welcome_free(w);
    return err;
  }
  *out = w;
  return MARMOT_OK;
}

/* Columns in KEY_PACKAGE_COLUMNS order. */
static MarmotError
key_package_from_row(GhStoreMarmot *self, sqlite3_stmt *stmt, MarmotKeyPackageInfo **out)
{
  *out = NULL;
  MarmotKeyPackageInfo *info = marmot_key_package_info_new();
  if (!info)
    return oom(self);
  MarmotError err = MARMOT_OK;
  TRY(read_fixed(self, stmt, 0, info->ref, 32));
  TRY(read_fixed(self, stmt, 1, info->owner_pubkey, 32));
  TRY(read_relays(self, stmt, 2, &info->relay_urls, &info->relay_count));
  info->created_at = sqlite3_column_int64(stmt, 3);
  info->active = sqlite3_column_int64(stmt, 4) != 0;
out:
  if (err != MARMOT_OK) {
    marmot_key_package_info_free(info);
    return err;
  }
  *out = info;
  return MARMOT_OK;
}

/* Runs @sql, whose only parameter ?1 is @key, as one write step. */
static MarmotError
exec_keyed(GhStoreMarmot *self, const char *sql, const void *key, size_t key_len,
           const gchar *what)
{
  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  TRY(prepare(self, sql, &stmt));
  BIND(bind_bytes(stmt, 1, key, key_len));
  TRY(step_done(self, stmt, what));
out:
  sqlite3_finalize(stmt);
  return err;
}

/* Whether @sql (parameter ?1 = @key) returns a row. */
static MarmotError
query_exists(GhStoreMarmot *self, const char *sql, const void *key, size_t key_len,
             gboolean *exists, const gchar *what)
{
  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  *exists = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, sql, &stmt));
  BIND(bind_bytes(stmt, 1, key, key_len));
  TRY(step_row(self, stmt, exists, what));
out:
  sqlite3_finalize(stmt);
  return err;
}

/* ---- Groups ------------------------------------------------------------------------ */

static MarmotError
ghm_all_groups(void *ctx, MarmotGroup ***out_groups, size_t *out_count)
{
  GhStoreMarmot *self = ctx;
  if (!out_groups || !out_count)
    return invalid(self, "all_groups needs its output arguments");
  *out_groups = NULL;
  *out_count = 0;

  sqlite3_stmt *stmt = NULL;
  MarmotGroup **groups = NULL;
  size_t n = 0, cap = 0;
  MarmotError err = MARMOT_OK;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT " GROUP_COLUMNS " FROM mls_group_info ORDER BY mls_group_id",
              &stmt));
  for (;;) {
    gboolean has_row = FALSE;
    TRY(step_row(self, stmt, &has_row, "Listing MLS groups"));
    if (!has_row)
      break;
    MarmotGroup **bigger = grow_array(groups, &cap, n, sizeof *groups);
    if (!bigger) {
      err = oom(self);
      goto out;
    }
    groups = bigger;
    TRY(group_from_row(self, stmt, &groups[n]));
    n++;
  }
out:
  sqlite3_finalize(stmt);
  if (err != MARMOT_OK) {
    for (size_t i = 0; i < n; i++)
      marmot_group_free(groups[i]);
    free(groups);
    return err;
  }
  *out_groups = groups;
  *out_count = n;
  return MARMOT_OK;
}

static MarmotError
find_group(GhStoreMarmot *self, const char *sql, const void *key, size_t key_len,
           MarmotGroup **out)
{
  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, sql, &stmt));
  BIND(bind_bytes(stmt, 1, key, key_len));
  TRY(step_row(self, stmt, &has_row, "Looking up an MLS group"));
  if (has_row)
    TRY(group_from_row(self, stmt, out));
out:
  sqlite3_finalize(stmt);
  return err;
}

static MarmotError
ghm_find_group_by_mls_id(void *ctx, const MarmotGroupId *mls_group_id, MarmotGroup **out)
{
  GhStoreMarmot *self = ctx;
  if (!out)
    return invalid(self, "find_group_by_mls_id needs an output argument");
  *out = NULL;
  MarmotError err = check_gid(self, mls_group_id);
  if (err != MARMOT_OK)
    return err;
  return find_group(self,
                    "SELECT " GROUP_COLUMNS " FROM mls_group_info WHERE mls_group_id = ?1",
                    mls_group_id->data, mls_group_id->len, out);
}

/* Nostr group ids are expected to be unique; duplicates resolve to the
 * lowest MLS group id so the answer never depends on write order. */
static MarmotError
ghm_find_group_by_nostr_id(void *ctx, const uint8_t nostr_group_id[32], MarmotGroup **out)
{
  GhStoreMarmot *self = ctx;
  if (!out)
    return invalid(self, "find_group_by_nostr_id needs an output argument");
  *out = NULL;
  if (!nostr_group_id)
    return invalid(self, "A Nostr group id is required");
  return find_group(self,
                    "SELECT " GROUP_COLUMNS " FROM mls_group_info WHERE nostr_group_id = ?1 "
                    "ORDER BY mls_group_id LIMIT 1",
                    nostr_group_id, 32, out);
}

static MarmotError
ghm_save_group(void *ctx, const MarmotGroup *group)
{
  GhStoreMarmot *self = ctx;
  if (!group)
    return invalid(self, "No group to save");
  MarmotError err = MARMOT_OK;
  if ((err = check_gid(self, &group->mls_group_id)) != MARMOT_OK ||
      (err = check_text(self, "A group name", group->name, GH_STORE_MARMOT_MAX_TEXT)) != MARMOT_OK ||
      (err = check_text(self, "A group description", group->description,
                        GH_STORE_MARMOT_MAX_TEXT)) != MARMOT_OK ||
      (err = check_admins(self, group->admin_pubkeys, group->admin_count)) != MARMOT_OK ||
      (err = check_text(self, "A last message id", group->last_message_id,
                        GH_STORE_MARMOT_MAX_MESSAGE_ID)) != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self,
    "INSERT INTO mls_group_info (" GROUP_COLUMNS ") "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13) "
    "ON CONFLICT (mls_group_id) DO UPDATE SET nostr_group_id = excluded.nostr_group_id, "
    "name = excluded.name, description = excluded.description, "
    "image_hash = excluded.image_hash, image_key = excluded.image_key, "
    "image_nonce = excluded.image_nonce, admin_pubkeys = excluded.admin_pubkeys, "
    "last_message_id = excluded.last_message_id, last_message_at = excluded.last_message_at, "
    "last_message_processed_at = excluded.last_message_processed_at, "
    "epoch = excluded.epoch, state = excluded.state", &stmt));
  BIND(bind_bytes(stmt, 1, group->mls_group_id.data, group->mls_group_id.len));
  BIND(bind_bytes(stmt, 2, group->nostr_group_id, 32));
  BIND(bind_text(stmt, 3, group->name));
  BIND(bind_text(stmt, 4, group->description));
  BIND(bind_optional_bytes(stmt, 5, group->image_hash, 32));
  BIND(bind_optional_bytes(stmt, 6, group->image_key, 32));
  BIND(bind_optional_bytes(stmt, 7, group->image_nonce, 12));
  BIND(bind_optional_bytes(stmt, 8, group->admin_pubkeys, group->admin_count * 32));
  BIND(bind_text(stmt, 9, group->last_message_id));
  BIND(sqlite3_bind_int64(stmt, 10, group->last_message_at));
  BIND(sqlite3_bind_int64(stmt, 11, group->last_message_processed_at));
  BIND(sqlite3_bind_int64(stmt, 12, (sqlite3_int64) group->epoch));
  BIND(sqlite3_bind_int64(stmt, 13, group->state));
  TRY(step_done(self, stmt, "Saving an MLS group"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

static MarmotError
ghm_delete_group(void *ctx, const MarmotGroupId *mls_group_id)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = check_gid(self, mls_group_id);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;
  TRY(exec_keyed(self, "DELETE FROM mls_group_relays WHERE mls_group_id = ?1",
                 mls_group_id->data, mls_group_id->len, "Deleting an MLS group's relays"));
  TRY(exec_keyed(self, "DELETE FROM mls_group_info WHERE mls_group_id = ?1",
                 mls_group_id->data, mls_group_id->len, "Deleting an MLS group"));
out:
  return txn_end(self, err);
}

/* ---- Messages ---------------------------------------------------------------------- */

/* Newest first by the requested key; the other time and the id break ties. */
static const char *
messages_order(MarmotSortOrder order)
{
  switch (order) {
  case MARMOT_SORT_CREATED_AT_FIRST:
    return " ORDER BY created_at DESC, processed_at DESC, id DESC";
  case MARMOT_SORT_PROCESSED_AT_FIRST:
    return " ORDER BY processed_at DESC, created_at DESC, id DESC";
  default:
    return NULL;
  }
}

static MarmotError
ghm_messages(void *ctx, const MarmotGroupId *group_id, const MarmotPagination *pagination,
             MarmotMessage ***out_msgs, size_t *out_count)
{
  GhStoreMarmot *self = ctx;
  if (!out_msgs || !out_count)
    return invalid(self, "messages needs its output arguments");
  *out_msgs = NULL;
  *out_count = 0;
  MarmotError err = check_gid(self, group_id);
  if (err != MARMOT_OK)
    return err;
  MarmotPagination page = pagination ? *pagination : marmot_pagination_default();
  const char *order = messages_order(page.sort_order);
  if (!order)
    return invalid(self, "Unknown message sort order %d", (int) page.sort_order);
  if (page.limit == 0)
    return MARMOT_OK;

  sqlite3_stmt *stmt = NULL;
  MarmotMessage **msgs = NULL;
  size_t n = 0, cap = 0;
  g_autofree gchar *sql = g_strconcat("SELECT " MESSAGE_COLUMNS " FROM mls_messages "
                                      "WHERE mls_group_id = ?1", order,
                                      " LIMIT ?2 OFFSET ?3", NULL);
  TRY(check_readable(self));
  TRY(prepare(self, sql, &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  BIND(sqlite3_bind_int64(stmt, 2, clamp_size(page.limit)));
  BIND(sqlite3_bind_int64(stmt, 3, clamp_size(page.offset)));
  for (;;) {
    gboolean has_row = FALSE;
    TRY(step_row(self, stmt, &has_row, "Listing MLS messages"));
    if (!has_row)
      break;
    MarmotMessage **bigger = grow_array(msgs, &cap, n, sizeof *msgs);
    if (!bigger) {
      err = oom(self);
      goto out;
    }
    msgs = bigger;
    TRY(message_from_row(self, stmt, &msgs[n]));
    n++;
  }
out:
  sqlite3_finalize(stmt);
  if (err != MARMOT_OK) {
    for (size_t i = 0; i < n; i++)
      marmot_message_free(msgs[i]);
    free(msgs);
    return err;
  }
  *out_msgs = msgs;
  *out_count = n;
  return MARMOT_OK;
}

static MarmotError
find_message(GhStoreMarmot *self, const char *sql, const void *key, size_t key_len,
             MarmotMessage **out)
{
  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, sql, &stmt));
  BIND(bind_bytes(stmt, 1, key, key_len));
  TRY(step_row(self, stmt, &has_row, "Looking up an MLS message"));
  if (has_row)
    TRY(message_from_row(self, stmt, out));
out:
  sqlite3_finalize(stmt);
  return err;
}

static MarmotError
ghm_last_message(void *ctx, const MarmotGroupId *group_id, MarmotSortOrder sort_order,
                 MarmotMessage **out)
{
  GhStoreMarmot *self = ctx;
  if (!out)
    return invalid(self, "last_message needs an output argument");
  *out = NULL;
  MarmotError err = check_gid(self, group_id);
  if (err != MARMOT_OK)
    return err;
  const char *order = messages_order(sort_order);
  if (!order)
    return invalid(self, "Unknown message sort order %d", (int) sort_order);
  g_autofree gchar *sql = g_strconcat("SELECT " MESSAGE_COLUMNS " FROM mls_messages "
                                      "WHERE mls_group_id = ?1", order, " LIMIT 1", NULL);
  return find_message(self, sql, group_id->data, group_id->len, out);
}

static MarmotError
ghm_save_message(void *ctx, const MarmotMessage *msg)
{
  GhStoreMarmot *self = ctx;
  if (!msg)
    return invalid(self, "No message to save");
  MarmotError err = MARMOT_OK;
  if ((err = check_gid(self, &msg->mls_group_id)) != MARMOT_OK ||
      (err = check_text(self, "A message's content", msg->content,
                        GH_STORE_MARMOT_MAX_JSON)) != MARMOT_OK ||
      (err = check_text(self, "A message's tags", msg->tags_json,
                        GH_STORE_MARMOT_MAX_JSON)) != MARMOT_OK ||
      (err = check_text(self, "A message's event", msg->event_json,
                        GH_STORE_MARMOT_MAX_JSON)) != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self,
    "INSERT INTO mls_messages (" MESSAGE_COLUMNS ") "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12) "
    "ON CONFLICT (id) DO UPDATE SET mls_group_id = excluded.mls_group_id, "
    "pubkey = excluded.pubkey, kind = excluded.kind, created_at = excluded.created_at, "
    "processed_at = excluded.processed_at, content = excluded.content, "
    "tags_json = excluded.tags_json, event_json = excluded.event_json, "
    "wrapper_event_id = excluded.wrapper_event_id, epoch = excluded.epoch, "
    "state = excluded.state", &stmt));
  BIND(bind_bytes(stmt, 1, msg->id, 32));
  BIND(bind_bytes(stmt, 2, msg->mls_group_id.data, msg->mls_group_id.len));
  BIND(bind_bytes(stmt, 3, msg->pubkey, 32));
  BIND(sqlite3_bind_int64(stmt, 4, msg->kind));
  BIND(sqlite3_bind_int64(stmt, 5, msg->created_at));
  BIND(sqlite3_bind_int64(stmt, 6, msg->processed_at));
  BIND(bind_text(stmt, 7, msg->content));
  BIND(bind_text(stmt, 8, msg->tags_json));
  BIND(bind_text(stmt, 9, msg->event_json));
  BIND(bind_bytes(stmt, 10, msg->wrapper_event_id, 32));
  BIND(sqlite3_bind_int64(stmt, 11, (sqlite3_int64) msg->epoch));
  BIND(sqlite3_bind_int64(stmt, 12, msg->state));
  TRY(step_done(self, stmt, "Saving an MLS message"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

static MarmotError
ghm_find_message_by_id(void *ctx, const uint8_t event_id[32], MarmotMessage **out)
{
  GhStoreMarmot *self = ctx;
  if (!out)
    return invalid(self, "find_message_by_id needs an output argument");
  *out = NULL;
  if (!event_id)
    return invalid(self, "An event id is required");
  return find_message(self, "SELECT " MESSAGE_COLUMNS " FROM mls_messages WHERE id = ?1",
                      event_id, 32, out);
}

static MarmotError
ghm_is_message_processed(void *ctx, const uint8_t wrapper_event_id[32], bool *out_processed)
{
  GhStoreMarmot *self = ctx;
  if (!wrapper_event_id || !out_processed)
    return invalid(self, "is_message_processed needs a wrapper id and an output argument");
  *out_processed = false;
  gboolean exists = FALSE;
  MarmotError err = query_exists(self,
    "SELECT 1 FROM mls_processed_messages WHERE wrapper_event_id = ?1",
    wrapper_event_id, 32, &exists, "Looking up a processed MLS message");
  *out_processed = exists;
  return err;
}

static MarmotError
ghm_save_processed_message(void *ctx, const uint8_t wrapper_event_id[32],
                           const uint8_t *message_event_id, int64_t processed_at,
                           uint64_t epoch, const MarmotGroupId *mls_group_id, int state,
                           const char *failure_reason)
{
  GhStoreMarmot *self = ctx;
  if (!wrapper_event_id)
    return invalid(self, "A wrapper event id is required");
  MarmotError err = check_optional_gid(self, mls_group_id);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self,
    "INSERT OR REPLACE INTO mls_processed_messages (wrapper_event_id, message_event_id, "
    "processed_at, epoch, mls_group_id, state, failure_reason) "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)", &stmt));
  BIND(bind_bytes(stmt, 1, wrapper_event_id, 32));
  BIND(bind_optional_bytes(stmt, 2, message_event_id, 32));
  BIND(sqlite3_bind_int64(stmt, 3, processed_at));
  BIND(sqlite3_bind_int64(stmt, 4, (sqlite3_int64) epoch));
  BIND(bind_optional_gid(stmt, 5, mls_group_id));
  BIND(sqlite3_bind_int64(stmt, 6, state));
  BIND(bind_text_len(stmt, 7, failure_reason,
                     failure_reason ? reason_length(failure_reason) : 0));
  TRY(step_done(self, stmt, "Recording a processed MLS message"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

/* ---- Welcomes ---------------------------------------------------------------------- */

static MarmotError
ghm_save_welcome(void *ctx, const MarmotWelcome *welcome)
{
  GhStoreMarmot *self = ctx;
  if (!welcome)
    return invalid(self, "No welcome to save");
  const char *const *relays = (const char *const *) welcome->group_relays;
  MarmotError err = MARMOT_OK;
  if ((err = check_optional_gid(self, &welcome->mls_group_id)) != MARMOT_OK ||
      (err = check_text(self, "A welcome event", welcome->event_json,
                        GH_STORE_MARMOT_MAX_JSON)) != MARMOT_OK ||
      (err = check_text(self, "A group name", welcome->group_name,
                        GH_STORE_MARMOT_MAX_TEXT)) != MARMOT_OK ||
      (err = check_text(self, "A group description", welcome->group_description,
                        GH_STORE_MARMOT_MAX_TEXT)) != MARMOT_OK ||
      (err = check_admins(self, welcome->group_admin_pubkeys,
                          welcome->group_admin_count)) != MARMOT_OK ||
      (err = check_relays(self, relays, welcome->group_relay_count)) != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  g_autoptr(GByteArray) relay_blob = relays_encode(relays, welcome->group_relay_count);
  /* One row per welcome, whichever of its two ids the caller knows. */
  TRY(prepare(self, "DELETE FROM mls_welcomes WHERE id = ?1 OR wrapper_event_id = ?2", &stmt));
  BIND(bind_bytes(stmt, 1, welcome->id, 32));
  BIND(bind_bytes(stmt, 2, welcome->wrapper_event_id, 32));
  TRY(step_done(self, stmt, "Replacing an MLS welcome"));
  g_clear_pointer(&stmt, sqlite3_finalize);

  TRY(prepare(self,
    "INSERT INTO mls_welcomes (" WELCOME_COLUMNS ") "
    "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13)", &stmt));
  BIND(bind_bytes(stmt, 1, welcome->id, 32));
  BIND(bind_bytes(stmt, 2, welcome->wrapper_event_id, 32));
  BIND(bind_text(stmt, 3, welcome->event_json));
  BIND(bind_optional_gid(stmt, 4, &welcome->mls_group_id));
  BIND(bind_bytes(stmt, 5, welcome->nostr_group_id, 32));
  BIND(bind_text(stmt, 6, welcome->group_name));
  BIND(bind_text(stmt, 7, welcome->group_description));
  BIND(bind_optional_bytes(stmt, 8, welcome->group_image_hash, 32));
  BIND(bind_optional_bytes(stmt, 9, welcome->group_admin_pubkeys,
                           welcome->group_admin_count * 32));
  BIND(bind_optional_bytes(stmt, 10, relay_blob ? relay_blob->data : NULL,
                           relay_blob ? relay_blob->len : 0));
  BIND(bind_bytes(stmt, 11, welcome->welcomer, 32));
  BIND(sqlite3_bind_int64(stmt, 12, welcome->member_count));
  BIND(sqlite3_bind_int64(stmt, 13, welcome->state));
  TRY(step_done(self, stmt, "Saving an MLS welcome"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

static MarmotError
ghm_find_welcome_by_event_id(void *ctx, const uint8_t event_id[32], MarmotWelcome **out)
{
  GhStoreMarmot *self = ctx;
  if (!out)
    return invalid(self, "find_welcome_by_event_id needs an output argument");
  *out = NULL;
  if (!event_id)
    return invalid(self, "An event id is required");

  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT " WELCOME_COLUMNS " FROM mls_welcomes WHERE id = ?1", &stmt));
  BIND(bind_bytes(stmt, 1, event_id, 32));
  TRY(step_row(self, stmt, &has_row, "Looking up an MLS welcome"));
  if (has_row)
    TRY(welcome_from_row(self, stmt, out));
out:
  sqlite3_finalize(stmt);
  return err;
}

/* In arrival order; the sort order does not apply to welcomes. */
static MarmotError
ghm_pending_welcomes(void *ctx, const MarmotPagination *pagination,
                     MarmotWelcome ***out_welcomes, size_t *out_count)
{
  GhStoreMarmot *self = ctx;
  if (!out_welcomes || !out_count)
    return invalid(self, "pending_welcomes needs its output arguments");
  *out_welcomes = NULL;
  *out_count = 0;
  MarmotPagination page = pagination ? *pagination : marmot_pagination_default();
  if (page.limit == 0)
    return MARMOT_OK;

  sqlite3_stmt *stmt = NULL;
  MarmotWelcome **welcomes = NULL;
  size_t n = 0, cap = 0;
  MarmotError err = MARMOT_OK;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT " WELCOME_COLUMNS " FROM mls_welcomes WHERE state = ?1 "
                    "ORDER BY rowid LIMIT ?2 OFFSET ?3", &stmt));
  BIND(sqlite3_bind_int64(stmt, 1, MARMOT_WELCOME_STATE_PENDING));
  BIND(sqlite3_bind_int64(stmt, 2, clamp_size(page.limit)));
  BIND(sqlite3_bind_int64(stmt, 3, clamp_size(page.offset)));
  for (;;) {
    gboolean has_row = FALSE;
    TRY(step_row(self, stmt, &has_row, "Listing pending MLS welcomes"));
    if (!has_row)
      break;
    MarmotWelcome **bigger = grow_array(welcomes, &cap, n, sizeof *welcomes);
    if (!bigger) {
      err = oom(self);
      goto out;
    }
    welcomes = bigger;
    TRY(welcome_from_row(self, stmt, &welcomes[n]));
    n++;
  }
out:
  sqlite3_finalize(stmt);
  if (err != MARMOT_OK) {
    for (size_t i = 0; i < n; i++)
      marmot_welcome_free(welcomes[i]);
    free(welcomes);
    return err;
  }
  *out_welcomes = welcomes;
  *out_count = n;
  return MARMOT_OK;
}

static MarmotError
ghm_find_processed_welcome(void *ctx, const uint8_t wrapper_event_id[32], bool *out_found,
                           int *out_state, char **out_failure_reason)
{
  GhStoreMarmot *self = ctx;
  if (!wrapper_event_id || !out_found || !out_state || !out_failure_reason)
    return invalid(self, "find_processed_welcome needs a wrapper id and output arguments");
  *out_found = false;
  *out_state = 0;
  *out_failure_reason = NULL;

  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT state, failure_reason FROM mls_processed_welcomes "
                    "WHERE wrapper_event_id = ?1", &stmt));
  BIND(bind_bytes(stmt, 1, wrapper_event_id, 32));
  TRY(step_row(self, stmt, &has_row, "Looking up a processed MLS welcome"));
  if (has_row) {
    TRY(read_text(self, stmt, 1, out_failure_reason));
    *out_state = sqlite3_column_int(stmt, 0);
    *out_found = true;
  }
out:
  sqlite3_finalize(stmt);
  return err;
}

static MarmotError
ghm_save_processed_welcome(void *ctx, const uint8_t wrapper_event_id[32],
                           const uint8_t *welcome_event_id, int64_t processed_at, int state,
                           const char *failure_reason)
{
  GhStoreMarmot *self = ctx;
  if (!wrapper_event_id)
    return invalid(self, "A wrapper event id is required");
  MarmotError err = txn_begin(self);
  if (err != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self,
    "INSERT OR REPLACE INTO mls_processed_welcomes (wrapper_event_id, welcome_event_id, "
    "processed_at, state, failure_reason) VALUES (?1, ?2, ?3, ?4, ?5)", &stmt));
  BIND(bind_bytes(stmt, 1, wrapper_event_id, 32));
  BIND(bind_optional_bytes(stmt, 2, welcome_event_id, 32));
  BIND(sqlite3_bind_int64(stmt, 3, processed_at));
  BIND(sqlite3_bind_int64(stmt, 4, state));
  BIND(bind_text_len(stmt, 5, failure_reason,
                     failure_reason ? reason_length(failure_reason) : 0));
  TRY(step_done(self, stmt, "Recording a processed MLS welcome"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

/* ---- Key package info ------------------------------------------------------------- */

static MarmotError
ghm_save_key_package_info(void *ctx, const MarmotKeyPackageInfo *info)
{
  GhStoreMarmot *self = ctx;
  if (!info)
    return invalid(self, "No key package info to save");
  const char *const *relays = (const char *const *) info->relay_urls;
  MarmotError err = check_relays(self, relays, info->relay_count);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  g_autoptr(GByteArray) relay_blob = relays_encode(relays, info->relay_count);
  TRY(prepare(self,
    "INSERT INTO mls_key_packages (" KEY_PACKAGE_COLUMNS ") VALUES (?1, ?2, ?3, ?4, ?5) "
    "ON CONFLICT (ref) DO UPDATE SET owner_pubkey = excluded.owner_pubkey, "
    "relay_urls = excluded.relay_urls, created_at = excluded.created_at, "
    "active = excluded.active", &stmt));
  BIND(bind_bytes(stmt, 1, info->ref, 32));
  BIND(bind_bytes(stmt, 2, info->owner_pubkey, 32));
  BIND(bind_optional_bytes(stmt, 3, relay_blob ? relay_blob->data : NULL,
                           relay_blob ? relay_blob->len : 0));
  BIND(sqlite3_bind_int64(stmt, 4, info->created_at));
  BIND(sqlite3_bind_int64(stmt, 5, info->active ? 1 : 0));
  TRY(step_done(self, stmt, "Saving MLS key package info"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

static MarmotError
ghm_find_key_package_by_ref(void *ctx, const uint8_t ref[32], MarmotKeyPackageInfo **out)
{
  GhStoreMarmot *self = ctx;
  if (!out)
    return invalid(self, "find_key_package_by_ref needs an output argument");
  *out = NULL;
  if (!ref)
    return invalid(self, "A KeyPackageRef is required");

  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT " KEY_PACKAGE_COLUMNS " FROM mls_key_packages WHERE ref = ?1",
              &stmt));
  BIND(bind_bytes(stmt, 1, ref, 32));
  TRY(step_row(self, stmt, &has_row, "Looking up MLS key package info"));
  if (has_row)
    TRY(key_package_from_row(self, stmt, out));
out:
  sqlite3_finalize(stmt);
  return err;
}

/* Every package of the owner, active or not, newest first (as the memory,
 * SQLite and nostrdb backends; the header's "active" is inaccurate). */
static MarmotError
ghm_find_key_packages_by_pubkey(void *ctx, const uint8_t pubkey[32],
                                MarmotKeyPackageInfo ***out, size_t *out_count)
{
  GhStoreMarmot *self = ctx;
  if (!out || !out_count)
    return invalid(self, "find_key_packages_by_pubkey needs its output arguments");
  *out = NULL;
  *out_count = 0;
  if (!pubkey)
    return invalid(self, "A public key is required");

  sqlite3_stmt *stmt = NULL;
  MarmotKeyPackageInfo **infos = NULL;
  size_t n = 0, cap = 0;
  MarmotError err = MARMOT_OK;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT " KEY_PACKAGE_COLUMNS " FROM mls_key_packages "
                    "WHERE owner_pubkey = ?1 ORDER BY created_at DESC, ref", &stmt));
  BIND(bind_bytes(stmt, 1, pubkey, 32));
  for (;;) {
    gboolean has_row = FALSE;
    TRY(step_row(self, stmt, &has_row, "Listing MLS key package info"));
    if (!has_row)
      break;
    MarmotKeyPackageInfo **bigger = grow_array(infos, &cap, n, sizeof *infos);
    if (!bigger) {
      err = oom(self);
      goto out;
    }
    infos = bigger;
    TRY(key_package_from_row(self, stmt, &infos[n]));
    n++;
  }
out:
  sqlite3_finalize(stmt);
  if (err != MARMOT_OK) {
    for (size_t i = 0; i < n; i++)
      marmot_key_package_info_free(infos[i]);
    free(infos);
    return err;
  }
  *out = infos;
  *out_count = n;
  return MARMOT_OK;
}

static MarmotError
ghm_deactivate_key_packages(void *ctx, const uint8_t pubkey[32])
{
  GhStoreMarmot *self = ctx;
  if (!pubkey)
    return invalid(self, "A public key is required");
  MarmotError err = txn_begin(self);
  if (err != MARMOT_OK)
    return err;
  TRY(exec_keyed(self, "UPDATE mls_key_packages SET active = 0 "
                       "WHERE owner_pubkey = ?1 AND active <> 0",
                 pubkey, 32, "Deactivating MLS key packages"));
out:
  return txn_end(self, err);
}

/* ---- Relays ------------------------------------------------------------------------ */

static MarmotError
ghm_group_relays(void *ctx, const MarmotGroupId *group_id, MarmotGroupRelay **out_relays,
                 size_t *out_count)
{
  GhStoreMarmot *self = ctx;
  if (!out_relays || !out_count)
    return invalid(self, "group_relays needs its output arguments");
  *out_relays = NULL;
  *out_count = 0;
  MarmotError err = check_gid(self, group_id);
  if (err != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  MarmotGroupRelay *relays = NULL;
  size_t n = 0, cap = 0;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT relay_url FROM mls_group_relays WHERE mls_group_id = ?1 "
                    "ORDER BY position", &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  for (;;) {
    gboolean has_row = FALSE;
    TRY(step_row(self, stmt, &has_row, "Listing MLS group relays"));
    if (!has_row)
      break;
    MarmotGroupRelay *bigger = grow_array(relays, &cap, n, sizeof *relays);
    if (!bigger) {
      err = oom(self);
      goto out;
    }
    relays = bigger;
    memset(&relays[n], 0, sizeof relays[n]);
    n++;
    TRY(read_text(self, stmt, 0, &relays[n - 1].relay_url));
    relays[n - 1].mls_group_id = marmot_group_id_new(group_id->data, group_id->len);
    if (!relays[n - 1].relay_url || !relays[n - 1].mls_group_id.data) {
      err = relays[n - 1].relay_url ? oom(self) : corrupt(self, stmt, 0);
      goto out;
    }
  }
out:
  sqlite3_finalize(stmt);
  if (err != MARMOT_OK) {
    for (size_t i = 0; i < n; i++) {
      free(relays[i].relay_url);
      marmot_group_id_free(&relays[i].mls_group_id);
    }
    free(relays);
    return err;
  }
  *out_relays = relays;
  *out_count = n;
  return MARMOT_OK;
}

/* In list order; a repeated URL keeps its first position. */
static MarmotError
ghm_replace_group_relays(void *ctx, const MarmotGroupId *group_id, const char **relay_urls,
                         size_t count)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = MARMOT_OK;
  if ((err = check_gid(self, group_id)) != MARMOT_OK ||
      (err = check_relays(self, (const char *const *) relay_urls, count)) != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(exec_keyed(self, "DELETE FROM mls_group_relays WHERE mls_group_id = ?1",
                 group_id->data, group_id->len, "Replacing MLS group relays"));
  if (count > 0) {
    TRY(prepare(self, "INSERT OR IGNORE INTO mls_group_relays (mls_group_id, relay_url, "
                      "position) VALUES (?1, ?2, ?3)", &stmt));
    for (size_t i = 0; i < count; i++) {
      sqlite3_reset(stmt);
      BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
      BIND(bind_text(stmt, 2, relay_urls[i]));
      BIND(sqlite3_bind_int64(stmt, 3, (sqlite3_int64) i));
      TRY(step_done(self, stmt, "Saving an MLS group relay"));
    }
  }
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

/* ---- Exporter secrets -------------------------------------------------------------- */

static MarmotError
ghm_get_exporter_secret(void *ctx, const MarmotGroupId *group_id, uint64_t epoch,
                        uint8_t out_secret[32])
{
  GhStoreMarmot *self = ctx;
  if (!out_secret)
    return invalid(self, "get_exporter_secret needs an output argument");
  MarmotError err = check_gid(self, group_id);
  if (err != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT secret FROM mls_exporter_secrets "
                    "WHERE mls_group_id = ?1 AND epoch = ?2", &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  BIND(sqlite3_bind_int64(stmt, 2, (sqlite3_int64) epoch));
  TRY(step_row(self, stmt, &has_row, "Looking up an MLS exporter secret"));
  if (!has_row) {
    err = MARMOT_ERR_STORAGE_NOT_FOUND;
    goto out;
  }
  TRY(read_fixed(self, stmt, 0, out_secret, 32));
out:
  sqlite3_finalize(stmt);
  return err;
}

static MarmotError
ghm_save_exporter_secret(void *ctx, const MarmotGroupId *group_id, uint64_t epoch,
                         const uint8_t secret[32])
{
  GhStoreMarmot *self = ctx;
  if (!secret)
    return invalid(self, "An exporter secret is required");
  MarmotError err = check_gid(self, group_id);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self, "INSERT OR REPLACE INTO mls_exporter_secrets (mls_group_id, epoch, "
                    "secret) VALUES (?1, ?2, ?3)", &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  BIND(sqlite3_bind_int64(stmt, 2, (sqlite3_int64) epoch));
  BIND(bind_bytes(stmt, 3, secret, 32));
  TRY(step_done(self, stmt, "Saving an MLS exporter secret"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

/* A missing secret is not an error (SQLite and nostrdb backends). */
static MarmotError
ghm_delete_exporter_secret(void *ctx, const MarmotGroupId *group_id, uint64_t epoch)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = check_gid(self, group_id);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self, "DELETE FROM mls_exporter_secrets WHERE mls_group_id = ?1 AND epoch = ?2",
              &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  BIND(sqlite3_bind_int64(stmt, 2, (sqlite3_int64) epoch));
  TRY(step_done(self, stmt, "Deleting an MLS exporter secret"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

/* ---- Snapshots (row copies, in the caller's transaction) --------------------------
 * Each table below is copied and restored with plain INSERT ... SELECT, so
 * SQLite moves every value unchanged. Parameters: ?1 the group id, ?2 the
 * snapshot name. The scopes of copy and clear are identical, so a restore can
 * never collide with a row it did not remove. */

typedef struct {
  const char *copy;
  const char *clear;
  const char *restore;
} SnapshotTable;

#define SNAPSHOT_INSERT "INSERT INTO mls_snapshot_rows (group_id, name, tbl, "
#define SNAPSHOT_SOURCE " FROM mls_snapshot_rows WHERE group_id = ?1 AND name = ?2 AND tbl = "
#define MLS_STATE_SCOPE " WHERE label IN ('" MLS_GROUP_STATE_LABEL "', '" \
                        MLS_GROUP_PARENT_LABEL "', '" MLS_GROUP_PENDING_LABEL "', '" \
                        MLS_GROUP_WELCOMES_LABEL "', '" MLS_GROUP_PROPOSALS_LABEL "', '" \
                        MLS_GROUP_LEAVING_LABEL "') AND key = ?1"

static const SnapshotTable snapshot_tables[] = {
  { SNAPSHOT_INSERT "c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12) "
    "SELECT ?1, ?2, 1, " GROUP_COLUMNS " FROM mls_group_info WHERE mls_group_id = ?1",
    "DELETE FROM mls_group_info WHERE mls_group_id = ?1",
    "INSERT INTO mls_group_info (" GROUP_COLUMNS ") "
    "SELECT c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12" SNAPSHOT_SOURCE "1" },
  { SNAPSHOT_INSERT "c0, c1, c2) SELECT ?1, ?2, 2, mls_group_id, relay_url, position "
    "FROM mls_group_relays WHERE mls_group_id = ?1",
    "DELETE FROM mls_group_relays WHERE mls_group_id = ?1",
    "INSERT INTO mls_group_relays (mls_group_id, relay_url, position) "
    "SELECT c0, c1, c2" SNAPSHOT_SOURCE "2" },
  { SNAPSHOT_INSERT "c0, c1, c2) SELECT ?1, ?2, 3, mls_group_id, epoch, secret "
    "FROM mls_exporter_secrets WHERE mls_group_id = ?1",
    "DELETE FROM mls_exporter_secrets WHERE mls_group_id = ?1",
    "INSERT INTO mls_exporter_secrets (mls_group_id, epoch, secret) "
    "SELECT c0, c1, c2" SNAPSHOT_SOURCE "3" },
  { SNAPSHOT_INSERT "c0, c1, c2) SELECT ?1, ?2, 4, label, key, value FROM mls_kv"
    MLS_STATE_SCOPE,
    "DELETE FROM mls_kv" MLS_STATE_SCOPE,
    "INSERT INTO mls_kv (label, key, value) SELECT c0, c1, c2" SNAPSHOT_SOURCE "4" },
};

/* Runs @sql with ?1 = the group id and, where it has one, ?2 = @name. */
static MarmotError
exec_snapshot_sql(GhStoreMarmot *self, const char *sql, const MarmotGroupId *gid,
                  const char *name, const gchar *what)
{
  sqlite3_stmt *stmt = NULL;
  MarmotError err = MARMOT_OK;
  TRY(prepare(self, sql, &stmt));
  BIND(bind_bytes(stmt, 1, gid->data, gid->len));
  if (sqlite3_bind_parameter_count(stmt) >= 2)
    BIND(bind_text(stmt, 2, name));
  TRY(step_done(self, stmt, what));
out:
  sqlite3_finalize(stmt);
  return err;
}

static MarmotError
ghm_create_snapshot(void *ctx, const MarmotGroupId *group_id, const char *name)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = check_snapshot(self, group_id, name);
  if (err != MARMOT_OK)
    return err;
  const gint64 now = gh_clock_get_unix(gh_store_get_clock(self->store));
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  gboolean exists = FALSE;
  TRY(query_exists(self, "SELECT 1 FROM mls_group_info WHERE mls_group_id = ?1",
                   group_id->data, group_id->len, &exists, "Looking up an MLS group"));
  if (!exists) {
    err = MARMOT_ERR_GROUP_NOT_FOUND;
    goto out;
  }
  /* A snapshot of the same name is replaced (its rows go with the header). */
  TRY(exec_snapshot_sql(self, "DELETE FROM mls_snapshots WHERE group_id = ?1 AND name = ?2",
                        group_id, name, "Replacing an MLS snapshot"));
  TRY(prepare(self, "INSERT INTO mls_snapshots (group_id, name, created_at, data) "
                    "VALUES (?1, ?2, ?3, ?4)", &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  BIND(bind_text(stmt, 2, name));
  BIND(sqlite3_bind_int64(stmt, 3, now));
  BIND(bind_bytes(stmt, 4, &snapshot_format, sizeof snapshot_format));
  TRY(step_done(self, stmt, "Creating an MLS snapshot"));
  for (gsize i = 0; i < G_N_ELEMENTS(snapshot_tables); i++)
    TRY(exec_snapshot_sql(self, snapshot_tables[i].copy, group_id, name,
                          "Copying MLS state into a snapshot"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

static MarmotError
ghm_rollback_snapshot(void *ctx, const MarmotGroupId *group_id, const char *name)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = check_snapshot(self, group_id, name);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  gboolean has_row = FALSE;
  const void *format = NULL;
  TRY(prepare(self, "SELECT data FROM mls_snapshots WHERE group_id = ?1 AND name = ?2",
              &stmt));
  BIND(bind_bytes(stmt, 1, group_id->data, group_id->len));
  BIND(bind_text(stmt, 2, name));
  TRY(step_row(self, stmt, &has_row, "Looking up an MLS snapshot"));
  if (!has_row) {
    err = MARMOT_ERR_STORAGE_NOT_FOUND;
    goto out;
  }
  format = sqlite3_column_blob(stmt, 0);
  if (sqlite3_column_type(stmt, 0) != SQLITE_BLOB ||
      sqlite3_column_bytes(stmt, 0) != (int) sizeof snapshot_format || !format ||
      memcmp(format, &snapshot_format, sizeof snapshot_format) != 0) {
    err = fail(self, MARMOT_ERR_SNAPSHOT_FAILED, GH_STORE_ERROR_CORRUPT,
               "The MLS snapshot has an unknown format");
    goto out;
  }
  g_clear_pointer(&stmt, sqlite3_finalize);

  for (gsize i = 0; i < G_N_ELEMENTS(snapshot_tables); i++)
    TRY(exec_snapshot_sql(self, snapshot_tables[i].clear, group_id, name,
                          "Clearing MLS state for a rollback"));
  for (gsize i = 0; i < G_N_ELEMENTS(snapshot_tables); i++)
    TRY(exec_snapshot_sql(self, snapshot_tables[i].restore, group_id, name,
                          "Restoring MLS state from a snapshot"));
  TRY(exec_snapshot_sql(self, "DELETE FROM mls_snapshots WHERE group_id = ?1 AND name = ?2",
                        group_id, name, "Consuming an MLS snapshot"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

/* Releasing a snapshot that is gone already is not an error. */
static MarmotError
ghm_release_snapshot(void *ctx, const MarmotGroupId *group_id, const char *name)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = check_snapshot(self, group_id, name);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;
  TRY(exec_snapshot_sql(self, "DELETE FROM mls_snapshots WHERE group_id = ?1 AND name = ?2",
                        group_id, name, "Releasing an MLS snapshot"));
out:
  return txn_end(self, err);
}

static MarmotError
ghm_prune_expired_snapshots(void *ctx, uint64_t min_timestamp, size_t *out_pruned)
{
  GhStoreMarmot *self = ctx;
  if (out_pruned)
    *out_pruned = 0;
  MarmotError err = txn_begin(self);
  if (err != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  size_t pruned = 0;
  TRY(prepare(self, "DELETE FROM mls_snapshots WHERE created_at < ?1", &stmt));
  BIND(sqlite3_bind_int64(stmt, 1, min_timestamp > (uint64_t) G_MAXINT64
                                     ? G_MAXINT64 : (sqlite3_int64) min_timestamp));
  TRY(step_done(self, stmt, "Pruning MLS snapshots"));
  pruned = (size_t) sqlite3_changes(ghm_db(self));
out:
  sqlite3_finalize(stmt);
  err = txn_end(self, err);
  if (err == MARMOT_OK && out_pruned)
    *out_pruned = pruned;
  return err;
}

/* ---- MLS key store (mls_kv) -------------------------------------------------------- */

static MarmotError
ghm_mls_store(void *ctx, const char *label, const uint8_t *key, size_t key_len,
              const uint8_t *value, size_t value_len)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = MARMOT_OK;
  if ((err = check_label_key(self, label, key, key_len)) != MARMOT_OK ||
      (err = check_bytes(self, "An MLS storage value", value, value_len,
                         GH_STORE_MARMOT_MAX_VALUE)) != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  TRY(prepare(self, "INSERT OR REPLACE INTO mls_kv (label, key, value) VALUES (?1, ?2, ?3)",
              &stmt));
  BIND(bind_text(stmt, 1, label));
  BIND(bind_bytes(stmt, 2, key, key_len));
  BIND(bind_bytes(stmt, 3, value, value_len));
  TRY(step_done(self, stmt, "Storing MLS state"));
out:
  sqlite3_finalize(stmt);
  return txn_end(self, err);
}

static MarmotError
ghm_mls_load(void *ctx, const char *label, const uint8_t *key, size_t key_len,
             uint8_t **out_value, size_t *out_value_len)
{
  GhStoreMarmot *self = ctx;
  if (!out_value || !out_value_len)
    return invalid(self, "mls_load needs its output arguments");
  *out_value = NULL;
  *out_value_len = 0;
  MarmotError err = check_label_key(self, label, key, key_len);
  if (err != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  gboolean has_row = FALSE;
  TRY(check_readable(self));
  TRY(prepare(self, "SELECT value FROM mls_kv WHERE label = ?1 AND key = ?2", &stmt));
  BIND(bind_text(stmt, 1, label));
  BIND(bind_bytes(stmt, 2, key, key_len));
  TRY(step_row(self, stmt, &has_row, "Loading MLS state"));
  if (!has_row) {
    err = MARMOT_ERR_STORAGE_NOT_FOUND;
    goto out;
  }
  TRY(read_blob(self, stmt, 0, out_value, out_value_len));
out:
  sqlite3_finalize(stmt);
  return err;
}

static MarmotError
ghm_mls_delete(void *ctx, const char *label, const uint8_t *key, size_t key_len)
{
  GhStoreMarmot *self = ctx;
  MarmotError err = check_label_key(self, label, key, key_len);
  if (err != MARMOT_OK)
    return err;
  if ((err = txn_begin(self)) != MARMOT_OK)
    return err;

  sqlite3_stmt *stmt = NULL;
  gboolean deleted = FALSE;
  TRY(prepare(self, "DELETE FROM mls_kv WHERE label = ?1 AND key = ?2", &stmt));
  BIND(bind_text(stmt, 1, label));
  BIND(bind_bytes(stmt, 2, key, key_len));
  TRY(step_done(self, stmt, "Deleting MLS state"));
  deleted = sqlite3_changes(ghm_db(self)) > 0;
out:
  sqlite3_finalize(stmt);
  err = txn_end(self, err);
  if (err == MARMOT_OK && !deleted)
    return MARMOT_ERR_STORAGE_NOT_FOUND;
  return err;
}

/* ---- libmarmot operation transactions (nostrc-qp24.7) --------------------------------
 * libmarmot (>= 0.7.0) brackets every operation that writes with these, so
 * all of its records -- an epoch transition's exporter secret, retained
 * parent, MLS state and group record, a pending Commit, its merge and the
 * Welcome outbox, a message with its ratchet step and processed marker --
 * commit as one SQLCipher transaction or not at all. Inside a caller's
 * transaction (T-mls) the operation is a savepoint of it. */

static MarmotError
ghm_begin(void *ctx)
{
  GhStoreMarmot *self = ctx;
  GError *error = NULL;
  if (gh_store_begin_named(self->store, "mls", &error))
    return MARMOT_OK;
  return fail_store(self, error);
}

/* A failed commit has rolled everything back (gh_store_commit()). */
static MarmotError
ghm_commit(void *ctx)
{
  GhStoreMarmot *self = ctx;
  GError *error = NULL;
  if (gh_store_commit(self->store, &error))
    return MARMOT_OK;
  return fail_store(self, error);
}

static void
ghm_rollback(void *ctx)
{
  GhStoreMarmot *self = ctx;
  gh_store_rollback(self->store);
}

/* ---- Lifecycle ----------------------------------------------------------------------- */

/* Only durable stores are accepted (gh_store_marmot_new()). */
static bool
ghm_is_persistent(void *ctx)
{
  (void) ctx;
  return true;
}

/* The store is borrowed: destroying the storage never closes it. */
static void
ghm_destroy(void *ctx)
{
  GhStoreMarmot *self = ctx;
  if (!self)
    return;
  g_clear_error(&self->error);
  g_free(self);
}

MarmotStorage *
gh_store_marmot_new(GhStore *store, GError **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(error == NULL || *error == NULL, NULL);
  if (gh_store_is_ephemeral(store)) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE,
                        "MLS needs a durable store; it is disabled without one");
    return NULL;
  }
  /* marmot_storage_free() releases the struct with free(). */
  MarmotStorage *storage = calloc(1, sizeof *storage);
  if (!storage) {
    g_set_error_literal(error, GH_STORE_ERROR, GH_STORE_ERROR_FAILED, "Out of memory");
    return NULL;
  }
  GhStoreMarmot *self = g_new0(GhStoreMarmot, 1);
  self->store = store;
  storage->ctx = self;

  storage->all_groups = ghm_all_groups;
  storage->find_group_by_mls_id = ghm_find_group_by_mls_id;
  storage->find_group_by_nostr_id = ghm_find_group_by_nostr_id;
  storage->save_group = ghm_save_group;
  storage->delete_group = ghm_delete_group;
  storage->messages = ghm_messages;
  storage->last_message = ghm_last_message;

  storage->save_message = ghm_save_message;
  storage->find_message_by_id = ghm_find_message_by_id;
  storage->is_message_processed = ghm_is_message_processed;
  storage->save_processed_message = ghm_save_processed_message;

  storage->save_welcome = ghm_save_welcome;
  storage->find_welcome_by_event_id = ghm_find_welcome_by_event_id;
  storage->pending_welcomes = ghm_pending_welcomes;
  storage->find_processed_welcome = ghm_find_processed_welcome;
  storage->save_processed_welcome = ghm_save_processed_welcome;

  storage->save_key_package_info = ghm_save_key_package_info;
  storage->find_key_package_by_ref = ghm_find_key_package_by_ref;
  storage->find_key_packages_by_pubkey = ghm_find_key_packages_by_pubkey;
  storage->deactivate_key_packages = ghm_deactivate_key_packages;

  storage->group_relays = ghm_group_relays;
  storage->replace_group_relays = ghm_replace_group_relays;

  storage->get_exporter_secret = ghm_get_exporter_secret;
  storage->save_exporter_secret = ghm_save_exporter_secret;
  storage->delete_exporter_secret = ghm_delete_exporter_secret;

  storage->create_snapshot = ghm_create_snapshot;
  storage->rollback_snapshot = ghm_rollback_snapshot;
  storage->release_snapshot = ghm_release_snapshot;
  storage->prune_expired_snapshots = ghm_prune_expired_snapshots;

  storage->mls_store = ghm_mls_store;
  storage->mls_load = ghm_mls_load;
  storage->mls_delete = ghm_mls_delete;

  storage->is_persistent = ghm_is_persistent;
  storage->destroy = ghm_destroy;

  storage->begin = ghm_begin;
  storage->commit = ghm_commit;
  storage->rollback = ghm_rollback;
  return storage;
}

GError *
gh_store_marmot_take_error(MarmotStorage *storage)
{
  g_return_val_if_fail(storage != NULL, NULL);
  g_return_val_if_fail(storage->destroy == ghm_destroy, NULL);
  GhStoreMarmot *self = storage->ctx;
  return g_steal_pointer(&self->error);
}
