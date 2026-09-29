/* Disappearing messages and retention (privacy charter G07): EX-1, EX-3,
 * EX-4, EX-5 and PT-7 (expiration bounds), the rounding and jitter of outer
 * expirations, per-conversation timers, the purge scheduler at open, on time
 * and daily, the conversation model following every purge, the outbox
 * letting go of purged entries, and T-purge crash cut points.
 *
 * Harnesses: real SQLCipher GhStores with the store's test hooks (H8 crash
 * cut points), the fake GhClock (H6) for every timer and jitter draw, and for
 * EX-1 (expiry-send.c) the mock org.nostr.Signer on a private bus with a
 * fake inbox resolver and recording relay transports. Each test runs in
 * isolated XDG directories; nothing sleeps. The crash scenarios fork before
 * the private bus exists. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "crash-harness.h"
#include "expiry-send.h"
#include "gh-expiry.h"
#include "gh-store-conversations.h"
#include "nostr-event.h"
#include "nostr-tag.h"

#include <glib/gstdio.h>
#include <sodium.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef GH_STORE_TEST_HOOKS
#error "test_expiry needs the store's test hooks (GH_STORE_TEST_HOOKS)"
#endif

#define T0   G_GINT64_CONSTANT(1900000000) /* 46 min 40 s past an hour */
#define HOUR G_GINT64_CONSTANT(3600)
#define DAY  GH_EXPIRY_TIMER_DAY

static gchar *hex_alice, *hex_bob, *hex_carol, *hex_dave;

/* ---- small helpers ------------------------------------------------------------ */

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static gint64
ceil_hour(gint64 t)
{
  return (t + HOUR - 1) / HOUR * HOUR;
}

/* PT-7: an outer expiration of a message sent at sent that expires at
 * expires lies in [expires, expires + min(D, 24 h) + 3600] on a whole hour. */
static void
assert_outer_bounds(gint64 sent, gint64 expires, gint64 value)
{
  gint64 d = expires - sent;
  g_assert_cmpint(value, >=, expires);
  g_assert_cmpint(value, <=, expires + MIN(d, DAY) + HOUR);
  g_assert_cmpint(value % HOUR, ==, 0);
}

/* ---- rumors and messages --------------------------------------------------------- */

typedef struct {
  const gchar *author;
  const gchar *to;
  gint64 created_at;
  const gchar *content;
  const gchar *subject;
  gint64 expiration; /* rumor expiration tag; 0 none */
} Rumor;

static gchar *
rumor_json(const Rumor *r)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, r->author);
  nostr_event_set_created_at(event, r->created_at);
  nostr_event_set_content(event, r->content ? r->content : "text");
  NostrTags *tags = nostr_tags_new(1, nostr_tag_new("p", r->to, NULL));
  if (r->subject)
    nostr_tags_append(tags, nostr_tag_new("subject", r->subject, NULL));
  if (r->expiration) {
    g_autofree gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, r->expiration);
    nostr_tags_append(tags, nostr_tag_new("expiration", value, NULL));
  }
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *copy = g_strdup(json);
  free(json);
  return copy;
}

/* The message as the account sees it; outer_expiry is the seal's or wrap's
 * expiration the unwrap would report (0: none). */
static GhMessage *
message_new(const Rumor *r, gint64 outer_expiry)
{
  g_autofree gchar *json = rumor_json(r);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(hex_alice, json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(message);
  if (outer_expiry)
    gh_message_set_expires_at(message, outer_expiry);
  return message;
}

static gchar *
rumor_id(const Rumor *r)
{
  g_autoptr(GhMessage) message = message_new(r, 0);
  return g_strdup(gh_message_get_rumor_id(message));
}

static gchar *
room_of(const gchar *a, const gchar *b)
{
  if (!b || g_strcmp0(a, b) == 0)
    return g_strdup(a);
  return strcmp(a, b) < 0 ? g_strconcat(a, ",", b, NULL) : g_strconcat(b, ",", a, NULL);
}

/* ---- stores ------------------------------------------------------------------------ */

typedef struct {
  GhClock *clock;        /* fake; survives restarts */
  GBytes *key;
  gchar *store_id;
  gint retention_days;
  gint64 default_timer;
  guint page_size;
  GhStore *store;
  GhStoreConversations *conversations;
  GhConversationStore *model;
  GhExpiry *expiry;
  GPtrArray *purged;     /* rumor ids from "purged", in order */
  guint purges;          /* "purged" emissions */
  guint outbox_purged;
  GPtrArray *timers;     /* "room=seconds" from "timer-changed" */
  guint added;           /* model "message-added" emissions */
} Fixture;

static void
on_purged(GhExpiry *expiry, const gchar *const *rumor_ids, guint n_outbox, gpointer data)
{
  Fixture *f = data;
  (void)expiry;
  f->purges++;
  f->outbox_purged += n_outbox;
  for (guint i = 0; rumor_ids && rumor_ids[i]; i++)
    g_ptr_array_add(f->purged, g_strdup(rumor_ids[i]));
}

static void
on_timer_changed(GhExpiry *expiry, const gchar *room_id, gint64 seconds, gpointer data)
{
  Fixture *f = data;
  (void)expiry;
  g_ptr_array_add(f->timers, g_strdup_printf("%s=%" G_GINT64_FORMAT, room_id, seconds));
}

static void
on_message_added(GhConversationStore *model, GhConversation *conversation, GhMessage *message,
                 gpointer data)
{
  (void)model; (void)conversation; (void)message;
  ((Fixture *)data)->added++;
}

static GhStore *
store_open(GBytes *key, const gchar *store_id, GhClock *clock, GError **error)
{
  GhStoreConfig config = { .account_pubkey = hex_alice, .clock = clock };
  return gh_store_open_with_key(&config, key, store_id, GH_STORE_OPEN_CREATE, error);
}

static void
expiry_start(Fixture *f)
{
  GhExpiryConfig config = {
    .store = f->store,
    .conversations = f->conversations,
    .retention_days = f->retention_days,
    .default_timer = f->default_timer,
  };
  f->expiry = gh_expiry_new(&config);
  g_signal_connect(f->expiry, "purged", G_CALLBACK(on_purged), f);
  g_signal_connect(f->expiry, "timer-changed", G_CALLBACK(on_timer_changed), f);
}

/* The store, its delegate attached to a fresh model and (with_expiry) the
 * GhExpiry over them, which has not purged yet. */
static void
fixture_open(Fixture *f, gboolean with_expiry)
{
  g_autoptr(GError) error = NULL;
  f->store = store_open(f->key, f->store_id, f->clock, &error);
  g_assert_no_error(error);
  g_assert_nonnull(f->store);
  f->conversations = gh_store_conversations_new(f->store);
  f->model = gh_conversation_store_new();
  g_signal_connect(f->model, "message-added", G_CALLBACK(on_message_added), f);
  g_assert_true(gh_store_conversations_attach(f->conversations, f->model, f->page_size, &error));
  g_assert_no_error(error);
  if (with_expiry)
    expiry_start(f);
}

static void
expiry_release(Fixture *f)
{
  if (f->expiry)
    g_object_run_dispose(G_OBJECT(f->expiry));
  g_clear_object(&f->expiry);
}

/* Quit. The application disposes the GhExpiry right after the store closed
 * ("store-closed"); store_first does the same here. */
static void
fixture_close(Fixture *f, gboolean store_first)
{
  if (!store_first)
    expiry_release(f);
  if (f->conversations)
    gh_store_conversations_close(f->conversations);
  g_clear_object(&f->model);
  g_clear_object(&f->conversations);
  g_clear_pointer(&f->store, gh_store_close);
  expiry_release(f);
}

static void
fixture_init_at(Fixture *f, gint64 start, gint retention_days, guint page_size,
                gboolean with_expiry)
{
  memset(f, 0, sizeof *f);
  guint8 raw[GH_STORE_KEY_SIZE];
  randombytes_buf(raw, sizeof raw);
  f->key = g_bytes_new(raw, sizeof raw);
  sodium_memzero(raw, sizeof raw);
  f->store_id = g_uuid_string_random();
  f->clock = gh_clock_new_fake(start * G_USEC_PER_SEC);
  f->retention_days = retention_days;
  f->page_size = page_size;
  f->purged = g_ptr_array_new_with_free_func(g_free);
  f->timers = g_ptr_array_new_with_free_func(g_free);
  fixture_open(f, with_expiry);
}

static void
fixture_init(Fixture *f, gboolean with_expiry)
{
  fixture_init_at(f, T0, 0, 0, with_expiry);
}

static void
fixture_clear(Fixture *f)
{
  fixture_close(f, FALSE);
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_delete_files(NULL, hex_alice, &error));
  g_assert_no_error(error);
  g_clear_pointer(&f->clock, gh_clock_unref);
  g_clear_pointer(&f->key, g_bytes_unref);
  g_clear_pointer(&f->store_id, g_free);
  g_clear_pointer(&f->purged, g_ptr_array_unref);
  g_clear_pointer(&f->timers, g_ptr_array_unref);
}

