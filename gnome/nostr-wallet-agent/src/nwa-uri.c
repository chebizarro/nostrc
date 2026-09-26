/* nwa-uri.c - see nwa-uri.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-uri.h"
#include "nwa-error.h"

#include <nostr/nip47/nwc.h>
#include <string.h>

#define NWC_SCHEME "nostr+walletconnect:"

void
nwa_uri_clear(NwaUri *u)
{
  if (!u) return;
  g_free(u->bolt11);
  g_free(u->lnurl);
  g_free(u->address);
  g_free(u->label);
  g_free(u->message);
  if (u->nwc_uri) {
    /* contains the pairing secret */
    memset(u->nwc_uri, 0, strlen(u->nwc_uri));
    g_free(u->nwc_uri);
  }
  memset(u, 0, sizeof *u);
}

static gboolean
has_prefix_ci(const gchar *s, const gchar *prefix)
{
  return g_ascii_strncasecmp(s, prefix, strlen(prefix)) == 0;
}

gboolean
nwa_uri_parse_btc_amount(const gchar *btc, guint64 *out_msat)
{
  *out_msat = 0;
  if (!btc || !*btc) return FALSE;
  const gchar *dot = strchr(btc, '.');
  gsize int_len = dot ? (gsize)(dot - btc) : strlen(btc);
  const gchar *frac = dot ? dot + 1 : "";
  gsize frac_len = strlen(frac);
  if (int_len == 0 && frac_len == 0) return FALSE;
  if (frac_len > 8) return FALSE; /* sub-satoshi precision is not valid BIP-21 */

  guint64 sats = 0;
  for (gsize i = 0; i < int_len; i++) {
    if (!g_ascii_isdigit(btc[i])) return FALSE;
    if (sats > (G_MAXUINT64 / 10 - 9)) return FALSE;
    sats = sats * 10 + (guint64)(btc[i] - '0');
  }
  if (sats > G_GUINT64_CONSTANT(21000000)) return FALSE; /* > 21M BTC */
  sats *= G_GUINT64_CONSTANT(100000000);
  guint64 fsats = 0;
  for (gsize i = 0; i < 8; i++) {
    guint d = 0;
    if (i < frac_len) {
      if (!g_ascii_isdigit(frac[i])) return FALSE;
      d = (guint)(frac[i] - '0');
    }
    fsats = fsats * 10 + d;
  }
  sats += fsats;
  *out_msat = sats * 1000;
  return TRUE;
}

static gchar *
pct_decode(const gchar *s, gssize len, GError **error)
{
  g_autofree gchar *raw = len < 0 ? g_strdup(s) : g_strndup(s, (gsize)len);
  gchar *dec = g_uri_unescape_string(raw, NULL);
  if (!dec)
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid percent-encoding");
  return dec;
}

/* lightning:<payload>. Returns kind via @out. */
static gboolean
classify_lightning_payload(const gchar *payload, NwaUri *out, GError **error)
{
  g_autofree gchar *p = g_strstrip(g_ascii_strdown(payload, -1));
  if (!*p) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "empty lightning URI");
    return FALSE;
  }
  if (g_str_has_prefix(p, "lnurl") || strchr(p, '@')) {
    out->kind = NWA_URI_LNURL;
    out->lnurl = g_steal_pointer(&p);
    return TRUE;
  }
  if (!g_str_has_prefix(p, "ln")) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "lightning URI does not contain a BOLT-11 invoice");
    return FALSE;
  }
  for (const gchar *c = p; *c; c++) {
    if (!g_ascii_isalnum(*c)) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                          "BOLT-11 invoice contains invalid characters");
      return FALSE;
    }
  }
  out->kind = NWA_URI_LIGHTNING_INVOICE;
  out->bolt11 = g_steal_pointer(&p);
  return TRUE;
}

static gboolean
parse_lightning(const gchar *rest, NwaUri *out, GError **error)
{
  if (g_str_has_prefix(rest, "//")) rest += 2;
  /* Some wallets append a query; the invoice is everything before it. */
  const gchar *q = strchr(rest, '?');
  g_autofree gchar *payload = pct_decode(rest, q ? (gssize)(q - rest) : -1, error);
  if (!payload) return FALSE;
  return classify_lightning_payload(payload, out, error);
}

