/*
 * test_kp_discovery.c — Marmot KeyPackage discovery via kind:10002 write
 * relays (nostrc-prqu.11).
 *
 * In-memory "relays" behind GnKpBackend. The invitee's kind:10002 lists a
 * write relay A, an unmarked relay B and a read-only relay C. A serves a
 * stale kind:30443 KeyPackage, B the rotated one (same (pubkey, d) slot),
 * and C an even newer one that must never be consulted. Real KeyPackages
 * from marmot-gobject, re-signed to set created_at.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gn-key-package-discovery.h"

#include <marmot-gobject-1.0/marmot-gobject.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <string.h>

#define SK_I "7f7ff03d123792d6ac594bfa67bf6d0c0ab55b6b1fdb6249303fe861f1ccba9a"
#define SK_X "3a3e5e1f0b87d1c0e6a4b3f2d8a9c7e6f5d4c3b2a1908f7e6d5c4b3a29181716"

static char *pk_i;

/* ---- events ---------------------------------------------------------------- */

static char *
resign(const char *json, const char *sk, gint64 created_at)
{
  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(ev, json, NULL), ==, 1);
  nostr_event_set_created_at(ev, created_at);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  char *out = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return out;
}

static char *
relay_list(const char *sk, gint64 created_at, const char *const *r_tags /* url,marker pairs */)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 10002);
  nostr_event_set_created_at(ev, created_at);
  nostr_event_set_content(ev, "");
  NostrTags *tags = nostr_tags_new(0);
  for (gsize i = 0; r_tags[i]; i += 2) {
    NostrTag *t = *r_tags[i + 1] ? nostr_tag_new("r", r_tags[i], r_tags[i + 1], NULL)
                                 : nostr_tag_new("r", r_tags[i], NULL);
    nostr_tags_append(tags, t);
  }
  nostr_event_set_tags(ev, tags);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  char *out = nostr_event_serialize_compact(ev);
  nostr_event_free(ev);
  return out;
}

typedef struct { GMainLoop *loop; GAsyncResult *res; } Wait;

static void
on_async(GObject *s, GAsyncResult *res, gpointer ud)
{
  (void)s;
  Wait *w = ud;
  w->res = g_object_ref(res);
  g_main_loop_quit(w->loop);
}

static char *
make_key_package(MarmotGobjectClient *client)
{
  Wait w = { g_main_loop_new(NULL, FALSE), NULL };
  const char *relays[] = { "wss://a.example", NULL };
  marmot_gobject_client_create_key_package_async(client, pk_i, SK_I, relays, NULL, on_async, &w);
  g_main_loop_run(w.loop);
  g_autoptr(GError) error = NULL;
  char *json = marmot_gobject_client_create_key_package_finish(client, w.res, &error);
  g_assert_no_error(error);
  g_object_unref(w.res);
  g_main_loop_unref(w.loop);
  return json;
}

/* ---- fake backend ------------------------------------------------------------ */

typedef struct {
  GPtrArray *local;          /* event JSONs */
  GHashTable *relays;        /* url -> GPtrArray of event JSONs */
  GPtrArray *queried;        /* "url kinds" strings */
  char **own;
} Fake;

static int
filter_kind(const char *filter)
{
  const char *k = strstr(filter, "\"kinds\":[");
  return k ? atoi(k + 9) : -1;
}

static gboolean
event_has_kind(const char *json, int kind)
{
  g_autofree char *needle = g_strdup_printf("\"kind\":%d,", kind);
  return strstr(json, needle) != NULL;
}

static GPtrArray *
fake_query_local(gpointer data, const char *filter)
{
  Fake *f = data;
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  int kind = filter_kind(filter);
  for (guint i = 0; i < f->local->len; i++)
    if (event_has_kind(g_ptr_array_index(f->local, i), kind))
      g_ptr_array_add(out, g_strdup(g_ptr_array_index(f->local, i)));
  return out;
}

