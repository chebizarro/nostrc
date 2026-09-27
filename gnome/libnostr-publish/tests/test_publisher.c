/* test_publisher.c - NostrPublisher multi-relay aggregation
 *
 * SPDX-License-Identifier: MIT
 *
 * Drives the engine with fixture transports and a vtable signer. Time is
 * synthetic (now_ts passed explicitly) so deadlines are deterministic.
 */

#include <nostr-publish/nostr-publish.h>

#include <json-glib/json-glib.h>
#include <glib.h>
#include <string.h>

#define PUBKEY "1111111111111111111111111111111111111111111111111111111111111111"
#define SIG    "22222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222222"
#define NOW    ((gint64)2000000000)

static gchar *
signed_event(const gchar *id)
{
  return g_strdup_printf("{\"id\":\"%s\",\"pubkey\":\"%s\",\"sig\":\"%s\","
                         "\"kind\":1,\"created_at\":1,\"tags\":[],"
                         "\"content\":\"hi\"}", id, PUBKEY, SIG);
}

/* ---- Mock signer ---- */

typedef struct {
  guint    calls;
  gboolean deny;
  gchar   *last_unsigned;
} MockSigner;

static gchar *
mock_sign(gpointer user_data, const gchar *unsigned_json,
          GCancellable *cancellable, GError **error)
{
  (void)cancellable;
  MockSigner *m = user_data;
  m->calls++;
  g_free(m->last_unsigned);
  m->last_unsigned = g_strdup(unsigned_json);
  if (m->deny) {
    g_set_error_literal(error, NOSTR_PUBLISH_SIGNER_ERROR,
                        NOSTR_PUBLISH_SIGNER_ERROR_DENIED, "user denied");
    return NULL;
  }
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, unsigned_json, -1, error))
    return NULL;
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *obj = json_node_get_object(root);
  json_object_set_string_member(obj, "id",
    "abababababababababababababababababababababababababababababababab");
  json_object_set_string_member(obj, "pubkey", PUBKEY);
  json_object_set_string_member(obj, "sig", SIG);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

static void
mock_free(gpointer data)
{
  MockSigner *m = data;
  g_free(m->last_unsigned);
  g_free(m);
}

static NostrPublishSigner *
mock_signer_new(MockSigner **out)
{
  MockSigner *m = g_new0(MockSigner, 1);
  NostrPublishSignerVTable vt = { mock_sign, mock_free };
  *out = m;
  return nostr_publish_signer_new_from_vtable(&vt, m);
}

/* ---- Callback recorder ---- */

typedef struct {
  guint               done_calls;
  guint               destroy_calls;
  NostrPublishVerdict verdict;
  gchar              *reason;
  gchar              *signed_json;
  gint64              completed_at;
  guint               n_relays;
  guint               n_accepted;
  GPtrArray          *relay_events;   /* "url=status" strings */
} Rec;

static void
rec_clear(Rec *r)
{
  g_clear_pointer(&r->reason, g_free);
  g_clear_pointer(&r->signed_json, g_free);
  g_clear_pointer(&r->relay_events, g_ptr_array_unref);
}

static void
on_relay(NostrPublisher *p, const gchar *event_id, const gchar *url,
         NostrPublishRelayStatus status, const gchar *reason, gpointer ud)
{
  (void)p; (void)event_id; (void)reason;
  Rec *r = ud;
  if (r->relay_events == NULL)
    r->relay_events = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(r->relay_events, g_strdup_printf("%s=%d", url, status));
}

static void
on_done(NostrPublisher *p, const NostrPublishResult *res, gpointer ud)
{
  Rec *r = ud;
  r->done_calls++;
  r->verdict      = nostr_publish_result_get_verdict(res);
  r->reason       = g_strdup(nostr_publish_result_get_reason(res));
  r->signed_json  = g_strdup(nostr_publish_result_get_signed_json(res));
  r->completed_at = nostr_publish_result_get_completed_at(res);
  r->n_relays     = nostr_publish_result_get_n_relays(res);
  r->n_accepted   = nostr_publish_result_get_n_accepted(res);
  /* The settled request is no longer in flight inside done_cb. */
  g_assert_false(nostr_publisher_is_in_flight(
                   p, nostr_publish_result_get_event_id(res)));
}