static void
advance(Fixture *f, gint64 seconds)
{
  gh_clock_fake_advance(f->clock, seconds * G_USEC_PER_SEC);
}

/* Runs what is due now: the fake clock fires a zero delay one microsecond
 * later, still within the same second. */
static void
run_due(Fixture *f)
{
  gh_clock_fake_advance(f->clock, 1);
}

static gint64
now(Fixture *f)
{
  return gh_clock_get_unix(f->clock);
}

static GhConversationAddResult
deliver(Fixture *f, const Rumor *r, const gchar *wrap_seed, gint64 outer_expiry)
{
  g_autoptr(GhMessage) message = message_new(r, outer_expiry);
  g_autofree gchar *wrap = hex_of(wrap_seed);
  return gh_conversation_store_admit(f->model, message, wrap, NULL);
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
  g_assert_cmpint(sqlite3_prepare_v2(gh_store_get_db(store), sql, -1, &stmt, NULL), ==, SQLITE_OK);
  int rc = sqlite3_step(stmt);
  g_assert_true(rc == SQLITE_ROW || rc == SQLITE_DONE);
  gint64 value = rc == SQLITE_ROW ? sqlite3_column_int64(stmt, 0) : -1;
  sqlite3_finalize(stmt);
  return value;
}

static gint64
stored(GhStore *store, const gchar *rumor)
{
  return sql_int(store, "SELECT count(*) FROM messages WHERE backend_msg_id = '%s'", rumor);
}

static gint64
stored_unread(GhStore *store, const gchar *room_id)
{
  return sql_int(store, "SELECT unread_count FROM conversations WHERE backend_key = '%s'",
                 room_id);
}

static gboolean
is_seen(GhStore *store, const gchar *rumor)
{
  gboolean seen = FALSE;
  g_assert_true(gh_store_seen_contains(store, GH_STORE_SEEN_RUMOR, rumor, &seen, NULL));
  return seen;
}

/* Every room's unread count is its messages from others after its read
 * marker, and every marker names a stored message of its room. */
static void
assert_read_state_consistent(GhStore *store)
{
  g_assert_cmpint(sql_int(store,
    "SELECT count(*) FROM conversations c WHERE c.last_read_msg IS NOT NULL AND NOT EXISTS "
    "(SELECT 1 FROM messages m WHERE m.id = c.last_read_msg AND m.conversation_id = c.id)"),
    ==, 0);
  g_assert_cmpint(sql_int(store,
    "SELECT count(*) FROM conversations c WHERE c.unread_count != "
    "(SELECT count(*) FROM messages m LEFT JOIN messages r ON r.id = c.last_read_msg "
    "WHERE m.conversation_id = c.id AND m.direction = 0 AND (r.id IS NULL OR "
    "m.created_at > r.created_at OR (m.created_at = r.created_at AND "
    "m.backend_msg_id > r.backend_msg_id)))"), ==, 0);
}

static void
assert_integrity(GhStore *store)
{
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_check_integrity(store, TRUE, &error));
  g_assert_no_error(error);
}

static gint64
wal_size(GhStore *store)
{
  g_autofree gchar *wal = g_strconcat(gh_store_get_path(store), "-wal", NULL);
  struct stat st;
  return stat(wal, &st) == 0 ? (gint64)st.st_size : -1;
}

static guint
n_items(gpointer model)
{
  return g_list_model_get_n_items(G_LIST_MODEL(model));
}

/* "room|room|..." in store order. */
static gchar *
room_order(GhConversationStore *model)
{
  GString *out = g_string_new(NULL);
  for (guint i = 0; i < n_items(model); i++) {
    g_autoptr(GhConversation) c = g_list_model_get_item(G_LIST_MODEL(model), i);
    g_string_append_printf(out, "%s%s", i ? "|" : "", gh_conversation_get_room_id(c));
  }
  return g_string_free(out, FALSE);
}

/* ---- policy ----------------------------------------------------------------------- */

