/* nwa-fixture-wallet - a NIP-47 wallet service and the relay it talks
 * through, in one test process on 127.0.0.1 (nostrc-4qbt).
 *
 * SPDX-License-Identifier: MIT
 *
 * For end-to-end tests that run the real nostr-wallet-agent binary (and the
 * browser bridge in front of it) against a paired wallet without network.
 *
 *   relay   ws://127.0.0.1:<port>: REQ (kinds / authors / #p / #e / since /
 *           limit), EVENT (signature-checked, OK frames, replaceable kinds
 *           10000-19999 keep the newest), CLOSE, EOSE.
 *   wallet  its own keypair; publishes a kind-13194 info event
 *           (encryption tag "nip44_v2 nip04" or --encryption-tag), answers
 *           kind-23194 requests addressed to it from known clients with
 *           signed kind-23195 responses in the request's scheme:
 *           get_info, get_balance, make_invoice (mints BOLT-11 invoices the
 *           agent can decode, see nwa-test-bolt11.h), pay_invoice (debits
 *           the balance; INSUFFICIENT_BALANCE), lookup_invoice,
 *           list_transactions; anything else NOT_IMPLEMENTED; unknown
 *           clients UNAUTHORIZED.
 *
 * stdout (line protocol, flushed):
 *   URI <nostr+walletconnect://…>   a pairing for a fresh client key
 *   RELAY <ws://127.0.0.1:port>
 *   READY
 *   AUTHORIZED <client-pubkey>      after an AUTH command
 *   REQUEST <method> <client-pubkey>
 * stdin commands:
 *   AUTH <nostr+walletauth://…>     accept a wallet-auth request: remember
 *                                   the client and publish a 13194 info event
 *                                   p-tagged to it (with its `state`)
 *   AUTH-NOSTATE <uri>              same, without echoing `state` (Alby style)
 *   AUTH-AS <sk-hex> <uri>          publish an Alby-style (no `state`)
 *                                   answer as another key: an impostor
 *                                   racing the real wallet
 *
 * Options: --balance MSAT (default 100000000), --encryption-tag TEXT.
 */
#include "nwa-bolt11.h"
#include "nwa-test-bolt11.h"

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <json-glib/json-glib.h>
#include <libsoup/soup.h>
#include <nostr/nip47/nwc.h>
#include <nostr/nip47/nwc_client.h>
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-tag.h>
#include <json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
  SoupWebsocketConnection *ws;
  GHashTable *subs; /* sub id -> JsonNode (array of filter objects) */
} Conn;

static struct {
  gchar *sk, *pk;
  gchar *relay_url;
  GPtrArray *conns;     /* Conn* */
  GPtrArray *events;    /* JsonNode* (event objects) */
  GHashTable *clients;  /* client pubkeys allowed to make requests */
  GHashTable *preimages;/* payment_hash -> preimage */
  GPtrArray *txs;       /* JsonObject* */
  guint64 balance;
  gchar *enc_tag;
} W;

static void out_line(const gchar *fmt, ...) G_GNUC_PRINTF(1, 2);

static void
out_line(const gchar *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stdout, fmt, ap);
  va_end(ap);
  fputc('\n', stdout);
  fflush(stdout);
}

static gchar *
dup_free(char *s)
{
  gchar *g = g_strdup(s);
  free(s);
  return g;
}

static gchar *
random_hex32(void)
{
  GString *s = g_string_new(NULL);
  for (int i = 0; i < 8; i++) g_string_append_printf(s, "%08x", g_random_int());
  return g_string_free(s, FALSE);
}

static gchar *
sha256_hex_of_hex(const gchar *hex)
{
  guint8 raw[32];
  nwa_test_hex_to_bytes(hex, raw);
  return g_compute_checksum_for_data(G_CHECKSUM_SHA256, raw, 32);
}

/* ---- relay ---- */

static const gchar *
tag_value(JsonObject *ev, const gchar *key)
{
  JsonArray *tags = json_object_get_array_member(ev, "tags");
  for (guint i = 0; tags && i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (json_array_get_length(t) >= 2 && g_strcmp0(json_array_get_string_element(t, 0), key) == 0)
      return json_array_get_string_element(t, 1);
  }
  return NULL;
}

static gboolean
array_has_string(JsonArray *a, const gchar *s)
{
  for (guint i = 0; a && s && i < json_array_get_length(a); i++)
    if (g_strcmp0(json_array_get_string_element(a, i), s) == 0) return TRUE;
  return FALSE;
}

