#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* memmem */
#endif

#include "gh-store-marmot.h"
#include "gh-mls-commits.h"

#include "canary-scan.h"
#include "crash-harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <glib/gstdio.h>
#include <marmot/marmot.h>
#include <secp256k1.h>
#include <secp256k1_extrakeys.h>
#include <sodium.h>
#include <sqlite3.h>

#ifndef GH_STORE_TEST_HOOKS
#error "test_store_marmot needs the store's test hooks (GH_STORE_TEST_HOOKS)"
#endif

/* G23 (privacy charter §3.9, §8.2): GhStoreMarmot, libmarmot's MarmotStorage
 * over the encrypted GhStore. libmarmot's storage contract cases, exact
 * round trips and bounds, snapshots (rollback restores byte-identical mls_kv),
 * T-mls atomicity under the H8 crash harness, the v2 migration, and a real
 * libmarmot group that survives store reopens. Every test runs in isolated
 * XDG directories; nothing sleeps (crash tests SIGKILL at named cut points,
 * snapshot times come from a fake GhClock). */

#define ACCOUNT_A "7e7e9c42a91bfef19fa929e5fda1b72e0ebc1a4c1141673e2794234d86addf4e"
#define T0        ((gint64) 1790000000)
#define RELAY_ONE "wss://relay-one.invalid"
#define RELAY_TWO "wss://relay-two.invalid"

#ifdef SECP256K1_CONTEXT_NONE
#define TEST_SECP256K1_FLAGS SECP256K1_CONTEXT_NONE
#else
#define TEST_SECP256K1_FLAGS SECP256K1_CONTEXT_SIGN
#endif

#define assert_marmot_ok(expr)                                                  \
  G_STMT_START {                                                                \
    MarmotError assert_err_ = (expr);                                           \
    if (assert_err_ != MARMOT_OK)                                               \
      g_error("%s failed: %s", #expr, marmot_error_string(assert_err_));        \
  } G_STMT_END

/* ---- Accounts and stores ------------------------------------------------------------ */

typedef struct {
  gchar *pubkey;   /* the store's account (64 lowercase hex) */
  GBytes *key;     /* the raw store key (a Secret Service item in the app) */
  gchar *store_id; /* the key item's store id */
} TestAccount;

static void
test_account_init(TestAccount *account, const gchar *pubkey)
{
  guint8 raw[GH_STORE_KEY_SIZE];
  randombytes_buf(raw, sizeof raw);
  account->pubkey = g_strdup(pubkey);
  account->key = g_bytes_new(raw, sizeof raw);
  account->store_id = g_uuid_string_random();
  sodium_memzero(raw, sizeof raw);
}

static void
test_account_clear(TestAccount *account)
{
  g_clear_pointer(&account->pubkey, g_free);
  g_clear_pointer(&account->key, g_bytes_unref);
  g_clear_pointer(&account->store_id, g_free);
}

static GhStore *
store_open_flags(const TestAccount *account, GhClock *clock, GhStoreOpenFlags flags)
{
  GhStoreConfig config = { .account_pubkey = account->pubkey, .clock = clock };
  g_autoptr(GError) error = NULL;
  GhStore *store = gh_store_open_with_key(&config, account->key, account->store_id,
                                          flags, &error);
  g_assert_no_error(error);
  g_assert_nonnull(store);
  return store;
}

static GhStore *
store_open(const TestAccount *account, GhClock *clock)
{
  return store_open_flags(account, clock, GH_STORE_OPEN_CREATE);
}

static MarmotStorage *
storage_new(GhStore *store)
{
  g_autoptr(GError) error = NULL;
  MarmotStorage *storage = gh_store_marmot_new(store, &error);
  g_assert_no_error(error);
  g_assert_nonnull(storage);
  return storage;
}

static void
assert_recorded(MarmotStorage *s, gint code)
{
  g_autoptr(GError) error = gh_store_marmot_take_error(s);
  g_assert_error(error, GH_STORE_ERROR, code);
}

static void
assert_nothing_recorded(MarmotStorage *s)
{
  g_autoptr(GError) error = gh_store_marmot_take_error(s);
  g_assert_no_error(error);
}

/* ---- SQL inspection --------------------------------------------------------------------- */

static gint64
sql_int(GhStore *store, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  g_assert_cmpint(sqlite3_step(stmt), ==, SQLITE_ROW);
  gint64 value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

static gchar *
sql_text(GhStore *store, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  int rc = sqlite3_step(stmt);
  g_assert_true(rc == SQLITE_ROW || rc == SQLITE_DONE);
  gchar *text = rc == SQLITE_ROW && sqlite3_column_text(stmt, 0)
                  ? g_strdup((const gchar *) sqlite3_column_text(stmt, 0)) : NULL;
  sqlite3_finalize(stmt);
  return text;
}

static void
sql_exec(GhStore *store, const gchar *sql)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_exec(store, sql, &error));
  g_assert_no_error(error);
}

static void
append_hex(GString *out, const guint8 *data, gsize len)
{
  static const gchar digits[] = "0123456789abcdef";
  for (gsize i = 0; i < len; i++) {
    g_string_append_c(out, digits[data[i] >> 4]);
    g_string_append_c(out, digits[data[i] & 0x0f]);
  }
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *) a, *(const gchar *const *) b);
}

/* Every row of @sql with each value's storage class and exact bytes, sorted:
 * equal dumps mean byte-identical contents. */
static gchar *
sql_dump(GhStore *store, const gchar *sql)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  g_autoptr(GPtrArray) rows = g_ptr_array_new_with_free_func(g_free);
  int rc;
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    GString *row = g_string_new(NULL);
    for (int i = 0; i < sqlite3_column_count(stmt); i++) {
      int type = sqlite3_column_type(stmt, i);
      g_string_append_printf(row, "%s%d:", i ? "," : "", type);
      if (type == SQLITE_INTEGER) {
        g_string_append_printf(row, "%" G_GINT64_FORMAT, (gint64) sqlite3_column_int64(stmt, i));
      } else if (type == SQLITE_BLOB) {
        const void *data = sqlite3_column_blob(stmt, i);
        append_hex(row, data, (gsize) sqlite3_column_bytes(stmt, i));
      } else if (type == SQLITE_TEXT) {
        const unsigned char *text = sqlite3_column_text(stmt, i);
        append_hex(row, text, (gsize) sqlite3_column_bytes(stmt, i));
      }
    }
    g_ptr_array_add(rows, g_string_free(row, FALSE));
  }
  g_assert_cmpint(rc, ==, SQLITE_DONE);
  sqlite3_finalize(stmt);
  g_ptr_array_sort(rows, compare_strings);
  g_ptr_array_add(rows, NULL);
  return g_strjoinv(";", (gchar **) rows->pdata);
}

/* All tables whose names match @like, table by table. */
static gchar *
db_dump(GhStore *store, const gchar *like)
{
  g_autofree gchar *list_sql = g_strdup_printf(
    "SELECT group_concat(name, ',') FROM (SELECT name FROM sqlite_master "
    "WHERE type = 'table' AND name LIKE '%s' AND name NOT LIKE 'sqlite_%%' ORDER BY name)",
    like);
  g_autofree gchar *names = sql_text(store, list_sql);
  g_assert_nonnull(names);
  g_auto(GStrv) tables = g_strsplit(names, ",", -1);
  GString *out = g_string_new(NULL);
  for (guint i = 0; tables[i]; i++) {
    g_autofree gchar *sql = g_strdup_printf("SELECT * FROM \"%s\"", tables[i]);
    g_autofree gchar *rows = sql_dump(store, sql);
    g_string_append_printf(out, "%s{%s}\n", tables[i], rows);
  }
  return g_string_free(out, FALSE);
}

/* What a group snapshot covers, for every group. */
static gchar *
group_state_dump(GhStore *store)
{
  g_autofree gchar *kv = sql_dump(store, "SELECT * FROM mls_kv");
  g_autofree gchar *info = sql_dump(store, "SELECT * FROM mls_group_info");
  g_autofree gchar *relays = sql_dump(store, "SELECT * FROM mls_group_relays");
  g_autofree gchar *secrets = sql_dump(store, "SELECT * FROM mls_exporter_secrets");
  return g_strdup_printf("kv{%s}\ninfo{%s}\nrelays{%s}\nsecrets{%s}\n", kv, info, relays,
                         secrets);
}

static gchar *
kv_dump(GhStore *store)
{
  return sql_dump(store, "SELECT * FROM mls_kv");
}

static void
assert_integrity(GhStore *store)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_check_integrity(store, TRUE, &error));
  g_assert_no_error(error);
}

/* ---- Records built the way libmarmot builds them (malloc(), it free()s) ------------ */

static char *
c_strfill(gsize len, char c)
{
  char *s = malloc(len + 1);
  g_assert_nonnull(s);
  memset(s, c, len);
  s[len] = '\0';
  return s;
}

static uint8_t *
c_bytes(gsize len, guint8 seed)
{
  uint8_t *b = malloc(len);
  g_assert_nonnull(b);
  for (gsize i = 0; i < len; i++)
    b[i] = (uint8_t) (seed + i * 31);
  return b;
}

static MarmotGroupId
gid_of(const gchar *s)
{
  return marmot_group_id_new((const uint8_t *) s, strlen(s));
}

static gchar *
hex32(const uint8_t bytes[32])
{
  GString *out = g_string_new(NULL);
  append_hex(out, bytes, 32);
  return g_string_free(out, FALSE);
}

/* ---- libmarmot storage contract (libmarmot/tests/test_storage_contract.c) ---------------
 * A faithful port: the same cases, in the same order, with the same data and
 * assertions, run against one storage instance as libmarmot's
 * run_contract_tests() runs them against its memory, SQLite and nostrdb
 * backends. That file cannot be linked (static cases beside its own main())
 * and its snapshot case asserts MARMOT_ERR_UNSUPPORTED, because no built-in
 * backend implements rollback; GhStoreMarmot does, so that single case checks
 * the real lifecycle with the same group and names (the snapshot tests below
 * go further). */

static MarmotGroup *
make_test_group(const uint8_t *gid_data, size_t gid_len, const char *name, uint64_t epoch)
{
  MarmotGroup *g = marmot_group_new();
  g_assert_nonnull(g);
  g->mls_group_id = marmot_group_id_new(gid_data, gid_len);
  memset(g->nostr_group_id, 0xBB, 32);
  g->name = strdup(name);
  g->description = strdup("test group description");
  g->state = MARMOT_GROUP_STATE_ACTIVE;
  g->epoch = epoch;
  return g;
}

static MarmotMessage *
make_test_message(const MarmotGroupId *gid, int index, int64_t created_at)
{
  MarmotMessage *m = marmot_message_new();
  g_assert_nonnull(m);
  memset(m->id, (uint8_t) index, 32);
  memset(m->pubkey, 0x11, 32);
  m->kind = MARMOT_KIND_GROUP_MESSAGE;
  m->mls_group_id = marmot_group_id_new(gid->data, gid->len);
  m->created_at = created_at;
  m->processed_at = created_at + 1;
  m->content = malloc(64);
  snprintf(m->content, 64, "Message #%d", index);
  m->epoch = 1;
  m->state = MARMOT_MSG_STATE_CREATED;
  memset(m->wrapper_event_id, (uint8_t) (index + 0x80), 32);
  return m;
}

static MarmotWelcome *
make_test_welcome(const MarmotGroupId *gid, int index)
{
  MarmotWelcome *w = marmot_welcome_new();
  g_assert_nonnull(w);
  memset(w->id, (uint8_t) (index + 0x50), 32);
  w->mls_group_id = marmot_group_id_new(gid->data, gid->len);
  memset(w->nostr_group_id, 0xCC, 32);
  w->group_name = strdup("Welcome Group");
  w->group_description = strdup("Welcome desc");
  w->state = MARMOT_WELCOME_STATE_PENDING;
  w->member_count = 5;
  memset(w->welcomer, 0x22, 32);
  memset(w->wrapper_event_id, (uint8_t) (index + 0xA0), 32);
  w->event_json = strdup("{\"kind\":444}");
  return w;
}

/* The contract's helper: the SQLite backend needs the group for FKs. */
static void
ensure_group(MarmotStorage *s, const MarmotGroupId *gid, const char *name)
{
  MarmotGroup *g = make_test_group(gid->data, gid->len, name, 1);
  s->save_group(s->ctx, g); /* ignore error if already exists */
  marmot_group_free(g);
}

