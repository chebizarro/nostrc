/* nwa-bolt11.c - see nwa-bolt11.h
 *
 * SPDX-License-Identifier: MIT
 */
#include "nwa-bolt11.h"
#include "nwa-error.h"

#include <string.h>

static const char BECH32_CHARSET[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

/* BOLT-11: signature is 65 bytes = 104 5-bit words; timestamp is 7 words. */
#define WORDS_TIMESTAMP 7
#define WORDS_SIGNATURE 104
#define WORDS_CHECKSUM  6

static gint
bech32_value(gchar c)
{
  const char *p = strchr(BECH32_CHARSET, c);
  return (p && c) ? (gint)(p - BECH32_CHARSET) : -1;
}

static guint32
bech32_polymod(const guint8 *values, gsize len)
{
  static const guint32 gen[5] = { 0x3b6a57b2, 0x26508e6d, 0x1ea119fa,
                                  0x3d4233dd, 0x2a1462b3 };
  guint32 chk = 1;
  for (gsize i = 0; i < len; i++) {
    guint8 top = (guint8)(chk >> 25);
    chk = ((chk & 0x1ffffff) << 5) ^ values[i];
    for (int j = 0; j < 5; j++)
      if ((top >> j) & 1) chk ^= gen[j];
  }
  return chk;
}

static gboolean
bech32_verify(const gchar *hrp, const guint8 *data, gsize data_len)
{
  gsize hlen = strlen(hrp);
  gsize n = hlen * 2 + 1 + data_len;
  g_autofree guint8 *buf = g_new(guint8, n);
  for (gsize i = 0; i < hlen; i++) {
    buf[i] = (guint8)hrp[i] >> 5;
    buf[hlen + 1 + i] = (guint8)hrp[i] & 31;
  }
  buf[hlen] = 0;
  memcpy(buf + hlen * 2 + 1, data, data_len);
  return bech32_polymod(buf, n) == 1;
}

/* 5-bit words → bytes, dropping trailing padding bits. */
static GBytes *
words_to_bytes(const guint8 *words, gsize n)
{
  GByteArray *out = g_byte_array_sized_new((guint)(n * 5 / 8 + 1));
  guint32 acc = 0;
  guint bits = 0;
  for (gsize i = 0; i < n; i++) {
    acc = (acc << 5) | words[i];
    bits += 5;
    while (bits >= 8) {
      bits -= 8;
      guint8 b = (guint8)((acc >> bits) & 0xff);
      g_byte_array_append(out, &b, 1);
    }
    acc &= (1u << bits) - 1;
  }
  return g_byte_array_free_to_bytes(out);
}

static gchar *
words_to_hex(const guint8 *words, gsize n, gsize want_bytes)
{
  g_autoptr(GBytes) b = words_to_bytes(words, n);
  gsize len = 0;
  const guint8 *d = g_bytes_get_data(b, &len);
  if (len < want_bytes) return NULL;
  GString *s = g_string_sized_new(want_bytes * 2);
  for (gsize i = 0; i < want_bytes; i++)
    g_string_append_printf(s, "%02x", d[i]);
  return g_string_free(s, FALSE);
}

static guint64
words_to_uint(const guint8 *words, gsize n)
{
  guint64 v = 0;
  for (gsize i = 0; i < n && i < 12; i++)
    v = (v << 5) | words[i];
  return v;
}

gboolean
nwa_bolt11_parse_hrp_amount(const gchar *amount, guint64 *out_msat)
{
  *out_msat = 0;
  if (!amount || !*amount)
    return TRUE; /* amount-less */

  gsize len = strlen(amount);
  gchar mult = amount[len - 1];
  gsize ndigits = len;
  if (!g_ascii_isdigit(mult)) {
    if (mult != 'm' && mult != 'u' && mult != 'n' && mult != 'p')
      return FALSE;
    ndigits--;
  } else {
    mult = 0;
  }
  if (ndigits == 0 || ndigits > 19)
    return FALSE;
  if (amount[0] == '0') /* BOLT-11: no leading zeros */
    return FALSE;

  guint64 v = 0;
  for (gsize i = 0; i < ndigits; i++) {
    if (!g_ascii_isdigit(amount[i]))
      return FALSE;
    guint d = (guint)(amount[i] - '0');
    if (v > (G_MAXUINT64 - d) / 10)
      return FALSE;
    v = v * 10 + d;
  }

  guint64 per_unit; /* msat per unit of the multiplier */
  switch (mult) {
    case 0:   per_unit = G_GUINT64_CONSTANT(100000000000); break; /* 1 BTC */
    case 'm': per_unit = G_GUINT64_CONSTANT(100000000); break;
    case 'u': per_unit = G_GUINT64_CONSTANT(100000); break;
    case 'n': per_unit = 100; break;
    case 'p':
      /* 1 pico-BTC = 0.1 msat: sub-msat amounts are invalid */
      if (v % 10 != 0) return FALSE;
      *out_msat = v / 10;
      return TRUE;
    default: return FALSE;
  }
  if (v > G_MAXUINT64 / per_unit)
    return FALSE;
  *out_msat = v * per_unit;
  return TRUE;
}

void
nwa_bolt11_clear(NwaBolt11 *b)
{
  if (!b) return;
  g_free(b->network);
  g_free(b->payment_hash);
  g_free(b->description);
  g_free(b->description_hash);
  g_free(b->payee);
  memset(b, 0, sizeof *b);
}

gboolean
nwa_bolt11_is_expired(const NwaBolt11 *b, gint64 now_unix)
{
  return b->timestamp + b->expiry < now_unix;
}

gboolean
nwa_bolt11_decode(const gchar *invoice, NwaBolt11 *out, GError **error)
{
  memset(out, 0, sizeof *out);
  if (!invoice || !*invoice) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "empty invoice");
    return FALSE;
  }

  gboolean has_lower = FALSE, has_upper = FALSE;
  for (const gchar *p = invoice; *p; p++) {
    if (g_ascii_islower(*p)) has_lower = TRUE;
    if (g_ascii_isupper(*p)) has_upper = TRUE;
    if ((guchar)*p < 33 || (guchar)*p > 126) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS,
                          "invoice contains invalid characters");
      return FALSE;
    }
  }
  if (has_lower && has_upper) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice has mixed case");
    return FALSE;
  }
  g_autofree gchar *s = g_ascii_strdown(invoice, -1);

  const gchar *sep = strrchr(s, '1');
  if (!sep || sep == s) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice has no bech32 separator");
    return FALSE;
  }
  g_autofree gchar *hrp = g_strndup(s, (gsize)(sep - s));
  const gchar *data = sep + 1;
  gsize dlen = strlen(data);
  if (dlen < WORDS_TIMESTAMP + WORDS_SIGNATURE + WORDS_CHECKSUM) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice is too short");
    return FALSE;
  }
  if (!g_str_has_prefix(hrp, "ln")) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "not a lightning invoice");
    return FALSE;
  }

  g_autofree guint8 *words = g_new(guint8, dlen);
  for (gsize i = 0; i < dlen; i++) {
    gint v = bech32_value(data[i]);
    if (v < 0) {
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invalid bech32 character");
      return FALSE;
    }
    words[i] = (guint8)v;
  }
  if (!bech32_verify(hrp, words, dlen)) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice checksum is invalid");
    return FALSE;
  }

  /* hrp = "ln" <currency letters> [<amount digits><multiplier>] */
  const gchar *cur = hrp + 2;
  const gchar *amt = cur;
  while (*amt && !g_ascii_isdigit(*amt)) amt++;
  if (amt == cur) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice has no currency prefix");
    return FALSE;
  }
  guint64 amount_msat = 0;
  if (!nwa_bolt11_parse_hrp_amount(amt, &amount_msat)) {
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice amount is invalid");
    return FALSE;
  }

  gsize nwords = dlen - WORDS_CHECKSUM;
  gsize tag_end = nwords - WORDS_SIGNATURE;
  NwaBolt11 b = { 0 };
  b.network = g_strndup(cur, (gsize)(amt - cur));
  b.amount_msat = amount_msat;
  b.timestamp = (gint64)words_to_uint(words, WORDS_TIMESTAMP);
  b.expiry = 3600;

  gsize i = WORDS_TIMESTAMP;
  while (i < tag_end) {
    if (i + 3 > tag_end) {
      nwa_bolt11_clear(&b);
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "truncated invoice field");
      return FALSE;
    }
    guint8 type = words[i];
    gsize len = (gsize)words[i + 1] * 32 + words[i + 2];
    const guint8 *fd = words + i + 3;
    if (i + 3 + len > tag_end) {
      nwa_bolt11_clear(&b);
      g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice field overruns data");
      return FALSE;
    }
    switch (BECH32_CHARSET[type]) {
      case 'p': /* payment hash: MUST skip unless 52 words */
        if (len == 52 && !b.payment_hash) b.payment_hash = words_to_hex(fd, len, 32);
        break;
      case 'h':
        if (len == 52 && !b.description_hash) b.description_hash = words_to_hex(fd, len, 32);
        break;
      case 'n':
        if (len == 53 && !b.payee) b.payee = words_to_hex(fd, len, 33);
        break;
      case 'x':
        if (len > 0 && len <= 12) b.expiry = (gint64)words_to_uint(fd, len);
        break;
      case 'd':
        if (!b.description) {
          g_autoptr(GBytes) db = words_to_bytes(fd, len);
          gsize dl = 0;
          const gchar *dd = g_bytes_get_data(db, &dl);
          if (dl && g_utf8_validate(dd, (gssize)dl, NULL))
            b.description = g_strndup(dd, dl);
          else if (dl == 0)
            b.description = g_strdup("");
        }
        break;
      default:
        break; /* unknown fields are ignored per BOLT-11 */
    }
    i += 3 + len;
  }

  if (!b.payment_hash) {
    nwa_bolt11_clear(&b);
    g_set_error_literal(error, NWA_ERROR, NWA_ERROR_INVALID_ARGS, "invoice has no payment hash");
    return FALSE;
  }
  *out = b;
  return TRUE;
}