static gboolean
array_has_int(JsonArray *a, gint64 v)
{
  for (guint i = 0; a && i < json_array_get_length(a); i++)
    if (json_array_get_int_element(a, i) == v) return TRUE;
  return FALSE;
}

static gboolean
tag_matches(JsonObject *ev, const gchar *key, JsonArray *wanted)
{
  JsonArray *tags = json_object_get_array_member(ev, "tags");
  for (guint i = 0; tags && i < json_array_get_length(tags); i++) {
    JsonArray *t = json_array_get_array_element(tags, i);
    if (json_array_get_length(t) >= 2 && g_strcmp0(json_array_get_string_element(t, 0), key) == 0 &&
        array_has_string(wanted, json_array_get_string_element(t, 1)))
      return TRUE;
  }
  return FALSE;
}

static gboolean
filter_matches(JsonObject *f, JsonObject *ev)
{
  if (json_object_has_member(f, "kinds") &&
      !array_has_int(json_object_get_array_member(f, "kinds"), json_object_get_int_member(ev, "kind")))
    return FALSE;
  if (json_object_has_member(f, "authors") &&
      !array_has_string(json_object_get_array_member(f, "authors"), json_object_get_string_member(ev, "pubkey")))
    return FALSE;
  if (json_object_has_member(f, "since") &&
      json_object_get_int_member(ev, "created_at") < json_object_get_int_member(f, "since"))
    return FALSE;
  GList *members = json_object_get_members(f);
  gboolean ok = TRUE;
  for (GList *l = members; ok && l; l = l->next) {
    const gchar *k = l->data;
    if (k[0] == '#' && k[1] && !k[2])
      ok = tag_matches(ev, k + 1, json_object_get_array_member(f, k));
  }
  g_list_free(members);
  return ok;
}

static gboolean
filters_match(JsonNode *filters, JsonObject *ev)
{
  JsonArray *a = json_node_get_array(filters);
  for (guint i = 0; i < json_array_get_length(a); i++)
    if (filter_matches(json_array_get_object_element(a, i), ev)) return TRUE;
  return FALSE;
}

static void
conn_send(Conn *c, const gchar *text)
{
  if (soup_websocket_connection_get_state(c->ws) == SOUP_WEBSOCKET_STATE_OPEN)
    soup_websocket_connection_send_text(c->ws, text);
}

static void
send_event_to(Conn *c, const gchar *sub, JsonNode *ev)
{
  g_autofree gchar *ej = json_to_string(ev, FALSE);
  g_autofree gchar *frame = g_strdup_printf("[\"EVENT\",\"%s\",%s]", sub, ej);
  conn_send(c, frame);
}

static void
store_and_broadcast(JsonNode *evn)
{
  JsonObject *ev = json_node_get_object(evn);
  gint64 kind = json_object_get_int_member(ev, "kind");
  const gchar *pk = json_object_get_string_member(ev, "pubkey");
  if (kind >= 10000 && kind < 20000) {
    for (guint i = W.events->len; i > 0; i--) {
      JsonObject *o = json_node_get_object(g_ptr_array_index(W.events, i - 1));
      if (json_object_get_int_member(o, "kind") == kind &&
          g_strcmp0(json_object_get_string_member(o, "pubkey"), pk) == 0)
        g_ptr_array_remove_index(W.events, i - 1);
    }
  }
  g_ptr_array_add(W.events, json_node_ref(evn));
  for (guint i = 0; i < W.conns->len; i++) {
    Conn *c = g_ptr_array_index(W.conns, i);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, c->subs);
    while (g_hash_table_iter_next(&it, &k, &v))
      if (filters_match(v, ev)) send_event_to(c, k, evn);
  }
}

/* Sign @kind/@content/@tags with @sk, store and broadcast it. */
static void
publish(const gchar *sk, int kind, const gchar *content, NostrTags *tags)
{
  NostrEvent *ev = nostr_event_new();
  nostr_event_set_kind(ev, kind);
  nostr_event_set_created_at(ev, (int64_t)time(NULL));
  nostr_event_set_content(ev, content);
  nostr_event_set_tags(ev, tags);
  if (nostr_event_sign(ev, sk) != 0) g_error("fixture-wallet: sign failed");
  g_autofree gchar *json = dup_free(nostr_event_serialize_compact(ev));
  nostr_event_free(ev);
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, json, -1, NULL)) g_error("fixture-wallet: bad own event");
  store_and_broadcast(json_parser_get_root(p));
}