static void
contract_group_save_and_find_by_mls_id(MarmotStorage *s)
{
  uint8_t gid[] = { 10, 20, 30, 40 };
  MarmotGroup *g = make_test_group(gid, sizeof(gid), "Alpha", 7);
  g_assert_cmpint(s->save_group(s->ctx, g), ==, MARMOT_OK);

  MarmotGroup *found = NULL;
  g_assert_cmpint(s->find_group_by_mls_id(s->ctx, &g->mls_group_id, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_cmpstr(found->name, ==, "Alpha");
  g_assert_cmpuint(found->epoch, ==, 7);
  g_assert_cmpint(found->state, ==, MARMOT_GROUP_STATE_ACTIVE);
  marmot_group_free(found);
  marmot_group_free(g);
}

static void
contract_group_find_by_nostr_id(MarmotStorage *s)
{
  uint8_t gid[] = { 11, 21, 31 };
  MarmotGroup *g = make_test_group(gid, sizeof(gid), "Beta", 3);
  memset(g->nostr_group_id, 0xDD, 32);
  g_assert_cmpint(s->save_group(s->ctx, g), ==, MARMOT_OK);

  MarmotGroup *found = NULL;
  g_assert_cmpint(s->find_group_by_nostr_id(s->ctx, g->nostr_group_id, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_cmpstr(found->name, ==, "Beta");
  marmot_group_free(found);
  marmot_group_free(g);
}

static void
contract_group_not_found(MarmotStorage *s)
{
  MarmotGroupId bad = marmot_group_id_new((uint8_t *) "nonexistent!!", 13);
  MarmotGroup *found = NULL;
  MarmotError err = s->find_group_by_mls_id(s->ctx, &bad, &found);
  g_assert_cmpint(err, ==, MARMOT_OK);
  g_assert_null(found);
  marmot_group_id_free(&bad);
}

static void
contract_group_upsert(MarmotStorage *s)
{
  uint8_t gid[] = { 50, 60, 70 };
  MarmotGroup *g = make_test_group(gid, sizeof(gid), "Original", 1);
  g_assert_cmpint(s->save_group(s->ctx, g), ==, MARMOT_OK);

  free(g->name);
  g->name = strdup("Updated");
  g->epoch = 99;
  g_assert_cmpint(s->save_group(s->ctx, g), ==, MARMOT_OK);

  MarmotGroup *found = NULL;
  g_assert_cmpint(s->find_group_by_mls_id(s->ctx, &g->mls_group_id, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_cmpstr(found->name, ==, "Updated");
  g_assert_cmpuint(found->epoch, ==, 99);
  marmot_group_free(found);
  marmot_group_free(g);
}

static void
contract_group_list_all(MarmotStorage *s)
{
  uint8_t gid1[] = { 1, 1, 1 };
  uint8_t gid2[] = { 2, 2, 2 };
  MarmotGroup *g1 = make_test_group(gid1, sizeof(gid1), "One", 1);
  MarmotGroup *g2 = make_test_group(gid2, sizeof(gid2), "Two", 2);
  g_assert_cmpint(s->save_group(s->ctx, g1), ==, MARMOT_OK);
  g_assert_cmpint(s->save_group(s->ctx, g2), ==, MARMOT_OK);

  MarmotGroup **groups = NULL;
  size_t count = 0;
  g_assert_cmpint(s->all_groups(s->ctx, &groups, &count), ==, MARMOT_OK);
  /* At least 2 (more from prior cases on the same instance). */
  g_assert_cmpuint(count, >=, 2);
  for (size_t i = 0; i < count; i++)
    marmot_group_free(groups[i]);
  free(groups);
  marmot_group_free(g1);
  marmot_group_free(g2);
}

static void
contract_group_delete_removes_group_and_relays(MarmotStorage *s)
{
  uint8_t gid_data[] = { 9, 8, 7, 6 };
  MarmotGroup *g = make_test_group(gid_data, sizeof(gid_data), "DeleteMe", 1);
  g_assert_cmpint(s->save_group(s->ctx, g), ==, MARMOT_OK);

  const char *urls[] = { "wss://delete.example.com" };
  g_assert_cmpint(s->replace_group_relays(s->ctx, &g->mls_group_id, urls, 1), ==, MARMOT_OK);
  g_assert_cmpint(s->delete_group(s->ctx, &g->mls_group_id), ==, MARMOT_OK);

  MarmotGroup *found = (MarmotGroup *) 0x1;
  g_assert_cmpint(s->find_group_by_mls_id(s->ctx, &g->mls_group_id, &found), ==, MARMOT_OK);
  g_assert_null(found);

  MarmotGroupRelay *relays = NULL;
  size_t relay_count = 99;
  g_assert_cmpint(s->group_relays(s->ctx, &g->mls_group_id, &relays, &relay_count), ==, MARMOT_OK);
  g_assert_cmpuint(relay_count, ==, 0);
  g_assert_true(relays == NULL || relay_count == 0);
  free(relays);
  marmot_group_free(g);
}

static void
contract_message_save_and_find(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "msg_grp", 7);
  ensure_group(s, &gid, "msg_grp");
  MarmotMessage *m = make_test_message(&gid, 1, 1000);
  g_assert_cmpint(s->save_message(s->ctx, m), ==, MARMOT_OK);

  MarmotMessage *found = NULL;
  g_assert_cmpint(s->find_message_by_id(s->ctx, m->id, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_cmpstr(found->content, ==, "Message #1");
  g_assert_cmpint(found->created_at, ==, 1000);
  marmot_message_free(found);
  marmot_message_free(m);
  marmot_group_id_free(&gid);
}

static void
contract_message_pagination(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "page_grp", 8);
  ensure_group(s, &gid, "page_grp");
  for (int i = 0; i < 10; i++) {
    MarmotMessage *m = make_test_message(&gid, 100 + i, 2000 + i);
    g_assert_cmpint(s->save_message(s->ctx, m), ==, MARMOT_OK);
    marmot_message_free(m);
  }

  MarmotPagination pg = marmot_pagination_default();
  MarmotMessage **msgs = NULL;
  size_t count = 0;
  g_assert_cmpint(s->messages(s->ctx, &gid, &pg, &msgs, &count), ==, MARMOT_OK);
  g_assert_cmpuint(count, ==, 10);
  for (size_t i = 0; i < count; i++)
    marmot_message_free(msgs[i]);
  free(msgs);

  pg.limit = 3;
  pg.offset = 2;
  g_assert_cmpint(s->messages(s->ctx, &gid, &pg, &msgs, &count), ==, MARMOT_OK);
  g_assert_cmpuint(count, ==, 3);
  for (size_t i = 0; i < count; i++)
    marmot_message_free(msgs[i]);
  free(msgs);
  marmot_group_id_free(&gid);
}

static void
contract_message_last(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "last_grp", 8);
  ensure_group(s, &gid, "last_grp");
  for (int i = 0; i < 5; i++) {
    MarmotMessage *m = make_test_message(&gid, 200 + i, 3000 + i);
    g_assert_cmpint(s->save_message(s->ctx, m), ==, MARMOT_OK);
    marmot_message_free(m);
  }

  MarmotMessage *last = NULL;
  g_assert_cmpint(s->last_message(s->ctx, &gid, MARMOT_SORT_CREATED_AT_FIRST, &last), ==, MARMOT_OK);
  g_assert_nonnull(last);
  g_assert_cmpint(last->created_at, ==, 3004);
  marmot_message_free(last);
  marmot_group_id_free(&gid);
}

static void
contract_message_processed_tracking(MarmotStorage *s)
{
  uint8_t wrapper_id[32];
  memset(wrapper_id, 0xF1, 32);

  bool processed = true;
  g_assert_cmpint(s->is_message_processed(s->ctx, wrapper_id, &processed), ==, MARMOT_OK);
  g_assert_false(processed);

  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "proc_grp", 8);
  uint8_t msg_id[32];
  memset(msg_id, 0xF2, 32);
  g_assert_cmpint(s->save_processed_message(s->ctx, wrapper_id, msg_id, 1234567890, 5, &gid,
                                            1 /* PROCESSED */, NULL), ==, MARMOT_OK);
  g_assert_cmpint(s->is_message_processed(s->ctx, wrapper_id, &processed), ==, MARMOT_OK);
  g_assert_true(processed);
  marmot_group_id_free(&gid);
}

static void
contract_welcome_save_and_find(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "wel_grp", 7);
  MarmotWelcome *w = make_test_welcome(&gid, 1);
  g_assert_cmpint(s->save_welcome(s->ctx, w), ==, MARMOT_OK);

  MarmotWelcome *found = NULL;
  g_assert_cmpint(s->find_welcome_by_event_id(s->ctx, w->id, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_cmpstr(found->group_name, ==, "Welcome Group");
  g_assert_cmpuint(found->member_count, ==, 5);
  g_assert_cmpint(found->state, ==, MARMOT_WELCOME_STATE_PENDING);
  marmot_welcome_free(found);
  marmot_welcome_free(w);
  marmot_group_id_free(&gid);
}

static void
contract_welcome_pending(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "pend_grp", 8);
  for (int i = 0; i < 3; i++) {
    MarmotWelcome *w = make_test_welcome(&gid, 30 + i);
    g_assert_cmpint(s->save_welcome(s->ctx, w), ==, MARMOT_OK);
    marmot_welcome_free(w);
  }

  MarmotPagination pg = marmot_pagination_default();
  MarmotWelcome **welcomes = NULL;
  size_t count = 0;
  g_assert_cmpint(s->pending_welcomes(s->ctx, &pg, &welcomes, &count), ==, MARMOT_OK);
  g_assert_cmpuint(count, >=, 3);
  for (size_t i = 0; i < count; i++)
    marmot_welcome_free(welcomes[i]);
  free(welcomes);
  marmot_group_id_free(&gid);
}

static void
contract_welcome_processed_tracking(MarmotStorage *s)
{
  uint8_t wrapper_id[32];
  memset(wrapper_id, 0xE1, 32);

  bool found = true;
  int state = 0;
  char *reason = NULL;
  g_assert_cmpint(s->find_processed_welcome(s->ctx, wrapper_id, &found, &state, &reason), ==,
                  MARMOT_OK);
  g_assert_false(found);

  g_assert_cmpint(s->save_processed_welcome(s->ctx, wrapper_id, NULL, 1234567890,
                                            1 /* ACCEPTED */, NULL), ==, MARMOT_OK);
  g_assert_cmpint(s->find_processed_welcome(s->ctx, wrapper_id, &found, &state, &reason), ==,
                  MARMOT_OK);
  g_assert_true(found);
  g_assert_cmpint(state, ==, 1);
  free(reason);
}

static void
contract_mls_store_roundtrip(MarmotStorage *s)
{
  uint8_t key[] = { 0xAA, 0xBB, 0xCC };
  uint8_t value[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x01, 0x02 };
  g_assert_cmpint(s->mls_store(s->ctx, "key_package", key, sizeof(key), value, sizeof(value)), ==,
                  MARMOT_OK);

  uint8_t *out = NULL;
  size_t out_len = 0;
  g_assert_cmpint(s->mls_load(s->ctx, "key_package", key, sizeof(key), &out, &out_len), ==,
                  MARMOT_OK);
  g_assert_cmpmem(out, out_len, value, sizeof(value));
  free(out);
}

static void
contract_mls_store_not_found(MarmotStorage *s)
{
  uint8_t key[] = { 0xFF, 0xFE, 0xFD, 0xFC };
  uint8_t *out = NULL;
  size_t out_len = 0;
  MarmotError err = s->mls_load(s->ctx, "nonexistent_label", key, sizeof(key), &out, &out_len);
  g_assert_cmpint(err, ==, MARMOT_ERR_STORAGE_NOT_FOUND);
  g_assert_null(out);
}

static void
contract_mls_store_upsert(MarmotStorage *s)
{
  uint8_t key[] = { 0x01, 0x02 };
  uint8_t v1[] = { 0x10 };
  uint8_t v2[] = { 0x20, 0x30 };
  g_assert_cmpint(s->mls_store(s->ctx, "epoch_key", key, sizeof(key), v1, sizeof(v1)), ==, MARMOT_OK);
  g_assert_cmpint(s->mls_store(s->ctx, "epoch_key", key, sizeof(key), v2, sizeof(v2)), ==, MARMOT_OK);

  uint8_t *out = NULL;
  size_t out_len = 0;
  g_assert_cmpint(s->mls_load(s->ctx, "epoch_key", key, sizeof(key), &out, &out_len), ==, MARMOT_OK);
  g_assert_cmpuint(out_len, ==, 2);
  g_assert_true(out[0] == 0x20 && out[1] == 0x30);
  free(out);
}

static void
contract_mls_store_delete(MarmotStorage *s)
{
  uint8_t key[] = { 0xD0, 0xD1 };
  uint8_t val[] = { 0x42 };
  g_assert_cmpint(s->mls_store(s->ctx, "deleteme", key, sizeof(key), val, sizeof(val)), ==, MARMOT_OK);
  g_assert_cmpint(s->mls_delete(s->ctx, "deleteme", key, sizeof(key)), ==, MARMOT_OK);

  uint8_t *out = NULL;
  size_t out_len = 0;
  g_assert_cmpint(s->mls_load(s->ctx, "deleteme", key, sizeof(key), &out, &out_len), ==,
                  MARMOT_ERR_STORAGE_NOT_FOUND);
}

static void
contract_mls_store_label_isolation(MarmotStorage *s)
{
  uint8_t key[] = { 0xAB };
  uint8_t v1[] = { 0x01 };
  uint8_t v2[] = { 0x02 };
  g_assert_cmpint(s->mls_store(s->ctx, "label_a", key, sizeof(key), v1, sizeof(v1)), ==, MARMOT_OK);
  g_assert_cmpint(s->mls_store(s->ctx, "label_b", key, sizeof(key), v2, sizeof(v2)), ==, MARMOT_OK);

  uint8_t *out = NULL;
  size_t out_len = 0;
  g_assert_cmpint(s->mls_load(s->ctx, "label_a", key, sizeof(key), &out, &out_len), ==, MARMOT_OK);
  g_assert_true(out_len == 1 && out[0] == 0x01);
  free(out);
  g_assert_cmpint(s->mls_load(s->ctx, "label_b", key, sizeof(key), &out, &out_len), ==, MARMOT_OK);
  g_assert_true(out_len == 1 && out[0] == 0x02);
  free(out);
}

static void
contract_exporter_secret_roundtrip(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "exp_grp", 7);
  uint8_t secret[32];
  memset(secret, 0x77, 32);
  g_assert_cmpint(s->save_exporter_secret(s->ctx, &gid, 5, secret), ==, MARMOT_OK);

  uint8_t out[32];
  g_assert_cmpint(s->get_exporter_secret(s->ctx, &gid, 5, out), ==, MARMOT_OK);
  g_assert_cmpmem(out, 32, secret, 32);
  /* Wrong epoch: not found. */
  g_assert_cmpint(s->get_exporter_secret(s->ctx, &gid, 999, out), ==, MARMOT_ERR_STORAGE_NOT_FOUND);
  marmot_group_id_free(&gid);
}

static void
contract_exporter_secret_overwrite(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "overwrite_grp", 13);
  uint8_t s1[32], s2[32];
  memset(s1, 0xAA, 32);
  memset(s2, 0xBB, 32);
  g_assert_cmpint(s->save_exporter_secret(s->ctx, &gid, 10, s1), ==, MARMOT_OK);
  g_assert_cmpint(s->save_exporter_secret(s->ctx, &gid, 10, s2), ==, MARMOT_OK);

  uint8_t out[32];
  g_assert_cmpint(s->get_exporter_secret(s->ctx, &gid, 10, out), ==, MARMOT_OK);
  g_assert_cmpmem(out, 32, s2, 32);
  marmot_group_id_free(&gid);
}

static void
contract_exporter_secret_delete(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "delete_exp_grp", 14);
  uint8_t secret[32];
  memset(secret, 0x55, 32);
  g_assert_cmpint(s->save_exporter_secret(s->ctx, &gid, 12, secret), ==, MARMOT_OK);
  g_assert_cmpint(s->delete_exporter_secret(s->ctx, &gid, 12), ==, MARMOT_OK);

  uint8_t out[32];
  g_assert_cmpint(s->get_exporter_secret(s->ctx, &gid, 12, out), ==, MARMOT_ERR_STORAGE_NOT_FOUND);
  marmot_group_id_free(&gid);
}

static void
contract_key_package_info_roundtrip(MarmotStorage *s)
{
  MarmotKeyPackageInfo info;
  memset(&info, 0, sizeof(info));
  memset(info.ref, 0xA1, 32);
  memset(info.owner_pubkey, 0xB2, 32);
  info.created_at = 1234567890;
  info.active = true;
  const char *urls[] = { "wss://relay1.example.com", "wss://relay2.example.com" };
  info.relay_urls = (char **) urls;
  info.relay_count = 2;
  g_assert_cmpint(s->save_key_package_info(s->ctx, &info), ==, MARMOT_OK);

  MarmotKeyPackageInfo *found = NULL;
  g_assert_cmpint(s->find_key_package_by_ref(s->ctx, info.ref, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_cmpmem(found->ref, 32, info.ref, 32);
  g_assert_cmpmem(found->owner_pubkey, 32, info.owner_pubkey, 32);
  g_assert_cmpuint(found->relay_count, ==, 2);
  g_assert_true(found->active);
  g_assert_cmpint(found->created_at, ==, 1234567890);
  marmot_key_package_info_free(found);

  MarmotKeyPackageInfo **infos = NULL;
  size_t count = 0;
  g_assert_cmpint(s->find_key_packages_by_pubkey(s->ctx, info.owner_pubkey, &infos, &count), ==,
                  MARMOT_OK);
  g_assert_cmpuint(count, ==, 1);
  g_assert_cmpmem(infos[0]->ref, 32, info.ref, 32);
  marmot_key_package_info_free(infos[0]);
  free(infos);

  uint8_t bad_ref[32];
  memset(bad_ref, 0xFF, 32);
  found = NULL;
  g_assert_cmpint(s->find_key_package_by_ref(s->ctx, bad_ref, &found), ==, MARMOT_OK);
  g_assert_null(found);
}

static void
contract_key_package_info_deactivate(MarmotStorage *s)
{
  uint8_t pk[32];
  memset(pk, 0xC3, 32);

  MarmotKeyPackageInfo info1;
  memset(&info1, 0, sizeof(info1));
  memset(info1.ref, 0xD1, 32);
  memcpy(info1.owner_pubkey, pk, 32);
  info1.created_at = 1000;
  info1.active = true;
  g_assert_cmpint(s->save_key_package_info(s->ctx, &info1), ==, MARMOT_OK);

  MarmotKeyPackageInfo info2;
  memset(&info2, 0, sizeof(info2));
  memset(info2.ref, 0xD2, 32);
  memcpy(info2.owner_pubkey, pk, 32);
  info2.created_at = 2000;
  info2.active = true;
  g_assert_cmpint(s->save_key_package_info(s->ctx, &info2), ==, MARMOT_OK);

  MarmotKeyPackageInfo **infos = NULL;
  size_t count = 0;
  g_assert_cmpint(s->find_key_packages_by_pubkey(s->ctx, pk, &infos, &count), ==, MARMOT_OK);
  g_assert_cmpuint(count, ==, 2);
  for (size_t i = 0; i < count; i++)
    marmot_key_package_info_free(infos[i]);
  free(infos);

  g_assert_cmpint(s->deactivate_key_packages(s->ctx, pk), ==, MARMOT_OK);

  MarmotKeyPackageInfo *found = NULL;
  g_assert_cmpint(s->find_key_package_by_ref(s->ctx, info1.ref, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_false(found->active);
  marmot_key_package_info_free(found);
  g_assert_cmpint(s->find_key_package_by_ref(s->ctx, info2.ref, &found), ==, MARMOT_OK);
  g_assert_nonnull(found);
  g_assert_false(found->active);
  marmot_key_package_info_free(found);
}

static void
contract_relay_replace_and_list(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "relay_grp", 9);
  ensure_group(s, &gid, "relay_grp");

  const char *urls[] = { "wss://relay1.example.com", "wss://relay2.example.com" };
  g_assert_cmpint(s->replace_group_relays(s->ctx, &gid, urls, 2), ==, MARMOT_OK);

  MarmotGroupRelay *relays = NULL;
  size_t count = 0;
  g_assert_cmpint(s->group_relays(s->ctx, &gid, &relays, &count), ==, MARMOT_OK);
  g_assert_cmpuint(count, ==, 2);
  bool found1 = false, found2 = false;
  for (size_t i = 0; i < count; i++) {
    if (strcmp(relays[i].relay_url, "wss://relay1.example.com") == 0)
      found1 = true;
    if (strcmp(relays[i].relay_url, "wss://relay2.example.com") == 0)
      found2 = true;
    free(relays[i].relay_url);
    marmot_group_id_free(&relays[i].mls_group_id);
  }
  free(relays);
  g_assert_true(found1 && found2);

  const char *new_urls[] = { "wss://relay3.example.com" };
  g_assert_cmpint(s->replace_group_relays(s->ctx, &gid, new_urls, 1), ==, MARMOT_OK);
  g_assert_cmpint(s->group_relays(s->ctx, &gid, &relays, &count), ==, MARMOT_OK);
  g_assert_cmpuint(count, ==, 1);
  g_assert_cmpstr(relays[0].relay_url, ==, "wss://relay3.example.com");
  free(relays[0].relay_url);
  marmot_group_id_free(&relays[0].mls_group_id);
  free(relays);
  marmot_group_id_free(&gid);
}

/* libmarmot's case asserts MARMOT_ERR_UNSUPPORTED everywhere; the same group
 * and names with GhStoreMarmot's real snapshots. */
static void
contract_snapshot_lifecycle(MarmotStorage *s)
{
  MarmotGroupId gid = marmot_group_id_new((uint8_t *) "snap_grp", 8);
  g_assert_true(s->is_persistent && s->is_persistent(s->ctx));

  /* The contract never saves "snap_grp": a snapshot needs the group. */
  g_assert_cmpint(s->create_snapshot(s->ctx, &gid, "before_commit"), ==, MARMOT_ERR_GROUP_NOT_FOUND);
  g_assert_cmpint(s->rollback_snapshot(s->ctx, &gid, "before_commit"), ==,
                  MARMOT_ERR_STORAGE_NOT_FOUND);
  g_assert_cmpint(s->release_snapshot(s->ctx, &gid, "before_commit"), ==, MARMOT_OK);

  ensure_group(s, &gid, "snap_grp");
  g_assert_cmpint(s->create_snapshot(s->ctx, &gid, "before_commit"), ==, MARMOT_OK);
  g_assert_cmpint(s->rollback_snapshot(s->ctx, &gid, "before_commit"), ==, MARMOT_OK);
  /* A rollback consumes the snapshot. */
  g_assert_cmpint(s->rollback_snapshot(s->ctx, &gid, "before_commit"), ==,
                  MARMOT_ERR_STORAGE_NOT_FOUND);
  g_assert_cmpint(s->create_snapshot(s->ctx, &gid, "before_commit"), ==, MARMOT_OK);
  g_assert_cmpint(s->release_snapshot(s->ctx, &gid, "before_commit"), ==, MARMOT_OK);
  g_assert_cmpint(s->rollback_snapshot(s->ctx, &gid, "before_commit"), ==,
                  MARMOT_ERR_STORAGE_NOT_FOUND);

  size_t pruned = 123;
  g_assert_cmpint(s->prune_expired_snapshots(s->ctx, 0, &pruned), ==, MARMOT_OK);
  g_assert_cmpuint(pruned, ==, 0);
  marmot_group_id_free(&gid);
}

typedef void (*ContractCase)(MarmotStorage *s);

static const struct {
  const gchar *name;
  ContractCase run;
} contract_cases[] = {
  { "test_group_save_and_find_by_mls_id", contract_group_save_and_find_by_mls_id },
  { "test_group_find_by_nostr_id", contract_group_find_by_nostr_id },
  { "test_group_not_found", contract_group_not_found },
  { "test_group_upsert", contract_group_upsert },
  { "test_group_list_all", contract_group_list_all },
  { "test_group_delete_removes_group_and_relays", contract_group_delete_removes_group_and_relays },
  { "test_message_save_and_find", contract_message_save_and_find },
  { "test_message_pagination", contract_message_pagination },
  { "test_message_last", contract_message_last },
  { "test_message_processed_tracking", contract_message_processed_tracking },
  { "test_welcome_save_and_find", contract_welcome_save_and_find },
  { "test_welcome_pending", contract_welcome_pending },
  { "test_welcome_processed_tracking", contract_welcome_processed_tracking },
  { "test_mls_store_roundtrip", contract_mls_store_roundtrip },
  { "test_mls_store_not_found", contract_mls_store_not_found },
  { "test_mls_store_upsert", contract_mls_store_upsert },
  { "test_mls_store_delete", contract_mls_store_delete },
  { "test_mls_store_label_isolation", contract_mls_store_label_isolation },
  { "test_exporter_secret_roundtrip", contract_exporter_secret_roundtrip },
  { "test_exporter_secret_overwrite", contract_exporter_secret_overwrite },
  { "test_exporter_secret_delete", contract_exporter_secret_delete },
  { "test_relay_replace_and_list", contract_relay_replace_and_list },
  { "test_key_package_info_roundtrip", contract_key_package_info_roundtrip },
  { "test_key_package_info_deactivate", contract_key_package_info_deactivate },
  { "test_snapshot_lifecycle (real snapshots)", contract_snapshot_lifecycle },
};

static void
test_contract_libmarmot_cases(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);

  /* "Implementations must provide all non-NULL function pointers." */
#define ASSERT_PROVIDED(fn) g_assert_true(s->fn != NULL)
  ASSERT_PROVIDED(all_groups);
  ASSERT_PROVIDED(find_group_by_mls_id);
  ASSERT_PROVIDED(find_group_by_nostr_id);
  ASSERT_PROVIDED(save_group);
  ASSERT_PROVIDED(delete_group);
  ASSERT_PROVIDED(messages);
  ASSERT_PROVIDED(last_message);
  ASSERT_PROVIDED(save_message);
  ASSERT_PROVIDED(find_message_by_id);
  ASSERT_PROVIDED(is_message_processed);
  ASSERT_PROVIDED(save_processed_message);
  ASSERT_PROVIDED(save_welcome);
  ASSERT_PROVIDED(find_welcome_by_event_id);
  ASSERT_PROVIDED(pending_welcomes);
  ASSERT_PROVIDED(find_processed_welcome);
  ASSERT_PROVIDED(save_processed_welcome);
  ASSERT_PROVIDED(save_key_package_info);
  ASSERT_PROVIDED(find_key_package_by_ref);
  ASSERT_PROVIDED(find_key_packages_by_pubkey);
  ASSERT_PROVIDED(deactivate_key_packages);
  ASSERT_PROVIDED(group_relays);
  ASSERT_PROVIDED(replace_group_relays);
  ASSERT_PROVIDED(get_exporter_secret);
  ASSERT_PROVIDED(save_exporter_secret);
  ASSERT_PROVIDED(delete_exporter_secret);
  ASSERT_PROVIDED(create_snapshot);
  ASSERT_PROVIDED(rollback_snapshot);
  ASSERT_PROVIDED(release_snapshot);
  ASSERT_PROVIDED(prune_expired_snapshots);
  ASSERT_PROVIDED(mls_store);
  ASSERT_PROVIDED(mls_load);
  ASSERT_PROVIDED(mls_delete);
  ASSERT_PROVIDED(is_persistent);
  ASSERT_PROVIDED(destroy);
#undef ASSERT_PROVIDED

  for (guint i = 0; i < G_N_ELEMENTS(contract_cases); i++) {
    g_test_message("libmarmot contract: %s", contract_cases[i].name);
    contract_cases[i].run(s);
  }
  /* The contract's persistence check for persistent backends. */
  g_assert_true(s->is_persistent(s->ctx));
  /* Not-found answers are not failures. */
  assert_nothing_recorded(s);
  assert_integrity(store);

  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

/* ---- Exact round trips ---------------------------------------------------------------------- */

static void
assert_optional_mem(const void *a, const void *b, gsize len)
{
  if (!a) {
    g_assert_null(b);
    return;
  }
  g_assert_nonnull(b);
  g_assert_cmpmem(a, len, b, len);
}

static MarmotGroup *
full_group(const MarmotGroupId *gid, const gchar *name)
{
  MarmotGroup *g = marmot_group_new();
  g->mls_group_id = marmot_group_id_new(gid->data, gid->len);
  for (guint i = 0; i < 32; i++)
    g->nostr_group_id[i] = (uint8_t) (i * 7 + 1);
  g->name = strdup(name);
  g->description = strdup("D\xc3\xa9scription with\ttab, \"quotes\", '\x01' and \xf0\x9f\x90\xb9");
  g->image_hash = c_bytes(32, 1);
  g->image_key = c_bytes(32, 2);
  g->image_nonce = c_bytes(12, 3);
  g->admin_count = 3;
  g->admin_pubkeys = (uint8_t (*)[32]) c_bytes(3 * 32, 4);
  g->last_message_id = strdup("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  g->last_message_at = T0;
  g->last_message_processed_at = T0 + 5;
  /* Above INT64_MAX: stored as the same 64 bits. */
  g->epoch = G_GUINT64_CONSTANT(0x8000000000000001);
  g->state = MARMOT_GROUP_STATE_PENDING;
  return g;
}

static void
assert_groups_equal(const MarmotGroup *a, const MarmotGroup *b)
{
  g_assert_true(marmot_group_id_equal(&a->mls_group_id, &b->mls_group_id));
  g_assert_cmpmem(a->nostr_group_id, 32, b->nostr_group_id, 32);
  g_assert_cmpstr(a->name, ==, b->name);
  g_assert_cmpstr(a->description, ==, b->description);
  assert_optional_mem(a->image_hash, b->image_hash, 32);
  assert_optional_mem(a->image_key, b->image_key, 32);
  assert_optional_mem(a->image_nonce, b->image_nonce, 12);
  g_assert_cmpuint(a->admin_count, ==, b->admin_count);
  assert_optional_mem(a->admin_pubkeys, b->admin_pubkeys, a->admin_count * 32);
  g_assert_cmpstr(a->last_message_id, ==, b->last_message_id);
  g_assert_cmpint(a->last_message_at, ==, b->last_message_at);
  g_assert_cmpint(a->last_message_processed_at, ==, b->last_message_processed_at);
  g_assert_cmpuint(a->epoch, ==, b->epoch);
  g_assert_cmpint(a->state, ==, b->state);
}

static void
test_roundtrip_group(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId gid = gid_of("round-trip group");

  MarmotGroup *g = full_group(&gid, "N\xc3\xa4me");
  assert_marmot_ok(s->save_group(s->ctx, g));
  MarmotGroup *found = NULL;
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &found));
  assert_groups_equal(g, found);
  marmot_group_free(found);
  assert_marmot_ok(s->find_group_by_nostr_id(s->ctx, g->nostr_group_id, &found));
  assert_groups_equal(g, found);
  marmot_group_free(found);

  /* An upsert replaces every field, including optional ones set to none. */
  MarmotGroup *bare = marmot_group_new();
  bare->mls_group_id = marmot_group_id_new(gid.data, gid.len);
  memset(bare->nostr_group_id, 0x5a, 32);
  bare->epoch = 3;
  assert_marmot_ok(s->save_group(s->ctx, bare));
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &found));
  assert_groups_equal(bare, found);
  g_assert_null(found->name);
  g_assert_null(found->admin_pubkeys);
  marmot_group_free(found);
  /* The old Nostr group id no longer finds it. */
  assert_marmot_ok(s->find_group_by_nostr_id(s->ctx, g->nostr_group_id, &found));
  g_assert_null(found);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_group_info"), ==, 1);

  /* Duplicated Nostr group ids resolve to the lowest MLS group id. */
  MarmotGroupId low = gid_of("a-lower-id");
  MarmotGroup *twin = full_group(&low, "Twin");
  memset(twin->nostr_group_id, 0x5a, 32);
  assert_marmot_ok(s->save_group(s->ctx, twin));
  for (guint i = 0; i < 2; i++) {
    assert_marmot_ok(s->find_group_by_nostr_id(s->ctx, bare->nostr_group_id, &found));
    g_assert_cmpstr(found->name, ==, "Twin");
    marmot_group_free(found);
    assert_marmot_ok(s->save_group(s->ctx, bare));
  }

  MarmotGroup **all = NULL;
  size_t n = 0;
  assert_marmot_ok(s->all_groups(s->ctx, &all, &n));
  g_assert_cmpuint(n, ==, 2);
  g_assert_true(marmot_group_id_equal(&all[0]->mls_group_id, &low));
  for (size_t i = 0; i < n; i++)
    marmot_group_free(all[i]);
  free(all);

  assert_nothing_recorded(s);
  marmot_group_free(twin);
  marmot_group_free(bare);
  marmot_group_free(g);
  marmot_group_id_free(&low);
  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

static MarmotMessage *
message_at(const MarmotGroupId *gid, guint8 id, gint64 created_at, gint64 processed_at)
{
  MarmotMessage *m = marmot_message_new();
  memset(m->id, id, 32);
  memset(m->pubkey, 0x33, 32);
  m->kind = 9;
  m->mls_group_id = marmot_group_id_new(gid->data, gid->len);
  m->created_at = created_at;
  m->processed_at = processed_at;
  m->content = malloc(32);
  snprintf(m->content, 32, "message %02x", id);
  memset(m->wrapper_event_id, (guint8) ~id, 32);
  m->epoch = 2;
  m->state = MARMOT_MSG_STATE_PROCESSED;
  return m;
}

static void
assert_messages_equal(const MarmotMessage *a, const MarmotMessage *b)
{
  g_assert_cmpmem(a->id, 32, b->id, 32);
  g_assert_cmpmem(a->pubkey, 32, b->pubkey, 32);
  g_assert_cmpuint(a->kind, ==, b->kind);
  g_assert_true(marmot_group_id_equal(&a->mls_group_id, &b->mls_group_id));
  g_assert_cmpint(a->created_at, ==, b->created_at);
  g_assert_cmpint(a->processed_at, ==, b->processed_at);
  g_assert_cmpstr(a->content, ==, b->content);
  g_assert_cmpstr(a->tags_json, ==, b->tags_json);
  g_assert_cmpstr(a->event_json, ==, b->event_json);
  g_assert_cmpmem(a->wrapper_event_id, 32, b->wrapper_event_id, 32);
  g_assert_cmpuint(a->epoch, ==, b->epoch);
  g_assert_cmpint(a->state, ==, b->state);
}

static void
assert_message_order(MarmotStorage *s, const MarmotGroupId *gid, MarmotSortOrder order,
                     gsize offset, gsize limit, const guint8 *expected, gsize n_expected)
{
  MarmotPagination page = { .limit = limit, .offset = offset, .sort_order = order };
  MarmotMessage **msgs = NULL;
  size_t n = 0;
  assert_marmot_ok(s->messages(s->ctx, gid, &page, &msgs, &n));
  g_assert_cmpuint(n, ==, n_expected);
  for (gsize i = 0; i < n; i++) {
    g_assert_cmpuint(msgs[i]->id[0], ==, expected[i]);
    marmot_message_free(msgs[i]);
  }
  free(msgs);
}

static void
test_roundtrip_messages(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId gid = gid_of("message group");
  MarmotGroupId other = gid_of("other group");

  /* Every field, then an upsert on the same id. */
  MarmotMessage *m = message_at(&gid, 0x10, T0, T0 + 1);
  m->tags_json = strdup("[[\"e\",\"\xc3\xa9\"]]");
  m->event_json = strdup("{\"kind\":445,\"content\":\"\\u0000 escaped\"}");
  assert_marmot_ok(s->save_message(s->ctx, m));
  MarmotMessage *found = NULL;
  assert_marmot_ok(s->find_message_by_id(s->ctx, m->id, &found));
  assert_messages_equal(m, found);
  marmot_message_free(found);
  free(m->content);
  m->content = NULL;
  m->state = MARMOT_MSG_STATE_EPOCH_INVALIDATED;
  assert_marmot_ok(s->save_message(s->ctx, m));
  assert_marmot_ok(s->find_message_by_id(s->ctx, m->id, &found));
  assert_messages_equal(m, found);
  marmot_message_free(found);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_messages"), ==, 1);

  /* Ordering: newest first by the key; the other time and then the id
   * break ties. */
  static const struct { guint8 id; gint64 created; gint64 processed; } rows[] = {
    { 0x01, 100, 500 }, { 0x02, 200, 400 }, { 0x03, 200, 300 },
    { 0x04, 300, 100 }, { 0x05, 300, 100 },
  };
  for (guint i = 0; i < G_N_ELEMENTS(rows); i++) {
    MarmotMessage *row = message_at(&other, rows[i].id, rows[i].created, rows[i].processed);
    assert_marmot_ok(s->save_message(s->ctx, row));
    marmot_message_free(row);
  }
  static const guint8 by_created[] = { 0x05, 0x04, 0x02, 0x03, 0x01 };
  static const guint8 by_processed[] = { 0x01, 0x02, 0x03, 0x05, 0x04 };
  assert_message_order(s, &other, MARMOT_SORT_CREATED_AT_FIRST, 0, 100, by_created, 5);
  assert_message_order(s, &other, MARMOT_SORT_PROCESSED_AT_FIRST, 0, 100, by_processed, 5);
  assert_message_order(s, &other, MARMOT_SORT_CREATED_AT_FIRST, 1, 2, by_created + 1, 2);
  assert_message_order(s, &other, MARMOT_SORT_PROCESSED_AT_FIRST, 3, 100, by_processed + 3, 2);
  assert_message_order(s, &other, MARMOT_SORT_CREATED_AT_FIRST, 5, 100, NULL, 0);
  assert_message_order(s, &other, MARMOT_SORT_CREATED_AT_FIRST, 0, 0, NULL, 0);
  assert_message_order(s, &other, MARMOT_SORT_CREATED_AT_FIRST, (gsize) -1, (gsize) -1, NULL, 0);

  MarmotMessage **msgs = NULL;
  size_t n = 0;
  assert_marmot_ok(s->messages(s->ctx, &other, NULL, &msgs, &n));
  g_assert_cmpuint(n, ==, 5);
  g_assert_cmpuint(msgs[0]->id[0], ==, 0x05);
  for (size_t i = 0; i < n; i++)
    marmot_message_free(msgs[i]);
  free(msgs);

  assert_marmot_ok(s->last_message(s->ctx, &other, MARMOT_SORT_CREATED_AT_FIRST, &found));
  g_assert_cmpuint(found->id[0], ==, 0x05);
  marmot_message_free(found);
  assert_marmot_ok(s->last_message(s->ctx, &other, MARMOT_SORT_PROCESSED_AT_FIRST, &found));
  g_assert_cmpuint(found->id[0], ==, 0x01);
  marmot_message_free(found);
  MarmotGroupId empty = gid_of("no messages");
  found = (MarmotMessage *) 0x1;
  assert_marmot_ok(s->last_message(s->ctx, &empty, MARMOT_SORT_CREATED_AT_FIRST, &found));
  g_assert_null(found);

  MarmotPagination bad = { .limit = 10, .offset = 0, .sort_order = (MarmotSortOrder) 7 };
  g_assert_cmpint(s->messages(s->ctx, &other, &bad, &msgs, &n), ==, MARMOT_ERR_INVALID_ARG);
  assert_recorded(s, GH_STORE_ERROR_INVALID);

  /* Processed-message records: optional ids, and long reasons are cut on a
   * character boundary (reasons are diagnostics, not data). */
  uint8_t wrapper[32];
  memset(wrapper, 0x77, 32);
  assert_marmot_ok(s->save_processed_message(s->ctx, wrapper, NULL, T0, 4, NULL, 3, NULL));
  bool processed = false;
  assert_marmot_ok(s->is_message_processed(s->ctx, wrapper, &processed));
  g_assert_true(processed);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_processed_messages WHERE "
                                 "message_event_id IS NULL AND mls_group_id IS NULL AND "
                                 "failure_reason IS NULL AND state = 3 AND epoch = 4"), ==, 1);
  /* 'x' then 2-byte characters: the byte at the limit continues one. */
  GString *long_reason = g_string_new("x");
  for (guint i = 0; i < 700; i++)
    g_string_append(long_reason, "\xc3\xa9");
  assert_marmot_ok(s->save_processed_message(s->ctx, wrapper, m->id, T0 + 1, 5, &gid, 1,
                                             long_reason->str));
  g_autofree gchar *stored_reason = sql_text(store, "SELECT failure_reason FROM mls_processed_messages");
  g_assert_cmpuint(strlen(stored_reason), ==, GH_STORE_MARMOT_MAX_REASON - 1);
  g_assert_true(g_utf8_validate(stored_reason, -1, NULL));
  g_assert_true(g_str_has_prefix(long_reason->str, stored_reason));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_processed_messages"), ==, 1);
  g_string_free(long_reason, TRUE);

  assert_nothing_recorded(s);
  marmot_message_free(m);
  marmot_group_id_free(&empty);
  marmot_group_id_free(&other);
  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

static void
assert_welcomes_equal(const MarmotWelcome *a, const MarmotWelcome *b)
{
  g_assert_cmpmem(a->id, 32, b->id, 32);
  g_assert_cmpstr(a->event_json, ==, b->event_json);
  g_assert_true(marmot_group_id_equal(&a->mls_group_id, &b->mls_group_id));
  g_assert_cmpmem(a->nostr_group_id, 32, b->nostr_group_id, 32);
  g_assert_cmpstr(a->group_name, ==, b->group_name);
  g_assert_cmpstr(a->group_description, ==, b->group_description);
  assert_optional_mem(a->group_image_hash, b->group_image_hash, 32);
  g_assert_cmpuint(a->group_admin_count, ==, b->group_admin_count);
  assert_optional_mem(a->group_admin_pubkeys, b->group_admin_pubkeys, a->group_admin_count * 32);
  g_assert_cmpuint(a->group_relay_count, ==, b->group_relay_count);
  for (size_t i = 0; i < a->group_relay_count; i++)
    g_assert_cmpstr(a->group_relays[i], ==, b->group_relays[i]);
  g_assert_cmpmem(a->welcomer, 32, b->welcomer, 32);
  g_assert_cmpuint(a->member_count, ==, b->member_count);
  g_assert_cmpint(a->state, ==, b->state);
  g_assert_cmpmem(a->wrapper_event_id, 32, b->wrapper_event_id, 32);
}

static void
test_roundtrip_welcomes_and_key_packages(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);

  /* A welcome as process_welcome stores it: no MLS group id yet, relays
   * exactly as the rumor listed them (empty, duplicated, odd bytes). */
  MarmotWelcome *w = marmot_welcome_new();
  memset(w->id, 0x41, 32);
  memset(w->wrapper_event_id, 0x42, 32);
  w->event_json = strdup("{\"kind\":444,\"content\":\"base64==\"}");
  memset(w->nostr_group_id, 0x43, 32);
  w->group_name = strdup("Invite \xe2\x9c\x89");
  w->group_image_hash = c_bytes(32, 9);
  w->group_admin_count = 2;
  w->group_admin_pubkeys = (uint8_t (*)[32]) c_bytes(64, 10);
  static const char *const relay_list[] = { RELAY_ONE, "", "wss://tab\there.invalid",
                                            "wss://\xc3\xbcni.invalid", RELAY_ONE };
  w->group_relay_count = G_N_ELEMENTS(relay_list);
  w->group_relays = calloc(w->group_relay_count, sizeof(char *));
  for (size_t i = 0; i < w->group_relay_count; i++)
    w->group_relays[i] = strdup(relay_list[i]);
  memset(w->welcomer, 0x44, 32);
  w->member_count = 12;
  w->state = MARMOT_WELCOME_STATE_PENDING;
  assert_marmot_ok(s->save_welcome(s->ctx, w));
  MarmotWelcome *found = NULL;
  assert_marmot_ok(s->find_welcome_by_event_id(s->ctx, w->id, &found));
  assert_welcomes_equal(w, found);
  marmot_welcome_free(found);

  /* Accepted: same id, now with a group id; still one row. */
  w->mls_group_id = gid_of("joined group");
  w->state = MARMOT_WELCOME_STATE_ACCEPTED;
  assert_marmot_ok(s->save_welcome(s->ctx, w));
  assert_marmot_ok(s->find_welcome_by_event_id(s->ctx, w->id, &found));
  assert_welcomes_equal(w, found);
  marmot_welcome_free(found);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_welcomes"), ==, 1);

  /* A record that only shares the wrapper id replaces it (memory backend). */
  MarmotWelcome *pending = make_test_welcome(&w->mls_group_id, 9);
  memcpy(pending->wrapper_event_id, w->wrapper_event_id, 32);
  assert_marmot_ok(s->save_welcome(s->ctx, pending));
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_welcomes"), ==, 1);
  assert_marmot_ok(s->find_welcome_by_event_id(s->ctx, w->id, &found));
  g_assert_null(found);
  MarmotWelcome *second = make_test_welcome(&w->mls_group_id, 10);
  assert_marmot_ok(s->save_welcome(s->ctx, second));

  /* Pending welcomes in arrival order, accepted ones excluded. */
  MarmotWelcome **list = NULL;
  size_t n = 0;
  assert_marmot_ok(s->pending_welcomes(s->ctx, NULL, &list, &n));
  g_assert_cmpuint(n, ==, 2);
  assert_welcomes_equal(pending, list[0]);
  assert_welcomes_equal(second, list[1]);
  for (size_t i = 0; i < n; i++)
    marmot_welcome_free(list[i]);
  free(list);
  MarmotPagination page = { .limit = 1, .offset = 1 };
  assert_marmot_ok(s->pending_welcomes(s->ctx, &page, &list, &n));
  g_assert_cmpuint(n, ==, 1);
  assert_welcomes_equal(second, list[0]);
  marmot_welcome_free(list[0]);
  free(list);
  second->state = MARMOT_WELCOME_STATE_DECLINED;
  assert_marmot_ok(s->save_welcome(s->ctx, second));
  assert_marmot_ok(s->pending_welcomes(s->ctx, NULL, &list, &n));
  g_assert_cmpuint(n, ==, 1);
  marmot_welcome_free(list[0]);
  free(list);

  /* Processed welcomes keep their state and reason. */
  bool was_found = false;
  int state = -1;
  char *reason = NULL;
  assert_marmot_ok(s->save_processed_welcome(s->ctx, w->wrapper_event_id, w->id, T0, 3,
                                             "Welcome failed: \xc3\xa9poch"));
  assert_marmot_ok(s->find_processed_welcome(s->ctx, w->wrapper_event_id, &was_found, &state,
                                             &reason));
  g_assert_true(was_found);
  g_assert_cmpint(state, ==, 3);
  g_assert_cmpstr(reason, ==, "Welcome failed: \xc3\xa9poch");
  free(reason);

  /* Key packages: relay lists exactly, every package of an owner (active or
   * not, newest first), updates by ref. */
  uint8_t owner[32], stranger[32];
  memset(owner, 0x61, 32);
  memset(stranger, 0x62, 32);
  MarmotKeyPackageInfo kp1 = { .created_at = 100, .active = true };
  memset(kp1.ref, 0x71, 32);
  memcpy(kp1.owner_pubkey, owner, 32);
  MarmotKeyPackageInfo kp2 = { .created_at = 200, .active = true };
  memset(kp2.ref, 0x72, 32);
  memcpy(kp2.owner_pubkey, owner, 32);
  kp2.relay_urls = (char **) relay_list;
  kp2.relay_count = G_N_ELEMENTS(relay_list);
  MarmotKeyPackageInfo kp3 = { .created_at = 300, .active = true };
  memset(kp3.ref, 0x73, 32);
  memcpy(kp3.owner_pubkey, stranger, 32);
  assert_marmot_ok(s->save_key_package_info(s->ctx, &kp1));
  assert_marmot_ok(s->save_key_package_info(s->ctx, &kp2));
  assert_marmot_ok(s->save_key_package_info(s->ctx, &kp3));
  MarmotKeyPackageInfo *info = NULL;
  assert_marmot_ok(s->find_key_package_by_ref(s->ctx, kp2.ref, &info));
  g_assert_cmpuint(info->relay_count, ==, G_N_ELEMENTS(relay_list));
  for (size_t i = 0; i < info->relay_count; i++)
    g_assert_cmpstr(info->relay_urls[i], ==, relay_list[i]);
  g_assert_cmpint(info->created_at, ==, 200);
  marmot_key_package_info_free(info);
  assert_marmot_ok(s->find_key_package_by_ref(s->ctx, kp1.ref, &info));
  g_assert_cmpuint(info->relay_count, ==, 0);
  g_assert_null(info->relay_urls);
  marmot_key_package_info_free(info);

  assert_marmot_ok(s->deactivate_key_packages(s->ctx, owner));
  MarmotKeyPackageInfo **infos = NULL;
  assert_marmot_ok(s->find_key_packages_by_pubkey(s->ctx, owner, &infos, &n));
  g_assert_cmpuint(n, ==, 2);
  g_assert_cmpmem(infos[0]->ref, 32, kp2.ref, 32);
  g_assert_cmpmem(infos[1]->ref, 32, kp1.ref, 32);
  g_assert_false(infos[0]->active || infos[1]->active);
  for (size_t i = 0; i < n; i++)
    marmot_key_package_info_free(infos[i]);
  free(infos);
  assert_marmot_ok(s->find_key_package_by_ref(s->ctx, kp3.ref, &info));
  g_assert_true(info->active);
  marmot_key_package_info_free(info);
  kp1.active = true;
  kp1.relay_urls = (char **) relay_list;
  kp1.relay_count = 1;
  assert_marmot_ok(s->save_key_package_info(s->ctx, &kp1));
  assert_marmot_ok(s->find_key_package_by_ref(s->ctx, kp1.ref, &info));
  g_assert_true(info->active);
  g_assert_cmpuint(info->relay_count, ==, 1);
  marmot_key_package_info_free(info);

  assert_nothing_recorded(s);
  marmot_welcome_free(second);
  marmot_welcome_free(pending);
  marmot_welcome_free(w);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

static void
test_roundtrip_kv_relays_secrets(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);

  /* Binary keys and values, the empty key and the empty value. */
  static const uint8_t key[] = { 0x00, 0xff, 0x00 };
  static const uint8_t value[] = { 0x00, 0x01, 0x00, 0x02 };
  assert_marmot_ok(s->mls_store(s->ctx, "binary", key, sizeof key, value, sizeof value));
  assert_marmot_ok(s->mls_store(s->ctx, "empty", NULL, 0, NULL, 0));
  uint8_t *out = NULL;
  size_t out_len = 99;
  assert_marmot_ok(s->mls_load(s->ctx, "binary", key, sizeof key, &out, &out_len));
  g_assert_cmpmem(out, out_len, value, sizeof value);
  free(out);
  assert_marmot_ok(s->mls_load(s->ctx, "empty", NULL, 0, &out, &out_len));
  /* Present but empty: a buffer of length 0, unlike "missing". */
  g_assert_nonnull(out);
  g_assert_cmpuint(out_len, ==, 0);
  free(out);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_kv WHERE label = 'empty' AND "
                                 "typeof(key) = 'blob' AND length(value) = 0"), ==, 1);
  g_autofree uint8_t *big = c_bytes(1024 * 1024, 7);
  assert_marmot_ok(s->mls_store(s->ctx, "big", key, sizeof key, big, 1024 * 1024));
  assert_marmot_ok(s->mls_load(s->ctx, "big", key, sizeof key, &out, &out_len));
  g_assert_cmpmem(out, out_len, big, 1024 * 1024);
  free(out);
  g_assert_cmpint(s->mls_delete(s->ctx, "binary", key, 2), ==, MARMOT_ERR_STORAGE_NOT_FOUND);
  assert_marmot_ok(s->mls_delete(s->ctx, "binary", key, sizeof key));
  g_assert_cmpint(s->mls_delete(s->ctx, "binary", key, sizeof key), ==,
                  MARMOT_ERR_STORAGE_NOT_FOUND);

  /* Relays: list order kept, a repeated URL keeps its first place, an empty
   * list clears, another group is unaffected. */
  MarmotGroupId gid = gid_of("relay group");
  MarmotGroupId other = gid_of("other relay group");
  const char *urls[] = { RELAY_TWO, RELAY_ONE, RELAY_TWO, "wss://third.invalid" };
  assert_marmot_ok(s->replace_group_relays(s->ctx, &gid, urls, G_N_ELEMENTS(urls)));
  assert_marmot_ok(s->replace_group_relays(s->ctx, &other, urls, 1));
  MarmotGroupRelay *relays = NULL;
  size_t n = 0;
  assert_marmot_ok(s->group_relays(s->ctx, &gid, &relays, &n));
  g_assert_cmpuint(n, ==, 3);
  g_assert_cmpstr(relays[0].relay_url, ==, RELAY_TWO);
  g_assert_cmpstr(relays[1].relay_url, ==, RELAY_ONE);
  g_assert_cmpstr(relays[2].relay_url, ==, "wss://third.invalid");
  for (size_t i = 0; i < n; i++) {
    g_assert_true(marmot_group_id_equal(&relays[i].mls_group_id, &gid));
    free(relays[i].relay_url);
    marmot_group_id_free(&relays[i].mls_group_id);
  }
  free(relays);
  assert_marmot_ok(s->replace_group_relays(s->ctx, &gid, NULL, 0));
  assert_marmot_ok(s->group_relays(s->ctx, &gid, &relays, &n));
  g_assert_cmpuint(n, ==, 0);
  g_assert_null(relays);
  assert_marmot_ok(s->group_relays(s->ctx, &other, &relays, &n));
  g_assert_cmpuint(n, ==, 1);
  free(relays[0].relay_url);
  marmot_group_id_free(&relays[0].mls_group_id);
  free(relays);

  /* Exporter secrets per epoch; deleting a missing one succeeds. */
  uint8_t secret[32], got[32];
  memset(secret, 0x3c, 32);
  assert_marmot_ok(s->save_exporter_secret(s->ctx, &gid, G_MAXUINT64, secret));
  assert_marmot_ok(s->get_exporter_secret(s->ctx, &gid, G_MAXUINT64, got));
  g_assert_cmpmem(got, 32, secret, 32);
  g_assert_cmpint(s->get_exporter_secret(s->ctx, &other, G_MAXUINT64, got), ==,
                  MARMOT_ERR_STORAGE_NOT_FOUND);
  assert_marmot_ok(s->delete_exporter_secret(s->ctx, &gid, 12345));

  assert_nothing_recorded(s);
  marmot_group_id_free(&other);
  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

