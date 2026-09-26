/* Engine behaviour against a mock session relay on a private AF_UNIX
 * socket: identifier look-ups, NIP-50 and scan fallbacks, the deadline
 * with a relay that never answers (cache-less session relay), the circuit
 * breaker, partial results, subsearch narrowing, NIP-05, cancellation. */
#include <string.h>

#include "nsp-engine.h"
#include "nsp-item.h"
#include "nsp-testutil.h"

#define DEADLINE_MS 400
#define SLACK_MS 250

typedef struct {
  gboolean done;
  char **ids;
  NspSearchStats st;
} Run;

static void on_done(GObject *src, GAsyncResult *res, gpointer user_data) {
  Run *r = user_data;
  r->ids = nsp_engine_search_finish(res, &r->st);
  r->done = TRUE;
}

static char **search(NspEngine *e, const char *const *terms, const char *const *prev,
                     NspSearchStats *st) {
  Run r = {0};
  nsp_engine_search_async(e, terms, prev, on_done, &r);
  g_assert_true(nsp_test_wait(&r.done, 5000));
  if (st) *st = r.st;
  return r.ids;
}

static char **search1(NspEngine *e, const char *term, NspSearchStats *st) {
  const char *terms[] = {term, NULL};
  return search(e, terms, NULL, st);
}

static NspEngine *engine_for(NspMockRelay *m, guint deadline_ms) {
  NspEngineOptions o = {.socket_path = m ? nsp_mock_relay_socket(m) : "/nonexistent/relay.sock",
                        .deadline_ms = deadline_ms};
  return nsp_engine_new(&o);
}

/* name/description of the meta for @id */
static void meta_of(NspEngine *e, const char *id, char **name, char **desc, GIcon **icon) {
  const char *ids[] = {id, NULL};
  g_autoptr(GVariant) metas = g_variant_ref_sink(nsp_engine_result_metas(e, ids));
  g_assert_cmpuint(g_variant_n_children(metas), ==, 1);
  g_autoptr(GVariant) m = g_variant_get_child_value(metas, 0);
  const char *v = NULL;
  g_assert_true(g_variant_lookup(m, "id", "&s", &v));
  g_assert_cmpstr(v, ==, id);
  g_assert_true(g_variant_lookup(m, "clipboardText", "&s", &v));
  g_assert_cmpstr(v, ==, id);
  g_assert_true(g_variant_lookup(m, "name", "s", name));
  g_assert_true(g_variant_lookup(m, "description", "s", desc));
  g_autoptr(GVariant) iv = g_variant_lookup_value(m, "icon", NULL);
  g_assert_nonnull(iv);
  if (icon) *icon = g_icon_deserialize(iv);
}

typedef struct {
  NspMockRelay *m;
  char *alice_pk, *bob_pk;
  char *alice_profile, *bob_profile, *note_hello_world, *note_hello_there, *note_other,
      *article;
} Fixture;

static void fixture_fill(Fixture *f, NspMockMode mode, gboolean nip50) {
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  f->m = nsp_mock_relay_new(mode, nip50);
  f->alice_pk = nsp_test_pubkey(NSP_TEST_SK_ALICE);
  f->bob_pk = nsp_test_pubkey(NSP_TEST_SK_BOB);
  f->alice_profile = nsp_test_event(NSP_TEST_SK_ALICE, 0, now - 1000,
                                    "{\"display_name\":\"Alice\",\"nip05\":\"alice@nos.social\"}", NULL);
  f->bob_profile = nsp_test_event(NSP_TEST_SK_BOB, 0, now - 1000,
                                  "{\"name\":\"bob\",\"nip05\":\"bob@nos.social\",\"about\":\"hello fan\"}", NULL);
  f->note_hello_world = nsp_test_event(NSP_TEST_SK_ALICE, 1, now - 60, "Hello world from Alice", NULL);
  f->note_hello_there = nsp_test_event(NSP_TEST_SK_BOB, 1, now - 30, "hello there", NULL);
  f->note_other = nsp_test_event(NSP_TEST_SK_BOB, 1, now - 10, "unrelated", NULL);
  const char *tags[] = {"d", "essay", "title", "Hello essay", NULL};
  f->article = nsp_test_event(NSP_TEST_SK_ALICE, 30023, now - 5000, "essay body", tags);
  const char *all[] = {f->alice_profile, f->bob_profile, f->article, f->note_hello_world,
                       f->note_hello_there, f->note_other};
  for (guint i = 0; i < G_N_ELEMENTS(all); i++) nsp_mock_relay_add(f->m, all[i]);
}