/* ---- wallet ---- */

static const gchar *const METHODS =
  "pay_invoice get_balance make_invoice lookup_invoice list_transactions get_info notifications";

static void
publish_info(const gchar *as_sk, const gchar *client_pk, const gchar *state)
{
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("encryption", W.enc_tag, NULL));
  nostr_tags_append(tags, nostr_tag_new("notifications", "payment_received payment_sent", NULL));
  if (client_pk) nostr_tags_append(tags, nostr_tag_new("p", client_pk, W.relay_url, NULL));
  if (state) nostr_tags_append(tags, nostr_tag_new("state", state, NULL));
  publish(as_sk ? as_sk : W.sk, 13194, METHODS, tags);
}

static JsonObject *
tx_new(const gchar *type, const gchar *invoice, const gchar *desc, const gchar *hash, gint64 amount)
{
  JsonObject *t = json_object_new();
  gint64 now = (gint64)time(NULL);
  json_object_set_string_member(t, "type", type);
  json_object_set_string_member(t, "invoice", invoice);
  json_object_set_string_member(t, "description", desc ? desc : "");
  json_object_set_string_member(t, "payment_hash", hash);
  json_object_set_int_member(t, "amount", amount);
  json_object_set_int_member(t, "fees_paid", 0);
  json_object_set_int_member(t, "created_at", now);
  json_object_set_int_member(t, "expires_at", now + 3600);
  return t;
}

static void
respond(const gchar *client_pk, const gchar *req_id, NostrNwcEncryption enc, const gchar *method,
        JsonObject *result, const gchar *err_code, const gchar *err_msg)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "result_type");
  json_builder_add_string_value(b, method);
  if (err_code) {
    json_builder_set_member_name(b, "error");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "code");
    json_builder_add_string_value(b, err_code);
    json_builder_set_member_name(b, "message");
    json_builder_add_string_value(b, err_msg ? err_msg : err_code);
    json_builder_end_object(b);
    json_builder_set_member_name(b, "result");
    json_builder_add_null_value(b);
  } else {
    json_builder_set_member_name(b, "result");
    JsonNode *r = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(r, result);
    json_builder_add_value(b, r);
  }
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  g_autofree gchar *plain = json_to_string(root, FALSE);

  NostrNwcClientSession s = { .wallet_pub_hex = (char *)client_pk, .enc = enc };
  char *cipher = NULL;
  if (nostr_nwc_client_encrypt(&s, W.sk, client_pk, plain, &cipher) != 0 || !cipher) {
    g_warning("fixture-wallet: encrypt failed");
    return;
  }
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", client_pk, NULL));
  nostr_tags_append(tags, nostr_tag_new("e", req_id, NULL));
  publish(W.sk, 23195, cipher, tags);
  free(cipher);
}