/* ---- Bounds ------------------------------------------------------------------------------ */

/* Each call is refused before any SQL runs: INVALID_ARG, a recorded
 * GH_STORE_ERROR_INVALID, and no table changes. */
static void
test_bounds(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId gid = gid_of("bounded");
  MarmotGroup *group = full_group(&gid, "Bounded");
  assert_marmot_ok(s->save_group(s->ctx, group));
  g_autofree gchar *before = db_dump(store, "mls_%");

#define EXPECT_INVALID(call)                                                    \
  G_STMT_START {                                                                \
    g_assert_cmpint((call), ==, MARMOT_ERR_INVALID_ARG);                        \
    assert_recorded(s, GH_STORE_ERROR_INVALID);                                 \
    g_autofree gchar *after_ = db_dump(store, "mls_%");                         \
    g_assert_cmpstr(after_, ==, before);                                        \
  } G_STMT_END

  /* Group ids: missing, empty, too long. */
  MarmotGroupId empty = { NULL, 0 };
  g_autofree uint8_t *long_id_data = c_bytes(GH_STORE_MARMOT_MAX_GROUP_ID + 1, 1);
  MarmotGroupId long_id = { long_id_data, GH_STORE_MARMOT_MAX_GROUP_ID + 1 };
  MarmotGroup *found = NULL;
  EXPECT_INVALID(s->find_group_by_mls_id(s->ctx, NULL, &found));
  EXPECT_INVALID(s->find_group_by_mls_id(s->ctx, &empty, &found));
  EXPECT_INVALID(s->find_group_by_mls_id(s->ctx, &long_id, &found));
  EXPECT_INVALID(s->save_exporter_secret(s->ctx, &long_id, 1, group->nostr_group_id));
  EXPECT_INVALID(s->delete_group(s->ctx, &empty));
  EXPECT_INVALID(s->save_group(s->ctx, NULL));

  /* Group fields at and beyond libmarmot's own limits. */
  char *saved_name = group->name;
  group->name = c_strfill(GH_STORE_MARMOT_MAX_TEXT + 1, 'n');
  EXPECT_INVALID(s->save_group(s->ctx, group));
  free(group->name);
  group->name = saved_name;
  saved_name = group->description;
  group->description = c_strfill(GH_STORE_MARMOT_MAX_TEXT + 1, 'd');
  EXPECT_INVALID(s->save_group(s->ctx, group));
  free(group->description);
  group->description = saved_name;
  saved_name = group->last_message_id;
  group->last_message_id = c_strfill(GH_STORE_MARMOT_MAX_MESSAGE_ID + 1, 'a');
  EXPECT_INVALID(s->save_group(s->ctx, group));
  free(group->last_message_id);
  group->last_message_id = saved_name;
  uint8_t (*saved_admins)[32] = group->admin_pubkeys;
  size_t saved_count = group->admin_count;
  group->admin_pubkeys = (uint8_t (*)[32]) c_bytes((GH_STORE_MARMOT_MAX_ADMINS + 1) * 32, 5);
  group->admin_count = GH_STORE_MARMOT_MAX_ADMINS + 1;
  EXPECT_INVALID(s->save_group(s->ctx, group));
  free(group->admin_pubkeys);
  group->admin_pubkeys = NULL;
  group->admin_count = 2;
  EXPECT_INVALID(s->save_group(s->ctx, group));
  group->admin_pubkeys = saved_admins;
  group->admin_count = saved_count;

  /* Relays: count, length, holes. */
  char *urls[GH_STORE_MARMOT_MAX_RELAYS + 1];
  for (guint i = 0; i < G_N_ELEMENTS(urls); i++)
    urls[i] = g_strdup_printf("wss://r%u.invalid", i);
  EXPECT_INVALID(s->replace_group_relays(s->ctx, &gid, (const char **) urls,
                                         GH_STORE_MARMOT_MAX_RELAYS + 1));
  EXPECT_INVALID(s->replace_group_relays(s->ctx, &gid, NULL, 1));
  char *long_url = c_strfill(GH_STORE_MARMOT_MAX_RELAY_URL + 1, 'u');
  const char *with_long[] = { RELAY_ONE, long_url };
  EXPECT_INVALID(s->replace_group_relays(s->ctx, &gid, with_long, 2));
  const char *with_hole[] = { RELAY_ONE, NULL };
  EXPECT_INVALID(s->replace_group_relays(s->ctx, &gid, with_hole, 2));
  MarmotKeyPackageInfo kp = { .relay_urls = urls, .relay_count = GH_STORE_MARMOT_MAX_RELAYS + 1 };
  EXPECT_INVALID(s->save_key_package_info(s->ctx, &kp));
  MarmotWelcome *w = make_test_welcome(&gid, 1);
  w->group_relays = urls;
  w->group_relay_count = GH_STORE_MARMOT_MAX_RELAYS + 1;
  EXPECT_INVALID(s->save_welcome(s->ctx, w));
  w->group_relays = NULL;
  w->group_relay_count = 0;
  free(w->event_json);
  w->event_json = c_strfill(GH_STORE_MARMOT_MAX_JSON + 1, 'j');
  EXPECT_INVALID(s->save_welcome(s->ctx, w));

  /* Messages. */
  MarmotMessage *m = message_at(&gid, 1, T0, T0);
  free(m->content);
  m->content = c_strfill(GH_STORE_MARMOT_MAX_JSON + 1, 'c');
  EXPECT_INVALID(s->save_message(s->ctx, m));
  marmot_group_id_free(&m->mls_group_id);
  EXPECT_INVALID(s->save_message(s->ctx, m));

  /* mls_kv: label, key, value. */
  static const uint8_t k[] = { 1 };
  g_autofree uint8_t *long_key = c_bytes(GH_STORE_MARMOT_MAX_KEY + 1, 2);
  char *long_label = c_strfill(GH_STORE_MARMOT_MAX_LABEL + 1, 'l');
  EXPECT_INVALID(s->mls_store(s->ctx, NULL, k, 1, k, 1));
  EXPECT_INVALID(s->mls_store(s->ctx, "", k, 1, k, 1));
  EXPECT_INVALID(s->mls_store(s->ctx, long_label, k, 1, k, 1));
  EXPECT_INVALID(s->mls_store(s->ctx, "label", long_key, GH_STORE_MARMOT_MAX_KEY + 1, k, 1));
  EXPECT_INVALID(s->mls_store(s->ctx, "label", NULL, 4, k, 1));
  EXPECT_INVALID(s->mls_store(s->ctx, "label", k, 1, NULL, 3));
  EXPECT_INVALID(s->mls_store(s->ctx, "label", k, 1, k, (size_t) GH_STORE_MARMOT_MAX_VALUE + 1));
  uint8_t *out = NULL;
  size_t out_len = 0;
  EXPECT_INVALID(s->mls_load(s->ctx, long_label, k, 1, &out, &out_len));
  EXPECT_INVALID(s->mls_delete(s->ctx, "label", long_key, GH_STORE_MARMOT_MAX_KEY + 1));

  /* Snapshot names. */
  char *long_name = c_strfill(GH_STORE_MARMOT_MAX_SNAPSHOT_NAME + 1, 's');
  EXPECT_INVALID(s->create_snapshot(s->ctx, &gid, NULL));
  EXPECT_INVALID(s->create_snapshot(s->ctx, &gid, ""));
  EXPECT_INVALID(s->create_snapshot(s->ctx, &gid, long_name));
  EXPECT_INVALID(s->rollback_snapshot(s->ctx, &gid, long_name));
  EXPECT_INVALID(s->release_snapshot(s->ctx, &empty, "name"));
#undef EXPECT_INVALID

  /* At the limits everything is accepted. */
  saved_name = group->name;
  group->name = c_strfill(GH_STORE_MARMOT_MAX_TEXT, 'n');
  free(group->admin_pubkeys);
  group->admin_pubkeys = (uint8_t (*)[32]) c_bytes(GH_STORE_MARMOT_MAX_ADMINS * 32, 6);
  group->admin_count = GH_STORE_MARMOT_MAX_ADMINS;
  assert_marmot_ok(s->save_group(s->ctx, group));
  free(group->name);
  group->name = saved_name;
  char *max_url = c_strfill(GH_STORE_MARMOT_MAX_RELAY_URL, 'u');
  g_free(urls[0]);
  urls[0] = g_strdup(max_url);
  assert_marmot_ok(s->replace_group_relays(s->ctx, &gid, (const char **) urls,
                                           GH_STORE_MARMOT_MAX_RELAYS));
  g_autofree uint8_t *max_id_data = c_bytes(GH_STORE_MARMOT_MAX_GROUP_ID, 8);
  MarmotGroupId max_id = { max_id_data, GH_STORE_MARMOT_MAX_GROUP_ID };
  assert_marmot_ok(s->save_exporter_secret(s->ctx, &max_id, 1, group->nostr_group_id));
  assert_nothing_recorded(s);

  for (guint i = 0; i < G_N_ELEMENTS(urls); i++)
    g_free(urls[i]);
  free(max_url);
  free(long_name);
  free(long_label);
  free(long_url);
  marmot_message_free(m);
  marmot_welcome_free(w);
  marmot_group_free(group);
  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

/* ---- Snapshots ----------------------------------------------------------------------------- */

static void
seed_group_state(MarmotStorage *s, const MarmotGroupId *gid, uint64_t epoch, guint8 seed,
                 gboolean with_mls_state)
{
  MarmotGroup *g = full_group(gid, "Seeded");
  g->epoch = epoch;
  assert_marmot_ok(s->save_group(s->ctx, g));
  marmot_group_free(g);
  const char *relays[] = { RELAY_ONE, RELAY_TWO };
  assert_marmot_ok(s->replace_group_relays(s->ctx, gid, relays, 2));
  uint8_t secret[32];
  memset(secret, seed, 32);
  assert_marmot_ok(s->save_exporter_secret(s->ctx, gid, epoch - 1, secret));
  secret[0] ^= 0xff;
  assert_marmot_ok(s->save_exporter_secret(s->ctx, gid, epoch, secret));
  if (with_mls_state) {
    g_autofree uint8_t *state = c_bytes(700, seed);
    assert_marmot_ok(s->mls_store(s->ctx, "mls_group", gid->data, gid->len, state, 700));
  }
}

/* A commit's worth of changes to one group's snapshot-covered state. */
static void
advance_group_state(MarmotStorage *s, const MarmotGroupId *gid, uint64_t epoch, guint8 seed)
{
  MarmotGroup *found = NULL;
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, gid, &found));
  g_assert_nonnull(found);
  found->epoch = epoch;
  free(found->name);
  found->name = strdup("Advanced");
  free(found->image_nonce);
  found->image_nonce = NULL;
  assert_marmot_ok(s->save_group(s->ctx, found));
  marmot_group_free(found);
  const char *relays[] = { "wss://moved.invalid" };
  assert_marmot_ok(s->replace_group_relays(s->ctx, gid, relays, 1));
  uint8_t secret[32];
  memset(secret, seed, 32);
  assert_marmot_ok(s->save_exporter_secret(s->ctx, gid, epoch, secret));
  assert_marmot_ok(s->delete_exporter_secret(s->ctx, gid, epoch - 2));
  g_autofree uint8_t *state = c_bytes(1300, seed);
  assert_marmot_ok(s->mls_store(s->ctx, "mls_group", gid->data, gid->len, state, 1300));
}