static void
test_policy(void)
{
  const gint64 valid[] = { 0, DAY, 7 * DAY, 28 * DAY };
  const gint64 invalid[] = { -1, 1, HOUR, DAY + 1, 2 * DAY, 14 * DAY, 29 * DAY, 365 * DAY };
  for (guint i = 0; i < G_N_ELEMENTS(valid); i++)
    g_assert_true(gh_expiry_timer_is_valid(valid[i]));
  for (guint i = 0; i < G_N_ELEMENTS(invalid); i++)
    g_assert_false(gh_expiry_timer_is_valid(invalid[i]));
  g_assert_cmpint(GH_EXPIRY_TIMER_WEEK, ==, 7 * DAY);
  g_assert_cmpint(GH_EXPIRY_TIMER_FOUR_WEEKS, ==, 28 * DAY);

  g_assert_cmpint(gh_expiry_message_expiration(T0, GH_EXPIRY_TIMER_OFF), ==, 0);
  g_assert_cmpint(gh_expiry_message_expiration(T0, DAY), ==, T0 + DAY);
  g_assert_cmpint(gh_expiry_message_expiration(T0, 28 * DAY), ==, T0 + 28 * DAY);

  /* D10: 0 keeps everything; days count back from now; absurd values cap. */
  g_assert_cmpint(gh_expiry_retention_cutoff(T0, 0), ==, 0);
  g_assert_cmpint(gh_expiry_retention_cutoff(T0, -5), ==, 0);
  g_assert_cmpint(gh_expiry_retention_cutoff(T0, 30), ==, T0 - 30 * DAY);
  g_assert_cmpint(gh_expiry_retention_cutoff(T0, 365), ==, T0 - 365 * DAY);
  g_assert_cmpint(gh_expiry_retention_cutoff(T0, G_MAXINT), ==,
                  MAX(T0 - (gint64)GH_EXPIRY_MAX_RETENTION_DAYS * DAY, 1));
}

/* ---- PT-7: rounding and bounds of outer expirations ------------------------------------- */

static void
test_pt7_rounding(void)
{
  const gint64 timers[] = { HOUR, DAY, 7 * DAY, 28 * DAY };
  const gint64 aligned = T0 - T0 % HOUR;
  const gint64 sends[] = { T0, aligned, aligned + 1, aligned - 1 };
  for (guint t = 0; t < G_N_ELEMENTS(timers); t++) {
    for (guint s = 0; s < G_N_ELEMENTS(sends); s++) {
      const gint64 sent = sends[s], expires = sent + timers[t];
      const gint64 max = MIN(timers[t], DAY);
      g_assert_cmpint(gh_expiry_outer_jitter_max(sent, expires), ==, max);
      /* The ends: no jitter rounds the message's own time up (an exact hour
       * stays), the widest rounds expires + min(D, 24 h) up. */
      g_assert_cmpint(gh_expiry_outer_expiration(sent, expires, 0), ==, ceil_hour(expires));
      g_assert_cmpint(gh_expiry_outer_expiration(sent, expires, max), ==,
                      ceil_hour(expires + max));
      /* Jitter outside [0, max] is clamped: it never widens the bound. */
      g_assert_cmpint(gh_expiry_outer_expiration(sent, expires, -7), ==, ceil_hour(expires));
      g_assert_cmpint(gh_expiry_outer_expiration(sent, expires, max + 5000), ==,
                      ceil_hour(expires + max));
      gint64 previous = 0;
      for (gint64 jitter = 0; jitter <= max + 96; jitter += 97) {
        gint64 value = gh_expiry_outer_expiration(sent, expires, MIN(jitter, max));
        assert_outer_bounds(sent, expires, value);
        g_assert_cmpint(value, >=, previous); /* monotone in the jitter */
        previous = value;
      }
    }
  }
  /* An exact hour with no jitter is kept as it is. */
  g_assert_cmpint(gh_expiry_outer_expiration(aligned - DAY, aligned, 0), ==, aligned);
  g_assert_cmpint(gh_expiry_outer_expiration(aligned - DAY, aligned, 1), ==, aligned + HOUR);
  /* Nothing is drawn for a message that does not expire after it was sent. */
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  GhNip17OuterExpiration outer = { { 1, 1 }, { 1, 1 } };
  g_assert_false(gh_expiry_draw_outer(clock, T0, T0, &outer));
  g_assert_false(gh_expiry_draw_outer(clock, T0, T0 - 1, &outer));
  g_assert_false(gh_expiry_draw_outer(clock, 0, T0, &outer));
  g_assert_cmpint(outer.recipient.seal, ==, 1);
}

/* ---- PT-7: jitter draws ------------------------------------------------------------------ */

static void
test_pt7_jitter(void)
{
  /* Half past an hour, so the first and last hour buckets are half hours. */
  const gint64 sent = T0 - T0 % HOUR + HOUR / 2, expires = sent + DAY;
  g_autoptr(GhClock) clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);

  /* Each layer takes the next draw from the clock, in a fixed order. */
  gh_clock_fake_push_random(clock, 0);
  gh_clock_fake_push_random(clock, (guint32)DAY);
  gh_clock_fake_push_random(clock, (guint32)HOUR);
  gh_clock_fake_push_random(clock, 1);
  GhNip17OuterExpiration outer;
  g_assert_true(gh_expiry_draw_outer(clock, sent, expires, &outer));
  g_assert_cmpint(outer.recipient.seal, ==, ceil_hour(expires));
  g_assert_cmpint(outer.recipient.wrap, ==, ceil_hour(expires + DAY));
  g_assert_cmpint(outer.self_copy.seal, ==, ceil_hour(expires + HOUR));
  g_assert_cmpint(outer.self_copy.wrap, ==, ceil_hour(expires + 1));

  /* Over 1,000 envelopes (4,000 draws from the fake clock's seeded PRNG):
   * every value within PT-7's bounds and a whole hour, the 25 hour buckets
   * used as often as their width says, and the layers of one message
   * rarely equal. */
  guint buckets[25] = { 0 };
  guint equal_wraps = 0, equal_layers = 0;
  gdouble sum = 0;
  for (guint i = 0; i < 1000; i++) {
    g_assert_true(gh_expiry_draw_outer(clock, sent, expires, &outer));
    const gint64 values[] = { outer.recipient.seal, outer.recipient.wrap,
                              outer.self_copy.seal, outer.self_copy.wrap };
    for (guint k = 0; k < G_N_ELEMENTS(values); k++) {
      assert_outer_bounds(sent, expires, values[k]);
      guint bucket = (guint)((values[k] - ceil_hour(expires)) / HOUR);
      g_assert_cmpuint(bucket, <, G_N_ELEMENTS(buckets));
      buckets[bucket]++;
      sum += (gdouble)(values[k] - expires);
    }
    equal_wraps += outer.recipient.wrap == outer.self_copy.wrap;
    equal_layers += outer.recipient.seal == outer.recipient.wrap;
  }
  /* Buckets 0 and 24 cover half an hour (1801 and 1800 of 86401 jitter
   * values), the others a whole one (3600): about 83 and 167 of 4000. */
  g_assert_cmpuint(buckets[0], >=, 40);
  g_assert_cmpuint(buckets[0], <=, 140);
  g_assert_cmpuint(buckets[24], >=, 40);
  g_assert_cmpuint(buckets[24], <=, 140);
  for (guint b = 1; b < 24; b++) {
    g_assert_cmpuint(buckets[b], >=, 100);
    g_assert_cmpuint(buckets[b], <=, 250);
  }
  /* Mean offset past the real expiration: half the spread plus about half
   * an hour of rounding (43200 + 1800). */
  g_assert_cmpfloat(sum / 4000, >, 40000);
  g_assert_cmpfloat(sum / 4000, <, 50000);
  g_assert_cmpuint(equal_wraps, <, 100);
  g_assert_cmpuint(equal_layers, <, 100);

  /* The system clock draws from the CSPRNG: the same bounds, spread out. */
  g_autoptr(GhClock) system = gh_clock_new_system();
  g_autoptr(GHashTable) distinct = g_hash_table_new(g_int64_hash, g_int64_equal);
  g_autofree gint64 *keep = g_new(gint64, 1000);
  for (guint i = 0; i < 1000; i++) {
    g_assert_true(gh_expiry_draw_outer(system, sent, expires, &outer));
    assert_outer_bounds(sent, expires, outer.recipient.seal);
    assert_outer_bounds(sent, expires, outer.recipient.wrap);
    assert_outer_bounds(sent, expires, outer.self_copy.seal);
    assert_outer_bounds(sent, expires, outer.self_copy.wrap);
    keep[i] = outer.recipient.wrap;
    g_hash_table_add(distinct, &keep[i]);
  }
  g_assert_cmpuint(g_hash_table_size(distinct), >=, 20);
  /* A timer under a day spreads by at most itself. */
  for (guint i = 0; i < 200; i++) {
    g_assert_true(gh_expiry_draw_outer(system, sent, sent + HOUR, &outer));
    assert_outer_bounds(sent, sent + HOUR, outer.recipient.wrap);
    g_assert_cmpint(outer.recipient.wrap, <=, ceil_hour(sent + 2 * HOUR));
  }
}

