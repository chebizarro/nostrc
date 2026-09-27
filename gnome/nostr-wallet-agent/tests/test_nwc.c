/* test_nwc.c - NIP-47 request/response encode/decode against a fixture
 * wallet, over libnostr-publish's in-process fixture transport (no network).
 *
 * The "wallet" below is a second keypair that decrypts the agent's kind-23194
 * requests exactly as a wallet service would and answers with signed
 * kind-23195 responses / 23197 notifications.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-error.h"
#include "nwa-nwc.h"

#include <nostr/nip47/nwc.h>
#include <nostr/nip47/nwc_client.h>
#include <nostr-event.h>
#include <nostr-tag.h>
#include <nostr-keys.h>
#include <json.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  gchar *wallet_sk, *wallet_pk;
  gchar *client_sk, *client_pk;
  gchar *uri;
  GPtrArray *transports;
  NwaNwcClient *client;
} Fx;

static NostrPublishTransport *
fixture_factory(const gchar *url, gpointer data)
{
  Fx *fx = data;
  NostrPublishTransport *t = nostr_publish_transport_new_fixture(url);
  g_ptr_array_add(fx->transports, nostr_publish_transport_ref(t));
  return t;
}

static gchar *
dup_free(char *s)
{
  gchar *g = g_strdup(s);
  free(s);
  return g;
}

static void
fx_setup(Fx *fx, guint n_relays)
{
  fx->wallet_sk = dup_free(nostr_key_generate_private());
  fx->wallet_pk = dup_free(nostr_key_get_public(fx->wallet_sk));
  fx->client_sk = dup_free(nostr_key_generate_private());
  fx->client_pk = dup_free(nostr_key_get_public(fx->client_sk));
  GString *u = g_string_new(NULL);
  g_string_append_printf(u, "nostr+walletconnect://%s?", fx->wallet_pk);
  for (guint i = 0; i < n_relays; i++)
    g_string_append_printf(u, "relay=wss%%3A%%2F%%2Frelay%u.example&", i);
  g_string_append_printf(u, "secret=%s&lud16=alice%%40example.com", fx->client_sk);
  fx->uri = g_string_free(u, FALSE);
  fx->transports = g_ptr_array_new_with_free_func((GDestroyNotify)nostr_publish_transport_unref);
  g_autoptr(GError) err = NULL;
  fx->client = nwa_nwc_client_new(fx->uri, fixture_factory, fx, &err);
  g_assert_no_error(err);
  nwa_nwc_client_set_timeouts(fx->client, 1, 0);
}

static void
fx_teardown(Fx *fx)
{
  g_clear_object(&fx->client);
  g_ptr_array_unref(fx->transports);
  g_free(fx->wallet_sk); g_free(fx->wallet_pk);
  g_free(fx->client_sk); g_free(fx->client_pk);
  g_free(fx->uri);
}

static NostrPublishTransport *
tp(Fx *fx, guint i)
{
  return g_ptr_array_index(fx->transports, i);
}

static GPtrArray *
take(Fx *fx, guint i)
{
  gsize n = 0;
  gchar **frames = nostr_publish_transport_fixture_take_sent(tp(fx, i), &n);
  GPtrArray *a = g_ptr_array_new_with_free_func(g_free);
  for (gsize k = 0; k < n; k++) g_ptr_array_add(a, frames[k]);
  g_free(frames);
  return a;
}

/* ---- wallet side ---- */

static gchar *
wallet_event(Fx *fx, const gchar *sk, int kind, const gchar *content, NostrTags *tags)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, content);
  nostr_event_set_tags(ev, tags ? tags : nostr_tags_new(0));
  g_assert_cmpint(nostr_event_sign(ev, sk ? sk : fx->wallet_sk), ==, 0);
  gchar *json = dup_free(nostr_event_serialize_compact(ev));
  nostr_event_free(ev);
  return json;
}

static void
deliver(Fx *fx, guint i, const gchar *sub, const gchar *event_json)
{
  g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub, event_json);
  nostr_publish_transport_fixture_deliver_frame(tp(fx, i), "EVENT", frame);
}