static gint64
snapshot_count(GhStore *store)
{
  return sql_int(store, "SELECT count(*) FROM mls_snapshots");
}

static gint64
snapshot_row_count(GhStore *store)
{
  return sql_int(store, "SELECT count(*) FROM mls_snapshot_rows");
}

/* Acceptance (§8.2 G23): a rollback restores byte-identical mls_kv. */
static void
test_snapshot_rollback_byte_identical(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId a = gid_of("group-A");
  MarmotGroupId b = gid_of("group-B");
  MarmotGroupId c = gid_of("group-C");

  seed_group_state(s, &a, 5, 0x10, TRUE);
  seed_group_state(s, &b, 9, 0x20, TRUE);
  seed_group_state(s, &c, 2, 0x30, FALSE);
  /* Account-scoped rows keyed by the same bytes as group A's id. */
  assert_marmot_ok(s->mls_store(s->ctx, "kp_priv", a.data, a.len, (const uint8_t *) "secret", 6));
  assert_marmot_ok(s->mls_store(s->ctx, "kp_slot", (const uint8_t *) "slot", 4,
                                (const uint8_t *) "value", 5));

  /* 1. Only A changes: after the rollback every covered table, mls_kv first
   *    of all, is byte-identical, and the snapshot is consumed. */
  g_autofree gchar *kv_before = kv_dump(store);
  g_autofree gchar *state_before = group_state_dump(store);
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "pending-commit"));
  g_assert_cmpint(snapshot_count(store), ==, 1);
  /* 1 info + 2 relays + 2 secrets + 1 MLS state row. */
  g_assert_cmpint(snapshot_row_count(store), ==, 6);
  advance_group_state(s, &a, 6, 0x11);
  g_autofree gchar *kv_changed = kv_dump(store);
  g_assert_cmpstr(kv_changed, !=, kv_before);
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &a, "pending-commit"));
  g_autofree gchar *kv_after = kv_dump(store);
  g_autofree gchar *state_after = group_state_dump(store);
  g_assert_cmpstr(kv_after, ==, kv_before);
  g_assert_cmpstr(state_after, ==, state_before);
  g_assert_cmpint(snapshot_count(store), ==, 0);
  g_assert_cmpint(snapshot_row_count(store), ==, 0);

  /* 2. State deleted after the snapshot comes back; state created after it
   *    goes away (group C had no MLS state when snapshotted). */
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "s"));
  assert_marmot_ok(s->create_snapshot(s->ctx, &c, "s"));
  assert_marmot_ok(s->mls_delete(s->ctx, "mls_group", a.data, a.len));
  assert_marmot_ok(s->mls_store(s->ctx, "mls_group", c.data, c.len, (const uint8_t *) "new", 3));
  assert_marmot_ok(s->delete_group(s->ctx, &a));
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &a, "s"));
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &c, "s"));
  g_autofree gchar *kv_restored = kv_dump(store);
  g_autofree gchar *state_restored = group_state_dump(store);
  g_assert_cmpstr(kv_restored, ==, kv_before);
  g_assert_cmpstr(state_restored, ==, state_before);

  /* 3. Isolation: rolling back A leaves group B's changes and every
   *    account-scoped row alone, even one keyed by A's id bytes. */
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "s"));
  advance_group_state(s, &a, 6, 0x12);
  advance_group_state(s, &b, 10, 0x22);
  assert_marmot_ok(s->mls_store(s->ctx, "kp_priv", a.data, a.len, (const uint8_t *) "rotated", 7));
  assert_marmot_ok(s->mls_store(s->ctx, "kp_full", a.data, a.len, (const uint8_t *) "full", 4));
  g_autofree gchar *b_state =
    sql_dump(store, "SELECT * FROM mls_kv WHERE key <> CAST('group-A' AS BLOB) OR label <> 'mls_group'");
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &a, "s"));
  g_autofree gchar *b_state_after =
    sql_dump(store, "SELECT * FROM mls_kv WHERE key <> CAST('group-A' AS BLOB) OR label <> 'mls_group'");
  g_assert_cmpstr(b_state_after, ==, b_state);
  g_assert_cmpint(sql_int(store, "SELECT epoch FROM mls_group_info WHERE mls_group_id = "
                                 "CAST('group-B' AS BLOB)"), ==, 10);
  g_assert_cmpint(sql_int(store, "SELECT epoch FROM mls_group_info WHERE mls_group_id = "
                                 "CAST('group-A' AS BLOB)"), ==, 5);
  /* A's own MLS state is the seeded one again. */
  g_autofree uint8_t *seeded = c_bytes(700, 0x10);
  uint8_t *loaded = NULL;
  size_t loaded_len = 0;
  assert_marmot_ok(s->mls_load(s->ctx, "mls_group", a.data, a.len, &loaded, &loaded_len));
  g_assert_cmpmem(loaded, loaded_len, seeded, 700);
  free(loaded);

  assert_nothing_recorded(s);
  assert_integrity(store);
  marmot_group_id_free(&c);
  marmot_group_id_free(&b);
  marmot_group_id_free(&a);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

static void
test_snapshot_semantics(void)
{
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, clock);
  MarmotStorage *s = storage_new(store);

  /* Snapshots copy these tables through explicit column lists (13 columns
   * fit mls_snapshot_rows' c0..c12). A migration that adds a column here
   * must extend snapshot_tables in gh-store-marmot.c, or snapshots would
   * silently drop it: this fails first. */
  static const struct { const gchar *table; gint64 columns; } covered[] = {
    { "mls_group_info", 13 }, { "mls_group_relays", 3 },
    { "mls_exporter_secrets", 3 }, { "mls_kv", 3 },
  };
  for (guint i = 0; i < G_N_ELEMENTS(covered); i++) {
    g_autofree gchar *sql = g_strdup_printf("SELECT count(*) FROM pragma_table_info('%s')",
                                            covered[i].table);
    g_assert_cmpint(sql_int(store, sql), ==, covered[i].columns);
  }
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM pragma_table_info('mls_snapshot_rows') "
                                 "WHERE name GLOB 'c[0-9]*'"), ==, 13);
  MarmotGroupId a = gid_of("group-A");
  MarmotGroupId b = gid_of("group-B");
  MarmotGroupId unknown = gid_of("never saved");

  /* Unknown group: nothing is written and nothing is recorded. */
  g_assert_cmpint(s->create_snapshot(s->ctx, &unknown, "s"), ==, MARMOT_ERR_GROUP_NOT_FOUND);
  g_assert_cmpint(snapshot_count(store), ==, 0);

  seed_group_state(s, &a, 5, 0x10, TRUE);
  seed_group_state(s, &b, 7, 0x20, TRUE);
  g_autofree gchar *state0 = group_state_dump(store);

  /* A snapshot of the same name is replaced by the later one. */
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "s"));
  advance_group_state(s, &a, 6, 0x11);
  g_autofree gchar *state1 = group_state_dump(store);
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "s"));
  g_assert_cmpint(snapshot_count(store), ==, 1);
  advance_group_state(s, &a, 7, 0x12);
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &a, "s"));
  g_autofree gchar *after_replaced = group_state_dump(store);
  g_assert_cmpstr(after_replaced, ==, state1);

  /* Several snapshots of a group; rolling back one keeps the others. The
   * same name on another group is a different snapshot. */
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "at-6"));
  advance_group_state(s, &a, 7, 0x13);
  g_autofree gchar *state2 = group_state_dump(store);
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "at-7"));
  assert_marmot_ok(s->create_snapshot(s->ctx, &b, "at-6"));
  advance_group_state(s, &a, 8, 0x14);
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &a, "at-6"));
  g_autofree gchar *back_to_1 = group_state_dump(store);
  g_assert_cmpstr(back_to_1, ==, state1);
  g_assert_cmpint(snapshot_count(store), ==, 2);
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &a, "at-7"));
  g_autofree gchar *back_to_2 = group_state_dump(store);
  g_assert_cmpstr(back_to_2, ==, state2);
  g_assert_cmpint(s->rollback_snapshot(s->ctx, &a, "at-6"), ==, MARMOT_ERR_STORAGE_NOT_FOUND);

  /* Release: removes the snapshot and its rows; again is not an error. */
  assert_marmot_ok(s->release_snapshot(s->ctx, &b, "at-6"));
  assert_marmot_ok(s->release_snapshot(s->ctx, &b, "at-6"));
  g_assert_cmpint(snapshot_count(store), ==, 0);
  g_assert_cmpint(snapshot_row_count(store), ==, 0);

  /* A snapshot in an unknown format is refused, and nothing changes. */
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "s"));
  sql_exec(store, "UPDATE mls_snapshots SET data = x'02'");
  advance_group_state(s, &a, 9, 0x15);
  g_autofree gchar *before_refused = group_state_dump(store);
  g_assert_cmpint(s->rollback_snapshot(s->ctx, &a, "s"), ==, MARMOT_ERR_SNAPSHOT_FAILED);
  assert_recorded(s, GH_STORE_ERROR_CORRUPT);
  g_autofree gchar *after_refused = group_state_dump(store);
  g_assert_cmpstr(after_refused, ==, before_refused);
  g_assert_cmpint(snapshot_count(store), ==, 1);
  assert_marmot_ok(s->release_snapshot(s->ctx, &a, "s"));

  /* Pruning by creation time (GhClock seconds): strictly older only. */
  assert_marmot_ok(s->create_snapshot(s->ctx, &a, "old"));
  gh_clock_fake_advance(clock, (gint64) 3600 * G_USEC_PER_SEC);
  assert_marmot_ok(s->create_snapshot(s->ctx, &b, "new"));
  g_assert_cmpint(sql_int(store, "SELECT created_at FROM mls_snapshots WHERE name = 'old'"), ==, T0);
  size_t pruned = 99;
  assert_marmot_ok(s->prune_expired_snapshots(s->ctx, (uint64_t) T0, &pruned));
  g_assert_cmpuint(pruned, ==, 0);
  assert_marmot_ok(s->prune_expired_snapshots(s->ctx, (uint64_t) T0 + 1, &pruned));
  g_assert_cmpuint(pruned, ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_snapshot_rows WHERE name = 'old'"), ==, 0);
  assert_marmot_ok(s->prune_expired_snapshots(s->ctx, (uint64_t) T0 + 3600, &pruned));
  g_assert_cmpuint(pruned, ==, 0);
  assert_marmot_ok(s->prune_expired_snapshots(s->ctx, G_MAXUINT64, NULL));
  g_assert_cmpint(snapshot_count(store), ==, 0);
  g_assert_cmpint(snapshot_row_count(store), ==, 0);

  assert_nothing_recorded(s);
  g_assert_cmpstr(state0, !=, state1);
  marmot_group_id_free(&unknown);
  marmot_group_id_free(&b);
  marmot_group_id_free(&a);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

/* ---- Transactions --------------------------------------------------------------------------- */

static void
test_caller_transaction(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId gid = gid_of("in a transaction");
  g_autoptr(GError) error = NULL;
  g_autofree gchar *empty = db_dump(store, "mls_%");

  /* Writes inside the caller's transaction are savepoints of it: visible to
   * later reads, gone with the caller's rollback. */
  g_assert_true(gh_store_begin(store, &error));
  seed_group_state(s, &gid, 3, 0x40, TRUE);
  assert_marmot_ok(s->create_snapshot(s->ctx, &gid, "s"));
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 1);
  MarmotGroup *found = NULL;
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &found));
  g_assert_nonnull(found);
  marmot_group_free(found);
  gh_store_rollback(store);
  g_autofree gchar *after_rollback = db_dump(store, "mls_%");
  g_assert_cmpstr(after_rollback, ==, empty);

  /* A failing write undoes only itself; the caller's other work survives. */
  g_assert_true(gh_store_begin(store, &error));
  seed_group_state(s, &gid, 3, 0x40, TRUE);
  g_assert_cmpint(s->create_snapshot(s->ctx, &gid, ""), ==, MARMOT_ERR_INVALID_ARG);
  assert_recorded(s, GH_STORE_ERROR_INVALID);
  g_assert_true(gh_store_commit(store, &error));
  g_assert_no_error(error);
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &found));
  g_assert_nonnull(found);
  marmot_group_free(found);

  /* If SQLite abandons the caller's transaction (simulated here by ending it
   * behind GhStore's back), no storage call runs until the caller rolls back. */
  g_assert_true(gh_store_begin(store, &error));
  g_assert_cmpint(sqlite3_exec(gh_store_get_db(store), "ROLLBACK", NULL, NULL, NULL), ==, SQLITE_OK);
  found = NULL;
  g_assert_cmpint(s->find_group_by_mls_id(s->ctx, &gid, &found), ==, MARMOT_ERR_STORAGE);
  g_assert_null(found);
  g_assert_cmpint(s->mls_store(s->ctx, "x", NULL, 0, NULL, 0), ==, MARMOT_ERR_STORAGE);
  {
    g_autoptr(GError) first = gh_store_marmot_take_error(s);
    g_assert_error(first, GH_STORE_ERROR, GH_STORE_ERROR_FAILED);
    g_assert_nonnull(strstr(first->message, "rolled back"));
  }
  gh_store_rollback(store);
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &found));
  g_assert_nonnull(found);
  marmot_group_free(found);

  /* The recorded error is the first one since the last take. */
  g_assert_cmpint(s->mls_store(s->ctx, "", NULL, 0, NULL, 0), ==, MARMOT_ERR_INVALID_ARG);
  g_assert_true(gh_store_begin(store, &error));
  g_assert_cmpint(sqlite3_exec(gh_store_get_db(store), "ROLLBACK", NULL, NULL, NULL), ==, SQLITE_OK);
  g_assert_cmpint(s->mls_store(s->ctx, "x", NULL, 0, NULL, 0), ==, MARMOT_ERR_STORAGE);
  gh_store_rollback(store);
  assert_recorded(s, GH_STORE_ERROR_INVALID);
  assert_nothing_recorded(s);

  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

/* Room for a few small writes, none for megabytes: a large value hits SQLITE_FULL. */
static void
limit_growth(GhStore *store)
{
  gint64 pages = sql_int(store, "PRAGMA page_count") + 16;
  g_autofree gchar *sql = g_strdup_printf("PRAGMA max_page_count = %" G_GINT64_FORMAT, pages);
  g_assert_cmpint(sql_int(store, sql), ==, pages);
}

static void
test_disk_full(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId gid = gid_of("full disk");
  seed_group_state(s, &gid, 3, 0x50, TRUE);
  g_autofree gchar *before = db_dump(store, "mls_%");
  limit_growth(store);

  /* The failure keeps its meaning (FULL) and leaves nothing behind. */
  g_autofree uint8_t *big = c_bytes(4 * 1024 * 1024, 1);
  g_assert_cmpint(s->mls_store(s->ctx, "mls_group", gid.data, gid.len, big, 4 * 1024 * 1024), ==,
                  MARMOT_ERR_STORAGE);
  assert_recorded(s, GH_STORE_ERROR_FULL);
  g_assert_cmpuint(gh_store_get_transaction_depth(store), ==, 0);
  g_autofree gchar *after = db_dump(store, "mls_%");
  g_assert_cmpstr(after, ==, before);

  /* Inside a caller's transaction the caller decides: roll back, retry later. */
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_begin(store, &error));
  assert_marmot_ok(s->create_snapshot(s->ctx, &gid, "pending"));
  g_assert_cmpint(s->mls_store(s->ctx, "mls_group", gid.data, gid.len, big, 4 * 1024 * 1024), ==,
                  MARMOT_ERR_STORAGE);
  assert_recorded(s, GH_STORE_ERROR_FULL);
  gh_store_rollback(store);
  g_autofree gchar *after_txn = db_dump(store, "mls_%");
  g_assert_cmpstr(after_txn, ==, before);

  g_assert_cmpint(sql_int(store, "PRAGMA max_page_count = 1000000"), >=, 1000000);
  assert_marmot_ok(s->mls_store(s->ctx, "mls_group", gid.data, gid.len, big, 4 * 1024 * 1024));
  assert_nothing_recorded(s);

  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

