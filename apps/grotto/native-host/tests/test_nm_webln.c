/* test_nm_webln — WebLN <-> org.nostr.Wallet1 translation (nostrc-jjyp):
 * sats <-> msat and range limits, makeInvoice argument resolution, BOLT-11
 * shape, GetInfo -> WebLN getInfo, and Wallet1 / bus error mapping. The
 * live agent path is covered by test_nm_webln_e2e. */
#include "nm_webln.h"

#include <string.h>

#define COFFEE "lnbc2500u1pvjluezsp5zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygspp5qqqsyqcyq5rqwzqf" \
               "qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqfqypqdq5xysxxatsyp3k7enxv4jsxqzpu9qrsgquk0rl77nj30yxdy8j9vdx" \
               "85fkpmdla2087ne0xh8nhedh8w27kyke0lp53ut353s06fv3qfegext0eh0ymjpf39tuven09sam30g4vgpfna3rh"

static JsonNode *parse(const gchar *json) {
  g_autoptr(JsonParser) p = json_parser_new();
  g_assert_true(json_parser_load_from_data(p, json, -1, NULL));
  return json_node_ref(json_parser_get_root(p));
}

static NmSatsStatus sats_of(const gchar *json, guint64 *v) {
  g_autoptr(JsonNode) n = parse(json);
  return nm_webln_parse_sats(n, v);
}