static void
on_destroy(gpointer ud)
{
  Rec *r = ud;
  r->destroy_calls++;
}

/* ---- Fixture ---- */

typedef struct {
  NostrPublisher        *pub;
  NostrPublishTransport *t[3];
} Fx;

static const gchar *const URLS[] = {
  "wss://r1.test", "wss://r2.test", "wss://r3.test", NULL
};

static void
fx_setup(Fx *fx)
{
  fx->pub = nostr_publisher_new(NULL);
  for (int i = 0; i < 3; i++) {
    fx->t[i] = nostr_publish_transport_new_fixture(URLS[i]);
    nostr_publish_transport_connect_async(fx->t[i]);
    nostr_publisher_bind_transport(fx->pub, URLS[i], fx->t[i]);
  }
}

static void
fx_teardown(Fx *fx)
{
  nostr_publisher_free(fx->pub);
  for (int i = 0; i < 3; i++)
    nostr_publish_transport_unref(fx->t[i]);
}

static guint
drain(NostrPublishTransport *t)
{
  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(t, &n);
  g_strfreev(frames);
  return (guint)n;
}

/* ---- Tests ---- */

static void
test_all_ack_publishes(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("aa");
  GError *err = NULL;

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  on_relay, on_done, &rec, on_destroy, &err));
  g_assert_no_error(err);
  g_assert_true(nostr_publisher_is_in_flight(fx.pub, "aa"));

  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(fx.t[0], &n);
  g_assert_cmpuint(n, ==, 1);
  g_autofree gchar *want = g_strdup_printf("[\"EVENT\",%s]", ev);
  g_assert_cmpstr(frames[0], ==, want);
  g_strfreev(frames);

  g_assert_true(nostr_publisher_record_ok(fx.pub, URLS[0], "aa", TRUE, ""));
  g_assert_true(nostr_publisher_record_ok(fx.pub, URLS[1], "aa", FALSE,
                                          "duplicate: already have it"));
  g_assert_cmpuint(rec.done_calls, ==, 0);
  g_assert_true(nostr_publisher_record_ok(fx.pub, URLS[2], "aa", TRUE, ""));

  g_assert_cmpuint(rec.done_calls, ==, 1);
  g_assert_cmpuint(rec.destroy_calls, ==, 1);
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_PUBLISHED);
  g_assert_cmpuint(rec.n_accepted, ==, 3);
  g_assert_cmpstr(rec.signed_json, ==, ev);
  g_assert_cmpuint(rec.relay_events->len, ==, 3);
  g_assert_false(nostr_publisher_is_in_flight(fx.pub, "aa"));
  /* Late / duplicate OK frames are ignored. */
  g_assert_false(nostr_publisher_record_ok(fx.pub, URLS[0], "aa", TRUE, ""));

  rec_clear(&rec);
  fx_teardown(&fx);
}

static void
test_transient_retry(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("bb");

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  NULL, on_done, &rec, NULL, NULL));
  nostr_publisher_record_ok(fx.pub, URLS[0], "bb", TRUE, "");
  nostr_publisher_record_ok(fx.pub, URLS[1], "bb", TRUE, "");
  nostr_publisher_record_ok(fx.pub, URLS[2], "bb", FALSE, "error: overloaded");

  g_assert_cmpuint(rec.done_calls, ==, 1);
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_RETRY);
  g_assert_cmpuint(rec.n_accepted, ==, 2);
  rec_clear(&rec);
  fx_teardown(&fx);
}