static void
test_store_kinds(void)
{
  g_autoptr(GError) error = NULL;

  /* MLS is disabled without durable storage (KC-4). */
  GhStore *ephemeral = gh_store_open_ephemeral(ACCOUNT_A, NULL, &error);
  g_assert_no_error(error);
  g_assert_null(gh_store_marmot_new(ephemeral, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
  g_clear_error(&error);
  gh_store_close(ephemeral);

  /* A read-only (STORE_CORRUPT) store still reads; every write fails. */
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  MarmotGroupId gid = gid_of("read only");
  seed_group_state(s, &gid, 3, 0x60, TRUE);
  marmot_storage_free(s);
  gh_store_close(store);

  store = store_open_flags(&account, NULL, GH_STORE_OPEN_ALLOW_CORRUPT);
  g_assert_true(gh_store_is_read_only(store));
  s = storage_new(store);
  MarmotGroup *found = NULL;
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &found));
  g_assert_nonnull(found);
  g_assert_cmpint(s->save_group(s->ctx, found), ==, MARMOT_ERR_STORAGE);
  assert_recorded(s, GH_STORE_ERROR_CORRUPT);
  g_assert_cmpint(s->create_snapshot(s->ctx, &gid, "s"), ==, MARMOT_ERR_STORAGE);
  assert_recorded(s, GH_STORE_ERROR_CORRUPT);
  marmot_group_free(found);

  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  test_account_clear(&account);
}

/* ---- Schema v2 migration ------------------------------------------------------------------ */

static const gchar *const v2_tables[] = {
  "mls_group_info", "mls_group_relays", "mls_exporter_secrets", "mls_messages",
  "mls_processed_messages", "mls_welcomes", "mls_processed_welcomes", "mls_key_packages",
  "mls_snapshot_rows",
};

/* Turns a fresh store back into what schema v1 created (a store from before
 * G23), with MLS state in v1's mls_kv. */
static void
make_v1_store(const TestAccount *account)
{
  GhStore *store = store_open(account, NULL);
  MarmotStorage *s = storage_new(store);
  assert_marmot_ok(s->mls_store(s->ctx, "kp_slot", (const uint8_t *) "owner", 5,
                                (const uint8_t *) "v1 state", 8));
  marmot_storage_free(s);
  for (guint i = 0; i < G_N_ELEMENTS(v2_tables); i++) {
    g_autofree gchar *sql = g_strdup_printf("DROP TABLE %s", v2_tables[i]);
    sql_exec(store, sql);
  }
  /* Later migrations are undone too: v3 (G19) added contacts.verified_at;
   * v4 (W18) the arrival order, read, timer and inbox columns and two
   * triggers. */
  sql_exec(store, "ALTER TABLE contacts DROP COLUMN verified_at");
  static const gchar *const v4_undo[] = {
    "DROP TRIGGER messages_admit_seq",
    "DROP TRIGGER messages_keep_read_marker",
    "ALTER TABLE messages DROP COLUMN seq",
    "ALTER TABLE conversations DROP COLUMN admit_seq",
    "ALTER TABLE conversations DROP COLUMN read_seq",
    "ALTER TABLE conversations DROP COLUMN reply_read_at",
    "ALTER TABLE conversations DROP COLUMN reply_read_id",
    "ALTER TABLE conversations DROP COLUMN timer_changed_at",
    "ALTER TABLE outbox_events DROP COLUMN no_inbox",
  };
  for (guint i = 0; i < G_N_ELEMENTS(v4_undo); i++)
    sql_exec(store, v4_undo[i]);
  sql_exec(store, "DELETE FROM schema_migrations WHERE version >= 2");
  sql_exec(store, "PRAGMA user_version = 1");
  gh_store_close(store);
}

static void
assert_migrated(const TestAccount *account)
{
  GhStore *store = store_open_flags(account, NULL, GH_STORE_OPEN_NONE);
  assert_integrity(store);
  g_assert_cmpint(sql_int(store, "PRAGMA user_version"), ==, GH_STORE_SCHEMA_VERSION);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM schema_migrations"), ==,
                  GH_STORE_SCHEMA_VERSION);
  for (guint i = 0; i < G_N_ELEMENTS(v2_tables); i++) {
    g_autofree gchar *sql = g_strdup_printf(
      "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = '%s'", v2_tables[i]);
    g_assert_cmpint(sql_int(store, sql), ==, 1);
  }
  MarmotStorage *s = storage_new(store);
  uint8_t *out = NULL;
  size_t out_len = 0;
  assert_marmot_ok(s->mls_load(s->ctx, "kp_slot", (const uint8_t *) "owner", 5, &out, &out_len));
  g_assert_cmpmem(out, out_len, "v1 state", 8);
  free(out);
  MarmotGroupId gid = gid_of("after migration");
  seed_group_state(s, &gid, 2, 0x70, TRUE);
  assert_marmot_ok(s->create_snapshot(s->ctx, &gid, "s"));
  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
}

static void
test_migration_v1_to_v2(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  g_assert_cmpint(GH_STORE_SCHEMA_VERSION, ==, 4);
  make_v1_store(&account);
  assert_migrated(&account);
  /* Reopening does not migrate again. */
  assert_migrated(&account);
  test_account_clear(&account);
}

static void
script_open(gpointer data)
{
  gh_store_close(store_open_flags(data, NULL, GH_STORE_OPEN_NONE));
}

/* H8: the migration is one transaction; a crash either side of its commit
 * leaves a store that opens as v2. */
static void
test_migration_crash(void)
{
  static const gchar *const cuts[] = { "migrate:before-commit", "migrate:after-commit" };
  for (guint i = 0; i < G_N_ELEMENTS(cuts); i++) {
    TestAccount account;
    test_account_init(&account, ACCOUNT_A);
    make_v1_store(&account);
    GhCrashOutcome outcome = gh_crash_harness_run(cuts[i], 1, script_open, &account);
    if (outcome != GH_CRASH_KILLED)
      g_error("Cut point %s: %s", cuts[i], gh_crash_outcome_to_string(outcome));
    assert_migrated(&account);
    /* Remove this account's store so the next cut starts from v1 again. */
    g_autoptr(GError) error = NULL;
    g_assert_true(gh_store_delete_files(NULL, account.pubkey, &error));
    g_assert_no_error(error);
    test_account_clear(&account);
  }
}

/* ---- libmarmot on GhStoreMarmot ------------------------------------------------------------ */

static void
nostr_keypair(uint8_t sk[32], uint8_t pk[32])
{
  secp256k1_context *ctx = secp256k1_context_create(TEST_SECP256K1_FLAGS);
  g_assert_nonnull(ctx);
  do
    randombytes_buf(sk, 32);
  while (!secp256k1_ec_seckey_verify(ctx, sk));
  secp256k1_keypair keypair;
  g_assert_true(secp256k1_keypair_create(ctx, &keypair, sk));
  secp256k1_xonly_pubkey xonly;
  g_assert_true(secp256k1_keypair_xonly_pub(ctx, &xonly, NULL, &keypair));
  g_assert_true(secp256k1_xonly_pubkey_serialize(ctx, pk, &xonly));
  secp256k1_context_destroy(ctx);
}

/* One Groundhog account: a Nostr identity, its encrypted store and, while
 * "running", a libmarmot client over GhStoreMarmot. */
typedef struct {
  uint8_t sk[32];
  uint8_t pk[32];
  TestAccount account;
  GhStore *store;
  MarmotStorage *storage; /* owned by marmot */
  Marmot *marmot;
} Actor;

static void
actor_init(Actor *actor)
{
  memset(actor, 0, sizeof *actor);
  nostr_keypair(actor->sk, actor->pk);
  g_autofree gchar *pubkey = hex32(actor->pk);
  test_account_init(&actor->account, pubkey);
}

static void
actor_start(Actor *actor)
{
  g_assert_null(actor->store);
  actor->store = store_open(&actor->account, NULL);
  actor->storage = storage_new(actor->store);
  actor->marmot = marmot_new(actor->storage);
  g_assert_nonnull(actor->marmot);
}

/* A quit: the client goes, the store is closed (WAL truncated). */
static void
actor_stop(Actor *actor)
{
  g_autoptr(GError) error = gh_store_marmot_take_error(actor->storage);
  g_assert_no_error(error);
  marmot_free(actor->marmot);
  actor->marmot = NULL;
  actor->storage = NULL;
  gh_store_close(actor->store);
  actor->store = NULL;
}

static void
actor_clear(Actor *actor)
{
  if (actor->store)
    actor_stop(actor);
  sodium_memzero(actor->sk, sizeof actor->sk);
  test_account_clear(&actor->account);
}

/* Creates a group with @relays and returns its MLS group id. */
static MarmotGroupId
actor_create_group(Actor *actor, const gchar *name, const char **relays, size_t n_relays,
                   uint64_t *out_epoch)
{
  MarmotGroupConfig config = {
    .name = (char *) name,
    .description = (char *) "G23 end to end",
    .admin_pubkeys = (uint8_t (*)[32]) actor->pk,
    .admin_count = 1,
    .relay_urls = (char **) relays,
    .relay_count = n_relays,
  };
  MarmotCreateGroupResult created;
  memset(&created, 0, sizeof created);
  assert_marmot_ok(marmot_create_group(actor->marmot, actor->pk, NULL, 0, &config, &created));
  MarmotGroupId gid = marmot_group_id_new(created.group->mls_group_id.data,
                                          created.group->mls_group_id.len);
  if (out_epoch)
    *out_epoch = created.group->epoch;
  marmot_create_group_result_free(&created);
  return gid;
}

static gchar *
actor_key_package(Actor *actor)
{
  MarmotKeyPackageResult kp;
  memset(&kp, 0, sizeof kp);
  assert_marmot_ok(marmot_create_key_package(actor->marmot, actor->pk, actor->sk, NULL, 0, &kp));
  gchar *json = g_strdup(kp.event_json);
  marmot_key_package_result_free(&kp);
  return json;
}

static void
free_strings_n(char **strings, size_t n)
{
  for (size_t i = 0; i < n; i++)
    free(strings[i]);
  free(strings);
}

/* Every label libmarmot has written must be classified: "mls_group",
 * "mls_group_parent", "mls_group_pending" and "mls_group_welcomes" are group
 * state (snapshots copy
 * them); the others are
 * account-scoped and must
 * stay out of group snapshots. A new label fails here, to be classified in
 * gh-store-marmot.c before it can silently escape (or join) a snapshot. */
static void
assert_labels_classified(GhStore *store)
{
  static const gchar *const known[] = {
    "mls_group", "mls_group_parent", "mls_group_pending", "mls_group_welcomes",
    "kp_slot", "kp_priv",
    "kp_full", "welcome_data", NULL,
  };
  g_autofree gchar *labels = sql_text(store, "SELECT group_concat(label, ',') FROM "
                                             "(SELECT DISTINCT label FROM mls_kv ORDER BY label)");
  g_assert_nonnull(labels);
  g_auto(GStrv) list = g_strsplit(labels, ",", -1);
  for (guint i = 0; list[i]; i++)
    if (!g_strv_contains(known, list[i]))
      g_error("libmarmot wrote an unclassified mls_kv label '%s'", list[i]);
}

static void
assert_app_message(Actor *receiver, const gchar *event_json, const gchar *expected)
{
  MarmotMessageResult result;
  memset(&result, 0, sizeof result);
  assert_marmot_ok(marmot_process_message(receiver->marmot, event_json, &result));
  g_assert_cmpint(result.type, ==, MARMOT_RESULT_APPLICATION_MESSAGE);
  g_assert_nonnull(result.app_msg.inner_event_json);
  g_assert_nonnull(strstr(result.app_msg.inner_event_json, expected));
  marmot_message_result_free(&result);
}

static gchar *
actor_message(Actor *sender, const MarmotGroupId *gid, const gchar *text)
{
  g_autofree gchar *inner =
    g_strdup_printf("{\"kind\":9,\"content\":\"%s\",\"created_at\":%" G_GINT64_FORMAT
                    ",\"tags\":[]}", text, (gint64) (g_get_real_time() / G_USEC_PER_SEC));
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  assert_marmot_ok(marmot_create_message(sender->marmot, gid, inner, &out));
  gchar *json = g_strdup(out.event_json);
  marmot_outgoing_message_free(&out);
  return json;
}

/* End-to-end smoke: a libmarmot client per account on its encrypted store;
 * create a group, add a member by KeyPackage, and every step continues after
 * the stores were closed and reopened (persistence across close/open). */
static void
test_e2e_persistence(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  const char *relays[] = { RELAY_ONE, RELAY_TWO };

  /* Bob publishes a KeyPackage and quits. */
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);

  /* Alice creates a group and quits. */
  actor_start(&alice);
  uint64_t epoch0 = 0;
  MarmotGroupId gid = actor_create_group(&alice, "G23 canary group", relays, 2, &epoch0);
  actor_stop(&alice);

  /* After a restart the group is still there, and Alice adds Bob by his
   * KeyPackage: that commit needs the MLS state from the reopened store. */
  actor_start(&alice);
  MarmotGroup *group = NULL;
  assert_marmot_ok(marmot_get_group(alice.marmot, &gid, &group));
  g_assert_nonnull(group);
  g_assert_cmpstr(group->name, ==, "G23 canary group");
  g_assert_cmpuint(group->epoch, ==, epoch0);
  g_assert_cmpuint(group->admin_count, ==, 1);
  g_assert_cmpmem(group->admin_pubkeys, 32, alice.pk, 32);
  marmot_group_free(group);
  MarmotGroupRelay *group_relays = NULL;
  size_t n_relays = 0;
  assert_marmot_ok(marmot_get_group_relay_urls(alice.marmot, &gid, &group_relays, &n_relays));
  g_assert_cmpuint(n_relays, ==, 2);
  g_assert_cmpstr(group_relays[0].relay_url, ==, RELAY_ONE);
  g_assert_cmpstr(group_relays[1].relay_url, ==, RELAY_TWO);
  for (size_t i = 0; i < n_relays; i++) {
    free(group_relays[i].relay_url);
    marmot_group_id_free(&group_relays[i].mls_group_id);
  }
  free(group_relays);
  const char *kps[] = { bob_kp };
  char **welcomes = NULL;
  size_t n_welcomes = 0;
  char *commit = NULL;
  assert_marmot_ok(marmot_add_members(alice.marmot, &gid, kps, 1, &welcomes, &n_welcomes, &commit));
  /* A relay accepted the Commit (libmarmot 0.5.0 merges only then). */
  assert_marmot_ok(marmot_merge_pending_commit(alice.marmot, &gid));
  g_assert_cmpuint(n_welcomes, ==, 1);
  g_assert_nonnull(commit);
  actor_stop(&alice);

  /* Bob restarts and processes the Welcome with the KeyPackage keys from his
   * store; the pending Welcome survives another restart before he accepts. */
  actor_start(&bob);
  uint8_t wrapper[32];
  randombytes_buf(wrapper, sizeof wrapper);
  MarmotWelcome *welcome = NULL;
  assert_marmot_ok(marmot_process_welcome(bob.marmot, wrapper, welcomes[0], &welcome));
  marmot_welcome_free(welcome);
  actor_stop(&bob);
  actor_start(&bob);
  MarmotWelcome **pending = NULL;
  size_t n_pending = 0;
  assert_marmot_ok(marmot_get_pending_welcomes(bob.marmot, NULL, &pending, &n_pending));
  g_assert_cmpuint(n_pending, ==, 1);
  g_assert_cmpmem(pending[0]->wrapper_event_id, 32, wrapper, 32);
  assert_marmot_ok(marmot_accept_welcome(bob.marmot, pending[0]));
  for (size_t i = 0; i < n_pending; i++)
    marmot_welcome_free(pending[i]);
  free(pending);
  actor_stop(&bob);

  /* Both restart: messages decrypt in both directions at the same epoch. */
  actor_start(&alice);
  actor_start(&bob);
  MarmotGroup *alice_view = NULL, *bob_view = NULL;
  assert_marmot_ok(marmot_get_group(alice.marmot, &gid, &alice_view));
  assert_marmot_ok(marmot_get_group(bob.marmot, &gid, &bob_view));
  g_assert_nonnull(alice_view);
  g_assert_nonnull(bob_view);
  g_assert_cmpuint(alice_view->epoch, ==, epoch0 + 1);
  g_assert_cmpuint(bob_view->epoch, ==, alice_view->epoch);
  g_assert_cmpstr(bob_view->name, ==, "G23 canary group");
  marmot_group_free(alice_view);
  marmot_group_free(bob_view);
  g_autofree gchar *to_bob = actor_message(&alice, &gid, "hello Bob, canary one");
  assert_app_message(&bob, to_bob, "hello Bob, canary one");
  g_autofree gchar *to_alice = actor_message(&bob, &gid, "hello Alice, canary two");
  assert_app_message(&alice, to_alice, "hello Alice, canary two");
  /* The accepted Welcome's raw data is gone; its record says accepted. */
  g_assert_cmpint(sql_int(bob.store, "SELECT count(*) FROM mls_kv WHERE label = 'welcome_data'"),
                  ==, 0);
  g_assert_cmpint(sql_int(bob.store, "SELECT state FROM mls_welcomes"), ==,
                  MARMOT_WELCOME_STATE_ACCEPTED);
  assert_labels_classified(alice.store);
  assert_labels_classified(bob.store);
  assert_integrity(alice.store);
  assert_integrity(bob.store);
  actor_stop(&alice);
  actor_stop(&bob);

  free_strings_n(welcomes, n_welcomes);
  free(commit);
  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

/* Snapshots of real libmarmot state: a Commit (metadata update, new epoch)
 * rolled back leaves mls_kv byte-identical and a working group. */
static void
test_snapshot_libmarmot_state(void)
{
  Actor alice;
  actor_init(&alice);
  actor_start(&alice);
  const char *relays[] = { RELAY_ONE };
  uint64_t epoch0 = 0;
  MarmotGroupId gid = actor_create_group(&alice, "Before", relays, 1, &epoch0);
  g_autofree gchar *kv_before = kv_dump(alice.store);
  g_autofree gchar *state_before = group_state_dump(alice.store);

  MarmotStorage *s = alice.storage;
  assert_marmot_ok(s->create_snapshot(s->ctx, &gid, "pending-commit"));
  MarmotGroupConfig update = {
    .name = (char *) "After",
    .admin_pubkeys = (uint8_t (*)[32]) alice.pk,
    .admin_count = 1,
  };
  g_autofree gchar *commit_json = NULL;
  assert_marmot_ok(marmot_update_group_metadata(alice.marmot, &gid, &update, &commit_json));
  g_assert_nonnull(commit_json);
  assert_marmot_ok(marmot_merge_pending_commit(alice.marmot, &gid));
  MarmotGroup *group = NULL;
  assert_marmot_ok(marmot_get_group(alice.marmot, &gid, &group));
  g_assert_cmpuint(group->epoch, ==, epoch0 + 1);
  marmot_group_free(group);
  g_autofree gchar *kv_committed = kv_dump(alice.store);
  g_assert_cmpstr(kv_committed, !=, kv_before);

  /* The Commit lost the race: roll back. */
  assert_marmot_ok(s->rollback_snapshot(s->ctx, &gid, "pending-commit"));
  g_autofree gchar *kv_after = kv_dump(alice.store);
  g_autofree gchar *state_after = group_state_dump(alice.store);
  g_assert_cmpstr(kv_after, ==, kv_before);
  g_assert_cmpstr(state_after, ==, state_before);
  assert_marmot_ok(marmot_get_group(alice.marmot, &gid, &group));
  g_assert_cmpstr(group->name, ==, "Before");
  g_assert_cmpuint(group->epoch, ==, epoch0);
  marmot_group_free(group);

  /* The restored state is live: the group moves on from it, after a restart. */
  actor_stop(&alice);
  actor_start(&alice);
  g_autofree gchar *message = actor_message(&alice, &gid, "after rollback");
  g_assert_nonnull(message);
  g_autofree gchar *commit_json2 = NULL;
  assert_marmot_ok(marmot_update_group_metadata(alice.marmot, &gid, &update, &commit_json2));
  assert_marmot_ok(marmot_merge_pending_commit(alice.marmot, &gid));
  assert_marmot_ok(marmot_get_group(alice.marmot, &gid, &group));
  g_assert_cmpuint(group->epoch, ==, epoch0 + 1);
  g_assert_cmpstr(group->name, ==, "After");
  marmot_group_free(group);
  actor_stop(&alice);

  marmot_group_id_free(&gid);
  actor_clear(&alice);
}

/* ---- T-mls under the crash harness (H8) ------------------------------------------------------ */

typedef struct {
  TestAccount *account;
  const MarmotGroupId *gid;
  const gchar *key_package; /* the invitee's */
  const gchar *invitee;     /* hex pubkey */
} TmlsScript;

static gchar *
event_id_of(const gchar *json)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, json, -1);
}

/* The child: one GhStore transaction holding a snapshot, libmarmot's
 * add-member Commit (MLS state, group record, exporter secret) and the
 * outbox rows for the Commit and the Welcome. The libmarmot client is
 * created inside it too, so its startup snapshot pruning is part of it. */
static void
script_tmls_add_member(gpointer data)
{
  TmlsScript *t = data;
  GhStore *store = store_open_flags(t->account, NULL, GH_STORE_OPEN_NONE);
  MarmotStorage *storage = storage_new(store);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_begin(store, &error));
  Marmot *marmot = marmot_new(storage);
  g_assert_nonnull(marmot);
  assert_marmot_ok(storage->create_snapshot(storage->ctx, t->gid, "t-mls"));
  const char *kps[] = { t->key_package };
  char **welcomes = NULL;
  size_t n_welcomes = 0;
  char *commit = NULL;
  assert_marmot_ok(marmot_add_members(marmot, t->gid, kps, 1, &welcomes, &n_welcomes, &commit));
  assert_marmot_ok(marmot_merge_pending_commit(marmot, t->gid));

  g_autofree gchar *gid_hex = marmot_group_id_to_hex(t->gid);
  gint64 conversation = 0;
  g_assert_true(gh_store_ensure_conversation(store, GH_STORE_BACKEND_MLS, gid_hex,
                                             GH_STORE_REQUEST_ACCEPTED, &conversation, &error));
  g_autofree gchar *op_id = gh_store_new_op_id();
  g_autofree gchar *commit_id = event_id_of(commit);
  g_autofree gchar *welcome_id = event_id_of(welcomes[0]);
  GhStoreOutgoing outgoing = {
    .conversation_id = conversation,
    .op_id = op_id,
    .backend_msg_id = commit_id,
    .sender_pubkey = t->account->pubkey,
    .kind = MARMOT_KIND_GROUP_MESSAGE,
    .created_at = (gint64) (g_get_real_time() / G_USEC_PER_SEC),
    .rumor_json = commit,
  };
  gint64 outbox = 0;
  g_assert_true(gh_store_enqueue(store, &outgoing, &outbox, NULL, &error));
  const gchar *targets[] = { RELAY_ONE, RELAY_TWO, NULL };
  GhStoreSealedEvent events[] = {
    { .role = GH_STORE_OUTBOX_ROLE_MLS_MESSAGE, .event_id = commit_id, .event_json = commit,
      .relay_urls = targets },
    { .role = GH_STORE_OUTBOX_ROLE_WELCOME_WRAP, .target_pubkey = t->invitee,
      .event_id = welcome_id, .event_json = welcomes[0], .relay_urls = targets },
  };
  g_assert_true(gh_store_seal(store, outbox, events, G_N_ELEMENTS(events), &error));
  g_assert_true(gh_store_commit(store, &error));
  free_strings_n(welcomes, n_welcomes);
  free(commit);
  marmot_free(marmot);
  gh_store_close(store);
}