/* ---- EX-3: purge on time, the model follows ------------------------------------------------ */

static void
test_ex3_purge_on_time(void)
{
  Fixture f;
  fixture_init(&f, TRUE);
  g_autofree gchar *ab = room_of(hex_alice, hex_bob);
  g_autofree gchar *ac = room_of(hex_alice, hex_carol);
  Rumor first = { hex_bob, hex_alice, T0 - 30, "first", NULL, 0 };
  Rumor soon = { hex_bob, hex_alice, T0 - 20, "soon", "Plans", T0 + 60 };
  Rumor later = { hex_bob, hex_alice, T0 - 10, "later", NULL, 0 };
  Rumor carol = { hex_carol, hex_alice, T0 - 15, "from carol", NULL, 0 };
  g_autofree gchar *first_id = rumor_id(&first);
  g_autofree gchar *soon_id = rumor_id(&soon);
  g_autofree gchar *later_id = rumor_id(&later);
  g_assert_cmpint(deliver(&f, &first, "w/first", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &soon, "w/soon", 0), ==, GH_CONVERSATION_ADD_NEW);
  /* The seal's or wrap's expiration counts when the rumor has none. */
  g_assert_cmpint(deliver(&f, &later, "w/later", T0 + 120), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &carol, "w/carol", 0), ==, GH_CONVERSATION_ADD_NEW);

  /* At open nothing has expired: nothing is purged or announced. */
  g_assert_true(gh_expiry_purge(f.expiry, NULL));
  g_assert_cmpuint(f.purges, ==, 0);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 60);
  GhConversation *room = gh_conversation_store_lookup(f.model, ab);
  g_assert_nonnull(room);
  /* Accepted: only then does the subject name the room. */
  gh_conversation_accept(room);
  g_assert_cmpstr(gh_conversation_get_title(room), ==, "Plans");
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "later");
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 3);
  g_autofree gchar *order_before = room_order(f.model);
  g_autofree gchar *expected_before = g_strconcat(ab, "|", ac, NULL);
  g_assert_cmpstr(order_before, ==, expected_before);

  /* Not a second early. */
  advance(&f, 59);
  g_assert_cmpint(stored(f.store, soon_id), ==, 1);
  g_assert_cmpuint(f.purges, ==, 0);

  /* EX-3 on H6: at expires_at the row leaves the store and the model, the
   * name its subject gave the room goes with it, the WAL is truncated and
   * "purged" names it (to withdraw its notification). */
  advance(&f, 1);
  g_assert_cmpint(stored(f.store, soon_id), ==, 0);
  g_assert_true(is_seen(f.store, soon_id));
  g_assert_null(gh_conversation_store_lookup_message(f.model, soon_id));
  g_assert_cmpuint(n_items(room), ==, 2);
  g_assert_cmpuint(f.purges, ==, 1);
  g_assert_cmpuint(f.purged->len, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.purged, 0), ==, soon_id);
  g_assert_cmpuint(f.outbox_purged, ==, 0);
  g_assert_null(gh_conversation_get_subject(room));
  g_assert_cmpstr(gh_conversation_get_title(room), !=, "Plans");
  g_assert_cmpint(sql_int(f.store, "SELECT title IS NULL FROM conversations WHERE "
                                   "backend_key = '%s'", ab), ==, 1);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "later");
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 2);
  g_assert_cmpint(stored_unread(f.store, ab), ==, 2);
  g_assert_cmpint(wal_size(f.store), ==, 0);
  assert_read_state_consistent(f.store);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 120);

  /* A message stored with a sooner expiry moves the wake-up earlier. */
  Rumor brief = { hex_bob, hex_alice, T0 + 60, "brief", NULL, T0 + 90 };
  g_autofree gchar *brief_id = rumor_id(&brief);
  g_assert_cmpint(deliver(&f, &brief, "w/brief", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 90);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "brief");
  advance(&f, 30);
  g_assert_cmpint(stored(f.store, brief_id), ==, 0);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "later");
  /* 30 s after the last truncation: this one waits out the minute. */
  g_assert_cmpint(wal_size(f.store), >, 0);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 120);

  /* The newest message goes: preview and last activity fall back and the
   * room moves below Carol's. A minute has passed: the WAL is truncated. */
  advance(&f, 30);
  g_assert_cmpint(stored(f.store, later_id), ==, 0);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "first");
  g_assert_cmpint(gh_conversation_get_last_activity(room), ==, T0 - 30);
  g_autofree gchar *order_after = room_order(f.model);
  g_autofree gchar *expected_after = g_strconcat(ac, "|", ab, NULL);
  g_assert_cmpstr(order_after, ==, expected_after);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 1);
  g_assert_cmpint(wal_size(f.store), ==, 0);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, 0);
  assert_read_state_consistent(f.store);

  /* A room whose last message expires is unlisted, as a restart would. */
  g_autofree gchar *ad = room_of(hex_alice, hex_dave);
  Rumor dave = { hex_dave, hex_alice, T0 + 120, "from dave", NULL, T0 + 200 };
  g_assert_cmpint(deliver(&f, &dave, "w/dave", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_nonnull(gh_conversation_store_lookup(f.model, ad));
  g_assert_cmpuint(n_items(f.model), ==, 3);
  advance(&f, T0 + 200 - now(&f));
  g_assert_null(gh_conversation_store_lookup(f.model, ad));
  g_assert_cmpuint(n_items(f.model), ==, 2);
  g_assert_cmpuint(f.purges, ==, 4);
  fixture_close(&f, TRUE);
  fixture_open(&f, FALSE);
  g_assert_null(gh_conversation_store_lookup(f.model, ad));
  g_assert_cmpuint(n_items(gh_conversation_store_lookup(f.model, ab)), ==, 1);
  g_assert_nonnull(gh_conversation_store_lookup_message(f.model, first_id));
  fixture_clear(&f);
}

/* ---- EX-3: at open ---------------------------------------------------------------------- */

static void
test_ex3_at_open(void)
{
  Fixture f;
  fixture_init(&f, FALSE);
  g_autofree gchar *ab = room_of(hex_alice, hex_bob);
  Rumor stays = { hex_bob, hex_alice, T0 - 10, "stays", NULL, 0 };
  Rumor goes = { hex_bob, hex_alice, T0 - 5, "goes", NULL, T0 + 60 };
  g_autofree gchar *goes_id = rumor_id(&goes);
  g_assert_cmpint(deliver(&f, &stays, "w/stays", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &goes, "w/goes", 0), ==, GH_CONVERSATION_ADD_NEW);

  /* Expired while the app was closed: not restored, yet still stored. */
  fixture_close(&f, FALSE);
  advance(&f, 120);
  fixture_open(&f, TRUE);
  g_assert_null(gh_conversation_store_lookup_message(f.model, goes_id));
  g_assert_cmpint(stored(f.store, goes_id), ==, 1);
  g_assert_cmpuint(n_items(gh_conversation_store_lookup(f.model, ab)), ==, 1);
  /* The purge at open is due at once and runs from the clock. */
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 120);
  run_due(&f);
  g_assert_cmpint(stored(f.store, goes_id), ==, 0);
  g_assert_true(is_seen(f.store, goes_id));
  g_assert_cmpuint(f.purges, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.purged, 0), ==, goes_id);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, 0);
  g_assert_cmpuint(n_items(gh_conversation_store_lookup(f.model, ab)), ==, 1);
  assert_read_state_consistent(f.store);

  /* Disposing it after its store closed touches nothing (the order the
   * application uses); a later purge call fails cleanly. */
  g_object_ref(f.expiry);
  GhExpiry *expiry = f.expiry;
  fixture_close(&f, TRUE);
  g_autoptr(GError) error = NULL;
  g_assert_false(gh_expiry_purge(expiry, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_STATE);
  g_assert_cmpint(gh_expiry_get_next_run(expiry), ==, 0);
  g_object_unref(expiry);
  fixture_open(&f, FALSE);
  fixture_clear(&f);
}

