/* test_walletauth.c - agent-generated NWC keys (nostr+walletauth), against
 * libnostr-publish's in-process fixture transport (no network).
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-error.h"
#include "nwa-walletauth.h"

#include <nostr/nip47/nwc.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  GPtrArray *transports;
  NwaWalletAuth *auth;
  gboolean done, ok, confirmed;
  guint why;
  gchar *wallet, *relay, *msg;
} Fx;

static gchar *
dup_free(char *s)
{
  gchar *g = g_strdup(s);
  free(s);
  return g;
}

static NostrPublishTransport *
factory(const gchar *url, gpointer data)
{
  Fx *fx = data;
  NostrPublishTransport *t = nostr_publish_transport_new_fixture(url);
  g_ptr_array_add(fx->transports, nostr_publish_transport_ref(t));
  return t;
}

static void
on_authorized(NwaWalletAuth *a, const gchar *wallet, const gchar *relay, gboolean confirmed, gpointer d)
{
  (void)a;
  Fx *fx = d;
  fx->done = fx->ok = TRUE;
  fx->wallet = g_strdup(wallet);
  fx->relay = g_strdup(relay);
  fx->confirmed = confirmed;
}

static void
on_failed(NwaWalletAuth *a, guint why, const gchar *msg, gpointer d)
{
  (void)a;
  Fx *fx = d;
  fx->done = TRUE;
  fx->why = why;
  fx->msg = g_strdup(msg);
}

static void
fx_setup_full(Fx *fx, guint settle_ms, guint timeout_s)
{
  memset(fx, 0, sizeof *fx);
  fx->transports = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_publish_transport_unref);
  const gchar *const relays[] = { "wss://r1.example", "wss://r2.example", "wss://r1.example", NULL };
  g_autoptr(GError) err = NULL;
  fx->auth = nwa_wallet_auth_new(relays, "Nostr Wallet (test)", factory, fx, &err);
  g_assert_no_error(err);
  nwa_wallet_auth_set_timing(fx->auth, settle_ms, timeout_s);
  g_signal_connect(fx->auth, "authorized", G_CALLBACK(on_authorized), fx);
  g_signal_connect(fx->auth, "failed", G_CALLBACK(on_failed), fx);
  nwa_wallet_auth_start(fx->auth);
}

static void
fx_setup(Fx *fx, guint settle_ms)
{
  fx_setup_full(fx, settle_ms, 30);
}

static void
fx_teardown(Fx *fx)
{
  g_clear_object(&fx->auth);
  g_ptr_array_unref(fx->transports);
  g_free(fx->wallet); g_free(fx->relay); g_free(fx->msg);
}

static gboolean
guard(gpointer d)
{
  (void)d;
  g_error("timed out");
  return G_SOURCE_REMOVE;
}

static void
spin_until_done(Fx *fx)
{
  guint g = g_timeout_add_seconds(10, guard, NULL);
  while (!fx->done) g_main_context_iteration(NULL, TRUE);
  g_source_remove(g);
}

static void
spin_ms(guint ms)
{
  gint64 end = g_get_monotonic_time() + (gint64)ms * 1000;
  while (g_get_monotonic_time() < end) g_main_context_iteration(NULL, FALSE);
}

typedef struct {
  const gchar *content;
  const gchar *state;      /* NULL: no state tag */
  const gchar *hint;       /* p-tag relay hint */
  gint64       created;    /* 0: now */
  gboolean     no_ptag;
} Answer;

/* A wallet's kind-13194 answer signed by @sk, delivered on transport @i. */
static void
answer(Fx *fx, guint i, const gchar *sk, const Answer *a)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("encryption", "nip44_v2 nip04", NULL));
  if (!a->no_ptag)
    nostr_tags_append(tags, a->hint ? nostr_tag_new("p", nwa_wallet_auth_get_client_pubkey(fx->auth), a->hint, NULL)
                                    : nostr_tag_new("p", nwa_wallet_auth_get_client_pubkey(fx->auth), NULL));
  if (a->state) nostr_tags_append(tags, nostr_tag_new("state", a->state, NULL));
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, 13194);
  nostr_event_set_created_at(ev, a->created ? a->created : (int64_t)time(NULL));
  nostr_event_set_content(ev, a->content ? a->content : "pay_invoice get_balance make_invoice");
  nostr_event_set_tags(ev, tags);
  g_assert_cmpint(nostr_event_sign(ev, sk), ==, 0);
  g_autofree gchar *ej = dup_free(nostr_event_serialize_compact(ev));
  nostr_event_free(ev);
  g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"nwa-auth\",%s]", ej);
  nostr_publish_transport_fixture_deliver_frame(g_ptr_array_index(fx->transports, i), "EVENT", frame);
}