static void
script_tmls_rollback(gpointer data)
{
  TmlsScript *t = data;
  GhStore *store = store_open_flags(t->account, NULL, GH_STORE_OPEN_NONE);
  MarmotStorage *storage = storage_new(store);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_begin(store, &error));
  assert_marmot_ok(storage->rollback_snapshot(storage->ctx, t->gid, "t-mls"));
  sql_exec(store, "UPDATE outbox SET state = 7"); /* the Commit is cancelled with it */
  g_assert_true(gh_store_commit(store, &error));
  marmot_storage_free(storage);
  gh_store_close(store);
}

static void
expect_killed(const gchar *cut, GhCrashScript script, gpointer data)
{
  GhCrashOutcome outcome = gh_crash_harness_run(cut, 1, script, data);
  if (outcome != GH_CRASH_KILLED)
    g_error("Cut point %s: %s", cut, gh_crash_outcome_to_string(outcome));
}

static gchar *
reopen_dump(TestAccount *account, gchar **out_group_state)
{
  GhStore *store = store_open_flags(account, NULL, GH_STORE_OPEN_NONE);
  assert_integrity(store);
  gchar *dump = db_dump(store, "%");
  if (out_group_state)
    *out_group_state = group_state_dump(store);
  gh_store_close(store);
  return dump;
}

/* Acceptance (§8.2 G23, §3.5 T-mls): MLS staged state, its snapshot and the
 * outbox events of the Commit commit together or not at all. */
static void
test_tmls_crash_atomicity(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  const char *relays[] = { RELAY_ONE, RELAY_TWO };
  actor_start(&alice);
  uint64_t epoch0 = 0;
  MarmotGroupId gid = actor_create_group(&alice, "T-mls", relays, 2, &epoch0);
  actor_stop(&alice);

  g_autofree gchar *bob_hex = hex32(bob.pk);
  TmlsScript script = { &alice.account, &gid, bob_kp, bob_hex };
  g_autofree gchar *group_state0 = NULL;
  g_autofree gchar *dump0 = reopen_dump(&alice.account, &group_state0);

  /* Killed anywhere before COMMIT: the whole database is as before, byte for
   * byte (MLS tables, snapshot, conversation, outbox, seen). The mid-points
   * come after the MLS writes, inside the nested T-enqueue and T-seal. */
  static const gchar *const before_commit[] = {
    "enqueue:outbox", "enqueue:message", "enqueue:seen", "enqueue:draft",
    "seal:event", "seal:targets", "seal:state", "txn:before-commit",
  };
  for (guint i = 0; i < G_N_ELEMENTS(before_commit); i++) {
    expect_killed(before_commit[i], script_tmls_add_member, &script);
    g_autofree gchar *dump = reopen_dump(&alice.account, NULL);
    if (g_strcmp0(dump, dump0) != 0)
      g_error("A crash at %s left part of the T-mls transaction behind", before_commit[i]);
  }

  /* Killed right after COMMIT: all of it is durable. */
  expect_killed("txn:after-commit", script_tmls_add_member, &script);
  GhStore *store = store_open_flags(&alice.account, NULL, GH_STORE_OPEN_NONE);
  assert_integrity(store);
  MarmotStorage *s = storage_new(store);
  MarmotGroup *group = NULL;
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, &gid, &group));
  g_assert_cmpuint(group->epoch, ==, epoch0 + 1);
  marmot_group_free(group);
  g_assert_cmpint(snapshot_count(store), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox WHERE state = 2"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_events"), ==, 2);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_targets"), ==, 4);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM conversations WHERE backend = 3"), ==, 1);
  g_autofree gchar *group_state1 = group_state_dump(store);
  g_assert_cmpstr(group_state1, !=, group_state0);
  marmot_storage_free(s);
  gh_store_close(store);

  /* Losing the Commit race is a T-mls too: the rollback and the outbox
   * change land together. Before COMMIT nothing moved... */
  g_autofree gchar *dump1 = reopen_dump(&alice.account, NULL);
  expect_killed("txn:before-commit", script_tmls_rollback, &script);
  g_autofree gchar *dump_rb = reopen_dump(&alice.account, NULL);
  g_assert_cmpstr(dump_rb, ==, dump1);
  /* ...after it, the group state is byte-identical to before the Commit. */
  expect_killed("txn:after-commit", script_tmls_rollback, &script);
  g_autofree gchar *group_state2 = NULL;
  g_autofree gchar *dump2 = reopen_dump(&alice.account, &group_state2);
  g_assert_cmpstr(dump2, !=, dump1);
  g_assert_cmpstr(group_state2, ==, group_state0);
  store = store_open_flags(&alice.account, NULL, GH_STORE_OPEN_NONE);
  g_assert_cmpint(snapshot_count(store), ==, 0);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox WHERE state = 7"), ==, 1);
  gh_store_close(store);

  /* The rolled-back group still works for libmarmot. */
  actor_start(&alice);
  g_autofree gchar *message = actor_message(&alice, &gid, "after the race");
  g_assert_nonnull(message);
  actor_stop(&alice);

  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

/* ---- No plaintext outside the encrypted database ----------------------------------------- */

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

static void
assert_file_clean(const gchar *path, const gchar *canary, const guint8 *secret)
{
  g_autoptr(GBytes) bytes = file_bytes(path);
  g_assert_nonnull(bytes);
  g_assert_cmpuint(g_bytes_get_size(bytes), >, 0);
  g_assert_false(bytes_contain(bytes, canary, strlen(canary)));
  g_assert_false(bytes_contain(bytes, secret, 32));
  g_assert_false(bytes_contain(bytes, "mls_group", 9));
}

static void
test_no_plaintext_on_disk(void)
{
  TestAccount account;
  test_account_init(&account, ACCOUNT_A);
  GhStore *store = store_open(&account, NULL);
  MarmotStorage *s = storage_new(store);
  g_autofree gchar *nonce = gh_store_new_op_id();
  g_autofree gchar *canary = g_strdup_printf("GROUNDHOG-MLS-CANARY-%s", nonce);
  uint8_t secret[32];
  randombytes_buf(secret, sizeof secret);

  MarmotGroupId gid = gid_of(canary);
  MarmotGroup *group = full_group(&gid, canary);
  assert_marmot_ok(s->save_group(s->ctx, group));
  assert_marmot_ok(s->save_exporter_secret(s->ctx, &gid, 1, secret));
  assert_marmot_ok(s->mls_store(s->ctx, "mls_group", gid.data, gid.len, secret, 32));
  assert_marmot_ok(s->mls_store(s->ctx, "kp_priv", secret, 32, (const uint8_t *) canary,
                                strlen(canary)));
  const char *relays[] = { canary };
  assert_marmot_ok(s->replace_group_relays(s->ctx, &gid, relays, 1));
  for (guint i = 0; i < 20; i++) {
    MarmotMessage *m = message_at(&gid, (guint8) i, T0 + i, T0 + i);
    free(m->content);
    m->content = strdup(canary);
    m->event_json = strdup(canary);
    assert_marmot_ok(s->save_message(s->ctx, m));
    marmot_message_free(m);
  }
  MarmotWelcome *w = make_test_welcome(&gid, 1);
  free(w->group_name);
  w->group_name = strdup(canary);
  assert_marmot_ok(s->save_welcome(s->ctx, w));
  marmot_welcome_free(w);
  assert_marmot_ok(s->create_snapshot(s->ctx, &gid, canary));
  assert_marmot_ok(s->save_processed_welcome(s->ctx, secret, NULL, T0, 3, canary));

  g_autofree gchar *path = g_strdup(gh_store_get_path(store));
  g_autofree gchar *wal = g_strconcat(path, "-wal", NULL);
  g_autofree gchar *shm = g_strconcat(path, "-shm", NULL);
  assert_file_clean(path, canary, secret);
  assert_file_clean(wal, canary, secret);
  assert_file_clean(shm, canary, secret);
  /* Nothing else appeared beside the database. */
  g_autofree gchar *dir = g_path_get_dirname(path);
  g_autoptr(GDir) listing = g_dir_open(dir, 0, NULL);
  g_assert_nonnull(listing);
  const gchar *name;
  while ((name = g_dir_read_name(listing)))
    g_assert_true(g_str_has_prefix(name, "store.db"));

  marmot_group_free(group);
  marmot_group_id_free(&gid);
  marmot_storage_free(s);
  gh_store_close(store);
  assert_file_clean(path, canary, secret);
  test_account_clear(&account);
}

/* ==== nostrc-qp24.7: the durable MLS Commit lifecycle ===================================== */

/* ---- Stored state, read the way libmarmot writes it -------------------------------------- */

static gboolean
kv_load(GhStore *store, const gchar *label, const MarmotGroupId *gid, GBytes **out)
{
  sqlite3_stmt *stmt = NULL;
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store),
                                     "SELECT value FROM mls_kv WHERE label = ?1 AND key = ?2",
                                     -1, &stmt, NULL), ==, SQLITE_OK);
  sqlite3_bind_text(stmt, 1, label, -1, SQLITE_STATIC);
  sqlite3_bind_blob(stmt, 2, gid->data, (int) gid->len, SQLITE_STATIC);
  int rc = sqlite3_step(stmt);
  g_assert_true(rc == SQLITE_ROW || rc == SQLITE_DONE);
  *out = rc == SQLITE_ROW ? g_bytes_new(sqlite3_column_blob(stmt, 0),
                                        (gsize) sqlite3_column_bytes(stmt, 0))
                          : NULL;
  sqlite3_finalize(stmt);
  return *out != NULL;
}

/* An MLS variable-length integer (RFC 9420 §2.1.2): its value, and its size
 * in *out_size. */
static guint64
read_vli(const guint8 *p, gsize avail, gsize *out_size)
{
  g_assert_cmpuint(avail, >=, 1);
  gsize size = (gsize) 1 << (p[0] >> 6);
  g_assert_cmpuint(size, <=, 4);
  g_assert_cmpuint(avail, >=, size);
  guint64 v = p[0] & 0x3f;
  for (gsize i = 1; i < size; i++)
    v = v << 8 | p[i];
  *out_size = size;
  return v;
}

static guint64
be64(const guint8 *p)
{
  guint64 v = 0;
  for (int i = 0; i < 8; i++)
    v = v << 8 | p[i];
  return v;
}

/* The epoch in a libmarmot MLS state blob (mls_group_serialize(): u32 magic,
 * u32 version, the group id with a variable-length size, u64 epoch). */
static guint64
state_epoch(GBytes *state)
{
  gsize len = 0;
  const guint8 *p = g_bytes_get_data(state, &len);
  g_assert_cmpuint(len, >=, 9);
  gsize vli_size = 0;
  guint64 gid_len = read_vli(p + 8, len - 8, &vli_size);
  gsize at = 8 + vli_size + (gsize) gid_len;
  g_assert_cmpuint(len, >=, at + 8);
  return be64(p + at);
}

/* The parent epoch of a retained-parent record (u8 version, u64 epoch, ...). */
static guint64
parent_epoch(GBytes *parent)
{
  gsize len = 0;
  const guint8 *p = g_bytes_get_data(parent, &len);
  g_assert_cmpuint(len, >=, 9);
  g_assert_cmpuint(p[0], ==, 1);
  return be64(p + 1);
}

/* No partial epoch anywhere: every group's record, MLS state, exporter
 * secret and retained parent belong to one transition. */
static void
assert_groups_consistent(GhStore *store)
{
  assert_integrity(store);
  MarmotStorage *s = storage_new(store);
  MarmotGroup **groups = NULL;
  size_t n = 0;
  assert_marmot_ok(s->all_groups(s->ctx, &groups, &n));
  for (size_t i = 0; i < n; i++) {
    const MarmotGroupId *gid = &groups[i]->mls_group_id;
    g_autoptr(GBytes) state = NULL, parent = NULL;
    g_assert_true(kv_load(store, "mls_group", gid, &state));
    guint64 epoch = state_epoch(state);
    if (epoch != groups[i]->epoch)
      g_error("Partial epoch: group record at %" G_GUINT64_FORMAT ", MLS state at %"
              G_GUINT64_FORMAT, groups[i]->epoch, epoch);
    uint8_t secret[32];
    assert_marmot_ok(s->get_exporter_secret(s->ctx, gid, epoch, secret));
    sodium_memzero(secret, sizeof secret);
    if (kv_load(store, "mls_group_parent", gid, &parent) && parent_epoch(parent) + 1 != epoch)
      g_error("Partial epoch: retained parent of %" G_GUINT64_FORMAT " under state %"
              G_GUINT64_FORMAT, parent_epoch(parent), epoch);
    marmot_group_free(groups[i]);
  }
  free(groups);
  marmot_storage_free(s);
}

static guint64
group_epoch(GhStore *store, const MarmotGroupId *gid)
{
  MarmotStorage *s = storage_new(store);
  MarmotGroup *group = NULL;
  assert_marmot_ok(s->find_group_by_mls_id(s->ctx, gid, &group));
  g_assert_nonnull(group);
  guint64 epoch = group->epoch;
  marmot_group_free(group);
  marmot_storage_free(s);
  return epoch;
}

/* ---- Crash cases: cut an operation at every write and at its commit ----------------------- */

typedef struct _CrashCase CrashCase;
typedef void (*CrashOpFunc)(GhStore *store, MarmotStorage *storage, Marmot *marmot,
                            CrashCase *c);
typedef void (*CrashCheckFunc)(GhStore *store, CrashCase *c);

struct _CrashCase {
  const gchar *name;
  TestAccount *account;
  const gchar *label;          /* the operation's outermost transaction: "mls" or "txn" */
  CrashOpFunc op;
  CrashCheckFunc check_after;  /* its effect, once committed */
  gpointer data;
  const gchar *const *extra_cuts; /* the store's own cut points inside it (T-enqueue, T-seal) */
  guint min_writes;            /* MLS writes it must make (marmot_new()'s pruning is one) */
  guint writes;                /* how many writes it made (set by crash_case_run) */
};

static void
script_crash_op(gpointer user_data)
{
  CrashCase *c = user_data;
  GhStore *store = store_open_flags(c->account, NULL, GH_STORE_OPEN_NONE);
  MarmotStorage *storage = storage_new(store);
  Marmot *marmot = marmot_new(storage);
  g_assert_nonnull(marmot);
  c->op(store, storage, marmot, c);
  marmot_free(marmot);
  gh_store_close(store);
}

typedef struct {
  gchar *path;
  GBytes *bytes;   /* NULL: the file did not exist */
} FileCopy;

static void
file_copy_free(gpointer p)
{
  FileCopy *f = p;
  g_free(f->path);
  g_clear_pointer(&f->bytes, g_bytes_unref);
  g_free(f);
}

static gchar *
account_store_path(TestAccount *account)
{
  GhStore *store = store_open_flags(account, NULL, GH_STORE_OPEN_NONE);
  gchar *path = g_strdup(gh_store_get_path(store));
  gh_store_close(store);
  return path;
}

/* The store's files as they are now (closed: the WAL is checkpointed). */
static GPtrArray *
files_save(TestAccount *account)
{
  g_autofree gchar *path = account_store_path(account);
  GPtrArray *files = g_ptr_array_new_with_free_func(file_copy_free);
  const gchar *suffixes[] = { "", "-wal", "-shm", "-journal" };
  for (guint i = 0; i < G_N_ELEMENTS(suffixes); i++) {
    FileCopy *f = g_new0(FileCopy, 1);
    f->path = g_strconcat(path, suffixes[i], NULL);
    gchar *data = NULL;
    gsize len = 0;
    if (g_file_get_contents(f->path, &data, &len, NULL))
      f->bytes = g_bytes_new_take(data, len);
    g_ptr_array_add(files, f);
  }
  return files;
}

static void
files_restore(GPtrArray *files)
{
  for (guint i = 0; i < files->len; i++) {
    FileCopy *f = g_ptr_array_index(files, i);
    if (!f->bytes) {
      g_remove(f->path);
      continue;
    }
    gsize len = 0;
    const gchar *data = g_bytes_get_data(f->bytes, &len);
    g_autoptr(GError) error = NULL;
    g_assert_true(g_file_set_contents_full(f->path, data, (gssize) len,
                                           G_FILE_SET_CONTENTS_NONE, 0600, &error));
    g_assert_no_error(error);
  }
}

static gchar *
reopen_checked(CrashCase *c)
{
  GhStore *store = store_open_flags(c->account, NULL, GH_STORE_OPEN_NONE);
  assert_groups_consistent(store);
  gchar *dump = db_dump(store, "%");
  gh_store_close(store);
  return dump;
}

/* Runs @c's operation killed at each cut point in turn, each time from the
 * same saved store files, then once to completion (the scenario continues
 * from there):
 *  - after every write ("mls:write", 1st, 2nd, ... until the operation
 *    completes without reaching it) and before its commit: the reopened
 *    database is byte-identical to before (all tables);
 *  - after its commit: the operation's effect is there (check_after);
 *  - every time: every group is consistent, no partial epoch. */
static void
crash_case_run(CrashCase *c)
{
  g_autoptr(GPtrArray) files = files_save(c->account);
  g_autofree gchar *dump0 = reopen_checked(c);
  guint n;
  for (n = 1; n < 1000; n++) {
    files_restore(files);
    GhCrashOutcome outcome = gh_crash_harness_run("mls:write", n, script_crash_op, c);
    if (outcome == GH_CRASH_COMPLETED)
      break;
    if (outcome != GH_CRASH_KILLED)
      g_error("%s, write %u: %s", c->name, n, gh_crash_outcome_to_string(outcome));
    g_autofree gchar *dump = reopen_checked(c);
    if (g_strcmp0(dump, dump0) != 0)
      g_error("%s: a crash after write %u of the operation left part of it behind",
              c->name, n);
  }
  c->writes = n - 1;
  /* Several records (the point of the transaction), each one cut. */
  if (c->writes < c->min_writes)
    g_error("%s made %u MLS writes, expected at least %u", c->name, c->writes, c->min_writes);

  for (guint i = 0; c->extra_cuts && c->extra_cuts[i]; i++) {
    files_restore(files);
    GhCrashOutcome outcome = gh_crash_harness_run(c->extra_cuts[i], 1, script_crash_op, c);
    if (outcome != GH_CRASH_KILLED)
      g_error("%s, %s: %s", c->name, c->extra_cuts[i], gh_crash_outcome_to_string(outcome));
    g_autofree gchar *dump = reopen_checked(c);
    if (g_strcmp0(dump, dump0) != 0)
      g_error("%s: a crash at %s left part of the operation behind", c->name, c->extra_cuts[i]);
  }

  g_autofree gchar *before_commit = g_strconcat(c->label, ":before-commit", NULL);
  g_autofree gchar *after_commit = g_strconcat(c->label, ":after-commit", NULL);
  guint killed = 0;
  for (n = 1; n < 100; n++) {
    files_restore(files);
    GhCrashOutcome outcome = gh_crash_harness_run(before_commit, n, script_crash_op, c);
    if (outcome == GH_CRASH_COMPLETED)
      break;
    if (outcome != GH_CRASH_KILLED)
      g_error("%s, %s #%u: %s", c->name, before_commit, n, gh_crash_outcome_to_string(outcome));
    killed++;
    g_autofree gchar *dump = reopen_checked(c);
    if (g_strcmp0(dump, dump0) != 0)
      g_error("%s: a crash before commit #%u left part of the operation behind", c->name, n);
  }
  g_assert_cmpuint(killed, >, 0);

  gboolean applied = FALSE;
  for (n = 1; n < 100; n++) {
    files_restore(files);
    GhCrashOutcome outcome = gh_crash_harness_run(after_commit, n, script_crash_op, c);
    if (outcome == GH_CRASH_COMPLETED)
      break;
    if (outcome != GH_CRASH_KILLED)
      g_error("%s, %s #%u: %s", c->name, after_commit, n, gh_crash_outcome_to_string(outcome));
    g_autofree gchar *dump = reopen_checked(c);
    if (g_strcmp0(dump, dump0) != 0) {
      /* Our commit: all of the operation is durable. */
      GhStore *store = store_open_flags(c->account, NULL, GH_STORE_OPEN_NONE);
      c->check_after(store, c);
      gh_store_close(store);
      applied = TRUE;
    }
  }
  if (!applied)
    g_error("%s: no commit of the operation was cut", c->name);

  /* The real run, from the same start. */
  files_restore(files);
  script_crash_op(c);
  GhStore *store = store_open_flags(c->account, NULL, GH_STORE_OPEN_NONE);
  assert_groups_consistent(store);
  c->check_after(store, c);
  gh_store_close(store);
  g_test_message("%s: %u writes, each cut", c->name, c->writes);
}

/* ---- The lifecycle's operations ---------------------------------------------------------- */

typedef struct {
  const char **kps;
  size_t n;
  char **welcomes;
  size_t n_welcomes;
} AddArgs;

static MarmotError
produce_add(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  AddArgs *a = data;
  return marmot_add_members(marmot, gid, a->kps, a->n, &a->welcomes, &a->n_welcomes, out);
}

static MarmotError
produce_rename(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  MarmotGroupConfig config = { .name = data };
  return marmot_update_group_metadata(marmot, gid, &config, out);
}

static MarmotError
produce_remove(Marmot *marmot, const MarmotGroupId *gid, gpointer data, char **out)
{
  return marmot_remove_members(marmot, gid, (const uint8_t (*)[32]) data, 1, out);
}

static GhMlsCommitPublish *
stage(Actor *actor, const MarmotGroupId *gid, GhMlsCommitProducer producer, gpointer data)
{
  g_autoptr(GError) error = NULL;
  GhMlsCommitPublish *publish = gh_mls_commit_stage(actor->store, actor->marmot,
                                                    actor->storage, gid,
                                                    actor->account.pubkey, producer, data,
                                                    &error);
  g_assert_no_error(error);
  g_assert_nonnull(publish);
  return publish;
}

static GhMlsCommitState
answer(Actor *actor, const GhMlsCommitPublish *publish, const gchar *relay, gboolean ok)
{
  g_autoptr(GError) error = NULL;
  GhMlsCommitState state = GH_MLS_COMMIT_PENDING;
  g_assert_true(gh_mls_commit_record_answer(actor->store, actor->marmot, actor->storage,
                                            publish, relay, ok, ok ? NULL : "blocked: test",
                                            &state, &error));
  g_assert_no_error(error);
  return state;
}

static GPtrArray *
resume(Actor *actor)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *list = gh_mls_commit_resume(actor->store, actor->marmot, actor->storage,
                                         actor->account.pubkey, &error);
  g_assert_no_error(error);
  g_assert_nonnull(list);
  return list;
}

static GhStoreOutboxEntry *
outbox_entry(GhStore *store, gint64 outbox_id)
{
  g_autoptr(GError) error = NULL;
  GhStoreOutboxEntry *entry = gh_store_outbox_load(store, outbox_id, &error);
  g_assert_no_error(error);
  g_assert_nonnull(entry);
  return entry;
}