static void
send_info(Fx *fx, const gchar *encryption)
{
  NostrTags *tags = nostr_tags_new(0);
  if (encryption)
    nostr_tags_append(tags, nostr_tag_new("encryption", encryption, NULL));
  nostr_tags_append(tags, nostr_tag_new("notifications", "payment_received payment_sent", NULL));
  g_autofree gchar *ev = wallet_event(fx, NULL, 13194,
    "pay_invoice get_balance make_invoice lookup_invoice list_transactions get_info notifications",
    tags);
  deliver(fx, 0, "nwa-info", ev);
}

typedef struct {
  gchar *id;
  gchar *method;
  gchar *params;
  NostrNwcEncryption enc;
  gchar *enc_tag;
  gboolean has_expiration;
} WalletReq;

static void
wallet_req_clear(WalletReq *r)
{
  g_free(r->id); g_free(r->method); g_free(r->params); g_free(r->enc_tag);
}

/* Find the single EVENT frame, verify it like a wallet would, decrypt it. */
static void
wallet_read_request(Fx *fx, GPtrArray *frames, WalletReq *out)
{
  memset(out, 0, sizeof *out);
  const gchar *frame = NULL;
  for (guint i = 0; i < frames->len; i++)
    if (g_str_has_prefix(g_ptr_array_index(frames, i), "[\"EVENT\""))
      frame = g_ptr_array_index(frames, i);
  g_assert_nonnull(frame);

  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, frame, -1, NULL));
  JsonNode *evn = json_array_get_element(json_node_get_array(json_parser_get_root(p)), 1);
  g_autofree gchar *ej = json_to_string(evn, FALSE);

  NostrEvent *ev = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize(ev, ej), ==, 0);
  g_assert_true(nostr_event_check_signature(ev));
  g_assert_cmpint(nostr_event_get_kind(ev), ==, 23194);
  /* signed by the pairing's client key — never the user's identity */
  g_assert_cmpstr(nostr_event_get_pubkey(ev), ==, fx->client_pk);

  const gchar *ptag = NULL;
  NostrTags *tags = nostr_event_get_tags(ev);
  for (size_t i = 0; i < nostr_tags_size(tags); i++) {
    NostrTag *t = nostr_tags_get(tags, i);
    const gchar *k = nostr_tag_get_key(t);
    if (g_strcmp0(k, "p") == 0) ptag = nostr_tag_get_value(t);
    if (g_strcmp0(k, "encryption") == 0) out->enc_tag = g_strdup(nostr_tag_get_value(t));
    if (g_strcmp0(k, "expiration") == 0) {
      gint64 exp = g_ascii_strtoll(nostr_tag_get_value(t), NULL, 10);
      g_assert_cmpint(exp, >, (gint64)time(NULL) - 5);
      out->has_expiration = TRUE;
    }
  }
  g_assert_cmpstr(ptag, ==, fx->wallet_pk);
  g_assert_nonnull(out->enc_tag);
  out->enc = g_str_equal(out->enc_tag, "nip44_v2") ? NOSTR_NWC_ENC_NIP44_V2 : NOSTR_NWC_ENC_NIP04;

  NostrNwcClientSession s = { .wallet_pub_hex = fx->client_pk, .enc = out->enc };
  char *plain = NULL;
  g_assert_cmpint(nostr_nwc_client_decrypt(&s, fx->wallet_sk, fx->client_pk,
                                           nostr_event_get_content(ev), &plain), ==, 0);
  g_autoptr(JsonParser) bp = json_parser_new();
  g_assert_true(json_parser_load_from_data(bp, plain, -1, NULL));
  JsonObject *body = json_node_get_object(json_parser_get_root(bp));
  out->method = g_strdup(json_object_get_string_member(body, "method"));
  out->params = json_to_string(json_object_get_member(body, "params"), FALSE);
  free(plain);
  char *id = nostr_event_get_id(ev);
  out->id = g_strdup(id);
  free(id);
  nostr_event_free(ev);
}

static gchar *
wallet_encrypt(Fx *fx, NostrNwcEncryption enc, const gchar *plain)
{
  NostrNwcClientSession s = { .wallet_pub_hex = fx->client_pk, .enc = enc };
  char *c = NULL;
  g_assert_cmpint(nostr_nwc_client_encrypt(&s, fx->wallet_sk, fx->client_pk, plain, &c), ==, 0);
  return dup_free(c);
}