static void fixture_clear(Fixture *f) {
  nsp_mock_relay_free(f->m);
  g_free(f->alice_pk);
  g_free(f->bob_pk);
  g_free(f->alice_profile);
  g_free(f->bob_profile);
  g_free(f->note_hello_world);
  g_free(f->note_hello_there);
  g_free(f->note_other);
  g_free(f->article);
}

static char *uri_of(const char *json) {
  g_autoptr(NdEvent) ev = nd_event_parse(json, -1, NULL);
  g_autoptr(NspItem) it = nsp_item_new_from_event(ev);
  return g_strdup(it->uri);
}

static char *npub_uri(const char *pk) {
  NdTarget t = {.entity = ND_ENTITY_PROFILE, .pubkey_hex = (char *)pk, .kind = 0};
  return nd_target_to_uri(&t);
}

/* ------------------------------------------------------------------ */

static void test_profile_lookup(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  g_autofree char *np = npub_uri(f.alice_pk);
  NspSearchStats st;
  g_auto(GStrv) ids = search1(e, np, &st);
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_assert_cmpstr(ids[0], ==, np);
  g_assert_cmpint(st.primary, ==, NSP_RELAY_EOSE);
  g_assert_cmpuint(st.requests, ==, 1);
  g_assert_false(st.deadline_hit);
  g_autofree char *name = NULL, *desc = NULL;
  g_autoptr(GIcon) icon = NULL;
  meta_of(e, ids[0], &name, &desc, &icon);
  g_assert_cmpstr(name, ==, "Alice");
  g_assert_true(g_str_has_prefix(desc, "alice@nos.social \xc2\xb7 npub1")); /* unverified: no ✓ */
  g_assert_true(G_IS_THEMED_ICON(icon)); /* no avatar cache: themed fallback */
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_note_lookup_fetches_author(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  g_autofree char *id = nsp_test_event_id(f.note_hello_world);
  NdTarget t = {.entity = ND_ENTITY_EVENT, .id_hex = id, .kind = -1};
  g_autofree char *note = nd_target_to_uri(&t); /* note1… (no kind) */
  NspSearchStats st;
  g_auto(GStrv) ids = search1(e, note, &st);
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_autofree char *want = uri_of(f.note_hello_world);
  g_assert_cmpstr(ids[0], ==, want); /* canonical nevent with kind + author */
  g_assert_true(g_str_has_prefix(ids[0], "nostr:nevent1"));
  g_assert_cmpuint(st.requests, ==, 2); /* event, then its author's kind 0 */
  g_autofree char *name = NULL, *desc = NULL;
  meta_of(e, ids[0], &name, &desc, NULL);
  g_assert_cmpstr(name, ==, "Alice \xc2\xb7 1 min ago");
  g_assert_cmpstr(desc, ==, "Hello world from Alice");
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_naddr_and_hex(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  NdTarget t = {.entity = ND_ENTITY_ADDRESS, .pubkey_hex = f.alice_pk, .kind = 30023,
                .identifier = (char *)"essay"};
  g_autofree char *naddr = nd_target_to_uri(&t);
  g_auto(GStrv) ids = search1(e, naddr, NULL);
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_assert_cmpstr(ids[0], ==, naddr);
  g_autofree char *name = NULL, *desc = NULL;
  meta_of(e, ids[0], &name, &desc, NULL);
  g_assert_cmpstr(desc, ==, "Hello essay");

  g_auto(GStrv) hex = search1(e, f.bob_pk, NULL);
  g_assert_cmpuint(g_strv_length(hex), ==, 1);
  g_autofree char *bob = npub_uri(f.bob_pk);
  g_assert_cmpstr(hex[0], ==, bob);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_text_nip50(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  NspSearchStats st;
  g_auto(GStrv) ids = search1(e, "hello", &st);
  g_assert_false(st.fallback_scan);
  g_assert_cmpuint(nsp_mock_relay_search_reqs(f.m), ==, 1);
  /* the mock's NIP-50 matches raw content: bob's kind 0 ("about": "hello
   * fan") and two notes; the article's "Hello essay" is a title tag */
  g_autofree char *hw = uri_of(f.note_hello_world), *ht = uri_of(f.note_hello_there);
  g_autofree char *bob = npub_uri(f.bob_pk);
  g_assert_cmpuint(g_strv_length(ids), ==, 3);
  g_assert_cmpstr(ids[0], ==, bob); /* profiles first */
  g_assert_cmpstr(ids[1], ==, ht);  /* then notes, newest first */
  g_assert_cmpstr(ids[2], ==, hw);
  g_autofree char *name = NULL, *desc = NULL;
  meta_of(e, ids[1], &name, &desc, NULL);
  g_assert_true(g_str_has_prefix(name, "bob \xc2\xb7 ")); /* author fetched */
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_text_scan_fallback(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, FALSE); /* no NIP-50: CLOSED unsupported */
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  NspSearchStats st;
  g_auto(GStrv) ids = search1(e, "HELLO", &st);
  g_assert_true(st.fallback_scan);
  g_assert_cmpint(st.primary, ==, NSP_RELAY_CLOSED);
  g_assert_cmpuint(st.requests, ==, 2); /* search (CLOSED) + scan; authors came with the scan */
  /* client-side match: bob's profile (about), article (title), 2 notes */
  g_assert_cmpuint(g_strv_length(ids), ==, 4);
  g_autofree char *bob = npub_uri(f.bob_pk);
  g_assert_cmpstr(ids[0], ==, bob); /* profiles first */
  g_autofree char *ht = uri_of(f.note_hello_there), *art = uri_of(f.article);
  g_assert_cmpstr(ids[1], ==, ht);
  g_assert_cmpstr(ids[3], ==, art); /* oldest */
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_deadline_silent_relay_and_circuit(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_SILENT, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  g_autofree char *np = npub_uri(f.alice_pk);
  NspSearchStats st;
  gint64 t0 = g_get_monotonic_time();
  g_auto(GStrv) ids = search1(e, np, &st);
  gint64 ms = (g_get_monotonic_time() - t0) / 1000;
  g_assert_cmpint(ms, <=, DEADLINE_MS + SLACK_MS);
  g_assert_cmpint(ms, >=, DEADLINE_MS / 2);
  g_assert_cmpint(st.primary, ==, NSP_RELAY_TIMEOUT);
  /* nothing stored locally, but the identifier is still actionable */
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_assert_cmpstr(ids[0], ==, np);
  g_autofree char *name = NULL, *desc = NULL;
  meta_of(e, ids[0], &name, &desc, NULL);
  g_assert_cmpstr(name, ==, "Open Nostr profile");

  /* the relay never EOSE'd: the breaker now skips it, answers are instant */
  guint reqs = nsp_mock_relay_reqs(f.m);
  t0 = g_get_monotonic_time();
  g_auto(GStrv) text = search1(e, "hello", &st);
  ms = (g_get_monotonic_time() - t0) / 1000;
  g_assert_true(st.circuit_open);
  g_assert_cmpuint(g_strv_length(text), ==, 0);
  g_assert_cmpint(ms, <, 100);
  g_auto(GStrv) again = search1(e, np, &st);
  g_assert_cmpuint(g_strv_length(again), ==, 1);
  g_assert_cmpuint(nsp_mock_relay_reqs(f.m), ==, reqs);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_partial_results_no_eose(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_NO_EOSE, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  NspSearchStats st;
  gint64 t0 = g_get_monotonic_time();
  g_auto(GStrv) ids = search1(e, "hello", &st);
  gint64 ms = (g_get_monotonic_time() - t0) / 1000;
  g_assert_cmpint(ms, <=, DEADLINE_MS + SLACK_MS);
  g_assert_cmpint(st.primary, ==, NSP_RELAY_TIMEOUT);
  g_assert_cmpuint(g_strv_length(ids), ==, 3); /* what arrived before the deadline */
  g_assert_false(st.circuit_open);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_no_socket(void) {
  NspEngine *e = engine_for(NULL, DEADLINE_MS);
  g_autofree char *pk = nsp_test_pubkey(NSP_TEST_SK_ALICE);
  g_autofree char *np = npub_uri(pk);
  NspSearchStats st;
  gint64 t0 = g_get_monotonic_time();
  g_auto(GStrv) ids = search1(e, np, &st);
  g_assert_cmpint((g_get_monotonic_time() - t0) / 1000, <, 100);
  g_assert_cmpint(st.primary, ==, NSP_RELAY_UNAVAILABLE);
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_auto(GStrv) text = search1(e, "hello", NULL);
  g_assert_cmpuint(g_strv_length(text), ==, 0);
  nsp_engine_free(e);
}

static void test_subsearch_narrowing(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  g_auto(GStrv) first = search1(e, "hello", NULL);
  g_assert_cmpuint(g_strv_length(first), ==, 3);
  guint reqs = nsp_mock_relay_reqs(f.m);

  const char *refined[] = {"hello", "WORLD", NULL};
  NspSearchStats st;
  g_auto(GStrv) sub = search(e, refined, (const char *const *)first, &st);
  g_assert_true(st.narrowed);
  g_assert_cmpuint(st.requests, ==, 0);
  g_assert_cmpuint(nsp_mock_relay_reqs(f.m), ==, reqs); /* no I/O */
  g_assert_cmpuint(g_strv_length(sub), ==, 1);
  g_autofree char *hw = uri_of(f.note_hello_world);
  g_assert_cmpstr(sub[0], ==, hw);

  /* nothing survives narrowing → a fresh search */
  const char *miss[] = {"hello", "zebra", NULL};
  g_auto(GStrv) sub2 = search(e, miss, (const char *const *)first, &st);
  g_assert_false(st.narrowed);
  g_assert_cmpuint(st.requests, ==, 1);
  g_assert_cmpuint(g_strv_length(sub2), ==, 0);

  /* empty previous set (e.g. "he" was too short) → fresh search */
  const char *none[] = {NULL};
  const char *hel[] = {"hello", NULL};
  g_auto(GStrv) sub3 = search(e, hel, none, &st);
  g_assert_false(st.narrowed);
  g_assert_cmpuint(g_strv_length(sub3), ==, 3);

  /* identifiers are exact look-ups, never narrowed */
  g_autofree char *np = npub_uri(f.alice_pk);
  const char *id_terms[] = {np, NULL};
  g_auto(GStrv) sub4 = search(e, id_terms, (const char *const *)first, &st);
  g_assert_false(st.narrowed);
  g_assert_cmpuint(g_strv_length(sub4), ==, 1);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_nip05(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  /* resolved (cache seeded, as after a background HTTPS look-up) */
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  nsp_nip05_cache_put(nsp_engine_get_nip05(e), "alice@nos.social", f.alice_pk);
  g_auto(GStrv) ids = search1(e, "Alice@Nos.Social", NULL);
  g_assert_cmpuint(g_strv_length(ids), ==, 1);
  g_autofree char *np = npub_uri(f.alice_pk);
  g_assert_cmpstr(ids[0], ==, np);
  g_autofree char *name = NULL, *desc = NULL;
  meta_of(e, ids[0], &name, &desc, NULL);
  g_assert_cmpstr(name, ==, "Alice");
  g_assert_true(g_str_has_prefix(desc, "\xe2\x9c\x93 alice@nos.social")); /* verified */
  nsp_engine_free(e);

  /* unresolved, network off: local kind-0 claims only, exact match */
  NspEngine *e2 = engine_for(f.m, DEADLINE_MS);
  g_auto(GStrv) claim = search1(e2, "bob@nos.social", NULL);
  g_assert_cmpuint(g_strv_length(claim), ==, 1);
  g_autofree char *bob = npub_uri(f.bob_pk);
  g_assert_cmpstr(claim[0], ==, bob);
  g_autofree char *n2 = NULL, *d2 = NULL;
  meta_of(e2, claim[0], &n2, &d2, NULL);
  g_assert_true(g_str_has_prefix(d2, "bob@nos.social")); /* no ✓ */
  g_auto(GStrv) nobody = search1(e2, "carol@nos.social", NULL);
  g_assert_cmpuint(g_strv_length(nobody), ==, 0);
  nsp_engine_free(e2);

  /* resolved but the store is empty/unreachable: bare, still verified */
  NspEngine *e3 = engine_for(NULL, DEADLINE_MS);
  nsp_nip05_cache_put(nsp_engine_get_nip05(e3), "alice@nos.social", f.alice_pk);
  g_auto(GStrv) bare = search1(e3, "alice@nos.social", NULL);
  g_assert_cmpuint(g_strv_length(bare), ==, 1);
  g_autofree char *n3 = NULL, *d3 = NULL;
  meta_of(e3, bare[0], &n3, &d3, NULL);
  g_assert_cmpstr(n3, ==, "Open Nostr profile");
  g_assert_true(g_str_has_prefix(d3, "\xe2\x9c\x93 alice@nos.social"));
  nsp_engine_free(e3);
  fixture_clear(&f);
}

static void test_secret_never_sent(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  g_auto(GStrv) ids = search1(e, "nsec1vl029mgpspedva04g90vltkh6fvh240zqtv9k0t9af8935ke9laqsnlfe5", NULL);
  g_assert_cmpuint(g_strv_length(ids), ==, 0);
  g_assert_cmpuint(nsp_mock_relay_reqs(f.m), ==, 0);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_forged_event_rejected(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_ANSWER, TRUE);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree char *good = nsp_test_event(NSP_TEST_SK_BOB, 1, now, "forgery target", NULL);
  GString *s = g_string_new(good);
  g_string_replace(s, "forgery target", "forgery TARGET", 1);
  g_autofree char *forged = g_string_free(s, FALSE);
  nsp_mock_relay_add(f.m, forged);
  NspEngine *e = engine_for(f.m, DEADLINE_MS);
  g_auto(GStrv) ids = search1(e, "forgery", NULL);
  g_assert_cmpuint(g_strv_length(ids), ==, 0);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static gboolean cancel_cb(gpointer e) {
  nsp_engine_cancel_all(e);
  return G_SOURCE_REMOVE;
}

static void test_cancel_all(void) {
  Fixture f = {0};
  fixture_fill(&f, NSP_MOCK_SILENT, TRUE);
  NspEngine *e = engine_for(f.m, 5000);
  Run r = {0};
  const char *terms[] = {"hello", NULL};
  gint64 t0 = g_get_monotonic_time();
  nsp_engine_search_async(e, terms, NULL, on_done, &r);
  g_timeout_add(50, cancel_cb, e);
  g_assert_true(nsp_test_wait(&r.done, 2000));
  g_assert_cmpint((g_get_monotonic_time() - t0) / 1000, <, 500);
  g_strfreev(r.ids);
  nsp_engine_free(e);
  fixture_clear(&f);
}

static void test_unknown_ids_skipped(void) {
  NspEngine *e = engine_for(NULL, DEADLINE_MS);
  const char *ids[] = {"nostr:npub1unknown", "garbage", NULL};
  g_autoptr(GVariant) metas = g_variant_ref_sink(nsp_engine_result_metas(e, ids));
  g_assert_cmpuint(g_variant_n_children(metas), ==, 0);
  nsp_engine_free(e);
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nsp/engine/profile", test_profile_lookup);
  g_test_add_func("/nsp/engine/note-author", test_note_lookup_fetches_author);
  g_test_add_func("/nsp/engine/naddr-hex", test_naddr_and_hex);
  g_test_add_func("/nsp/engine/text-nip50", test_text_nip50);
  g_test_add_func("/nsp/engine/text-scan-fallback", test_text_scan_fallback);
  g_test_add_func("/nsp/engine/deadline-silent-circuit", test_deadline_silent_relay_and_circuit);
  g_test_add_func("/nsp/engine/partial-no-eose", test_partial_results_no_eose);
  g_test_add_func("/nsp/engine/no-socket", test_no_socket);
  g_test_add_func("/nsp/engine/subsearch", test_subsearch_narrowing);
  g_test_add_func("/nsp/engine/nip05", test_nip05);
  g_test_add_func("/nsp/engine/secret", test_secret_never_sent);
  g_test_add_func("/nsp/engine/forged", test_forged_event_rejected);
  g_test_add_func("/nsp/engine/cancel-all", test_cancel_all);
  g_test_add_func("/nsp/engine/unknown-ids", test_unknown_ids_skipped);
  return g_test_run();
}