/* ---- a purge that fails waits a minute ------------------------------------------------------ */

static void
test_retry(void)
{
  Fixture f;
  fixture_init(&f, TRUE);
  g_assert_true(gh_expiry_purge(f.expiry, NULL));
  Rumor goes = { hex_bob, hex_alice, T0 - 5, "goes", NULL, T0 + 60 };
  g_autofree gchar *goes_id = rumor_id(&goes);
  g_assert_cmpint(deliver(&f, &goes, "w/goes", 0), ==, GH_CONVERSATION_ADD_NEW);
  /* The delegate is closed (as right before its store closes): purges fail.
   * Everything due waits for the retry a minute later instead of spinning. */
  gh_store_conversations_close(f.conversations);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*could not delete expired messages*");
  advance(&f, 60);
  g_test_assert_expected_messages();
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 120);
  g_assert_cmpuint(gh_clock_fake_get_n_timeouts(f.clock), ==, 1);
  g_assert_cmpint(stored(f.store, goes_id), ==, 1);
  g_test_expect_message(NULL, G_LOG_LEVEL_WARNING, "*could not delete expired messages*");
  advance(&f, 60);
  g_test_assert_expected_messages();
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 180);
  g_assert_cmpuint(f.purges, ==, 0);
  fixture_clear(&f);
}

/* ---- a damaged (read-only) store: the model still drops expired messages ------------------- */

static void
test_read_only(void)
{
  Fixture f;
  fixture_init(&f, FALSE);
  g_autofree gchar *ab = room_of(hex_alice, hex_bob);
  Rumor stays = { hex_bob, hex_alice, T0 - 10, "stays", NULL, 0 };
  Rumor goes = { hex_bob, hex_alice, T0 - 5, "goes", NULL, T0 + 60 };
  Rumor alone = { hex_carol, hex_alice, T0 - 4, "alone", NULL, T0 + 90 };
  g_autofree gchar *goes_id = rumor_id(&goes);
  g_autofree gchar *alone_id = rumor_id(&alone);
  g_assert_cmpint(deliver(&f, &stays, "w/stays", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &goes, "w/goes", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &alone, "w/alone", 0), ==, GH_CONVERSATION_ADD_NEW);
  fixture_close(&f, FALSE);

  /* STORE_CORRUPT: opened read-only, nothing can be deleted. */
  GhStoreConfig config = { .account_pubkey = hex_alice, .clock = f.clock };
  g_autoptr(GError) error = NULL;
  f.store = gh_store_open_with_key(&config, f.key, f.store_id, GH_STORE_OPEN_ALLOW_CORRUPT,
                                   &error);
  g_assert_no_error(error);
  g_assert_true(gh_store_is_read_only(f.store));
  f.conversations = gh_store_conversations_new(f.store);
  f.model = gh_conversation_store_new();
  g_assert_true(gh_store_conversations_attach(f.conversations, f.model, 0, &error));
  expiry_start(&f);
  g_assert_true(gh_expiry_purge(f.expiry, &error));
  g_assert_no_error(error);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 60);

  /* On time the message leaves the model (and so every view); the store
   * keeps it, and the room left empty is unlisted. */
  advance(&f, 60);
  g_assert_null(gh_conversation_store_lookup_message(f.model, goes_id));
  g_assert_cmpint(stored(f.store, goes_id), ==, 1);
  g_assert_cmpuint(n_items(gh_conversation_store_lookup(f.model, ab)), ==, 1);
  g_assert_cmpuint(f.purges, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.purged, 0), ==, goes_id);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 90);
  advance(&f, 30);
  g_assert_null(gh_conversation_store_lookup_message(f.model, alone_id));
  g_autofree gchar *ac = room_of(hex_alice, hex_carol);
  g_assert_null(gh_conversation_store_lookup(f.model, ac));
  g_assert_cmpuint(f.purges, ==, 2);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, 0);
  fixture_close(&f, TRUE);
  fixture_open(&f, FALSE);
  fixture_clear(&f);
}