static void
handle_request(JsonObject *ev)
{
  const gchar *client = json_object_get_string_member(ev, "pubkey");
  const gchar *id = json_object_get_string_member(ev, "id");
  const gchar *enc_s = tag_value(ev, "encryption");
  NostrNwcEncryption enc = g_strcmp0(enc_s, "nip44_v2") == 0 ? NOSTR_NWC_ENC_NIP44_V2 : NOSTR_NWC_ENC_NIP04;

  NostrNwcClientSession s = { .wallet_pub_hex = (char *)client, .enc = enc };
  char *plain = NULL;
  if (nostr_nwc_client_decrypt(&s, W.sk, client, json_object_get_string_member(ev, "content"), &plain) != 0) {
    g_message("fixture-wallet: cannot decrypt request %s", id);
    return;
  }
  g_autoptr(JsonParser) p = json_parser_new();
  gboolean ok = json_parser_load_from_data(p, plain, -1, NULL);
  free(plain);
  if (!ok || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) return;
  JsonObject *body = json_node_get_object(json_parser_get_root(p));
  const gchar *method = json_object_get_string_member_with_default(body, "method", "");
  JsonObject *params = json_object_has_member(body, "params") &&
                       JSON_NODE_HOLDS_OBJECT(json_object_get_member(body, "params"))
                       ? json_object_get_object_member(body, "params") : NULL;
  out_line("REQUEST %s %s", method, client);

  if (!g_hash_table_contains(W.clients, client)) {
    respond(client, id, enc, method, NULL, "UNAUTHORIZED", "no wallet connection for this key");
    return;
  }
  JsonObject *r = json_object_new();
  if (g_str_equal(method, "get_info")) {
    json_object_set_string_member(r, "alias", "fixture wallet");
    json_object_set_string_member(r, "color", "#f7931a");
    json_object_set_string_member(r, "pubkey",
                                  "02eec7245d6b7d2ccb30380bfbe2a3648cd7a942653f5aa340edcea1f283686619");
    json_object_set_string_member(r, "network", "mainnet");
    json_object_set_int_member(r, "block_height", 850000);
    json_object_set_string_member(r, "block_hash", "00000000000000000000fixture");
    JsonArray *m = json_array_new();
    g_auto(GStrv) ms = g_strsplit(METHODS, " ", -1);
    for (guint i = 0; ms[i]; i++) json_array_add_string_element(m, ms[i]);
    json_object_set_array_member(r, "methods", m);
    respond(client, id, enc, method, r, NULL, NULL);
  } else if (g_str_equal(method, "get_balance")) {
    json_object_set_int_member(r, "balance", (gint64)W.balance);
    respond(client, id, enc, method, r, NULL, NULL);
  } else if (g_str_equal(method, "make_invoice")) {
    gint64 amount = params ? json_object_get_int_member_with_default(params, "amount", 0) : 0;
    const gchar *desc = params ? json_object_get_string_member_with_default(params, "description", "") : "";
    g_autofree gchar *preimage = random_hex32();
    g_autofree gchar *hash = sha256_hex_of_hex(preimage);
    g_autofree gchar *inv = nwa_test_bolt11_mint((guint64)amount, desc, NULL, hash, (gint64)time(NULL), 0);
    g_hash_table_insert(W.preimages, g_strdup(hash), g_strdup(preimage));
    JsonObject *t = tx_new("incoming", inv, desc, hash, amount);
    g_ptr_array_add(W.txs, json_object_ref(t));
    json_object_unref(r);
    respond(client, id, enc, method, t, NULL, NULL);
  } else if (g_str_equal(method, "pay_invoice")) {
    const gchar *inv = params ? json_object_get_string_member_with_default(params, "invoice", NULL) : NULL;
    NwaBolt11 b = { 0 };
    g_autoptr(GError) err = NULL;
    if (!inv || !nwa_bolt11_decode(inv, &b, &err)) {
      respond(client, id, enc, method, NULL, "OTHER", err ? err->message : "no invoice");
      json_object_unref(r);
      return;
    }
    guint64 amount = b.amount_msat ? b.amount_msat
                                   : (guint64)json_object_get_int_member_with_default(params, "amount", 0);
    if (amount > W.balance) {
      respond(client, id, enc, method, NULL, "INSUFFICIENT_BALANCE", "not enough funds");
    } else {
      W.balance -= amount;
      const gchar *pre = g_hash_table_lookup(W.preimages, b.payment_hash);
      g_autofree gchar *rnd = pre ? NULL : random_hex32();
      json_object_set_string_member(r, "preimage", pre ? pre : rnd);
      json_object_set_int_member(r, "fees_paid", 0);
      g_ptr_array_add(W.txs, tx_new("outgoing", inv, b.description, b.payment_hash, (gint64)amount));
      respond(client, id, enc, method, json_object_ref(r), NULL, NULL);
    }
    json_object_unref(r);
    nwa_bolt11_clear(&b);
  } else if (g_str_equal(method, "lookup_invoice") || g_str_equal(method, "list_transactions")) {
    if (g_str_equal(method, "list_transactions")) {
      JsonArray *a = json_array_new();
      for (guint i = 0; i < W.txs->len; i++)
        json_array_add_object_element(a, json_object_ref(g_ptr_array_index(W.txs, i)));
      json_object_set_array_member(r, "transactions", a);
      respond(client, id, enc, method, r, NULL, NULL);
    } else {
      const gchar *h = params ? json_object_get_string_member_with_default(params, "payment_hash", "") : "";
      JsonObject *hit = NULL;
      for (guint i = 0; i < W.txs->len; i++)
        if (g_strcmp0(json_object_get_string_member(g_ptr_array_index(W.txs, i), "payment_hash"), h) == 0)
          hit = g_ptr_array_index(W.txs, i);
      json_object_unref(r);
      if (hit) respond(client, id, enc, method, json_object_ref(hit), NULL, NULL);
      else respond(client, id, enc, method, NULL, "NOT_FOUND", "no such invoice");
    }
  } else {
    json_object_unref(r);
    respond(client, id, enc, method, NULL, "NOT_IMPLEMENTED", "fixture does not do that");
  }
}