/* Delivers @event_json to @actor; returns the result type. */
static MarmotMessageResultType
deliver_to(Actor *actor, const gchar *event_json, MarmotError *out_err)
{
  MarmotMessageResult result;
  memset(&result, 0, sizeof result);
  MarmotError err = marmot_process_message(actor->marmot, event_json, &result);
  MarmotMessageResultType type = result.type;
  marmot_message_result_free(&result);
  if (out_err)
    *out_err = err;
  else if (err != MARMOT_OK)
    g_error("process_message: %s", marmot_error_string(err));
  return type;
}

/* The unsent Welcomes of @gid's merged Adds, marked sent (the app gift-wraps
 * and sends them; here they are handed over directly). */
static gchar **
take_welcomes(Actor *actor, const MarmotGroupId *gid, gsize *out_n)
{
  MarmotUnsentWelcome *w = NULL;
  size_t n = 0;
  assert_marmot_ok(marmot_get_unsent_welcomes(actor->marmot, gid, &w, &n));
  gchar **out = g_new0(gchar *, n + 1);
  for (size_t i = 0; i < n; i++) {
    out[i] = g_strdup(w[i].rumor_json);
    assert_marmot_ok(marmot_mark_welcomes_sent(actor->marmot, gid,
                                               (const uint8_t (*)[32]) w[i].id, 1));
  }
  marmot_unsent_welcomes_free(w, n);
  *out_n = n;
  return out;
}

static void
actor_join(Actor *actor, const gchar *welcome_rumor)
{
  uint8_t wrapper[32];
  randombytes_buf(wrapper, sizeof wrapper);
  MarmotWelcome *welcome = NULL;
  assert_marmot_ok(marmot_process_welcome(actor->marmot, wrapper, welcome_rumor, &welcome));
  assert_marmot_ok(marmot_accept_welcome(actor->marmot, welcome));
  marmot_welcome_free(welcome);
}

/* ---- Crash suite: every libmarmot and lifecycle operation --------------------------------- */

typedef struct {
  MarmotGroupId *gid;
  AddArgs add;
  GhMlsCommitPublish *publish;      /* in: the Commit answered; out: the staged one */
  const gchar *relay;
  gboolean accepted;
  gchar *event_json;                /* an event to process */
  guint64 epoch_before;
  const gchar *expect_text;         /* in the processed message */
} OpData;

static void
op_stage_add(GhStore *store, MarmotStorage *storage, Marmot *marmot, CrashCase *c)
{
  OpData *d = c->data;
  AddArgs args = d->add;
  g_autoptr(GError) error = NULL;
  GhMlsCommitPublish *publish = gh_mls_commit_stage(store, marmot, storage, d->gid,
                                                    c->account->pubkey, produce_add, &args,
                                                    &error);
  g_assert_no_error(error);
  g_assert_nonnull(publish);
  free_strings_n(args.welcomes, args.n_welcomes);
  gh_mls_commit_publish_free(d->publish);
  d->publish = publish;   /* only the final (parent) run keeps it */
}

static void
check_staged(GhStore *store, CrashCase *c)
{
  OpData *d = c->data;
  g_autoptr(GBytes) pending = NULL;
  g_assert_true(kv_load(store, "mls_group_pending", d->gid, &pending));
  g_assert_cmpuint(group_epoch(store, d->gid), ==, d->epoch_before);   /* nothing applied */
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox WHERE backend = 3 AND "
                                 "state = 2"), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM outbox_targets"), ==, 2);
}

static void
op_answer(GhStore *store, MarmotStorage *storage, Marmot *marmot, CrashCase *c)
{
  OpData *d = c->data;
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_mls_commit_record_answer(store, marmot, storage, d->publish, d->relay,
                                            d->accepted, NULL, NULL, &error));
  g_assert_no_error(error);
}

static void
check_merged(GhStore *store, CrashCase *c)
{
  OpData *d = c->data;
  g_autoptr(GBytes) pending = NULL;
  g_assert_false(kv_load(store, "mls_group_pending", d->gid, &pending));
  g_assert_cmpuint(group_epoch(store, d->gid), ==, d->epoch_before + 1);
  g_autofree gchar *sql = g_strdup_printf(
    "SELECT count(*) FROM outbox_targets WHERE outcome = 1 AND relay_url = '%s'", d->relay);
  g_assert_cmpint(sql_int(store, sql), ==, 1);
}

static void
check_cleared(GhStore *store, CrashCase *c)
{
  OpData *d = c->data;
  g_autoptr(GBytes) pending = NULL;
  g_assert_false(kv_load(store, "mls_group_pending", d->gid, &pending));
  g_assert_cmpuint(group_epoch(store, d->gid), ==, d->epoch_before);
  g_autofree gchar *sql = g_strdup_printf(
    "SELECT count(*) FROM outbox WHERE id = %" G_GINT64_FORMAT " AND state = 7 AND "
    "last_error = '" GH_MLS_COMMIT_REFUSED_REASON "'", d->publish->outbox_id);
  g_assert_cmpint(sql_int(store, sql), ==, 1);
}

static void
op_process(GhStore *store, MarmotStorage *storage, Marmot *marmot, CrashCase *c)
{
  (void) store;
  (void) storage;
  OpData *d = c->data;
  MarmotMessageResult result;
  memset(&result, 0, sizeof result);
  assert_marmot_ok(marmot_process_message(marmot, d->event_json, &result));
  g_assert_true(result.type == MARMOT_RESULT_COMMIT ||
                result.type == MARMOT_RESULT_APPLICATION_MESSAGE);
  marmot_message_result_free(&result);
}

static void
check_advanced(GhStore *store, CrashCase *c)
{
  OpData *d = c->data;
  g_assert_cmpuint(group_epoch(store, d->gid), ==, d->epoch_before + 1);
}

static void
check_message_stored(GhStore *store, CrashCase *c)
{
  OpData *d = c->data;
  g_autofree gchar *sql = g_strdup_printf(
    "SELECT count(*) FROM mls_messages WHERE instr(content, '%s') > 0", d->expect_text);
  g_assert_cmpint(sql_int(store, sql), ==, 1);
  g_assert_cmpint(sql_int(store, "SELECT count(*) FROM mls_processed_messages"), >=, 1);
}

#define CRASH_WRAPPER_BYTE 0x5a

/* One operation: accepting the Welcome processed before (the join). */
static void
op_accept(GhStore *store, MarmotStorage *storage, Marmot *marmot, CrashCase *c)
{
  (void) store;
  (void) storage;
  (void) c;
  uint8_t wrapper[32];
  memset(wrapper, CRASH_WRAPPER_BYTE, sizeof wrapper);
  MarmotGroup *group = NULL;
  assert_marmot_ok(marmot_accept_welcome_by_wrapper_id(marmot, wrapper, &group));
  marmot_group_free(group);
}

static void
check_joined(GhStore *store, CrashCase *c)
{
  OpData *d = c->data;
  g_assert_cmpuint(group_epoch(store, d->gid), ==, d->epoch_before);
  g_assert_cmpint(sql_int(store, "SELECT state FROM mls_welcomes"), ==,
                  MARMOT_WELCOME_STATE_ACCEPTED);
}

static CrashCase
crash_case(const gchar *name, TestAccount *account, const gchar *label, CrashOpFunc op,
           CrashCheckFunc check_after, gpointer data, const gchar *const *extra_cuts)
{
  CrashCase c = {
    .name = name, .account = account, .label = label, .op = op,
    .check_after = check_after, .data = data, .extra_cuts = extra_cuts, .min_writes = 3,
  };
  return c;
}

static const gchar *const module_cuts[] = {
  "enqueue:outbox", "enqueue:message", "enqueue:seen", "enqueue:draft",
  "seal:event", "seal:targets", "seal:state", NULL,
};

/* Acceptance (qp24.7): crash-injection cuts at every record write of the
 * stage, the first and a later OK, the apply (merge), the rollback (clear),
 * a received Commit, a received message and a late one, and a join: each
 * operation is all or nothing, and no group is ever left in a partial epoch. */
static void
test_lifecycle_crash_every_write(void)
{
  Actor alice, bob, charlie;
  actor_init(&alice);
  actor_init(&bob);
  actor_init(&charlie);
  const char *relays[] = { RELAY_ONE, RELAY_TWO };

  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&charlie);
  g_autofree gchar *charlie_kp = actor_key_package(&charlie);
  actor_stop(&charlie);

  /* Alice creates the group with Bob. */
  actor_start(&alice);
  MarmotGroupConfig config = {
    .name = (char *) "Crash suite",
    .admin_pubkeys = (uint8_t (*)[32]) alice.pk,
    .admin_count = 1,
    .relay_urls = (char **) relays,
    .relay_count = 2,
  };
  const char *kps0[] = { bob_kp };
  MarmotCreateGroupResult created;
  memset(&created, 0, sizeof created);
  assert_marmot_ok(marmot_create_group(alice.marmot, alice.pk, kps0, 1, &config, &created));
  MarmotGroupId gid = marmot_group_id_new(created.group->mls_group_id.data,
                                          created.group->mls_group_id.len);
  guint64 epoch = created.group->epoch;
  g_autofree gchar *bob_welcome = g_strdup(created.welcome_rumor_jsons[0]);
  marmot_create_group_result_free(&created);
  actor_stop(&alice);

  OpData d = { .gid = &gid };
  CrashCase c;

  /* Bob joins (a Welcome join writes the state, secret, group, relays, ...). */
  actor_start(&bob);
  {
    uint8_t wrapper[32];
    memset(wrapper, CRASH_WRAPPER_BYTE, sizeof wrapper);
    MarmotWelcome *welcome = NULL;
    assert_marmot_ok(marmot_process_welcome(bob.marmot, wrapper, bob_welcome, &welcome));
    marmot_welcome_free(welcome);
  }
  actor_stop(&bob);
  d.epoch_before = epoch;
  c = crash_case("accept Welcome", &bob.account, "mls", op_accept, check_joined, &d, NULL);
  crash_case_run(&c);

  /* Alice stages an Add of Charlie (T-mls: pending Commit + outbox). */
  const char *kps1[] = { charlie_kp };
  d.add = (AddArgs) { .kps = kps1, .n = 1 };
  c = crash_case("stage Add", &alice.account, "txn", op_stage_add, check_staged, &d, module_cuts);
  c.min_writes = 2;   /* the pruning and the pending Commit; the outbox rows are cut above */
  crash_case_run(&c);
  g_assert_nonnull(d.publish);
  gchar *add_commit = g_strdup(d.publish->event_json);

  /* The first OK merges it; a later OK is only recorded. */
  d.relay = RELAY_TWO;
  d.accepted = TRUE;
  c = crash_case("first OK (merge)", &alice.account, "txn", op_answer, check_merged, &d, NULL);
  crash_case_run(&c);
  d.relay = RELAY_ONE;   /* check_merged still expects one past the staged epoch */
  c = crash_case("later OK", &alice.account, "txn", op_answer, check_merged, &d, NULL);
  c.min_writes = 1;   /* only the pruning: a later OK writes outbox rows, no MLS state */
  crash_case_run(&c);
  epoch++;

  /* Bob applies Alice's Commit from the relay. */
  d.event_json = add_commit;
  d.epoch_before = epoch - 1;
  c = crash_case("receive Commit", &bob.account, "mls", op_process, check_advanced, &d, NULL);
  crash_case_run(&c);

  /* Charlie joins through the Welcome outbox. */
  actor_start(&alice);
  gsize n_welcomes = 0;
  g_auto(GStrv) welcomes = take_welcomes(&alice, &gid, &n_welcomes);
  g_assert_cmpuint(n_welcomes, ==, 1);
  actor_stop(&alice);
  actor_start(&charlie);
  actor_join(&charlie, welcomes[0]);
  actor_stop(&charlie);

  /* A message Bob sends reaches Charlie: ratchet, message, marker, group. */
  actor_start(&bob);
  g_autofree gchar *msg = actor_message(&bob, &gid, "crash-suite live message");
  g_autofree gchar *late = actor_message(&bob, &gid, "crash-suite late message");
  actor_stop(&bob);
  d.event_json = msg;
  d.expect_text = "crash-suite live message";
  c = crash_case("receive message", &charlie.account, "mls", op_process, check_message_stored, &d, NULL);
  crash_case_run(&c);

  /* Alice renames (staged, merged on an OK); Charlie applies it, then reads
   * Bob's message of the previous epoch with the retained parent. */
  actor_start(&alice);
  g_autoptr(GhMlsCommitPublish) rename = stage(&alice, &gid, produce_rename, (gpointer) "Renamed");
  g_assert_cmpint(answer(&alice, rename, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_MERGED);
  actor_stop(&alice);
  actor_start(&charlie);
  g_assert_cmpint(deliver_to(&charlie, rename->event_json, NULL), ==, MARMOT_RESULT_COMMIT);
  actor_stop(&charlie);
  d.event_json = late;
  d.expect_text = "crash-suite late message";
  c = crash_case("receive late message", &charlie.account, "mls", op_process, check_message_stored, &d, NULL);
  crash_case_run(&c);

  /* Rollback: a Commit every relay refused is cleared with its entry. */
  actor_start(&alice);
  epoch = group_epoch(alice.store, &gid);
  g_autoptr(GhMlsCommitPublish) refused = stage(&alice, &gid, produce_rename,
                                                (gpointer) "Refused");
  g_assert_cmpint(answer(&alice, refused, RELAY_ONE, FALSE), ==, GH_MLS_COMMIT_PENDING);
  actor_stop(&alice);
  gh_mls_commit_publish_free(d.publish);
  d.publish = refused;
  refused = NULL;
  d.relay = RELAY_TWO;
  d.accepted = FALSE;
  d.epoch_before = epoch;
  c = crash_case("last refusal (clear)", &alice.account, "txn", op_answer, check_cleared, &d, NULL);
  c.min_writes = 2;   /* the pruning and the pending Commit's removal; plus the outbox */
  crash_case_run(&c);

  g_free(add_commit);
  gh_mls_commit_publish_free(d.publish);
  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
  actor_clear(&charlie);
}

/* ---- Restart, answers, echo, supersession -------------------------------------------------- */

/* Alice (admin, and @co_admin when given) creates a group on both relays with
 * @kps; returns its id and the Welcome rumors. */
static MarmotGroupId
create_group_with(Actor *alice, const gchar *name, const char **kps, size_t n_kps,
                  const uint8_t *co_admin, gchar ***out_welcomes)
{
  const char *relays[] = { RELAY_ONE, RELAY_TWO };
  uint8_t admins[2][32];
  memcpy(admins[0], alice->pk, 32);
  if (co_admin)
    memcpy(admins[1], co_admin, 32);
  MarmotGroupConfig config = {
    .name = (char *) name,
    .admin_pubkeys = admins,
    .admin_count = co_admin ? 2 : 1,
    .relay_urls = (char **) relays,
    .relay_count = 2,
  };
  MarmotCreateGroupResult created;
  memset(&created, 0, sizeof created);
  assert_marmot_ok(marmot_create_group(alice->marmot, alice->pk, kps, n_kps, &config, &created));
  MarmotGroupId gid = marmot_group_id_new(created.group->mls_group_id.data,
                                          created.group->mls_group_id.len);
  if (out_welcomes) {
    *out_welcomes = g_new0(gchar *, n_kps + 1);
    for (size_t i = 0; i < n_kps; i++)
      (*out_welcomes)[i] = g_strdup(created.welcome_rumor_jsons[i]);
  }
  marmot_create_group_result_free(&created);
  return gid;
}

static void
assert_relays(const GhMlsCommitPublish *publish, const gchar *const *want)
{
  g_assert_cmpuint(g_strv_length(publish->relay_urls), ==, g_strv_length((GStrv) want));
  for (guint i = 0; want[i]; i++)
    g_assert_cmpstr(publish->relay_urls[i], ==, want[i]);
}

/* ---- A send cut at every write and commit (nostrc-ai04) ------------------------------------ */

typedef struct {
  MarmotGroupId *gid;
  Actor *receiver;
  GPtrArray *receiver_files;   /* the receiver before any of the sends below */
  gchar *reference;            /* a send from the same start: the generation the op uses */
  gchar *sent;                 /* the op's event (kept from the real run) */
} SendCrash;

static const gchar SEND_INNER[] =
  "{\"kind\":9,\"content\":\"crash-suite send\",\"created_at\":1700000000,\"tags\":[]}";

static void
op_send(GhStore *store, MarmotStorage *storage, Marmot *marmot, CrashCase *c)
{
  (void) store;
  (void) storage;
  SendCrash *d = c->data;
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  assert_marmot_ok(marmot_create_message(marmot, d->gid, SEND_INNER, &out));
  g_free(d->sent);
  d->sent = g_strdup(out.event_json);
  marmot_outgoing_message_free(&out);
}

/* The op's ratchet step is durable: a send from the stored state now is read
 * after the reference (the op's generation, consumed first by a receiver). Had
 * the step been lost, that send would reuse the op's generation -- the same
 * key and nonce as an event that may already be out -- and the receiver, which
 * consumed that generation, would refuse it. */
static void
check_send_step_stored(GhStore *store, CrashCase *c)
{
  SendCrash *d = c->data;
  MarmotStorage *storage = storage_new(store);
  Marmot *marmot = marmot_new(storage);
  g_assert_nonnull(marmot);
  MarmotOutgoingMessage out;
  memset(&out, 0, sizeof out);
  assert_marmot_ok(marmot_create_message(marmot, d->gid, SEND_INNER, &out));
  g_autofree gchar *next = g_strdup(out.event_json);
  marmot_outgoing_message_free(&out);
  marmot_free(marmot);

  files_restore(d->receiver_files);
  actor_start(d->receiver);
  MarmotError err = MARMOT_OK;
  g_assert_cmpint(deliver_to(d->receiver, d->reference, &err), ==,
                  MARMOT_RESULT_APPLICATION_MESSAGE);
  g_assert_cmpint(deliver_to(d->receiver, next, &err), ==, MARMOT_RESULT_APPLICATION_MESSAGE);
  g_assert_cmpint(err, ==, MARMOT_OK);
  actor_stop(d->receiver);
}

/* Acceptance (nostrc-ai04): marmot_create_message() stores the advanced sender
 * ratchet in the operation's T-mls transaction before it returns the event.
 * Killed at any write or before the commit, nothing is stored and no event
 * left the process; killed after the commit (the event lost before it could
 * be published) or run to completion, the step is durable and the next send
 * uses a new generation. */
static void
test_send_crash_never_reuses_a_generation(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&alice);
  const char *kps[] = { bob_kp };
  g_auto(GStrv) welcomes = NULL;
  MarmotGroupId gid = create_group_with(&alice, "Send crash", kps, 1, NULL, &welcomes);
  g_autofree gchar *first = actor_message(&alice, &gid, "first");
  actor_stop(&alice);
  actor_start(&bob);
  actor_join(&bob, welcomes[0]);
  g_assert_cmpint(deliver_to(&bob, first, NULL), ==, MARMOT_RESULT_APPLICATION_MESSAGE);
  actor_stop(&bob);

  SendCrash d = { .gid = &gid, .receiver = &bob };
  d.receiver_files = files_save(&bob.account);
  /* The reference: what Alice's next send is, from the state the op starts in. */
  g_autoptr(GPtrArray) alice_files = files_save(&alice.account);
  actor_start(&alice);
  d.reference = actor_message(&alice, &gid, "reference");
  actor_stop(&alice);
  files_restore(alice_files);

  CrashCase c = crash_case("send message", &alice.account, "mls", op_send,
                           check_send_step_stored, &d, NULL);
  c.min_writes = 2;   /* marmot_new()'s pruning and the ratchet step */
  crash_case_run(&c);
  g_assert_nonnull(d.sent);

  g_free(d.sent);
  g_free(d.reference);
  g_ptr_array_unref(d.receiver_files);
  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

/* Acceptance (qp24.7): restart is idempotent and republishes the same signed
 * event from the pending record, on the relays that have not answered; a
 * refusal is not repeated; the relay echo merges; a late OK changes nothing. */
static void
test_lifecycle_restart_republishes(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&alice);
  const char *kps[] = { bob_kp };
  g_auto(GStrv) welcomes = NULL;
  MarmotGroupId gid = create_group_with(&alice, "Restart", kps, 1, NULL, &welcomes);
  guint64 epoch0 = group_epoch(alice.store, &gid);
  actor_stop(&alice);
  actor_start(&bob);
  actor_join(&bob, welcomes[0]);
  actor_stop(&bob);

  /* Staged; the app is killed before any relay answered (or it timed out). */
  actor_start(&alice);
  g_autoptr(GhMlsCommitPublish) staged = stage(&alice, &gid, produce_rename, (gpointer) "Renamed");
  static const gchar *const both[] = { RELAY_ONE, RELAY_TWO, NULL };
  assert_relays(staged, both);
  actor_stop(&alice);

  for (guint round = 0; round < 2; round++) {
    actor_start(&alice);
    g_autoptr(GPtrArray) list = resume(&alice);
    g_assert_cmpuint(list->len, ==, 1);
    GhMlsCommitPublish *again = g_ptr_array_index(list, 0);
    /* The same signed event, byte for byte: never re-signed or rebuilt. */
    g_assert_cmpstr(again->event_json, ==, staged->event_json);
    g_assert_cmpstr(again->event_id, ==, staged->event_id);
    g_assert_cmpint(again->outbox_id, ==, staged->outbox_id);
    g_assert_cmpint(again->outbox_event_id, ==, staged->outbox_event_id);
    assert_relays(again, both);
    g_assert_cmpint(sql_int(alice.store, "SELECT count(*) FROM outbox"), ==, 1);
    g_assert_cmpint(sql_int(alice.store, "SELECT count(*) FROM outbox_events"), ==, 1);
    g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0);   /* not applied */
    actor_stop(&alice);
  }

  /* Relay one refuses: it is not asked again; relay two still is. */
  actor_start(&alice);
  g_assert_cmpint(answer(&alice, staged, RELAY_ONE, FALSE), ==, GH_MLS_COMMIT_PENDING);
  actor_stop(&alice);
  actor_start(&alice);
  {
    g_autoptr(GPtrArray) list = resume(&alice);
    g_assert_cmpuint(list->len, ==, 1);
    static const gchar *const two[] = { RELAY_TWO, NULL };
    assert_relays(g_ptr_array_index(list, 0), two);
    g_assert_cmpstr(((GhMlsCommitPublish *) g_ptr_array_index(list, 0))->event_json, ==,
                    staged->event_json);
  }
  actor_stop(&alice);

  /* Relay two stored it but its OK was lost: Bob gets it, and so does
   * Alice, whose own echo merges it. */
  actor_start(&bob);
  g_assert_cmpint(deliver_to(&bob, staged->event_json, NULL), ==, MARMOT_RESULT_COMMIT);
  actor_stop(&bob);
  actor_start(&alice);
  g_assert_cmpint(deliver_to(&alice, staged->event_json, NULL), ==, MARMOT_RESULT_COMMIT);
  g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0 + 1);
  actor_stop(&alice);

  /* After a restart nothing is left to publish, and the entry settled. */
  actor_start(&alice);
  {
    g_autoptr(GPtrArray) list = resume(&alice);
    g_assert_cmpuint(list->len, ==, 0);
    g_autoptr(GhStoreOutboxEntry) entry = outbox_entry(alice.store, staged->outbox_id);
    g_assert_cmpint(entry->state, ==, GH_STORE_OUTBOX_SETTLED);
  }
  /* The lost OK arrives late: recorded, nothing else moves. */
  g_autofree gchar *before = group_state_dump(alice.store);
  g_assert_cmpint(answer(&alice, staged, RELAY_TWO, TRUE), ==, GH_MLS_COMMIT_MERGED);
  g_autofree gchar *after = group_state_dump(alice.store);
  g_assert_cmpstr(after, ==, before);
  actor_stop(&alice);

  /* Both are in the same epoch and talk. */
  actor_start(&alice);
  actor_start(&bob);
  g_assert_cmpuint(group_epoch(bob.store, &gid), ==, epoch0 + 1);
  g_autofree gchar *m1 = actor_message(&alice, &gid, "after the restart");
  assert_app_message(&bob, m1, "after the restart");
  g_autofree gchar *m2 = actor_message(&bob, &gid, "and back");
  assert_app_message(&alice, m2, "and back");
  actor_stop(&alice);
  actor_stop(&bob);

  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