/* ---- EX-4: expired on arrival ------------------------------------------------------------ */

static void
test_ex4_arrival(void)
{
  Fixture f;
  fixture_init(&f, TRUE);
  g_assert_true(gh_expiry_purge(f.expiry, NULL));
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, 0);
  /* Expired by its rumor tag, exactly now, or by the seal's or wrap's. */
  Rumor gone = { hex_bob, hex_alice, T0 - 10, "gone", NULL, T0 - 1 };
  Rumor edge = { hex_bob, hex_alice, T0 - 9, "edge", NULL, T0 };
  Rumor outer = { hex_bob, hex_alice, T0 - 8, "outer", NULL, 0 };
  g_autofree gchar *gone_id = rumor_id(&gone);
  g_autofree gchar *edge_id = rumor_id(&edge);
  g_autofree gchar *outer_id = rumor_id(&outer);
  g_assert_cmpint(deliver(&f, &gone, "w/gone", 0), ==, GH_CONVERSATION_ADD_HIDDEN);
  g_assert_cmpint(deliver(&f, &edge, "w/edge", 0), ==, GH_CONVERSATION_ADD_HIDDEN);
  g_assert_cmpint(deliver(&f, &outer, "w/outer", T0 - 5), ==, GH_CONVERSATION_ADD_HIDDEN);
  /* EX-4: recorded as seen only: not stored, never listed or announced
   * (nothing to notify), and nothing scheduled. */
  const gchar *ids[] = { gone_id, edge_id, outer_id };
  for (guint i = 0; i < G_N_ELEMENTS(ids); i++) {
    g_assert_true(is_seen(f.store, ids[i]));
    g_assert_cmpint(stored(f.store, ids[i]), ==, 0);
    g_assert_null(gh_conversation_store_lookup_message(f.model, ids[i]));
  }
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM conversations"), ==, 0);
  g_assert_cmpuint(n_items(f.model), ==, 0);
  g_assert_cmpuint(f.added, ==, 0);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, 0);
  /* Another wrap of it (backfill) does not bring it back. */
  g_assert_cmpint(deliver(&f, &gone, "w/gone-again", 0), ==, GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpuint(n_items(f.model), ==, 0);

  /* One that expires later is shown and scheduled; once it has expired and
   * been purged, backfill does not bring it back either (EX-6). */
  Rumor minute = { hex_bob, hex_alice, T0 - 1, "for a minute", NULL, T0 + 60 };
  g_autofree gchar *minute_id = rumor_id(&minute);
  g_assert_cmpint(deliver(&f, &minute, "w/minute", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(f.added, ==, 1);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + 60);
  advance(&f, 60);
  g_assert_cmpint(stored(f.store, minute_id), ==, 0);
  g_assert_cmpuint(n_items(f.model), ==, 0);
  g_assert_cmpint(deliver(&f, &minute, "w/minute-again", 0), ==, GH_CONVERSATION_ADD_DUPLICATE);
  g_assert_cmpint(stored(f.store, minute_id), ==, 0);
  g_assert_cmpuint(n_items(f.model), ==, 0);
  fixture_clear(&f);
}

/* ---- EX-5: retention ----------------------------------------------------------------------- */

static void
test_ex5_retention(void)
{
  Fixture f;
  fixture_init_at(&f, T0 - 40 * DAY, 0, 0, FALSE);
  g_autofree gchar *ab = room_of(hex_alice, hex_bob);
  /* Received 40 and 20 days ago (read), and today (unread). */
  Rumor old = { hex_bob, hex_alice, T0 - 40 * DAY - 100, "old", NULL, 0 };
  Rumor mid = { hex_bob, hex_alice, T0 - 20 * DAY - 100, "mid", NULL, 0 };
  Rumor fresh = { hex_bob, hex_alice, T0 - 100, "fresh", NULL, 0 };
  g_autofree gchar *old_id = rumor_id(&old);
  g_autofree gchar *mid_id = rumor_id(&mid);
  g_autofree gchar *fresh_id = rumor_id(&fresh);
  g_assert_cmpint(deliver(&f, &old, "w/old", 0), ==, GH_CONVERSATION_ADD_NEW);
  advance(&f, 20 * DAY);
  g_assert_cmpint(deliver(&f, &mid, "w/mid", 0), ==, GH_CONVERSATION_ADD_NEW);
  gh_conversation_mark_read(gh_conversation_store_lookup(f.model, ab));
  advance(&f, 20 * DAY);
  g_assert_cmpint(deliver(&f, &fresh, "w/fresh", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpuint(gh_conversation_get_unread_count(gh_conversation_store_lookup(f.model, ab)),
                   ==, 1);

  /* 30 days, at open: what was received more than 30 days ago goes. */
  fixture_close(&f, FALSE);
  f.retention_days = 30;
  fixture_open(&f, TRUE);
  g_assert_cmpuint(n_items(gh_conversation_store_lookup(f.model, ab)), ==, 3);
  g_assert_true(gh_expiry_purge(f.expiry, NULL));
  g_assert_cmpint(stored(f.store, old_id), ==, 0);
  g_assert_cmpint(stored(f.store, mid_id), ==, 1);
  g_assert_cmpuint(f.purges, ==, 1);
  g_assert_cmpstr(g_ptr_array_index(f.purged, 0), ==, old_id);
  GhConversation *room = gh_conversation_store_lookup(f.model, ab);
  g_assert_cmpuint(n_items(room), ==, 2);
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 1);
  g_assert_cmpint(stored_unread(f.store, ab), ==, 1);
  assert_read_state_consistent(f.store);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, T0 + DAY);

  /* Daily: "mid" crosses the line after 10 days (received exactly 20 days
   * before T0), not before. Its read marker moves back to nothing, and the
   * unread count stays with what came after it. */
  for (guint day = 1; day <= 10; day++) {
    advance(&f, DAY);
    g_assert_cmpint(stored(f.store, mid_id), ==, 1);
  }
  g_assert_cmpuint(f.purges, ==, 1);
  advance(&f, DAY);
  g_assert_cmpint(stored(f.store, mid_id), ==, 0);
  g_assert_cmpuint(f.purges, ==, 2);
  g_assert_cmpuint(n_items(room), ==, 1);
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "fresh");
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 1);
  g_assert_cmpint(stored_unread(f.store, ab), ==, 1);
  g_assert_cmpint(sql_int(f.store, "SELECT last_read_msg IS NULL FROM conversations "
                                   "WHERE backend_key = '%s'", ab), ==, 1);
  assert_read_state_consistent(f.store);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, now(&f) + DAY);

  /* Keep forever: no more daily runs. A year: runs now, keeps "fresh". */
  gh_expiry_set_retention_days(f.expiry, 0);
  g_assert_cmpint(gh_expiry_get_retention_days(f.expiry), ==, 0);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, 0);
  gh_expiry_set_retention_days(f.expiry, 365);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, now(&f));
  run_due(&f);
  g_assert_cmpint(stored(f.store, fresh_id), ==, 1);
  g_assert_cmpuint(f.purges, ==, 2);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, now(&f) + DAY);
  /* A longer period waits for the daily run; a shorter one runs now. */
  gh_expiry_set_retention_days(f.expiry, 400);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, now(&f) + DAY);
  advance(&f, 5 * DAY); /* T0 + 16 days */
  gh_expiry_set_retention_days(f.expiry, 30);
  g_assert_cmpint(gh_expiry_get_next_run(f.expiry), ==, now(&f));
  run_due(&f);
  g_assert_cmpint(stored(f.store, fresh_id), ==, 1);
  advance(&f, 14 * DAY); /* T0 + 30 days: received exactly 30 days ago */
  g_assert_cmpint(stored(f.store, fresh_id), ==, 1);
  advance(&f, DAY);
  g_assert_cmpint(stored(f.store, fresh_id), ==, 0);
  g_assert_null(gh_conversation_store_lookup(f.model, ab));
  fixture_clear(&f);
}