static void
wallet_respond(Fx *fx, const WalletReq *req, const gchar *signer_sk, const gchar *body)
{
  g_autofree gchar *cipher = wallet_encrypt(fx, req->enc, body);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", req->id, NULL));
  nostr_tags_append(tags, nostr_tag_new("p", fx->client_pk, NULL));
  g_autofree gchar *ev = wallet_event(fx, signer_sk, 23195, cipher, tags);
  deliver(fx, 0, "nwa-rsp", ev);
}

/* ---- client side ---- */

typedef struct {
  gboolean done;
  JsonNode *result;
  GError *error;
} Res;

static void
on_done(GObject *src, GAsyncResult *r, gpointer data)
{
  Res *res = data;
  res->result = nwa_nwc_client_request_finish(NWA_NWC_CLIENT(src), r, &res->error);
  res->done = TRUE;
}

static gboolean
on_guard(gpointer data)
{
  (void)data;
  g_error("test timed out waiting for the NWC client");
  return G_SOURCE_REMOVE;
}

static void
wait_for(Res *r)
{
  guint guard = g_timeout_add_seconds(10, on_guard, NULL);
  while (!r->done) g_main_context_iteration(NULL, TRUE);
  g_source_remove(guard);
}

static void
res_clear(Res *r)
{
  if (r->result) json_node_unref(r->result);
  g_clear_error(&r->error);
  memset(r, 0, sizeof *r);
}

/* ---- tests ---- */

static void
test_subscriptions(void)
{
  Fx fx = { 0 };
  fx_setup(&fx, 2);
  nwa_nwc_client_start(fx.client);
  g_assert_cmpuint(fx.transports->len, ==, 2);
  for (guint i = 0; i < 2; i++) {
    g_autoptr(GPtrArray) f = take(&fx, i);
    g_assert_cmpuint(f->len, ==, 2);
    const gchar *info = g_ptr_array_index(f, 0);
    const gchar *rsp = g_ptr_array_index(f, 1);
    g_assert_true(g_str_has_prefix(info, "[\"REQ\",\"nwa-info\""));
    g_assert_nonnull(strstr(info, "\"kinds\":[13194]"));
    g_assert_nonnull(strstr(info, fx.wallet_pk));
    g_assert_true(g_str_has_prefix(rsp, "[\"REQ\",\"nwa-rsp\""));
    g_assert_nonnull(strstr(rsp, "\"kinds\":[23195,23196,23197]"));
    g_autofree gchar *authors = g_strdup_printf("\"authors\":[\"%s\"]", fx.wallet_pk);
    g_autofree gchar *ptag = g_strdup_printf("\"#p\":[\"%s\"]", fx.client_pk);
    g_assert_nonnull(strstr(rsp, authors));
    g_assert_nonnull(strstr(rsp, ptag));
  }
  g_assert_cmpstr(nwa_nwc_client_get_lud16(fx.client), ==, "alice@example.com");
  g_assert_cmpstr(nwa_nwc_client_get_client_pubkey(fx.client), ==, fx.client_pk);
  fx_teardown(&fx);
}

/* NIP-47 spells the tag value "nip44_v2"; older nips/nip47 code (and some
 * wallets) wrote "nip44-v2". Both select NIP-44; requests always carry the
 * spec spelling. */
static void
test_encryption_tag_spellings(void)
{
  static const gchar *const tags[] = { "nip44_v2", "nip44-v2", "nip04 nip44-v2", "nip44_v2,nip04" };
  for (guint i = 0; i < G_N_ELEMENTS(tags); i++) {
    Fx fx = { 0 };
    fx_setup(&fx, 1);
    nwa_nwc_client_start(fx.client);
    g_ptr_array_unref(take(&fx, 0));
    send_info(&fx, tags[i]);
    g_assert_cmpstr(nwa_nwc_client_get_encryption(fx.client), ==, "nip44_v2");
    Res r = { 0 };
    nwa_nwc_client_request_async(fx.client, "get_balance", NULL, NULL, on_done, &r);
    g_autoptr(GPtrArray) f = take(&fx, 0);
    WalletReq req;
    wallet_read_request(&fx, f, &req);
    g_assert_cmpstr(req.enc_tag, ==, "nip44_v2");
    wallet_respond(&fx, &req, NULL, "{\"result_type\":\"get_balance\",\"result\":{\"balance\":1}}");
    wait_for(&r);
    g_assert_no_error(r.error);
    res_clear(&r);
    wallet_req_clear(&req);
    fx_teardown(&fx);
  }
}