static void test_sats(void) {
  guint64 v = 0;
  g_assert_cmpint(sats_of("21", &v), ==, NM_SATS_OK); g_assert_cmpuint(v, ==, 21);
  g_assert_cmpint(sats_of("\"21\"", &v), ==, NM_SATS_OK); g_assert_cmpuint(v, ==, 21);
  g_assert_cmpint(sats_of("\"000042\"", &v), ==, NM_SATS_OK); g_assert_cmpuint(v, ==, 42);
  g_assert_cmpint(sats_of("0", &v), ==, NM_SATS_OK); g_assert_cmpuint(v, ==, 0);
  g_assert_cmpint(sats_of("1000.0", &v), ==, NM_SATS_OK); g_assert_cmpuint(v, ==, 1000);
  g_assert_cmpint(sats_of("4294967", &v), ==, NM_SATS_OK); g_assert_cmpuint(v, ==, NM_WEBLN_MAX_SATS);
  g_assert_cmpint(sats_of("4294968", &v), ==, NM_SATS_TOO_LARGE);
  g_assert_cmpint(sats_of("\"99999999999999999999999\"", &v), ==, NM_SATS_TOO_LARGE);
  g_assert_cmpint(sats_of("1e300", &v), ==, NM_SATS_TOO_LARGE);
  g_assert_cmpint(sats_of("9223372036854775807", &v), ==, NM_SATS_TOO_LARGE);
  static const gchar *const invalid[] = {
    "-1", "1.5", "\"1.5\"", "\"-3\"", "\"+3\"", "\"1e3\"", "\"\"", "\" 21\"", "\"0x10\"",
    "true", "[21]", "{\"v\":21}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++)
    g_assert_cmpint(sats_of(invalid[i], &v), ==, NM_SATS_INVALID);
  g_assert_cmpint(sats_of("null", &v), ==, NM_SATS_ABSENT);
  g_assert_cmpint(nm_webln_parse_sats(NULL, &v), ==, NM_SATS_ABSENT);

  guint32 msat = 0;
  g_assert_true(nm_webln_sats_to_msat(1, &msat)); g_assert_cmpuint(msat, ==, 1000);
  g_assert_true(nm_webln_sats_to_msat(NM_WEBLN_MAX_SATS, &msat));
  g_assert_cmpuint(msat, ==, 4294967000u);
  g_assert_false(nm_webln_sats_to_msat(NM_WEBLN_MAX_SATS + 1, &msat));
}

static gboolean resolve(const gchar *json, guint32 *msat, const gchar **memo, NmErrorCode *code,
                        JsonNode **keep) {
  *keep = parse(json);
  const gchar *why = NULL;
  gboolean ok = nm_webln_resolve_invoice(json_node_get_object(*keep), msat, memo, code, &why);
  if (!ok) g_assert_nonnull(why);
  return ok;
}

static void test_resolve_invoice(void) {
  guint32 msat = 0;
  const gchar *memo = NULL;
  NmErrorCode code;
  JsonNode *k = NULL;

  g_assert_true(resolve("{\"amount\":21,\"defaultMemo\":\"coffee\"}", &msat, &memo, &code, &k));
  g_assert_cmpuint(msat, ==, 21000); g_assert_cmpstr(memo, ==, "coffee"); json_node_unref(k);
  g_assert_true(resolve("{\"amount\":\"100\"}", &msat, &memo, &code, &k));
  g_assert_cmpuint(msat, ==, 100000); g_assert_cmpstr(memo, ==, ""); json_node_unref(k);
  /* no amount: defaultAmount, then a non-zero minimumAmount */
  g_assert_true(resolve("{\"defaultAmount\":50,\"minimumAmount\":10,\"maximumAmount\":100}", &msat, &memo, &code, &k));
  g_assert_cmpuint(msat, ==, 50000); json_node_unref(k);
  g_assert_true(resolve("{\"minimumAmount\":10,\"maximumAmount\":100}", &msat, &memo, &code, &k));
  g_assert_cmpuint(msat, ==, 10000); json_node_unref(k);
  g_assert_true(resolve("{\"amount\":4294967}", &msat, &memo, &code, &k));
  g_assert_cmpuint(msat, ==, 4294967000u); json_node_unref(k);
  g_assert_true(resolve("{\"amount\":1,\"defaultMemo\":null}", &msat, &memo, &code, &k));
  g_assert_cmpstr(memo, ==, ""); json_node_unref(k);

  static const gchar *const invalid[] = {
    "{}", "{\"amount\":0}", "{\"maximumAmount\":100}", "{\"minimumAmount\":0}",
    "{\"amount\":-5}", "{\"amount\":1.5}", "{\"amount\":\"ten\"}",
    "{\"amount\":5,\"minimumAmount\":10}", "{\"amount\":500,\"maximumAmount\":100}",
    "{\"minimumAmount\":100,\"maximumAmount\":10}", "{\"amount\":5,\"defaultMemo\":7}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(invalid); i++) {
    g_assert_false(resolve(invalid[i], &msat, &memo, &code, &k));
    g_assert_cmpint(code, ==, NM_ERR_INVALID_REQUEST);
    json_node_unref(k);
  }
  static const gchar *const too_large[] = {
    "{\"amount\":4294968}", "{\"amount\":\"100000000000\"}", "{\"defaultAmount\":5000000}",
    "{\"amount\":1,\"maximumAmount\":99999999999}",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(too_large); i++) {
    g_assert_false(resolve(too_large[i], &msat, &memo, &code, &k));
    g_assert_cmpint(code, ==, NM_ERR_TOO_LARGE);
    json_node_unref(k);
  }
  /* memo length is bytes, BOLT-11's 639 */
  g_autofree gchar *m639 = g_strnfill(639, 'm');
  g_autofree gchar *m640 = g_strnfill(640, 'm');
  g_autofree gchar *j639 = g_strdup_printf("{\"amount\":1,\"defaultMemo\":\"%s\"}", m639);
  g_autofree gchar *j640 = g_strdup_printf("{\"amount\":1,\"defaultMemo\":\"%s\"}", m640);
  g_assert_true(resolve(j639, &msat, &memo, &code, &k)); json_node_unref(k);
  g_assert_false(resolve(j640, &msat, &memo, &code, &k)); json_node_unref(k);
}

static void test_bolt11_shape(void) {
  g_assert_true(nm_webln_is_bolt11(COFFEE));
  g_autofree gchar *upper = g_ascii_strup(COFFEE, -1);
  g_assert_true(nm_webln_is_bolt11(upper));
  g_assert_true(nm_webln_is_bolt11("lntb1pvjluezpp5qqqsyqcyq5rqwzqfqqqsyqcyq5rqwzqf"));
  g_autofree gchar *mixed = g_strdup(COFFEE);
  mixed[10] = g_ascii_toupper(mixed[10]);
  static const gchar *const bad[] = {
    NULL, "", "lnbc", "bitcoin:bc1qxyz", "lightning:" COFFEE, "lnurl1dp68gurn8ghj7um9", /* no data after sep */
    "lnbc2500u1pvjlue zsp5zyg3zyg3zyg3zyg3zyg3", "lnbc2500u1pvjluezbp5zyg3zyg3zyg3zyg3zyg3zyg3", /* 'b' not bech32 */
    "user@example.com",
  };
  for (gsize i = 0; i < G_N_ELEMENTS(bad); i++) g_assert_false(nm_webln_is_bolt11(bad[i]));
  g_assert_false(nm_webln_is_bolt11(mixed));
  g_autofree gchar *huge = g_strconcat("lnbc1", g_strnfill(NM_WEBLN_MAX_BOLT11, 'q'), NULL);
  g_assert_false(nm_webln_is_bolt11(huge));
}

static JsonObject *info_obj(GVariant *d, JsonNode **keep) {
  *keep = nm_webln_info_from_vardict(g_variant_ref_sink(d));
  g_variant_unref(d);
  return *keep ? json_node_get_object(*keep) : NULL;
}

static void test_info_mapping(void) {
  JsonNode *k = NULL;
  /* unpaired: no info (the provider turns this into not_paired) */
  g_assert_null(info_obj(g_variant_new_parsed("{'paired': <false>}"), &k));
  g_assert_null(info_obj(g_variant_new_parsed("@a{sv} {}"), &k));

  JsonObject *o = info_obj(g_variant_new_parsed(
      "{'paired': <true>, 'alias': <'Zeus node'>, 'pubkey': <'02abc'>, 'color': <'#ff9900'>,"
      " 'lud16': <'me@example.com'>, 'wallet_pubkey': <'wpk'>, 'client_pubkey': <'cpk'>,"
      " 'relays': <['wss://r.example']>, 'methods': <['pay_invoice', 'get_info', 'get_balance']>}"), &k);
  JsonObject *node = json_object_get_object_member(o, "node");
  g_assert_cmpstr(json_object_get_string_member(node, "alias"), ==, "Zeus node");
  g_assert_cmpstr(json_object_get_string_member(node, "pubkey"), ==, "02abc");
  g_assert_cmpstr(json_object_get_string_member(node, "color"), ==, "#ff9900");
  JsonArray *m = json_object_get_array_member(o, "methods");
  g_assert_cmpuint(json_array_get_length(m), ==, 3);
  g_assert_cmpstr(json_array_get_string_element(m, 0), ==, "getInfo");
  g_assert_cmpstr(json_array_get_string_element(m, 1), ==, "sendPayment");
  g_assert_cmpstr(json_array_get_string_element(m, 2), ==, "getBalance");
  g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(o, "supports"), 0), ==, "lightning");
  /* the pairing's own keys and relays are not the page's business */
  g_assert_false(json_object_has_member(o, "wallet_pubkey"));
  g_assert_false(json_object_has_member(o, "relays"));
  json_node_unref(k);

  /* no get_info answer: alias falls back to lud16; no methods advertised -> all */
  o = info_obj(g_variant_new_parsed("{'paired': <true>, 'lud16': <'me@example.com'>, 'methods': <@as []>}"), &k);
  node = json_object_get_object_member(o, "node");
  g_assert_cmpstr(json_object_get_string_member(node, "alias"), ==, "me@example.com");
  g_assert_false(json_object_has_member(node, "pubkey"));
  g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "methods")), ==, 4);
  json_node_unref(k);
}