/* ---- websocket plumbing ---- */

static void
on_ws_message(SoupWebsocketConnection *ws, gint type, GBytes *msg, gpointer data)
{
  (void)ws; (void)type;
  Conn *c = data;
  gsize n = 0;
  const gchar *text = g_bytes_get_data(msg, &n);
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, text, (gssize)n, NULL) || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(p)))
    return;
  JsonArray *a = json_node_get_array(json_parser_get_root(p));
  guint len = json_array_get_length(a);
  const gchar *verb = len ? json_array_get_string_element(a, 0) : NULL;
  if (g_strcmp0(verb, "REQ") == 0 && len >= 3) {
    const gchar *sub = json_array_get_string_element(a, 1);
    JsonNode *filters = json_node_new(JSON_NODE_ARRAY);
    JsonArray *fa = json_array_new();
    for (guint i = 2; i < len; i++)
      json_array_add_element(fa, json_node_copy(json_array_get_element(a, i)));
    json_node_take_array(filters, fa);
    g_hash_table_replace(c->subs, g_strdup(sub), filters);
    for (guint i = 0; i < W.events->len; i++) {
      JsonNode *ev = g_ptr_array_index(W.events, i);
      if (filters_match(filters, json_node_get_object(ev))) send_event_to(c, sub, ev);
    }
    g_autofree gchar *eose = g_strdup_printf("[\"EOSE\",\"%s\"]", sub);
    conn_send(c, eose);
  } else if (g_strcmp0(verb, "CLOSE") == 0 && len >= 2) {
    g_hash_table_remove(c->subs, json_array_get_string_element(a, 1));
  } else if (g_strcmp0(verb, "EVENT") == 0 && len >= 2) {
    JsonNode *evn = json_array_get_element(a, 1);
    if (!JSON_NODE_HOLDS_OBJECT(evn)) return;
    g_autofree gchar *ej = json_to_string(evn, FALSE);
    NostrEvent *ev = nostr_event_new();
    gboolean valid = nostr_event_deserialize(ev, ej) == 0 && nostr_event_check_signature(ev);
    nostr_event_free(ev);
    JsonObject *o = json_node_get_object(evn);
    const gchar *id = json_object_get_string_member_with_default(o, "id", "");
    g_autofree gchar *ok = g_strdup_printf("[\"OK\",\"%s\",%s,\"%s\"]", id, valid ? "true" : "false",
                                           valid ? "" : "invalid: bad signature");
    conn_send(c, ok);
    if (!valid) return;
    g_autoptr(JsonNode) copy = json_node_copy(evn);
    store_and_broadcast(copy);
    if (json_object_get_int_member(o, "kind") == 23194 && g_strcmp0(tag_value(o, "p"), W.pk) == 0)
      handle_request(json_node_get_object(copy));
  }
}

static void
on_ws_closed(SoupWebsocketConnection *ws, gpointer data)
{
  (void)ws;
  g_ptr_array_remove(W.conns, data);
}

static void
conn_free(gpointer p)
{
  Conn *c = p;
  g_signal_handlers_disconnect_by_data(c->ws, c);
  g_object_unref(c->ws);
  g_hash_table_unref(c->subs);
  g_free(c);
}

static void
on_ws(SoupServer *srv, SoupServerMessage *msg, const char *path, SoupWebsocketConnection *ws, gpointer data)
{
  (void)srv; (void)msg; (void)path; (void)data;
  Conn *c = g_new0(Conn, 1);
  c->ws = g_object_ref(ws);
  c->subs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_unref);
  g_ptr_array_add(W.conns, c);
  g_signal_connect(ws, "message", G_CALLBACK(on_ws_message), c);
  g_signal_connect(ws, "closed", G_CALLBACK(on_ws_closed), c);
}

/* ---- stdin commands ---- */