static void
test_nip44_roundtrip(void)
{
  Fx fx = { 0 };
  fx_setup(&fx, 1);
  nwa_nwc_client_start(fx.client);
  g_ptr_array_unref(take(&fx, 0));

  /* request before the info event is queued, not sent with a guessed scheme */
  Res r = { 0 };
  nwa_nwc_client_request_async(fx.client, "get_balance", NULL, NULL, on_done, &r);
  g_autoptr(GPtrArray) none = take(&fx, 0);
  g_assert_cmpuint(none->len, ==, 0);

  send_info(&fx, "nip44_v2 nip04");
  g_assert_cmpstr(nwa_nwc_client_get_encryption(fx.client), ==, "nip44_v2");
  g_assert_true(nwa_nwc_client_supports(fx.client, "pay_invoice"));
  g_assert_false(nwa_nwc_client_supports(fx.client, "pay_keysend"));

  g_autoptr(GPtrArray) f = take(&fx, 0);
  WalletReq req;
  wallet_read_request(&fx, f, &req);
  g_assert_cmpstr(req.method, ==, "get_balance");
  g_assert_cmpstr(req.params, ==, "{}");
  g_assert_cmpstr(req.enc_tag, ==, "nip44_v2");
  g_assert_true(req.has_expiration);

  wallet_respond(&fx, &req, NULL,
                 "{\"result_type\":\"get_balance\",\"result\":{\"balance\":21000}}");
  wait_for(&r);
  g_assert_no_error(r.error);
  g_assert_cmpint(json_object_get_int_member(json_node_get_object(r.result), "balance"), ==, 21000);
  res_clear(&r);
  wallet_req_clear(&req);

  /* params are carried through; wallet error maps to NWA_ERROR_WALLET */
  g_autoptr(JsonObject) params = json_object_new();
  json_object_set_string_member(params, "invoice", "lnbc1test");
  nwa_nwc_client_request_async(fx.client, "pay_invoice", params, NULL, on_done, &r);
  g_autoptr(GPtrArray) f2 = take(&fx, 0);
  wallet_read_request(&fx, f2, &req);
  g_assert_cmpstr(req.method, ==, "pay_invoice");
  g_assert_cmpstr(req.params, ==, "{\"invoice\":\"lnbc1test\"}");
  wallet_respond(&fx, &req, NULL,
                 "{\"result_type\":\"pay_invoice\",\"error\":{\"code\":\"INSUFFICIENT_BALANCE\","
                 "\"message\":\"not enough sats\"}}");
  wait_for(&r);
  g_assert_error(r.error, NWA_ERROR, NWA_ERROR_WALLET);
  g_assert_nonnull(strstr(r.error->message, "[INSUFFICIENT_BALANCE]"));
  g_assert_true(nwa_nwc_error_is_definite(r.error));
  res_clear(&r);
  wallet_req_clear(&req);
  fx_teardown(&fx);
}