static void
test_numeric_quorum(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("cc");
  NostrPublishPolicy policy = {0};
  policy.quorum = 2;

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, &policy, NOW,
                  NULL, on_done, &rec, NULL, NULL));
  nostr_publisher_record_ok(fx.pub, URLS[0], "cc", TRUE, "");
  nostr_publisher_record_ok(fx.pub, URLS[1], "cc", TRUE, "");
  /* Quorum met but the verdict waits for every relay to settle. */
  g_assert_cmpuint(rec.done_calls, ==, 0);
  nostr_publisher_record_ok(fx.pub, URLS[2], "cc", FALSE, "error: nope");
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_PUBLISHED);
  rec_clear(&rec);
  fx_teardown(&fx);
}

static void
test_permanent_short_circuits(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("dd");

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  on_relay, on_done, &rec, on_destroy, NULL));
  nostr_publisher_record_ok(fx.pub, URLS[0], "dd", TRUE, "");
  nostr_publisher_record_ok(fx.pub, URLS[1], "dd", FALSE, "invalid: banned author");

  g_assert_cmpuint(rec.done_calls, ==, 1);
  g_assert_cmpuint(rec.destroy_calls, ==, 1);
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_FAILED_PERMANENT);
  g_assert_cmpstr(rec.reason, ==, "invalid: banned author");
  /* r3 never answered; its late OK is ignored. */
  g_assert_false(nostr_publisher_record_ok(fx.pub, URLS[2], "dd", TRUE, ""));
  g_assert_cmpuint(rec.done_calls, ==, 1);
  rec_clear(&rec);
  fx_teardown(&fx);
}

static void
test_deadline_sweep(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("ee");

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  on_relay, on_done, &rec, NULL, NULL));
  nostr_publisher_record_ok(fx.pub, URLS[0], "ee", TRUE, "");

  g_assert_true(nostr_publisher_tick(fx.pub, NOW + 119));
  g_assert_cmpuint(rec.done_calls, ==, 0);
  g_assert_false(nostr_publisher_tick(fx.pub, NOW + 120));
  g_assert_cmpuint(rec.done_calls, ==, 1);
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_RETRY);
  g_assert_cmpint(rec.completed_at, ==, NOW + 120);
  g_assert_cmpuint(rec.relay_events->len, ==, 3);
  g_autofree gchar *timed_out =
    g_strdup_printf("%s=%d", URLS[1], NOSTR_PUBLISH_RELAY_TIMED_OUT);
  g_assert_cmpstr(g_ptr_array_index(rec.relay_events, 1), ==, timed_out);
  rec_clear(&rec);
  fx_teardown(&fx);
}

static void
test_all_unreachable_is_synchronous_retry(void)
{
  NostrPublisher *pub = nostr_publisher_new(NULL);   /* nothing bound */
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("ff");

  g_assert_true(nostr_publisher_publish_signed(pub, ev, URLS, NULL, NOW,
                  on_relay, on_done, &rec, on_destroy, NULL));
  g_assert_cmpuint(rec.done_calls, ==, 1);
  g_assert_cmpuint(rec.destroy_calls, ==, 1);
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_RETRY);
  g_assert_cmpint(rec.completed_at, ==, NOW);
  g_assert_cmpuint(rec.relay_events->len, ==, 3);
  g_assert_cmpuint(nostr_publisher_get_n_in_flight(pub), ==, 0);
  rec_clear(&rec);
  nostr_publisher_free(pub);
}

static void
test_rejections(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  GError *err = NULL;
  g_autofree gchar *ev = signed_event("gg");

  g_assert_false(nostr_publisher_publish_signed(fx.pub, "{\"kind\":1}", URLS,
                   NULL, NOW, NULL, on_done, &rec, on_destroy, &err));
  g_assert_error(err, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_INVALID_EVENT);
  g_clear_error(&err);

  const gchar *none[] = { NULL };
  g_assert_false(nostr_publisher_publish_signed(fx.pub, ev, none, NULL, NOW,
                   NULL, on_done, &rec, on_destroy, &err));
  g_assert_error(err, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_NO_RELAYS);
  g_clear_error(&err);

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  NULL, on_done, &rec, NULL, NULL));
  g_assert_false(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                   NULL, on_done, &rec, on_destroy, &err));
  g_assert_error(err, NOSTR_PUBLISH_ERROR,
                 NOSTR_PUBLISH_ERROR_ALREADY_IN_FLIGHT);
  g_clear_error(&err);

  g_assert_cmpuint(rec.done_calls, ==, 0);
  g_assert_cmpuint(rec.destroy_calls, ==, 3);   /* once per rejected call */
  rec_clear(&rec);
  fx_teardown(&fx);   /* drops the in-flight request without done_cb */
  g_assert_cmpuint(rec.done_calls, ==, 0);
}