static gchar *
new_sk(void)
{
  return dup_free(nostr_key_generate_private());
}

static gchar *
pk_of(const gchar *sk)
{
  return dup_free(nostr_key_get_public(sk));
}

static void
test_uri_and_subscription(void)
{
  Fx fx;
  fx_setup(&fx, 50);
  const gchar *pk = nwa_wallet_auth_get_client_pubkey(fx.auth);
  const gchar *uri = nwa_wallet_auth_get_uri(fx.auth);
  g_autofree gchar *prefix = g_strdup_printf("nostr+walletauth://%s?", pk);
  g_assert_true(g_str_has_prefix(uri, prefix));
  g_assert_nonnull(strstr(uri, "relay=wss%3A%2F%2Fr1.example&relay=wss%3A%2F%2Fr2.example&name="));
  g_assert_null(strstr(strstr(uri, "r2.example") + 1, "r1.example")); /* deduplicated */
  g_assert_nonnull(strstr(uri, "name=Nostr%20Wallet%20%28test%29"));
  g_assert_nonnull(strstr(uri, "request_methods=pay_invoice%20get_balance%20make_invoice"));
  g_assert_nonnull(strstr(uri, "notification_types=payment_received%20payment_sent"));
  g_autofree gchar *pkp = g_strdup_printf("&pubkey=%s&state=%s", pk, nwa_wallet_auth_get_state(fx.auth));
  g_assert_true(g_str_has_suffix(uri, pkp));
  g_assert_cmpuint(strlen(nwa_wallet_auth_get_state(fx.auth)), ==, 32);
  g_assert_null(strstr(uri, "secret"));

  g_assert_cmpuint(fx.transports->len, ==, 2);
  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(g_ptr_array_index(fx.transports, 0), &n);
  g_assert_cmpuint(n, ==, 1);
  g_autofree gchar *ptag = g_strdup_printf("\"#p\":[\"%s\"]", pk);
  g_assert_nonnull(strstr(frames[0], "\"kinds\":[13194]"));
  g_assert_nonnull(strstr(frames[0], ptag));
  g_strfreev(frames);

  /* the pairing built from the answer is a normal NWC URI with our key */
  g_autofree gchar *wsk = new_sk();
  g_autofree gchar *wpk = pk_of(wsk);
  gchar *nwc = nwa_wallet_auth_build_nwc_uri(fx.auth, wpk, "wss://r1.example");
  NostrNwcConnection c = { 0 };
  g_assert_cmpint(nostr_nwc_uri_parse(nwc, &c), ==, 0);
  g_assert_cmpstr(c.wallet_pubkey_hex, ==, wpk);
  g_autofree gchar *derived = pk_of(c.secret_hex);
  g_assert_cmpstr(derived, ==, pk);
  nostr_nwc_connection_clear(&c);
  memset(nwc, 0, strlen(nwc));
  g_free(nwc);
  fx_teardown(&fx);

  /* argument validation */
  g_autoptr(GError) e1 = NULL;
  const gchar *const bad[] = { "https://relay.example", NULL };
  g_assert_null(nwa_wallet_auth_new(bad, "x", NULL, NULL, &e1));
  g_assert_error(e1, NWA_ERROR, NWA_ERROR_INVALID_ARGS);
  g_autoptr(GError) e2 = NULL;
  const gchar *const many[] = { "wss://a", "wss://b", "wss://c", "wss://d", "wss://e", "wss://f", NULL };
  g_assert_null(nwa_wallet_auth_new(many, "x", NULL, NULL, &e2));
  g_autoptr(GError) e3 = NULL;
  const gchar *const one[] = { "wss://a", NULL };
  g_assert_null(nwa_wallet_auth_new(one, "", NULL, NULL, &e3));
  g_autoptr(GError) e4 = NULL;
  const gchar *const none[] = { NULL };
  g_assert_null(nwa_wallet_auth_new(none, "x", NULL, NULL, &e4));
}