static void
test_forged_response_ignored(void)
{
  Fx fx = { 0 };
  fx_setup(&fx, 1);
  nwa_nwc_client_start(fx.client);
  send_info(&fx, "nip44_v2");
  g_ptr_array_unref(take(&fx, 0));

  Res r = { 0 };
  nwa_nwc_client_request_async(fx.client, "pay_invoice", NULL, NULL, on_done, &r);
  g_autoptr(GPtrArray) f = take(&fx, 0);
  WalletReq req;
  wallet_read_request(&fx, f, &req);

  /* attacker knows the request id but not the wallet key */
  g_autofree gchar *mallory = dup_free(nostr_key_generate_private());
  wallet_respond(&fx, &req, mallory,
                 "{\"result_type\":\"pay_invoice\",\"result\":{\"preimage\":\"forged\"}}");
  /* tampered copy of a real event (bad signature) */
  g_autofree gchar *cipher = wallet_encrypt(&fx, req.enc, "{\"result\":{\"preimage\":\"x\"}}");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("e", req.id, NULL));
  g_autofree gchar *real = wallet_event(&fx, NULL, 23195, cipher, tags);
  g_autofree gchar *tampered = g_strdup(real);
  gchar *sig = strstr(tampered, "\"sig\":\"");
  g_assert_nonnull(sig);
  sig[8] = sig[8] == 'a' ? 'b' : 'a';
  deliver(&fx, 0, "nwa-rsp", tampered);

  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_false(r.done);

  wallet_respond(&fx, &req, NULL,
                 "{\"result_type\":\"pay_invoice\",\"result\":{\"preimage\":\"cafe\",\"fees_paid\":1000}}");
  wait_for(&r);
  g_assert_no_error(r.error);
  JsonObject *o = json_node_get_object(r.result);
  g_assert_cmpstr(json_object_get_string_member(o, "preimage"), ==, "cafe");
  g_assert_cmpint(json_object_get_int_member(o, "fees_paid"), ==, 1000);
  res_clear(&r);
  wallet_req_clear(&req);
  fx_teardown(&fx);
}

static void
test_nip04_fallbacks(void)
{
  /* info event without an encryption tag => NIP-04 */
  Fx fx = { 0 };
  fx_setup(&fx, 1);
  nwa_nwc_client_start(fx.client);
  send_info(&fx, NULL);
  g_assert_cmpstr(nwa_nwc_client_get_encryption(fx.client), ==, "nip04");
  g_ptr_array_unref(take(&fx, 0));
  Res r = { 0 };
  nwa_nwc_client_request_async(fx.client, "get_info", NULL, NULL, on_done, &r);
  g_autoptr(GPtrArray) f = take(&fx, 0);
  WalletReq req;
  wallet_read_request(&fx, f, &req);
  g_assert_cmpstr(req.enc_tag, ==, "nip04");
  wallet_respond(&fx, &req, NULL,
                 "{\"result_type\":\"get_info\",\"result\":{\"alias\":\"fixture\",\"network\":\"regtest\"}}");
  wait_for(&r);
  g_assert_no_error(r.error);
  g_assert_cmpstr(json_object_get_string_member(json_node_get_object(r.result), "alias"), ==, "fixture");
  res_clear(&r);
  wallet_req_clear(&req);
  fx_teardown(&fx);

  /* no info event stored on the relay (EOSE) => NIP-04 */
  Fx fx2 = { 0 };
  fx_setup(&fx2, 1);
  nwa_nwc_client_start(fx2.client);
  g_ptr_array_unref(take(&fx2, 0));
  nwa_nwc_client_request_async(fx2.client, "get_balance", NULL, NULL, on_done, &r);
  nostr_publish_transport_fixture_deliver_frame(tp(&fx2, 0), "EOSE", "[\"EOSE\",\"nwa-info\"]");
  g_autoptr(GPtrArray) f2 = take(&fx2, 0);
  wallet_read_request(&fx2, f2, &req);
  g_assert_cmpstr(req.enc_tag, ==, "nip04");
  wallet_respond(&fx2, &req, NULL, "{\"result_type\":\"get_balance\",\"result\":{\"balance\":5}}");
  wait_for(&r);
  g_assert_no_error(r.error);
  res_clear(&r);
  wallet_req_clear(&req);
  fx_teardown(&fx2);
}

static void
on_notification(NwaNwcClient *c, const gchar *type, const gchar *json, gpointer data)
{
  (void)c;
  GPtrArray *seen = data;
  g_ptr_array_add(seen, g_strdup_printf("%s %s", type, json));
}

