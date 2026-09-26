/* test_uri.c - scheme URI parsing (lightning:, BIP-21 bitcoin:,
 * nostr+walletconnect:) and BOLT-11 decoding against the spec vectors.
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-bolt11.h"
#include "nwa-error.h"
#include "nwa-uri.h"
#include "bolt11-vectors.h"

#include <glib.h>

#define PK  "b889ff5b1513b641e2a139f661a661364979c5beee91842f8f0ef42ab558e9d4"
#define SEC "71a8c14c1407c113601079c4302dab36460f0ccd0ad506f1f2dc73b5100e4f3c"

/* ---- BOLT-11 ---- */

static void
test_hrp_amounts(void)
{
  static const struct { const gchar *in; gboolean ok; guint64 msat; } cases[] = {
    { "",            TRUE,  0 },
    { "2500u",       TRUE,  G_GUINT64_CONSTANT(250000000) },
    { "20m",         TRUE,  G_GUINT64_CONSTANT(2000000000) },
    { "1",           TRUE,  G_GUINT64_CONSTANT(100000000000) },
    { "10n",         TRUE,  1000 },
    { "10p",         TRUE,  1 },
    { "9678785340p", TRUE,  G_GUINT64_CONSTANT(967878534) },
    { "15p",         FALSE, 0 },  /* sub-msat */
    { "0u",          FALSE, 0 },  /* leading zero */
    { "2500x",       FALSE, 0 },
    { "u",           FALSE, 0 },
    { "99999999999999999999m", FALSE, 0 }, /* overflow */
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    guint64 v = 42;
    gboolean ok = nwa_bolt11_parse_hrp_amount(cases[i].in, &v);
    g_assert_cmpint(ok, ==, cases[i].ok);
    if (ok) g_assert_cmpuint(v, ==, cases[i].msat);
  }
}

static void
test_bolt11_donation(void)
{
  NwaBolt11 b;
  g_autoptr(GError) err = NULL;
  g_assert_true(nwa_bolt11_decode(V_DONATION, &b, &err));
  g_assert_no_error(err);
  g_assert_cmpstr(b.network, ==, "bc");
  g_assert_cmpuint(b.amount_msat, ==, 0);
  g_assert_cmpint(b.timestamp, ==, 1496314658);
  g_assert_cmpint(b.expiry, ==, 3600);
  g_assert_cmpstr(b.payment_hash, ==,
                  "0001020304050607080900010203040506070809000102030405060708090102");
  g_assert_cmpstr(b.description, ==, "Please consider supporting this project");
  g_assert_null(b.description_hash);
  g_assert_false(nwa_bolt11_is_expired(&b, 1496314658 + 3600));
  g_assert_true(nwa_bolt11_is_expired(&b, 1496314658 + 3601));
  nwa_bolt11_clear(&b);
}

static void
test_bolt11_amounts_and_fields(void)
{
  NwaBolt11 b;
  g_assert_true(nwa_bolt11_decode(V_COFFEE, &b, NULL));
  g_assert_cmpuint(b.amount_msat, ==, 250000000);
  g_assert_cmpstr(b.description, ==, "1 cup coffee");
  g_assert_cmpint(b.expiry, ==, 60);
  nwa_bolt11_clear(&b);

  g_assert_true(nwa_bolt11_decode(V_NONSENSE, &b, NULL));
  g_assert_cmpstr(b.description, ==, "ナンセンス 1杯");
  nwa_bolt11_clear(&b);

  g_assert_true(nwa_bolt11_decode(V_HASHED, &b, NULL));
  g_assert_cmpuint(b.amount_msat, ==, 2000000000);
  g_assert_null(b.description);
  g_assert_cmpstr(b.description_hash, ==,
                  "3925b6f67e2c340036ed12093dd44e0368df1b6ea26c53dbe4811f58fd5db8c1");
  nwa_bolt11_clear(&b);

  g_assert_true(nwa_bolt11_decode(V_PICO, &b, NULL));
  g_assert_cmpuint(b.amount_msat, ==, 967878534);
  nwa_bolt11_clear(&b);

  g_assert_true(nwa_bolt11_decode(V_UPPER, &b, NULL));
  g_assert_cmpuint(b.amount_msat, ==, G_GUINT64_CONSTANT(2500000000));
  g_assert_cmpstr(b.description, ==, "coffee beans");
  nwa_bolt11_clear(&b);
}

static void
test_bolt11_invalid(void)
{
  const gchar *bad[] = { V_BADSUM, V_BADMULT, V_SUBMSAT, "", "lnbc1", "notaninvoice",
                         "lnbc2500u1pvjluezsp5zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsPP5" };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    NwaBolt11 b;
    g_autoptr(GError) err = NULL;
    g_assert_false(nwa_bolt11_decode(bad[i], &b, &err));
    g_assert_error(err, NWA_ERROR, NWA_ERROR_INVALID_ARGS);
  }
  /* mixed case */
  g_autofree gchar *mixed = g_strdup(V_COFFEE);
  mixed[0] = 'L';
  NwaBolt11 b;
  g_assert_false(nwa_bolt11_decode(mixed, &b, NULL));
}