static void
test_confirmed(void)
{
  Fx fx;
  fx_setup(&fx, 5000);
  g_autofree gchar *wsk = new_sk();
  g_autofree gchar *wpk = pk_of(wsk);
  /* another request's state is ignored, the right one accepted at once */
  answer(&fx, 1, wsk, &(Answer){ .state = "00000000000000000000000000000000" });
  spin_ms(100);
  g_assert_false(fx.done);
  answer(&fx, 1, wsk, &(Answer){ .state = nwa_wallet_auth_get_state(fx.auth) });
  spin_until_done(&fx);
  g_assert_true(fx.ok);
  g_assert_true(fx.confirmed);
  g_assert_cmpstr(fx.wallet, ==, wpk);
  g_assert_cmpstr(fx.relay, ==, "wss://r2.example"); /* where it arrived */
  fx_teardown(&fx);
}

static void
test_unconfirmed_settles(void)
{
  Fx fx;
  fx_setup(&fx, 150);
  g_autofree gchar *wsk = new_sk();
  g_autofree gchar *wpk = pk_of(wsk);
  answer(&fx, 0, wsk, &(Answer){ .hint = "wss://wallet-relay.example" });
  g_assert_cmpuint(fx.transports->len, ==, 3); /* the hint is subscribed too */
  answer(&fx, 1, wsk, &(Answer){ 0 });        /* same wallet via another relay: no conflict */
  spin_until_done(&fx);
  g_assert_true(fx.ok);
  g_assert_false(fx.confirmed);
  g_assert_cmpstr(fx.wallet, ==, wpk);
  g_assert_cmpstr(fx.relay, ==, "wss://wallet-relay.example");
  fx_teardown(&fx);
}

static void
test_conflict_and_garbage(void)
{
  Fx fx;
  fx_setup(&fx, 300);
  g_autofree gchar *a = new_sk();
  g_autofree gchar *b = new_sk();
  answer(&fx, 0, a, &(Answer){ 0 });
  /* malformed answers from others never count as a conflict */
  answer(&fx, 0, b, &(Answer){ .content = "{\"not\":\"methods\"}" });
  answer(&fx, 0, b, &(Answer){ .no_ptag = TRUE });
  answer(&fx, 0, b, &(Answer){ .created = (gint64)time(NULL) + 3600 });
  answer(&fx, 0, b, &(Answer){ .created = (gint64)time(NULL) - 3600 });
  spin_ms(50);
  g_assert_false(fx.done);
  answer(&fx, 1, b, &(Answer){ 0 });
  spin_until_done(&fx);
  g_assert_false(fx.ok);
  g_assert_cmpuint(fx.why, ==, NWA_WALLET_AUTH_CONFLICT);
  fx_teardown(&fx);
}

static void
test_confirmed_wins_settle(void)
{
  Fx fx;
  fx_setup(&fx, 2000);
  g_autofree gchar *impostor = new_sk();
  g_autofree gchar *wallet = new_sk();
  g_autofree gchar *wpk = pk_of(wallet);
  answer(&fx, 0, impostor, &(Answer){ 0 });
  answer(&fx, 0, wallet, &(Answer){ .state = nwa_wallet_auth_get_state(fx.auth) });
  spin_until_done(&fx);
  g_assert_true(fx.ok && fx.confirmed);
  g_assert_cmpstr(fx.wallet, ==, wpk);
  fx_teardown(&fx);
}

static void
test_stop_and_timeout(void)
{
  Fx fx;
  fx_setup(&fx, 50);
  nwa_wallet_auth_stop(fx.auth);
  spin_until_done(&fx);
  g_assert_cmpuint(fx.why, ==, NWA_WALLET_AUTH_STOPPED);
  fx_teardown(&fx);

  fx_setup_full(&fx, 50, 1);
  spin_until_done(&fx);
  g_assert_cmpuint(fx.why, ==, NWA_WALLET_AUTH_TIMEOUT);
  fx_teardown(&fx);
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/walletauth/uri-and-subscription", test_uri_and_subscription);
  g_test_add_func("/walletauth/confirmed", test_confirmed);
  g_test_add_func("/walletauth/unconfirmed-settles", test_unconfirmed_settles);
  g_test_add_func("/walletauth/conflict-and-garbage", test_conflict_and_garbage);
  g_test_add_func("/walletauth/confirmed-wins-settle", test_confirmed_wins_settle);
  g_test_add_func("/walletauth/stop-and-timeout", test_stop_and_timeout);
  return g_test_run();
}