static void
test_notifications(void)
{
  Fx fx = { 0 };
  fx_setup(&fx, 1);
  g_autoptr(GPtrArray) seen = g_ptr_array_new_with_free_func(g_free);
  g_signal_connect(fx.client, "notification", G_CALLBACK(on_notification), seen);
  nwa_nwc_client_start(fx.client);
  send_info(&fx, "nip44_v2");

  g_autofree gchar *cipher = wallet_encrypt(&fx, NOSTR_NWC_ENC_NIP44_V2,
    "{\"notification_type\":\"payment_received\",\"notification\":{\"type\":\"incoming\","
    "\"amount\":21000,\"payment_hash\":\"ab\",\"preimage\":\"secret\"}}");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", fx.client_pk, NULL));
  g_autofree gchar *ev = wallet_event(&fx, NULL, 23197, cipher, tags);
  deliver(&fx, 0, "nwa-rsp", ev);
  deliver(&fx, 0, "nwa-rsp", ev); /* duplicate from a second relay path */
  g_assert_cmpuint(seen->len, ==, 1);
  g_assert_true(g_str_has_prefix(g_ptr_array_index(seen, 0), "payment_received {"));
  g_assert_nonnull(strstr(g_ptr_array_index(seen, 0), "\"amount\":21000"));
  fx_teardown(&fx);
}

static void
test_relay_rejection_and_timeout(void)
{
  Fx fx = { 0 };
  fx_setup(&fx, 1);
  nwa_nwc_client_start(fx.client);
  send_info(&fx, "nip44_v2");
  g_ptr_array_unref(take(&fx, 0));

  /* the only relay refuses the event: definite failure */
  Res r = { 0 };
  nwa_nwc_client_request_async(fx.client, "pay_invoice", NULL, NULL, on_done, &r);
  g_autoptr(GPtrArray) f = take(&fx, 0);
  WalletReq req;
  wallet_read_request(&fx, f, &req);
  g_autofree gchar *ok = g_strdup_printf("[\"OK\",\"%s\",false,\"blocked: no\"]", req.id);
  nostr_publish_transport_fixture_deliver_frame(tp(&fx, 0), "OK", ok);
  wait_for(&r);
  g_assert_error(r.error, NWA_ERROR, NWA_ERROR_RELAY);
  g_assert_true(nwa_nwc_error_is_definite(r.error));
  res_clear(&r);
  wallet_req_clear(&req);

  /* delivered but unanswered: timeout, outcome unknown */
  nwa_nwc_client_request_async(fx.client, "pay_invoice", NULL, NULL, on_done, &r);
  g_ptr_array_unref(take(&fx, 0));
  wait_for(&r);
  g_assert_error(r.error, NWA_ERROR, NWA_ERROR_TIMEOUT);
  g_assert_false(nwa_nwc_error_is_definite(r.error));
  res_clear(&r);

  /* stop() with a request in flight: also unknown */
  nwa_nwc_client_request_async(fx.client, "pay_invoice", NULL, NULL, on_done, &r);
  nwa_nwc_client_stop(fx.client);
  wait_for(&r);
  g_assert_error(r.error, NWA_ERROR, NWA_ERROR_TIMEOUT);
  res_clear(&r);
  fx_teardown(&fx);
}

static void
test_invalid_uris(void)
{
  static const gchar no_relay[] =
    "nostr+walletconnect://b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
    "?secret=71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c";
  const gchar *bad[] = {
    "nostr+walletconnect://abc?relay=wss://r&secret=00",
    no_relay,
    "https://example.com",
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) err = NULL;
    g_assert_null(nwa_nwc_client_new(bad[i], NULL, NULL, &err));
    g_assert_error(err, NWA_ERROR, NWA_ERROR_INVALID_ARGS);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nwc/subscriptions", test_subscriptions);
  g_test_add_func("/nwc/nip44-roundtrip", test_nip44_roundtrip);
  g_test_add_func("/nwc/encryption-tag-spellings", test_encryption_tag_spellings);
  g_test_add_func("/nwc/forged-response-ignored", test_forged_response_ignored);
  g_test_add_func("/nwc/nip04-fallbacks", test_nip04_fallbacks);
  g_test_add_func("/nwc/notifications", test_notifications);
  g_test_add_func("/nwc/relay-rejection-and-timeout", test_relay_rejection_and_timeout);
  g_test_add_func("/nwc/invalid-uris", test_invalid_uris);
  return g_test_run();
}