/* nostr+walletauth://<client-pk>?relay=…&state=… -> client pk + state */
static gboolean
parse_walletauth(const gchar *uri, gchar **client, gchar **state)
{
  const gchar *pfx = "nostr+walletauth://";
  if (!g_str_has_prefix(uri, pfx)) return FALSE;
  const gchar *rest = uri + strlen(pfx);
  const gchar *q = strchr(rest, '?');
  *client = q ? g_strndup(rest, (gsize)(q - rest)) : g_strdup(rest);
  *state = NULL;
  if (q) {
    g_autoptr(GHashTable) params = g_uri_parse_params(q + 1, -1, "&", G_URI_PARAMS_NONE, NULL);
    if (params && g_hash_table_lookup(params, "state"))
      *state = g_strdup(g_hash_table_lookup(params, "state"));
  }
  return strlen(*client) == 64;
}

static void read_next_line(GDataInputStream *in);

static void
on_line(GObject *src, GAsyncResult *res, gpointer data)
{
  (void)data;
  GDataInputStream *in = G_DATA_INPUT_STREAM(src);
  g_autofree gchar *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);
  if (!line) exit(0); /* stdin closed: the test is done with us */
  g_strstrip(line);
  g_auto(GStrv) w = g_strsplit(line, " ", 3);
  gchar *client = NULL, *state = NULL;
  if (w[0] && w[1] && (g_str_equal(w[0], "AUTH") || g_str_equal(w[0], "AUTH-NOSTATE")) &&
      parse_walletauth(w[1], &client, &state)) {
    g_hash_table_add(W.clients, g_strdup(client));
    publish_info(NULL, client, g_str_equal(w[0], "AUTH") ? state : NULL);
    out_line("AUTHORIZED %s", client);
  } else if (w[0] && w[1] && w[2] && g_str_equal(w[0], "AUTH-AS") && parse_walletauth(w[2], &client, &state)) {
    publish_info(w[1], client, NULL);
    out_line("IMPOSTOR %s", client);
  } else if (*line) {
    g_message("fixture-wallet: unknown command: %s", line);
  }
  g_free(client);
  g_free(state);
  read_next_line(in);
}

static void
read_next_line(GDataInputStream *in)
{
  g_data_input_stream_read_line_async(in, G_PRIORITY_DEFAULT, NULL, on_line, NULL);
}

int
main(int argc, char **argv)
{
  W.balance = 100000000;
  W.enc_tag = g_strdup("nip44_v2 nip04");
  for (int i = 1; i + 1 < argc; i += 2) {
    if (g_str_equal(argv[i], "--balance")) W.balance = g_ascii_strtoull(argv[i + 1], NULL, 10);
    else if (g_str_equal(argv[i], "--encryption-tag")) { g_free(W.enc_tag); W.enc_tag = g_strdup(argv[i + 1]); }
  }
  W.sk = dup_free(nostr_key_generate_private());
  W.pk = dup_free(nostr_key_get_public(W.sk));
  W.conns = g_ptr_array_new_with_free_func(conn_free);
  W.events = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_unref);
  W.clients = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  W.preimages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  W.txs = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);

  SoupServer *srv = soup_server_new(NULL, NULL);
  soup_server_add_websocket_handler(srv, NULL, NULL, NULL, on_ws, NULL, NULL);
  g_autoptr(GError) err = NULL;
  if (!soup_server_listen_local(srv, 0, SOUP_SERVER_LISTEN_IPV4_ONLY, &err)) {
    g_printerr("fixture-wallet: listen: %s\n", err->message);
    return 1;
  }
  GSList *uris = soup_server_get_uris(srv);
  W.relay_url = g_strdup_printf("ws://127.0.0.1:%d", g_uri_get_port(uris->data));
  g_slist_free_full(uris, (GDestroyNotify)g_uri_unref);

  gchar *client_sk = dup_free(nostr_key_generate_private());
  gchar *client_pk = dup_free(nostr_key_get_public(client_sk));
  g_hash_table_add(W.clients, client_pk);
  publish_info(NULL, NULL, NULL);

  g_autofree gchar *relay_enc = g_uri_escape_string(W.relay_url, NULL, FALSE);
  out_line("URI nostr+walletconnect://%s?relay=%s&secret=%s&lud16=fixture%%40wallet.example",
           W.pk, relay_enc, client_sk);
  out_line("RELAY %s", W.relay_url);
  out_line("READY");
  g_free(client_sk);

  GInputStream *stdin_raw = g_unix_input_stream_new(0, FALSE);
  GDataInputStream *in = g_data_input_stream_new(stdin_raw);
  read_next_line(in);
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  g_main_loop_run(loop);
  return 0;
}