/* BIP-21: bitcoin:<address>[?<param>=<value>(&...)*] */
static gboolean
parse_bitcoin(const gchar *rest, NwaUri *out, GError **error)
{
  if (g_str_has_prefix(rest, "//")) rest += 2;
  const gchar *q = strchr(rest, '?');
  g_autofree gchar *address = pct_decode(rest, q ? (gssize)(q - rest) : -1, error);
  if (!address) return FALSE;
  for (const gchar *c = address; *c; c++) {
    if (!g_ascii_isalnum(*c)) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid bitcoin address");
      return FALSE;
    }
  }

  g_autofree gchar *lightning = NULL;
  g_autofree gchar *label = NULL;
  g_autofree gchar *message = NULL;
  guint64 amount_msat = 0;
  gboolean have_amount = FALSE;

  if (q) {
    g_auto(GStrv) params = g_strsplit(q + 1, "&", -1);
    for (guint i = 0; params[i]; i++) {
      if (!*params[i]) continue;
      const gchar *eq = strchr(params[i], '=');
      g_autofree gchar *key = eq ? g_strndup(params[i], (gsize)(eq - params[i]))
                                 : g_strdup(params[i]);
      g_autofree gchar *val = pct_decode(eq ? eq + 1 : "", -1, error);
      if (!val) return FALSE;
      gchar *lkey = g_ascii_strdown(key, -1);
      g_free(key);
      key = lkey;

      if (g_str_equal(key, "amount")) {
        if (have_amount || !nwa_uri_parse_btc_amount(val, &amount_msat)) {
          g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid BIP-21 amount");
          return FALSE;
        }
        have_amount = TRUE;
      } else if (g_str_equal(key, "lightning")) {
        if (!lightning) lightning = g_steal_pointer(&val);
      } else if (g_str_equal(key, "label")) {
        if (!label) label = g_steal_pointer(&val);
      } else if (g_str_equal(key, "message")) {
        if (!message) message = g_steal_pointer(&val);
      } else if (g_str_has_prefix(key, "req-")) {
        /* BIP-21: unknown required parameters make the URI invalid */
        g_set_error(error, NWA_ERROR, NWA_ERROR_UNSUPPORTED,
                    "unsupported required BIP-21 parameter '%s'", key);
        return FALSE;
      }
      /* other optional parameters are ignored */
    }
  }

  out->address = g_steal_pointer(&address);
  out->amount_msat = amount_msat;
  out->label = g_steal_pointer(&label);
  out->message = g_steal_pointer(&message);

  if (lightning) {
    if (!classify_lightning_payload(lightning, out, error))
      return FALSE;
    return TRUE;
  }
  if (!*out->address) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "bitcoin URI has neither an address nor a lightning invoice");
    return FALSE;
  }
  out->kind = NWA_URI_BITCOIN_ONCHAIN;
  return TRUE;
}

gchar *
nwa_uri_normalize_nwc(const gchar *uri, GError **error)
{
  if (!uri || !has_prefix_ci(uri, NWC_SCHEME)) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "not a nostr+walletconnect URI");
    return NULL;
  }
  const gchar *rest = uri + strlen(NWC_SCHEME);
  if (g_str_has_prefix(rest, "//")) rest += 2;
  gchar *norm = g_strconcat("nostr+walletconnect://", rest, NULL);

  NostrNwcConnection c = { 0 };
  if (nostr_nwc_uri_parse(norm, &c) != 0) {
    memset(norm, 0, strlen(norm));
    g_free(norm);
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "invalid nostr+walletconnect URI (need a 64-hex wallet pubkey and secret)");
    return NULL;
  }
  gboolean relay_ok = c.relays && c.relays[0];
  for (gsize i = 0; relay_ok && c.relays[i]; i++) {
    if (!g_str_has_prefix(c.relays[i], "wss://") && !g_str_has_prefix(c.relays[i], "ws://"))
      relay_ok = FALSE;
  }
  nostr_nwc_connection_clear(&c);
  if (!relay_ok) {
    memset(norm, 0, strlen(norm));
    g_free(norm);
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                        "nostr+walletconnect URI needs at least one ws:// or wss:// relay");
    return NULL;
  }
  return norm;
}

gboolean
nwa_uri_parse(const gchar *uri, NwaUri *out, GError **error)
{
  memset(out, 0, sizeof *out);
  if (!uri) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "no URI");
    return FALSE;
  }
  g_autofree gchar *u = g_strstrip(g_strdup(uri));

  gboolean ok;
  if (has_prefix_ci(u, "lightning:")) {
    ok = parse_lightning(u + strlen("lightning:"), out, error);
  } else if (has_prefix_ci(u, "bitcoin:")) {
    ok = parse_bitcoin(u + strlen("bitcoin:"), out, error);
  } else if (has_prefix_ci(u, NWC_SCHEME)) {
    out->nwc_uri = nwa_uri_normalize_nwc(u, error);
    out->kind = NWA_URI_NWC_PAIRING;
    ok = out->nwc_uri != NULL;
  } else {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_UNSUPPORTED, "unsupported URI scheme");
    ok = FALSE;
  }
  memset(u, 0, strlen(u));
  if (!ok) nwa_uri_clear(out);
  return ok;
}