/* First OK merges once; a Commit superseded by a competing one is dropped
 * (on resume, or when an OK comes), its entry cancelled and its Welcomes
 * never queued; an all-refused one is cleared. */
static void
test_lifecycle_superseded_and_refused(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  /* Bob's account key sorts below Alice's: between two privileged Commits
   * of the same epoch, Bob's wins. */
  while (memcmp(bob.pk, alice.pk, 32) > 0) {
    actor_clear(&bob);
    actor_init(&bob);
  }
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&alice);
  const char *kps[] = { bob_kp };
  g_auto(GStrv) welcomes = NULL;
  MarmotGroupId gid = create_group_with(&alice, "Race", kps, 1, bob.pk, &welcomes);
  guint64 epoch0 = group_epoch(alice.store, &gid);
  actor_stop(&alice);
  actor_start(&bob);
  actor_join(&bob, welcomes[0]);
  actor_stop(&bob);

  /* Both rename at once; Bob's reaches a relay first and wins. */
  actor_start(&alice);
  g_autoptr(GhMlsCommitPublish) ours = stage(&alice, &gid, produce_rename, (gpointer) "Alice's");
  actor_stop(&alice);
  actor_start(&bob);
  g_autoptr(GhMlsCommitPublish) theirs = stage(&bob, &gid, produce_rename, (gpointer) "Bob's");
  g_assert_cmpint(answer(&bob, theirs, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_MERGED);
  g_assert_cmpint(answer(&bob, theirs, RELAY_TWO, TRUE), ==, GH_MLS_COMMIT_MERGED);
  g_autoptr(GhStoreOutboxEntry) settled = outbox_entry(bob.store, theirs->outbox_id);
  g_assert_cmpint(settled->state, ==, GH_STORE_OUTBOX_SETTLED);
  actor_stop(&bob);

  actor_start(&alice);
  g_assert_cmpint(deliver_to(&alice, theirs->event_json, NULL), ==, MARMOT_RESULT_COMMIT);
  g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0 + 1);
  actor_stop(&alice);

  /* Restart: the superseded Commit is dropped, its entry cancelled. */
  actor_start(&alice);
  {
    g_autoptr(GPtrArray) list = resume(&alice);
    g_assert_cmpuint(list->len, ==, 0);
    g_autoptr(GhStoreOutboxEntry) entry = outbox_entry(alice.store, ours->outbox_id);
    g_assert_cmpint(entry->state, ==, GH_STORE_OUTBOX_CANCELLED);
    g_assert_cmpstr(entry->last_error, ==, GH_MLS_COMMIT_SUPERSEDED_REASON);
    g_autoptr(GBytes) pending = NULL;
    g_assert_false(kv_load(alice.store, "mls_group_pending", &gid, &pending));
  }
  /* An OK for it now changes nothing but its target. */
  g_autofree gchar *before = group_state_dump(alice.store);
  g_assert_cmpint(answer(&alice, ours, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_SUPERSEDED);
  g_autofree gchar *after = group_state_dump(alice.store);
  g_assert_cmpstr(after, ==, before);

  /* Without a restart: the next race's OK finds it superseded. */
  g_autoptr(GhMlsCommitPublish) ours2 = stage(&alice, &gid, produce_rename, (gpointer) "Alice 2");
  actor_stop(&alice);
  actor_start(&bob);
  g_autoptr(GhMlsCommitPublish) theirs2 = stage(&bob, &gid, produce_rename, (gpointer) "Bob 2");
  g_assert_cmpint(answer(&bob, theirs2, RELAY_TWO, TRUE), ==, GH_MLS_COMMIT_MERGED);
  actor_stop(&bob);
  actor_start(&alice);
  MarmotError err = MARMOT_OK;
  g_assert_cmpint(deliver_to(&alice, theirs2->event_json, &err), ==, MARMOT_RESULT_COMMIT);
  g_assert_cmpint(answer(&alice, ours2, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_SUPERSEDED);
  g_autoptr(GhStoreOutboxEntry) cancelled = outbox_entry(alice.store, ours2->outbox_id);
  g_assert_cmpint(cancelled->state, ==, GH_STORE_OUTBOX_CANCELLED);
  g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0 + 2);

  /* Every relay refuses the next one: cleared, entry cancelled. */
  g_autoptr(GhMlsCommitPublish) refused = stage(&alice, &gid, produce_rename, (gpointer) "No");
  g_assert_cmpint(answer(&alice, refused, RELAY_ONE, FALSE), ==, GH_MLS_COMMIT_PENDING);
  g_assert_cmpint(answer(&alice, refused, RELAY_TWO, FALSE), ==, GH_MLS_COMMIT_CLEARED);
  g_autoptr(GhStoreOutboxEntry) gone = outbox_entry(alice.store, refused->outbox_id);
  g_assert_cmpint(gone->state, ==, GH_STORE_OUTBOX_CANCELLED);
  g_assert_cmpstr(gone->last_error, ==, GH_MLS_COMMIT_REFUSED_REASON);
  g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0 + 2);
  {
    g_autoptr(GPtrArray) list = resume(&alice);
    g_assert_cmpuint(list->len, ==, 0);
  }
  actor_stop(&alice);

  /* They converged on Bob's branch. */
  actor_start(&alice);
  actor_start(&bob);
  g_autofree gchar *m = actor_message(&bob, &gid, "on Bob's branch");
  assert_app_message(&alice, m, "on Bob's branch");
  actor_stop(&alice);
  actor_stop(&bob);

  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

/* Acceptance (qp24.7): third parties converge, and a removed member is
 * isolated, with every step across restarts through the lifecycle. */
static void
test_lifecycle_three_members(void)
{
  Actor alice, bob, charlie;
  actor_init(&alice);
  actor_init(&bob);
  actor_init(&charlie);
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&charlie);
  g_autofree gchar *charlie_kp = actor_key_package(&charlie);
  actor_stop(&charlie);

  actor_start(&alice);
  MarmotGroupId gid = create_group_with(&alice, "Three", NULL, 0, NULL, NULL);
  const char *kps[] = { bob_kp, charlie_kp };
  AddArgs add = { .kps = kps, .n = 2 };
  g_autoptr(GhMlsCommitPublish) added = stage(&alice, &gid, produce_add, &add);
  free_strings_n(add.welcomes, add.n_welcomes);
  actor_stop(&alice);
  actor_start(&alice);   /* the OK comes after a restart */
  g_assert_cmpint(answer(&alice, added, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_MERGED);
  gsize n = 0;
  g_auto(GStrv) welcomes = take_welcomes(&alice, &gid, &n);
  g_assert_cmpuint(n, ==, 2);
  actor_stop(&alice);
  actor_start(&bob);
  actor_join(&bob, welcomes[0]);
  actor_stop(&bob);
  actor_start(&charlie);
  actor_join(&charlie, welcomes[1]);
  actor_stop(&charlie);

  Actor *all[] = { &alice, &bob, &charlie };
  for (guint i = 0; i < 3; i++)
    actor_start(all[i]);
  guint64 epoch = group_epoch(alice.store, &gid);
  for (guint s = 0; s < 3; s++) {
    g_assert_cmpuint(group_epoch(all[s]->store, &gid), ==, epoch);
    g_autofree gchar *text = g_strdup_printf("from member %u", s);
    g_autofree gchar *m = actor_message(all[s], &gid, text);
    for (guint r = 0; r < 3; r++)
      if (r != s)
        assert_app_message(all[r], m, text);
  }
  for (guint i = 0; i < 3; i++)
    actor_stop(all[i]);

  /* Alice removes Charlie. */
  actor_start(&alice);
  g_autoptr(GhMlsCommitPublish) removed = stage(&alice, &gid, produce_remove, charlie.pk);
  g_assert_cmpint(answer(&alice, removed, RELAY_TWO, TRUE), ==, GH_MLS_COMMIT_MERGED);
  actor_stop(&alice);
  actor_start(&bob);
  g_assert_cmpint(deliver_to(&bob, removed->event_json, NULL), ==, MARMOT_RESULT_COMMIT);
  actor_stop(&bob);
  actor_start(&charlie);
  MarmotError err = MARMOT_OK;
  (void) deliver_to(&charlie, removed->event_json, &err);   /* whatever it makes of it */
  actor_stop(&charlie);

  actor_start(&alice);
  actor_start(&bob);
  actor_start(&charlie);
  g_assert_cmpuint(group_epoch(bob.store, &gid), ==, epoch + 1);
  g_autofree gchar *secret = actor_message(&alice, &gid, "Charlie must not read this");
  assert_app_message(&bob, secret, "Charlie must not read this");
  MarmotMessageResult result;
  memset(&result, 0, sizeof result);
  err = marmot_process_message(charlie.marmot, secret, &result);
  g_assert_cmpint(err, !=, MARMOT_OK);
  g_assert_null(result.app_msg.inner_event_json);
  marmot_message_result_free(&result);
  g_autofree gchar *reply = actor_message(&bob, &gid, "nor this");
  assert_app_message(&alice, reply, "nor this");
  memset(&result, 0, sizeof result);
  g_assert_cmpint(marmot_process_message(charlie.marmot, reply, &result), !=, MARMOT_OK);
  marmot_message_result_free(&result);
  actor_stop(&alice);
  actor_stop(&bob);
  actor_stop(&charlie);

  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
  actor_clear(&charlie);
}

/* Acceptance (qp24.7): a full disk fails closed (GH_STORE_ERROR_FULL,
 * nothing of the step stored, the Commit neither staged nor merged), and so
 * does a wrong or locked key (no store, so no MLS state at all). */
static void
test_lifecycle_disk_full_and_key(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&alice);
  const char *kps[] = { bob_kp };
  g_auto(GStrv) welcomes = NULL;
  MarmotGroupId gid = create_group_with(&alice, "Full", kps, 1, NULL, &welcomes);
  guint64 epoch0 = group_epoch(alice.store, &gid);

  /* No room to grow: staging fails with FULL and stores nothing. */
  g_autofree gchar *before = db_dump(alice.store, "%");
  gint64 pages = sql_int(alice.store, "PRAGMA page_count");
  g_autofree gchar *limit = g_strdup_printf("PRAGMA max_page_count = %" G_GINT64_FORMAT, pages);
  g_assert_cmpint(sql_int(alice.store, limit), ==, pages);
  g_autoptr(GError) error = NULL;
  GhMlsCommitPublish *none = gh_mls_commit_stage(alice.store, alice.marmot, alice.storage, &gid,
                                                 alice.account.pubkey, produce_rename,
                                                 (gpointer) "No room", &error);
  g_assert_null(none);
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL);
  g_clear_error(&error);
  g_assert_cmpuint(gh_store_get_transaction_depth(alice.store), ==, 0);
  g_autofree gchar *after = db_dump(alice.store, "%");
  g_assert_cmpstr(after, ==, before);

  /* With room, it stages; full again, the first OK cannot merge: nothing of
   * it is recorded and the Commit stays pending. */
  g_assert_cmpint(sql_int(alice.store, "PRAGMA max_page_count = 1000000"), >=, 1000000);
  g_autoptr(GhMlsCommitPublish) staged = stage(&alice, &gid, produce_rename, (gpointer) "Room");
  g_autofree gchar *staged_dump = db_dump(alice.store, "%");
  pages = sql_int(alice.store, "PRAGMA page_count");
  g_free(limit);
  limit = g_strdup_printf("PRAGMA max_page_count = %" G_GINT64_FORMAT, pages);
  g_assert_cmpint(sql_int(alice.store, limit), ==, pages);
  GhMlsCommitState state = GH_MLS_COMMIT_PENDING;
  if (gh_mls_commit_record_answer(alice.store, alice.marmot, alice.storage, staged, RELAY_ONE,
                                  TRUE, NULL, &state, &error)) {
    /* The merge fit into free pages: then it must be complete. */
    g_assert_no_error(error);
    g_assert_cmpint(state, ==, GH_MLS_COMMIT_MERGED);
    g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0 + 1);
    g_test_message("the merge needed no new page; FULL covered by the stage");
  } else {
    g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL);
    g_clear_error(&error);
    g_autofree gchar *not_merged = db_dump(alice.store, "%");
    g_assert_cmpstr(not_merged, ==, staged_dump);
    g_assert_cmpuint(group_epoch(alice.store, &gid), ==, epoch0);
    g_assert_cmpint(sql_int(alice.store, "PRAGMA max_page_count = 1000000"), >=, 1000000);
    g_assert_cmpint(answer(&alice, staged, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_MERGED);
  }
  assert_groups_consistent(alice.store);
  actor_stop(&alice);

  /* A wrong key (or a locked keyring: no key) opens nothing. */
  guint8 raw[GH_STORE_KEY_SIZE];
  randombytes_buf(raw, sizeof raw);
  g_autoptr(GBytes) wrong = g_bytes_new(raw, sizeof raw);
  sodium_memzero(raw, sizeof raw);
  GhStoreConfig config = { .account_pubkey = alice.account.pubkey };
  GhStore *store = gh_store_open_with_key(&config, wrong, alice.account.store_id,
                                          GH_STORE_OPEN_NONE, &error);
  g_assert_null(store);
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_KEY);

  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

/* ---- Privacy: no MLS secret or plaintext outside the encrypted store ---------------------- */

static void
add_state_needles(CanaryScan *scan, GhStore *store, const MarmotGroupId *gid)
{
  static const gchar *const labels[] = { "mls_group", "mls_group_parent", "mls_group_pending",
                                         "mls_group_welcomes" };
  for (guint i = 0; i < G_N_ELEMENTS(labels); i++) {
    g_autoptr(GBytes) value = NULL;
    if (!kv_load(store, labels[i], gid, &value))
      continue;
    gsize len = 0;
    const guint8 *p = g_bytes_get_data(value, &len);
    /* Slices of the record: its secrets sit somewhere inside. */
    for (gsize at = 0; at + 48 <= len; at += len / 6 + 1) {
      g_autofree gchar *label = g_strdup_printf("%s[%" G_GSIZE_FORMAT "]", labels[i], at);
      canary_scan_add_bytes(scan, label, p + at, 48);
    }
  }
  MarmotStorage *s = storage_new(store);
  guint64 epoch = group_epoch(store, gid);
  for (guint64 e = 0; e <= epoch; e++) {
    uint8_t secret[32];
    if (s->get_exporter_secret(s->ctx, gid, e, secret) != MARMOT_OK)
      continue;
    g_autofree gchar *label = g_strdup_printf("exporter secret %" G_GUINT64_FORMAT, e);
    canary_scan_add_bytes(scan, label, secret, sizeof secret);
    sodium_memzero(secret, sizeof secret);
  }
  marmot_storage_free(s);
}

/* Acceptance (qp24.7): after a full lifecycle -- stage, restart, merge,
 * Welcome, messages, a late message -- the canary scanner finds no MLS
 * secret (exporter secrets, MLS state, pending Commit, Welcome outbox) and
 * no plaintext anywhere under the test's home, cache, config and data
 * directories (the SQLCipher files included), and nothing but the store. */
static void
test_lifecycle_no_secrets_on_disk(void)
{
  Actor alice, bob;
  actor_init(&alice);
  actor_init(&bob);
  g_autofree gchar *nonce = gh_store_new_op_id();
  g_autofree gchar *name_canary = g_strdup_printf("MLS-GROUP-CANARY-%s", nonce);
  g_autofree gchar *text_canary = g_strdup_printf("MLS-MESSAGE-CANARY-%s", nonce);
  g_autofree gchar *late_canary = g_strdup_printf("MLS-LATE-CANARY-%s", nonce);

  actor_start(&bob);
  g_autofree gchar *bob_kp = actor_key_package(&bob);
  actor_stop(&bob);
  actor_start(&alice);
  MarmotGroupId gid = create_group_with(&alice, "canary", NULL, 0, NULL, NULL);
  const char *kps[] = { bob_kp };
  AddArgs add = { .kps = kps, .n = 1 };
  g_autoptr(GhMlsCommitPublish) added = stage(&alice, &gid, produce_add, &add);
  free_strings_n(add.welcomes, add.n_welcomes);
  actor_stop(&alice);
  actor_start(&alice);
  g_autoptr(GPtrArray) resumed = resume(&alice);
  g_assert_cmpuint(resumed->len, ==, 1);
  g_assert_cmpint(answer(&alice, added, RELAY_ONE, TRUE), ==, GH_MLS_COMMIT_MERGED);
  gsize n = 0;
  g_auto(GStrv) welcomes = take_welcomes(&alice, &gid, &n);
  actor_stop(&alice);
  actor_start(&bob);
  actor_join(&bob, welcomes[0]);
  g_autofree gchar *late = actor_message(&bob, &gid, late_canary);
  actor_stop(&bob);

  actor_start(&alice);
  g_autoptr(GhMlsCommitPublish) renamed = stage(&alice, &gid, produce_rename, name_canary);
  g_assert_cmpint(answer(&alice, renamed, RELAY_TWO, TRUE), ==, GH_MLS_COMMIT_MERGED);
  g_autofree gchar *msg = actor_message(&alice, &gid, text_canary);
  /* One still pending, so its record is on disk too. */
  g_autoptr(GhMlsCommitPublish) pending = stage(&alice, &gid, produce_rename, name_canary);
  actor_stop(&alice);
  actor_start(&bob);
  g_assert_cmpint(deliver_to(&bob, renamed->event_json, NULL), ==, MARMOT_RESULT_COMMIT);
  assert_app_message(&bob, msg, text_canary);
  actor_stop(&bob);
  actor_start(&alice);
  assert_app_message(&alice, late, late_canary);   /* through the retained parent */
  actor_stop(&alice);

  CanaryScan *scan = canary_scan_new();
  canary_scan_add(scan, "group name", name_canary);
  canary_scan_add(scan, "message", text_canary);
  canary_scan_add(scan, "late message", late_canary);
  Actor *both[] = { &alice, &bob };
  for (guint i = 0; i < 2; i++) {
    actor_start(both[i]);
    add_state_needles(scan, both[i]->store, &gid);
    actor_stop(both[i]);
  }
  const gchar *roots[] = { g_get_home_dir(), g_get_user_data_dir(), g_get_user_cache_dir(),
                           g_get_user_config_dir(), g_get_user_state_dir(),
                           g_get_user_runtime_dir() };
  guint scanned = 0;
  for (guint i = 0; i < G_N_ELEMENTS(roots); i++) {
    guint files = 0;
    canary_scan_tree(scan, roots[i], &files);
    scanned += files;
  }
  g_assert_cmpuint(scanned, >, 0);
  canary_scan_check_clean(scan, "MLS secrets and plaintext outside the encrypted store");

  /* The scan is not vacuous: a leaked exporter secret (hex) is found. */
  {
    actor_start(&alice);
    MarmotStorage *s = alice.storage;
    uint8_t secret[32];
    assert_marmot_ok(s->get_exporter_secret(s->ctx, &gid, group_epoch(alice.store, &gid),
                                            secret));
    actor_stop(&alice);
    g_autofree gchar *hex = hex32(secret);
    sodium_memzero(secret, sizeof secret);
    g_assert_cmpint(g_mkdir_with_parents(g_get_user_cache_dir(), 0700), ==, 0);
    g_autofree gchar *leak = g_build_filename(g_get_user_cache_dir(), "leak.txt", NULL);
    g_assert_true(g_file_set_contents(leak, hex, -1, NULL));
    g_assert_cmpuint(canary_scan_tree(scan, g_get_user_cache_dir(), NULL), >, 0);
    g_remove(leak);
    canary_scan_clear_hits(scan);
  }
  canary_scan_free(scan);

  /* Nothing but the stores themselves was written. */
  for (guint i = 0; i < 2; i++) {
    g_autofree gchar *path = account_store_path(&both[i]->account);
    g_autofree gchar *dir = g_path_get_dirname(path);
    g_autoptr(GDir) listing = g_dir_open(dir, 0, NULL);
    g_assert_nonnull(listing);
    const gchar *name;
    while ((name = g_dir_read_name(listing)))
      g_assert_true(g_str_has_prefix(name, "store.db"));
  }

  marmot_group_id_free(&gid);
  actor_clear(&alice);
  actor_clear(&bob);
}

int
main(int argc, char **argv)
{
  umask(022);
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  if (sodium_init() < 0)
    g_error("libsodium failed to initialize");

  g_test_add_func("/store-marmot/contract/libmarmot-cases", test_contract_libmarmot_cases);
  g_test_add_func("/store-marmot/roundtrip/group", test_roundtrip_group);
  g_test_add_func("/store-marmot/roundtrip/messages", test_roundtrip_messages);
  g_test_add_func("/store-marmot/roundtrip/welcomes-key-packages",
                  test_roundtrip_welcomes_and_key_packages);
  g_test_add_func("/store-marmot/roundtrip/kv-relays-secrets", test_roundtrip_kv_relays_secrets);
  g_test_add_func("/store-marmot/bounds", test_bounds);
  g_test_add_func("/store-marmot/snapshot/rollback-byte-identical",
                  test_snapshot_rollback_byte_identical);
  g_test_add_func("/store-marmot/snapshot/semantics", test_snapshot_semantics);
  g_test_add_func("/store-marmot/snapshot/libmarmot-state", test_snapshot_libmarmot_state);
  g_test_add_func("/store-marmot/transaction/caller", test_caller_transaction);
  g_test_add_func("/store-marmot/transaction/disk-full", test_disk_full);
  g_test_add_func("/store-marmot/store-kinds", test_store_kinds);
  g_test_add_func("/store-marmot/migration/v1-to-v2", test_migration_v1_to_v2);
  g_test_add_func("/store-marmot/migration/crash", test_migration_crash);
  g_test_add_func("/store-marmot/t-mls/crash-atomicity", test_tmls_crash_atomicity);
  g_test_add_func("/store-marmot/e2e/persistence", test_e2e_persistence);
  g_test_add_func("/store-marmot/privacy/no-plaintext-on-disk", test_no_plaintext_on_disk);
  g_test_add_func("/store-marmot/lifecycle/crash-every-write", test_lifecycle_crash_every_write);
  g_test_add_func("/store-marmot/lifecycle/send-crash-never-reuses-a-generation",
                  test_send_crash_never_reuses_a_generation);
  g_test_add_func("/store-marmot/lifecycle/restart-republishes", test_lifecycle_restart_republishes);
  g_test_add_func("/store-marmot/lifecycle/superseded-and-refused",
                  test_lifecycle_superseded_and_refused);
  g_test_add_func("/store-marmot/lifecycle/three-members", test_lifecycle_three_members);
  g_test_add_func("/store-marmot/lifecycle/disk-full-and-key", test_lifecycle_disk_full_and_key);
  g_test_add_func("/store-marmot/privacy/lifecycle-no-secrets-on-disk",
                  test_lifecycle_no_secrets_on_disk);
  return g_test_run();
}