/* ---- the model when every listed message went --------------------------------------------- */

static void
test_model_pages(void)
{
  Fixture f;
  fixture_init_at(&f, T0, 0, 2, TRUE);
  g_autofree gchar *ab = room_of(hex_alice, hex_bob);
  /* Four messages, two listed (page size 2); the listed two expire. An own
   * reply among them accepted the room. */
  Rumor m1 = { hex_bob, hex_alice, T0 - 40, "one", "Trip", 0 };
  Rumor m2 = { hex_alice, hex_bob, T0 - 30, "two", NULL, 0 };
  Rumor m3 = { hex_bob, hex_alice, T0 - 20, "three", NULL, T0 + 30 };
  Rumor m4 = { hex_alice, hex_bob, T0 - 10, "four", "Trip!", T0 + 30 };
  g_assert_cmpint(deliver(&f, &m1, "w/1", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &m2, "w/2", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &m3, "w/3", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(deliver(&f, &m4, "w/4", 0), ==, GH_CONVERSATION_ADD_NEW);
  fixture_close(&f, FALSE);
  fixture_open(&f, TRUE);
  GhConversation *room = gh_conversation_store_lookup(f.model, ab);
  g_assert_cmpuint(n_items(room), ==, 2);
  g_assert_true(gh_conversation_get_has_older(room));
  g_assert_cmpstr(gh_conversation_get_title(room), ==, "Trip!");
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "four");
  g_assert_false(gh_conversation_get_is_request(room));
  g_assert_true(gh_expiry_purge(f.expiry, NULL));

  /* Both listed messages go: the room shows its newest remaining page, its
   * name comes from the newest remaining subject, and it stays accepted. */
  advance(&f, 30);
  g_assert_cmpuint(f.purged->len, ==, 2);
  g_assert_cmpuint(n_items(room), ==, 2);
  g_assert_false(gh_conversation_get_has_older(room));
  g_assert_cmpstr(gh_conversation_get_preview(room), ==, "two");
  g_assert_cmpstr(gh_conversation_get_title(room), ==, "Trip");
  g_assert_cmpint(gh_conversation_get_last_activity(room), ==, T0 - 30);
  g_assert_false(gh_conversation_get_is_request(room));
  g_assert_cmpuint(gh_conversation_get_unread_count(room), ==, 0);
  g_assert_cmpint(stored_unread(f.store, ab), ==, 0);
  assert_read_state_consistent(f.store);
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM conversations WHERE title = 'Trip'"),
                  ==, 1);
  fixture_clear(&f);
}

/* ---- per-conversation timers ---------------------------------------------------------------- */