static void
fake_query_relays_async(gpointer data, const char *const *relays, const char *filter,
                        GCancellable *c, GAsyncReadyCallback cb, gpointer ud)
{
  Fake *f = data;
  GTask *task = g_task_new(NULL, c, cb, ud);
  GPtrArray *out = g_ptr_array_new_with_free_func(g_free);
  int kind = filter_kind(filter);
  for (gsize r = 0; relays && relays[r]; r++) {
    g_ptr_array_add(f->queried, g_strdup_printf("%s %d", relays[r], kind));
    GPtrArray *evs = g_hash_table_lookup(f->relays, relays[r]);
    for (guint i = 0; evs && i < evs->len; i++)
      if (event_has_kind(g_ptr_array_index(evs, i), kind))
        g_ptr_array_add(out, g_strdup(g_ptr_array_index(evs, i)));
  }
  g_task_return_pointer(task, out, (GDestroyNotify)g_ptr_array_unref);
  g_object_unref(task);
}

static GPtrArray *
fake_query_relays_finish(gpointer data, GAsyncResult *res, GError **error)
{
  (void)data;
  return g_task_propagate_pointer(G_TASK(res), error);
}

static char **
fake_own_relays(gpointer data)
{
  return g_strdupv(((Fake *)data)->own);
}

static void
fake_add(Fake *f, const char *url, const char *json)
{
  GPtrArray *evs = g_hash_table_lookup(f->relays, url);
  if (!evs) {
    evs = g_ptr_array_new_with_free_func(g_free);
    g_hash_table_insert(f->relays, g_strdup(url), evs);
  }
  g_ptr_array_add(evs, g_strdup(json));
}

static Fake *
fake_new(void)
{
  Fake *f = g_new0(Fake, 1);
  f->local = g_ptr_array_new_with_free_func(g_free);
  f->relays = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify)g_ptr_array_unref);
  f->queried = g_ptr_array_new_with_free_func(g_free);
  const char *own[] = { "wss://own.example", NULL };
  f->own = g_strdupv((char **)own);
  return f;
}

static void
fake_free(Fake *f)
{
  g_ptr_array_unref(f->local);
  g_hash_table_unref(f->relays);
  g_ptr_array_unref(f->queried);
  g_strfreev(f->own);
  g_free(f);
}

static gboolean
queried(Fake *f, const char *what)
{
  for (guint i = 0; i < f->queried->len; i++)
    if (g_strcmp0(g_ptr_array_index(f->queried, i), what) == 0)
      return TRUE;
  return FALSE;
}

static char *
discover(Fake *f, GError **error)
{
  GnKpBackend b = { fake_query_local, fake_query_relays_async, fake_query_relays_finish,
                    fake_own_relays, f };
  Wait w = { g_main_loop_new(NULL, FALSE), NULL };
  gn_kp_discover_async(&b, pk_i, NULL, on_async, &w);
  if (!w.res)
    g_main_loop_run(w.loop);
  char *json = gn_kp_discover_finish(w.res, error);
  g_object_unref(w.res);
  g_main_loop_unref(w.loop);
  return json;
}

/* ---- tests ---------------------------------------------------------------------- */

static void
test_write_relays_parsing(void)
{
  const char *tags[] = { "wss://a.example", "write", "wss://b.example/", "",
                         "wss://c.example", "read", "wss://b.example", "",
                         "https://not-a-relay.example", "", "wss://d.example", "WRITE", NULL };
  g_autofree char *list = relay_list(SK_I, 1700000000, tags);
  g_auto(GStrv) w = gn_kp_write_relays_from_relay_list(list);
  g_assert_cmpuint(g_strv_length(w), ==, 2);
  g_assert_cmpstr(w[0], ==, "wss://a.example");
  g_assert_cmpstr(w[1], ==, "wss://b.example");  /* unmarked, de-duplicated */

  g_auto(GStrv) none = gn_kp_write_relays_from_relay_list("{\"kind\":3,\"tags\":[[\"r\",\"wss://x\"]]}");
  g_assert_cmpuint(g_strv_length(none), ==, 0);
  g_auto(GStrv) junk = gn_kp_write_relays_from_relay_list("not json");
  g_assert_nonnull(junk);
  g_assert_null(junk[0]);
}