static void
test_free_runs_destroy_without_done(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("hh");
  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  NULL, on_done, &rec, on_destroy, NULL));
  fx_teardown(&fx);
  g_assert_cmpuint(rec.done_calls, ==, 0);
  g_assert_cmpuint(rec.destroy_calls, ==, 1);
  rec_clear(&rec);
}

/* done_cb re-publishes the same event: legal because the settled request
 * has already left the in-flight map. */
typedef struct {
  NostrPublisher *pub;
  const gchar    *signed_json;
  guint           done_calls;
  gboolean        republished;
} Repub;

static void
on_done_republish(NostrPublisher *p, const NostrPublishResult *res,
                  gpointer ud)
{
  Repub *r = ud;
  r->done_calls++;
  if (!r->republished &&
      nostr_publish_result_get_verdict(res) == NOSTR_PUBLISH_VERDICT_RETRY) {
    r->republished = TRUE;
    GError *err = NULL;
    g_assert_true(nostr_publisher_publish_signed(p, r->signed_json, URLS,
                    NULL, NOW, NULL, on_done_republish, r, NULL, &err));
    g_assert_no_error(err);
  }
}

static void
test_reentrant_republish(void)
{
  Fx fx; fx_setup(&fx);
  g_autofree gchar *ev = signed_event("ii");
  Repub r = { fx.pub, ev, 0, FALSE };
  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  NULL, on_done_republish, &r, NULL, NULL));
  nostr_publisher_tick(fx.pub, NOW + 500);   /* times out -> RETRY -> re-publish */
  g_assert_cmpuint(r.done_calls, ==, 1);
  g_assert_true(nostr_publisher_is_in_flight(fx.pub, "ii"));
  for (int i = 0; i < 3; i++)
    nostr_publisher_record_ok(fx.pub, URLS[i], "ii", TRUE, "");
  g_assert_cmpuint(r.done_calls, ==, 2);
  fx_teardown(&fx);
}

static void
test_bound_disconnected_is_unreachable(void)
{
  Fx fx; fx_setup(&fx);
  Rec rec = {0};
  g_autofree gchar *ev = signed_event("jj");
  nostr_publish_transport_fixture_set_state(fx.t[2], FALSE, NULL);

  g_assert_true(nostr_publisher_publish_signed(fx.pub, ev, URLS, NULL, NOW,
                  on_relay, on_done, &rec, NULL, NULL));
  g_assert_cmpuint(rec.relay_events->len, ==, 1);   /* r3 unreachable */
  nostr_publisher_record_ok(fx.pub, URLS[0], "jj", TRUE, "");
  nostr_publisher_record_ok(fx.pub, URLS[1], "jj", TRUE, "");
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_RETRY);
  rec_clear(&rec);
  fx_teardown(&fx);
}

/* ---- Publisher-owned transports (factory mode) ---- */

typedef struct {
  GPtrArray *made;     /* NostrPublishTransport* (ref) */
} Factory;

static NostrPublishTransport *
fixture_factory(const gchar *url, gpointer ud)
{
  Factory *f = ud;
  if (g_str_has_suffix(url, "refuse.test"))
    return NULL;
  NostrPublishTransport *t = nostr_publish_transport_new_fixture(url);
  g_ptr_array_add(f->made, nostr_publish_transport_ref(t));
  return t;
}