/* ---- lightning: ---- */

static void
test_lightning_uris(void)
{
  NwaUri u;
  g_autoptr(GError) err = NULL;

  g_assert_true(nwa_uri_parse("lightning:" V_COFFEE, &u, &err));
  g_assert_no_error(err);
  g_assert_cmpint(u.kind, ==, NWA_URI_LIGHTNING_INVOICE);
  g_assert_cmpstr(u.bolt11, ==, V_COFFEE);
  nwa_uri_clear(&u);

  g_assert_true(nwa_uri_parse("LIGHTNING:" V_UPPER, &u, NULL));
  g_assert_cmpint(u.kind, ==, NWA_URI_LIGHTNING_INVOICE);
  g_assert_true(g_str_has_prefix(u.bolt11, "lnbc25m1"));
  nwa_uri_clear(&u);

  g_assert_true(nwa_uri_parse("lightning://" V_COFFEE, &u, NULL));
  g_assert_cmpstr(u.bolt11, ==, V_COFFEE);
  nwa_uri_clear(&u);

  g_assert_true(nwa_uri_parse("  lightning:" V_COFFEE "  ", &u, NULL));
  nwa_uri_clear(&u);

  g_assert_true(nwa_uri_parse("lightning:LNURL1DP68GURN8GHJ7UM9WFMXJCM99E3K7MF0V9CXJ0M385EKVCENXC6R2C35XVUKXEFCV5MKVV34X5EKZD3EV56NYD3HXQURZEPEXEJXXEPNXSCRVWFNV9NXZCN9XQ6XYEFHVGCXXCMYXYMNSERXFQ5FNS", &u, NULL));
  g_assert_cmpint(u.kind, ==, NWA_URI_LNURL);
  nwa_uri_clear(&u);

  g_assert_true(nwa_uri_parse("lightning:alice@example.com", &u, NULL));
  g_assert_cmpint(u.kind, ==, NWA_URI_LNURL);
  g_assert_cmpstr(u.lnurl, ==, "alice@example.com");
  nwa_uri_clear(&u);

  const gchar *bad[] = { "lightning:", "lightning:hello", "lightning:lnbc 1", "mailto:a@b" };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) e = NULL;
    g_assert_false(nwa_uri_parse(bad[i], &u, &e));
    g_assert_nonnull(e);
  }
}

/* ---- bitcoin: (BIP-21) ---- */

static void
test_btc_amounts(void)
{
  static const struct { const gchar *in; gboolean ok; guint64 msat; } cases[] = {
    { "1",          TRUE,  G_GUINT64_CONSTANT(100000000000) },
    { "20.3",       TRUE,  G_GUINT64_CONSTANT(2030000000000) },
    { "0.00000001", TRUE,  1000 },
    { "1.",         TRUE,  G_GUINT64_CONSTANT(100000000000) },
    { ".5",         TRUE,  G_GUINT64_CONSTANT(50000000000) },
    { "0.000000001", FALSE, 0 },
    { "abc",        FALSE, 0 },
    { "1,5",        FALSE, 0 },
    { "-1",         FALSE, 0 },
    { "21000001",   FALSE, 0 },
    { ".",          FALSE, 0 },
    { "",           FALSE, 0 },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    guint64 v = 7;
    g_assert_cmpint(nwa_uri_parse_btc_amount(cases[i].in, &v), ==, cases[i].ok);
    if (cases[i].ok) g_assert_cmpuint(v, ==, cases[i].msat);
  }
}