static NmErrorCode map_remote(const gchar *name, const gchar *text, gchar **msg) {
  g_autoptr(GError) e = g_dbus_error_new_for_dbus_error(name, text);
  return nm_webln_error_from_dbus(e, msg);
}

static void test_error_mapping(void) {
  static const struct { const gchar *name, *text; NmErrorCode code; gboolean passes_message; } cases[] = {
    { "org.nostr.Wallet1.Error.InvalidArgs",    "invoice has expired", NM_ERR_INVALID_REQUEST, TRUE },
    { "org.nostr.Wallet1.Error.NotPaired",      "no wallet is paired", NM_ERR_NOT_PAIRED, FALSE },
    { "org.nostr.Wallet1.Error.Denied",         "payment declined", NM_ERR_REJECTED, TRUE },
    { "org.nostr.Wallet1.Error.BudgetExceeded", "over budget", NM_ERR_BUDGET_EXCEEDED, TRUE },
    { "org.nostr.Wallet1.Error.Timeout",        "wallet timeout", NM_ERR_TIMEOUT, FALSE },
    { "org.nostr.Wallet1.Error.WalletError",    "[INSUFFICIENT_BALANCE] not enough", NM_ERR_WALLET_ERROR, TRUE },
    { "org.nostr.Wallet1.Error.WalletError",    "[NOT_IMPLEMENTED] get_balance", NM_ERR_UNSUPPORTED, TRUE },
    { "org.nostr.Wallet1.Error.WalletError",    "[RATE_LIMITED] slow down", NM_ERR_RATE_LIMITED, TRUE },
    { "org.nostr.Wallet1.Error.WalletError",    "[RESTRICTED] no", NM_ERR_REJECTED, TRUE },
    { "org.nostr.Wallet1.Error.RelayError",     "no relay accepted", NM_ERR_WALLET_UNAVAILABLE, TRUE },
    { "org.nostr.Wallet1.Error.Unsupported",    "LNURL", NM_ERR_UNSUPPORTED, TRUE },
    { "org.nostr.Wallet1.Error.RateLimited",    "too many prompts", NM_ERR_RATE_LIMITED, TRUE },
    { "org.nostr.Wallet1.Error.Keyring",        "secret service locked", NM_ERR_INTERNAL, FALSE },
    { "org.nostr.Wallet1.Error.Failed",         "boom", NM_ERR_INTERNAL, FALSE },
    { "org.nostr.Wallet1.Error.SomethingNew",   "?", NM_ERR_INTERNAL, FALSE },
    { "org.freedesktop.DBus.Error.ServiceUnknown", "no agent", NM_ERR_WALLET_UNAVAILABLE, FALSE },
    { "org.freedesktop.DBus.Error.NameHasNoOwner", "no agent", NM_ERR_WALLET_UNAVAILABLE, FALSE },
    { "org.freedesktop.DBus.Error.NoReply",     "slow", NM_ERR_TIMEOUT, FALSE },
    { "org.freedesktop.DBus.Error.UnknownMethod", "old agent", NM_ERR_UNSUPPORTED, TRUE },
    { "org.nostr.Signer.Error.ApprovalDenied",  "wrong daemon", NM_ERR_REJECTED, FALSE },
  };
  for (gsize i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autofree gchar *msg = NULL;
    NmErrorCode code = map_remote(cases[i].name, cases[i].text, &msg);
    if (code != cases[i].code) g_error("%s: got %s", cases[i].name, nm_error_code_str(code));
    g_assert_cmpint(msg != NULL, ==, cases[i].passes_message);
    if (msg && g_str_has_prefix(cases[i].name, "org.nostr.Wallet1")) {
      g_assert_cmpstr(msg, ==, cases[i].text); /* "GDBus.Error:<name>: " stripped */
    }
  }
  /* agent text is bounded */
  g_autofree gchar *longtxt = g_strnfill(1000, 'x');
  g_autofree gchar *msg = NULL;
  map_remote("org.nostr.Wallet1.Error.WalletError", longtxt, &msg);
  g_assert_cmpuint(g_utf8_strlen(msg, -1), ==, 301);
  /* local timeout */
  g_autoptr(GError) t = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Timeout was reached");
  g_autofree gchar *tm = NULL;
  g_assert_cmpint(nm_webln_error_from_dbus(t, &tm), ==, NM_ERR_TIMEOUT);
  g_assert_null(tm);
  /* stable wire strings for the new codes */
  g_assert_cmpstr(nm_error_code_str(NM_ERR_NOT_PAIRED), ==, "not_paired");
  g_assert_cmpstr(nm_error_code_str(NM_ERR_WALLET_UNAVAILABLE), ==, "wallet_unavailable");
  g_assert_cmpstr(nm_error_code_str(NM_ERR_BUDGET_EXCEEDED), ==, "budget_exceeded");
  g_assert_cmpstr(nm_error_code_str(NM_ERR_WALLET_ERROR), ==, "wallet_error");
}

int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/nmh/webln/sats", test_sats);
  g_test_add_func("/nmh/webln/resolve-invoice", test_resolve_invoice);
  g_test_add_func("/nmh/webln/bolt11-shape", test_bolt11_shape);
  g_test_add_func("/nmh/webln/info-mapping", test_info_mapping);
  g_test_add_func("/nmh/webln/error-mapping", test_error_mapping);
  return g_test_run();
}