static void
test_discovery(void)
{
  MarmotGobjectMemoryStorage *store = marmot_gobject_memory_storage_new();
  MarmotGobjectClient *client = marmot_gobject_client_new(MARMOT_GOBJECT_STORAGE(store));
  g_autofree char *kp1 = make_key_package(client);
  g_autofree char *kp2 = make_key_package(client);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree char *stale = resign(kp1, SK_I, now - 1000);
  g_autofree char *rotated = resign(kp2, SK_I, now - 10);
  g_autofree char *decoy = resign(kp1, SK_I, now + 1000);  /* read-only relay only */

  const char *tags[] = { "wss://a.example", "write", "wss://b.example/", "",
                         "wss://c.example", "read", NULL };
  g_autofree char *list = relay_list(SK_I, now - 5000, tags);
  g_autoptr(GError) error = NULL;

  /* 1. Relay list in the local store: KeyPackages only from A and B. */
  Fake *f = fake_new();
  g_ptr_array_add(f->local, g_strdup(list));
  fake_add(f, "wss://a.example", stale);
  fake_add(f, "wss://b.example", rotated);
  fake_add(f, "wss://c.example", decoy);
  g_autofree char *got = discover(f, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(got, ==, rotated);
  g_assert_true(queried(f, "wss://a.example 30443"));
  g_assert_true(queried(f, "wss://b.example 30443"));
  g_assert_false(queried(f, "wss://c.example 30443"));
  g_assert_false(queried(f, "wss://own.example 10002"));
  fake_free(f);

  /* 2. No local relay list: fetched from our own relays first. */
  f = fake_new();
  fake_add(f, "wss://own.example", list);
  fake_add(f, "wss://a.example", stale);
  fake_add(f, "wss://b.example", rotated);
  fake_add(f, "wss://c.example", decoy);
  g_autofree char *got2 = discover(f, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(got2, ==, rotated);
  g_assert_true(queried(f, "wss://own.example 10002"));
  g_assert_false(queried(f, "wss://c.example 30443"));
  fake_free(f);

  /* 3. A forged relay list pointing at a hostile relay is ignored. */
  f = fake_new();
  const char *evil_tags[] = { "wss://evil.example", "", NULL };
  g_autofree char *evil_list = relay_list(SK_X, now, evil_tags);
  /* Claim the invitee's pubkey without their signature. */
  g_autoptr(GString) forged = g_string_new(evil_list);
  g_autofree char *pk_x = nostr_key_get_public(SK_X);
  g_string_replace(forged, pk_x, pk_i, 0);
  fake_add(f, "wss://own.example", forged->str);
  fake_add(f, "wss://evil.example", stale);
  g_ptr_array_add(f->local, g_strdup(rotated));
  g_autofree char *got3 = discover(f, &error);
  g_assert_no_error(error);
  g_assert_cmpstr(got3, ==, rotated);           /* local store only */
  g_assert_false(queried(f, "wss://evil.example 30443"));
  fake_free(f);

  /* 4. Nothing anywhere: NOT_FOUND. */
  f = fake_new();
  g_autofree char *none = discover(f, &error);
  g_assert_null(none);
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
  g_clear_error(&error);
  fake_free(f);

  g_object_unref(client);
  g_object_unref(store);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  pk_i = nostr_key_get_public(SK_I);
  g_test_add_func("/mls/kp-discovery/write-relays", test_write_relays_parsing);
  g_test_add_func("/mls/kp-discovery/stale-vs-rotated", test_discovery);
  return g_test_run();
}