static void
test_bitcoin_uris(void)
{
  NwaUri u;
  g_autoptr(GError) err = NULL;

  /* BIP-21 with a lightning fallback (unified QR) */
  g_assert_true(nwa_uri_parse("bitcoin:BC1QYLH3U67J673H6Y6ALV70M0PL2YZ53TZHVXGG7U"
                              "?amount=0.0025&label=sbddesign%3A%20For%20lunch%20Tuesday"
                              "&message=For%20lunch%20Tuesday&lightning=" V_COFFEE, &u, &err));
  g_assert_no_error(err);
  g_assert_cmpint(u.kind, ==, NWA_URI_LIGHTNING_INVOICE);
  g_assert_cmpstr(u.bolt11, ==, V_COFFEE);
  g_assert_cmpstr(u.address, ==, "BC1QYLH3U67J673H6Y6ALV70M0PL2YZ53TZHVXGG7U");
  g_assert_cmpuint(u.amount_msat, ==, 250000000);
  g_assert_cmpstr(u.label, ==, "sbddesign: For lunch Tuesday");
  g_assert_cmpstr(u.message, ==, "For lunch Tuesday");
  nwa_uri_clear(&u);

  /* parameter keys are case-insensitive; address may be empty */
  g_assert_true(nwa_uri_parse("BITCOIN:?LIGHTNING=" V_COFFEE, &u, NULL));
  g_assert_cmpint(u.kind, ==, NWA_URI_LIGHTNING_INVOICE);
  g_assert_cmpstr(u.address, ==, "");
  nwa_uri_clear(&u);

  /* plain on-chain */
  g_assert_true(nwa_uri_parse("bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT", &u, NULL));
  g_assert_cmpint(u.kind, ==, NWA_URI_BITCOIN_ONCHAIN);
  g_assert_cmpstr(u.address, ==, "1BoatSLRHtKNngkdXEeobR76b53LETtpyT");
  g_assert_cmpuint(u.amount_msat, ==, 0);
  nwa_uri_clear(&u);

  g_assert_true(nwa_uri_parse("bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT?amount=20.3&label=Luke-Jr", &u, NULL));
  g_assert_cmpint(u.kind, ==, NWA_URI_BITCOIN_ONCHAIN);
  g_assert_cmpuint(u.amount_msat, ==, G_GUINT64_CONSTANT(2030000000000));
  g_assert_cmpstr(u.label, ==, "Luke-Jr");
  nwa_uri_clear(&u);

  /* unknown optional params are ignored, unknown req- params are fatal */
  g_assert_true(nwa_uri_parse("bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT?somethingyoudontunderstand=50", &u, NULL));
  nwa_uri_clear(&u);
  {
    g_autoptr(GError) e = NULL;
    g_assert_false(nwa_uri_parse("bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT?req-somethingyoudontunderstand=50", &u, &e));
    g_assert_error(e, NWA_ERROR, NWA_ERROR_UNSUPPORTED);
  }
  const gchar *bad[] = {
    "bitcoin:",
    "bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT?amount=0.000000001",
    "bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT?amount=abc",
    "bitcoin:1BoatSLRHtKNngkdXEeobR76b53LETtpyT?amount=1&amount=2",
    "bitcoin:1Boat<script>?amount=1",
    "bitcoin:?lightning=nope",
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) e = NULL;
    g_assert_false(nwa_uri_parse(bad[i], &u, &e));
    g_assert_nonnull(e);
  }
}

/* ---- nostr+walletconnect: ---- */

static void
test_nwc_uris(void)
{
  NwaUri u;
  g_autoptr(GError) err = NULL;
  const gchar *good = "nostr+walletconnect://" PK "?relay=wss%3A%2F%2Frelay.example.com&secret=" SEC
                      "&lud16=alice%40example.com";
  g_assert_true(nwa_uri_parse(good, &u, &err));
  g_assert_no_error(err);
  g_assert_cmpint(u.kind, ==, NWA_URI_NWC_PAIRING);
  g_assert_cmpstr(u.nwc_uri, ==, good);
  nwa_uri_clear(&u);

  /* scheme without "//" is normalized */
  g_assert_true(nwa_uri_parse("nostr+walletconnect:" PK "?relay=wss://r.example&secret=" SEC, &u, NULL));
  g_assert_cmpstr(u.nwc_uri, ==, "nostr+walletconnect://" PK "?relay=wss://r.example&secret=" SEC);
  nwa_uri_clear(&u);

  const gchar *bad[] = {
    "nostr+walletconnect://" PK "?secret=" SEC,                                /* no relay */
    "nostr+walletconnect://" PK "?relay=https%3A%2F%2Fx.example&secret=" SEC,  /* not ws */
    "nostr+walletconnect://" PK "?relay=wss%3A%2F%2Fx.example",                /* no secret */
    "nostr+walletconnect://abcd?relay=wss%3A%2F%2Fx.example&secret=" SEC,      /* short pk */
    "nostr+walletconnect://" PK "?relay=wss%3A%2F%2Fx.example&secret=zz",      /* bad secret */
  };
  for (guint i = 0; i < G_N_ELEMENTS(bad); i++) {
    g_autoptr(GError) e = NULL;
    g_assert_false(nwa_uri_parse(bad[i], &u, &e));
    g_assert_error(e, NWA_ERROR, NWA_ERROR_INVALID_ARGS);
  }
}

int
main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/bolt11/hrp-amounts", test_hrp_amounts);
  g_test_add_func("/bolt11/donation", test_bolt11_donation);
  g_test_add_func("/bolt11/amounts-and-fields", test_bolt11_amounts_and_fields);
  g_test_add_func("/bolt11/invalid", test_bolt11_invalid);
  g_test_add_func("/uri/lightning", test_lightning_uris);
  g_test_add_func("/uri/bip21-amounts", test_btc_amounts);
  g_test_add_func("/uri/bitcoin", test_bitcoin_uris);
  g_test_add_func("/uri/nostr-walletconnect", test_nwc_uris);
  return g_test_run();
}