static void
test_timers(void)
{
  Fixture f;
  fixture_init(&f, TRUE);
  g_autofree gchar *ab = room_of(hex_alice, hex_bob);
  g_autofree gchar *ac = room_of(hex_alice, hex_carol);
  g_autofree gchar *bc = room_of(hex_bob, hex_carol);
  g_autofree gchar *unsorted = strcmp(hex_alice, hex_bob) < 0
    ? g_strconcat(hex_bob, ",", hex_alice, NULL) : g_strconcat(hex_alice, ",", hex_bob, NULL);
  gint64 timer = -1;
  g_autoptr(GError) error = NULL;

  /* Off by default; a room not stored yet reports the default. */
  g_assert_true(gh_expiry_get_timer(f.expiry, ab, &timer, &error));
  g_assert_cmpint(timer, ==, 0);
  gh_expiry_set_default_timer(f.expiry, GH_EXPIRY_TIMER_WEEK);
  g_assert_true(gh_expiry_get_timer(f.expiry, ac, &timer, &error));
  g_assert_cmpint(timer, ==, GH_EXPIRY_TIMER_WEEK);
  /* However a conversation is created, it starts with the default. */
  Rumor hello = { hex_carol, hex_alice, T0 - 5, "hello", NULL, 0 };
  g_assert_cmpint(deliver(&f, &hello, "w/hello", 0), ==, GH_CONVERSATION_ADD_NEW);
  g_assert_cmpint(sql_int(f.store, "SELECT disappearing_s FROM conversations WHERE "
                                   "backend_key = '%s'", ac), ==, GH_EXPIRY_TIMER_WEEK);
  g_assert_cmpuint(f.timers->len, ==, 0);

  /* Set on a room not stored yet: created as accepted, announced once. */
  g_assert_true(gh_expiry_set_timer(f.expiry, ab, DAY, &error));
  g_assert_no_error(error);
  g_assert_cmpuint(f.timers->len, ==, 1);
  g_autofree gchar *announced = g_strdup_printf("%s=%" G_GINT64_FORMAT, ab, DAY);
  g_assert_cmpstr(g_ptr_array_index(f.timers, 0), ==, announced);
  g_assert_cmpint(sql_int(f.store, "SELECT disappearing_s FROM conversations WHERE "
                                   "backend_key = '%s' AND request_state = 0", ab), ==, DAY);
  g_assert_true(gh_expiry_get_timer(f.expiry, ab, &timer, &error));
  g_assert_cmpint(timer, ==, DAY);
  g_assert_true(gh_expiry_set_timer(f.expiry, ab, DAY, &error));
  g_assert_cmpuint(f.timers->len, ==, 1);
  g_assert_true(gh_expiry_set_timer(f.expiry, ab, GH_EXPIRY_TIMER_OFF, &error));
  g_assert_cmpuint(f.timers->len, ==, 2);
  g_assert_true(gh_expiry_get_timer(f.expiry, ab, &timer, &error));
  g_assert_cmpint(timer, ==, 0);

  /* Only the four timers, only canonical rooms of this account. */
  const gint64 refused[] = { HOUR, 2 * DAY, -1 };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
    g_assert_false(gh_expiry_set_timer(f.expiry, ab, refused[i], &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
  }
  const gchar *rooms[] = { bc, unsorted, "", "not-a-room", NULL };
  for (guint i = 0; i < G_N_ELEMENTS(rooms); i++) {
    g_assert_false(gh_expiry_set_timer(f.expiry, rooms[i], DAY, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_assert_false(gh_expiry_get_timer(f.expiry, rooms[i], &timer, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
  }
  g_assert_cmpint(sql_int(f.store, "SELECT count(*) FROM conversations"), ==, 2);
  /* A default that is not a timer turns it off. */
  gh_expiry_set_default_timer(f.expiry, 12345);
  g_autofree gchar *ad = room_of(hex_alice, hex_dave);
  g_assert_true(gh_expiry_get_timer(f.expiry, ad, &timer, &error));
  g_assert_cmpint(timer, ==, 0);
  /* The store bounds what it keeps whatever the caller. */
  g_assert_false(gh_store_set_disappearing(f.store, 1, GH_STORE_MAX_DISAPPEARING + 1, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_INVALID);
  g_clear_error(&error);
  g_assert_false(gh_store_set_disappearing(f.store, 999, DAY, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  g_clear_error(&error);
  fixture_clear(&f);
}

/* ---- H8: crash inside T-purge ------------------------------------------------------------- */

typedef struct {
  GBytes *key;
  const gchar *store_id;
  gint64 at;
} PurgeScript;

/* The child: open, purge (30 days of retention), crash at the armed point. */
static void
script_purge(gpointer data)
{
  PurgeScript *s = data;
  GhClock *clock = gh_clock_new_fake(s->at * G_USEC_PER_SEC);
  GhStore *store = store_open(s->key, s->store_id, clock, NULL);
  if (!store)
    _exit(3);
  gh_store_purge_full(store, s->at - 30 * DAY, NULL, NULL, NULL);
  /* Returning means the cut point was never reached (COMPLETED). */
}

static void
test_crash_purge(void)
{
  g_auto(GStrv) cuts = gh_store_test_list_cut_points("purge:");
  g_assert_cmpuint(g_strv_length(cuts), >=, 6);
  for (guint i = 0; cuts[i]; i++) {
    Fixture f;
    fixture_init_at(&f, T0 - 40 * DAY, 0, 0, FALSE);
    g_autofree gchar *ab = room_of(hex_alice, hex_bob);
    /* An old message (retention), one that expires, both read; one unread. */
    Rumor old = { hex_bob, hex_alice, T0 - 40 * DAY, "old", NULL, 0 };
    Rumor goes = { hex_bob, hex_alice, T0 - 70, "goes", NULL, T0 + 10 };
    Rumor last = { hex_bob, hex_alice, T0 - 65, "last", NULL, 0 };
    g_autofree gchar *old_id = rumor_id(&old);
    g_autofree gchar *goes_id = rumor_id(&goes);
    g_assert_cmpint(deliver(&f, &old, "w/old", 0), ==, GH_CONVERSATION_ADD_NEW);
    advance(&f, 40 * DAY - 60);
    g_assert_cmpint(deliver(&f, &goes, "w/goes", 0), ==, GH_CONVERSATION_ADD_NEW);
    gh_conversation_mark_read(gh_conversation_store_lookup(f.model, ab));
    g_assert_cmpint(deliver(&f, &last, "w/last", 0), ==, GH_CONVERSATION_ADD_NEW);
    fixture_close(&f, FALSE);

    PurgeScript script = { f.key, f.store_id, T0 + 20 };
    GhCrashOutcome outcome = gh_crash_harness_run(cuts[i], 1, script_purge, &script);
    if (outcome != GH_CRASH_KILLED)
      g_error("cut point %s: %s", cuts[i], gh_crash_outcome_to_string(outcome));

    /* All of the purge or none of it, and the read state consistent. */
    advance(&f, 80);
    fixture_open(&f, FALSE);
    assert_integrity(f.store);
    assert_read_state_consistent(f.store);
    gint64 left = stored(f.store, old_id) + stored(f.store, goes_id);
    g_assert_true(left == 0 || left == 2);
    g_assert_cmpint(stored_unread(f.store, ab), ==, 1);
    GhStorePurgeStats stats;
    g_assert_true(gh_store_purge(f.store, now(&f) - 30 * DAY, &stats, NULL));
    g_assert_cmpuint(stats.n_expired + stats.n_retention, ==, (guint)left);
    g_assert_cmpint(stored(f.store, old_id) + stored(f.store, goes_id), ==, 0);
    assert_read_state_consistent(f.store);
    g_assert_cmpint(stored_unread(f.store, ab), ==, 1);
    fixture_clear(&f);
  }
}

int
main(int argc, char **argv)
{
  umask(022);
  g_test_init(&argc, &argv, G_TEST_OPTION_ISOLATE_DIRS, NULL);
  if (sodium_init() < 0)
    g_error("libsodium failed to initialize");
  hex_alice = hex_of("alice");
  hex_bob = hex_of("bob");
  hex_carol = hex_of("carol");
  hex_dave = hex_of("dave");

  /* The crash scenarios fork; they run before any D-Bus connection exists. */
  g_test_add_func("/groundhog/expiry/crash/purge", test_crash_purge);
  g_test_add_func("/groundhog/expiry/policy", test_policy);
  g_test_add_func("/groundhog/expiry/pt7/rounding", test_pt7_rounding);
  g_test_add_func("/groundhog/expiry/pt7/jitter", test_pt7_jitter);
  g_test_add_func("/groundhog/expiry/ex3/purge-on-time", test_ex3_purge_on_time);
  g_test_add_func("/groundhog/expiry/ex3/at-open", test_ex3_at_open);
  g_test_add_func("/groundhog/expiry/retry", test_retry);
  g_test_add_func("/groundhog/expiry/read-only", test_read_only);
  g_test_add_func("/groundhog/expiry/ex4/arrival", test_ex4_arrival);
  g_test_add_func("/groundhog/expiry/ex5/retention", test_ex5_retention);
  g_test_add_func("/groundhog/expiry/model/pages", test_model_pages);
  g_test_add_func("/groundhog/expiry/timers", test_timers);
  g_test_add_func("/groundhog/expiry/ex1/envelope", test_expiry_ex1_envelope);
  g_test_add_func("/groundhog/expiry/ex1/outbox", test_expiry_ex1_outbox);
  int status = g_test_run();
  test_expiry_send_cleanup();
  g_free(hex_alice);
  g_free(hex_bob);
  g_free(hex_carol);
  g_free(hex_dave);
  return status;
}