static void
test_owned_transports(void)
{
  MockSigner *ms = NULL;
  g_autoptr(NostrPublishSigner) signer = mock_signer_new(&ms);
  NostrPublisher *pub = nostr_publisher_new(signer);
  Factory f = { g_ptr_array_new_with_free_func(
                  (GDestroyNotify)nostr_publish_transport_unref) };
  nostr_publisher_set_transport_factory(pub, fixture_factory, &f, NULL);

  const gchar *targets[] = { "wss://o1.test", "wss://refuse.test", NULL };
  Rec rec = {0};
  GError *err = NULL;
  g_assert_true(nostr_publisher_publish(pub,
                  "{\"kind\":1,\"created_at\":1,\"tags\":[],\"content\":\"x\"}",
                  targets, NULL, NOW, on_relay, on_done, &rec, NULL, &err));
  g_assert_no_error(err);
  g_assert_cmpuint(ms->calls, ==, 1);
  g_assert_cmpuint(f.made->len, ==, 1);
  NostrPublishTransport *o1 = g_ptr_array_index(f.made, 0);

  /* Fixture connects synchronously: the queued frame was flushed. */
  g_assert_true(nostr_publish_transport_is_connected(o1));
  g_assert_cmpuint(drain(o1), ==, 1);

  /* OK frames on an owned transport are routed by the publisher itself. */
  nostr_publish_transport_fixture_deliver_frame(o1, "OK",
    "[\"OK\",\"abababababababababababababababababababababababababababababababab\",true,\"\"]");
  g_assert_cmpuint(rec.done_calls, ==, 1);
  /* refuse.test had no transport -> RETRY with 1 of 2 accepted. */
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_RETRY);
  g_assert_cmpuint(rec.n_accepted, ==, 1);
  g_assert_nonnull(strstr(rec.signed_json, PUBKEY));

  /* A second publish reuses the connected owned transport. */
  rec_clear(&rec); memset(&rec, 0, sizeof rec);
  g_autofree gchar *ev2 = signed_event("kk");
  const gchar *only_o1[] = { "wss://o1.test", NULL };
  g_assert_true(nostr_publisher_publish_signed(pub, ev2, only_o1, NULL, NOW,
                  NULL, on_done, &rec, NULL, NULL));
  g_assert_cmpuint(f.made->len, ==, 1);
  g_assert_cmpuint(drain(o1), ==, 1);

  /* Connection failure settles relays waiting on it as UNREACHABLE. */
  GError *boom = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED,
                                     "refused");
  nostr_publish_transport_fixture_set_state(o1, FALSE, boom);
  g_error_free(boom);
  g_assert_cmpuint(rec.done_calls, ==, 1);
  g_assert_cmpint(rec.verdict, ==, NOSTR_PUBLISH_VERDICT_RETRY);

  /* NIP-42: an AUTH challenge on an owned transport is answered with a
   * kind-22242 event signed by the publisher's signer. */
  nostr_publish_transport_fixture_set_state(o1, TRUE, NULL);
  nostr_publish_transport_fixture_deliver_frame(o1, "AUTH",
                                                "[\"AUTH\",\"chal-123\"]");
  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(o1, &n);
  g_assert_cmpuint(n, ==, 1);
  g_assert_true(g_str_has_prefix(frames[0], "[\"AUTH\",{"));
  g_strfreev(frames);
  g_assert_nonnull(strstr(ms->last_unsigned, "\"kind\":22242"));
  g_assert_nonnull(strstr(ms->last_unsigned, "[\"relay\",\"wss://o1.test\"]"));
  g_assert_nonnull(strstr(ms->last_unsigned, "[\"challenge\",\"chal-123\"]"));

  rec_clear(&rec);
  nostr_publisher_free(pub);
  /* The publisher disconnected its owned transport on free. */
  g_assert_false(nostr_publish_transport_is_connected(o1));
  g_ptr_array_unref(f.made);
}

static void
test_publish_sign_denied(void)
{
  MockSigner *ms = NULL;
  g_autoptr(NostrPublishSigner) signer = mock_signer_new(&ms);
  NostrPublisher *pub = nostr_publisher_new(signer);
  ms->deny = TRUE;
  Rec rec = {0};
  GError *err = NULL;
  g_assert_false(nostr_publisher_publish(pub, "{\"kind\":1}", URLS, NULL, NOW,
                   NULL, on_done, &rec, on_destroy, &err));
  g_assert_error(err, NOSTR_PUBLISH_SIGNER_ERROR,
                 NOSTR_PUBLISH_SIGNER_ERROR_DENIED);
  g_assert_true(nostr_publish_signer_error_is_permanent(err));
  g_clear_error(&err);
  g_assert_cmpuint(rec.done_calls, ==, 0);
  g_assert_cmpuint(rec.destroy_calls, ==, 1);

  NostrPublisher *unsigned_pub = nostr_publisher_new(NULL);
  g_assert_false(nostr_publisher_publish(unsigned_pub, "{\"kind\":1}", URLS,
                   NULL, NOW, NULL, on_done, &rec, NULL, &err));
  g_assert_error(err, NOSTR_PUBLISH_ERROR, NOSTR_PUBLISH_ERROR_NO_SIGNER);
  g_clear_error(&err);

  GError *transient = g_error_new_literal(NOSTR_PUBLISH_SIGNER_ERROR,
      NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT, "no owner");
  g_assert_false(nostr_publish_signer_error_is_permanent(transient));
  g_error_free(transient);
  g_assert_false(nostr_publish_signer_error_is_permanent(NULL));

  nostr_publisher_free(unsigned_pub);
  nostr_publisher_free(pub);
}

/* nostrc-k95e: NIP-44 through the signer is opt-in per signer; the vtable
 * layout (and every positional initialiser of it) is unchanged. */
static gchar *
fake_nip44(gpointer user_data, const gchar *plaintext, const gchar *peer,
           GCancellable *cancellable, GError **error)
{
  (void)cancellable; (void)error;
  g_assert_nonnull(user_data);   /* the vtable's user_data */
  return g_strdup_printf("enc(%s->%s)", plaintext, peer);
}

static void
test_signer_nip44_hook(void)
{
  MockSigner *ms = NULL;
  g_autoptr(NostrPublishSigner) signer = mock_signer_new(&ms);
  GError *err = NULL;
  g_assert_null(nostr_publish_signer_nip44_encrypt(signer, "hi", "ab", NULL, &err));
  g_assert_error(err, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED);
  g_assert_true(nostr_publish_signer_error_is_permanent(err));
  g_clear_error(&err);
  nostr_publish_signer_set_nip44_encrypt(signer, fake_nip44);
  g_autofree gchar *out = nostr_publish_signer_nip44_encrypt(signer, "hi", "ab", NULL, &err);
  g_assert_no_error(err);
  g_assert_cmpstr(out, ==, "enc(hi->ab)");
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nostr-publish/publisher/all-ack", test_all_ack_publishes);
  g_test_add_func("/nostr-publish/publisher/transient-retry", test_transient_retry);
  g_test_add_func("/nostr-publish/publisher/numeric-quorum", test_numeric_quorum);
  g_test_add_func("/nostr-publish/publisher/permanent", test_permanent_short_circuits);
  g_test_add_func("/nostr-publish/publisher/deadline", test_deadline_sweep);
  g_test_add_func("/nostr-publish/publisher/all-unreachable", test_all_unreachable_is_synchronous_retry);
  g_test_add_func("/nostr-publish/publisher/rejections", test_rejections);
  g_test_add_func("/nostr-publish/publisher/free", test_free_runs_destroy_without_done);
  g_test_add_func("/nostr-publish/publisher/reentrant", test_reentrant_republish);
  g_test_add_func("/nostr-publish/publisher/bound-disconnected", test_bound_disconnected_is_unreachable);
  g_test_add_func("/nostr-publish/publisher/owned-transports", test_owned_transports);
  g_test_add_func("/nostr-publish/publisher/sign-denied", test_publish_sign_denied);
  g_test_add_func("/nostr-publish/signer/nip44-hook", test_signer_nip44_hook);
  return g_test_run();
}
