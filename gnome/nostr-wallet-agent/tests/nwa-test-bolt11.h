/* nwa-test-bolt11.h - mint BOLT-11 invoices for tests (header-only).
 *
 * SPDX-License-Identifier: MIT
 *
 * The agent's decoder (src/nwa-bolt11.c) verifies the bech32 checksum and
 * parses the fields but — by design — not the node signature (the wallet
 * does that). Tests can therefore mint invoices with a zero signature that
 * the agent accepts exactly like real ones: fresh timestamps (the spec
 * vectors are all long expired), chosen amounts, `d` or `h` descriptions.
 * Never use outside tests: a real wallet rejects the signature.
 */
#ifndef NWA_TEST_BOLT11_H
#define NWA_TEST_BOLT11_H

#include <glib.h>
#include <string.h>

static const char nwa_test_b32[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

static guint32
nwa_test_b32_polymod(const guint8 *v, gsize n)
{
  static const guint32 gen[5] = { 0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3 };
  guint32 chk = 1;
  for (gsize i = 0; i < n; i++) {
    guint8 top = (guint8)(chk >> 25);
    chk = ((chk & 0x1ffffff) << 5) ^ v[i];
    for (int j = 0; j < 5; j++)
      if ((top >> j) & 1) chk ^= gen[j];
  }
  return chk;
}

/* 8-bit bytes -> 5-bit words (MSB first, zero padded). */
static void
nwa_test_words_from_bytes(GByteArray *w, const guint8 *b, gsize n)
{
  guint32 acc = 0;
  int bits = 0;
  for (gsize i = 0; i < n; i++) {
    acc = (acc << 8) | b[i];
    bits += 8;
    while (bits >= 5) {
      guint8 x = (guint8)((acc >> (bits - 5)) & 31);
      g_byte_array_append(w, &x, 1);
      bits -= 5;
    }
  }
  if (bits > 0) {
    guint8 x = (guint8)((acc << (5 - bits)) & 31);
    g_byte_array_append(w, &x, 1);
  }
}

static void
nwa_test_words_uint(GByteArray *w, guint64 v, guint nwords)
{
  for (guint i = nwords; i > 0; i--) {
    guint8 x = (guint8)((v >> (5 * (i - 1))) & 31);
    g_byte_array_append(w, &x, 1);
  }
}

static void
nwa_test_hex_to_bytes(const gchar *hex, guint8 out[32])
{
  for (int i = 0; i < 32; i++)
    out[i] = (guint8)(g_ascii_xdigit_value(hex[2 * i]) * 16 + g_ascii_xdigit_value(hex[2 * i + 1]));
}

static void
nwa_test_tag(GByteArray *w, guint8 type, const GByteArray *data)
{
  g_byte_array_append(w, &type, 1);
  nwa_test_words_uint(w, data->len, 2);
  g_byte_array_append(w, data->data, data->len);
}

/* @amount_msat 0 = amount-less. Exactly one of @description / @description_hash_hex.
 * @payment_hash_hex: 64 hex. @expiry_s 0 = default (3600). Returns a lower-case invoice. */
static gchar *
nwa_test_bolt11_mint(guint64 amount_msat, const gchar *description,
                     const gchar *description_hash_hex, const gchar *payment_hash_hex,
                     gint64 timestamp, guint expiry_s)
{
  GString *hrp = g_string_new("lnbc");
  if (amount_msat) {
    if (amount_msat % 100 == 0)
      g_string_append_printf(hrp, "%" G_GUINT64_FORMAT "n", amount_msat / 100);
    else
      g_string_append_printf(hrp, "%" G_GUINT64_FORMAT "p", amount_msat * 10);
  }
  GByteArray *w = g_byte_array_new();
  nwa_test_words_uint(w, (guint64)timestamp, 7);
  guint8 raw[32];
  GByteArray *d = g_byte_array_new();
  nwa_test_hex_to_bytes(payment_hash_hex, raw);
  nwa_test_words_from_bytes(d, raw, 32);
  nwa_test_tag(w, 1, d);
  g_byte_array_set_size(d, 0);
  if (description_hash_hex) {
    nwa_test_hex_to_bytes(description_hash_hex, raw);
    nwa_test_words_from_bytes(d, raw, 32);
    nwa_test_tag(w, 23, d);
  } else {
    const gchar *desc = description ? description : "";
    nwa_test_words_from_bytes(d, (const guint8 *)desc, strlen(desc));
    nwa_test_tag(w, 13, d);
  }
  if (expiry_s) {
    g_byte_array_set_size(d, 0);
    guint n = 1;
    while (n < 7 && ((guint64)expiry_s >> (5 * n))) n++;
    nwa_test_words_uint(d, expiry_s, n);
    nwa_test_tag(w, 6, d);
  }
  g_byte_array_free(d, TRUE);
  guint8 zero = 0;
  for (int i = 0; i < 104; i++) g_byte_array_append(w, &zero, 1); /* signature: not checked */

  /* checksum over hrp-expand || data || 000000 */
  GByteArray *chk = g_byte_array_new();
  for (gsize i = 0; i < hrp->len; i++) { guint8 x = (guint8)(hrp->str[i] >> 5); g_byte_array_append(chk, &x, 1); }
  g_byte_array_append(chk, &zero, 1);
  for (gsize i = 0; i < hrp->len; i++) { guint8 x = (guint8)(hrp->str[i] & 31); g_byte_array_append(chk, &x, 1); }
  g_byte_array_append(chk, w->data, w->len);
  for (int i = 0; i < 6; i++) g_byte_array_append(chk, &zero, 1);
  guint32 pm = nwa_test_b32_polymod(chk->data, chk->len) ^ 1;
  g_byte_array_free(chk, TRUE);

  GString *out = g_string_new(hrp->str);
  g_string_append_c(out, '1');
  for (guint i = 0; i < w->len; i++) g_string_append_c(out, nwa_test_b32[w->data[i]]);
  for (int i = 0; i < 6; i++) g_string_append_c(out, nwa_test_b32[(pm >> (5 * (5 - i))) & 31]);
  g_byte_array_free(w, TRUE);
  g_string_free(hrp, TRUE);
  return g_string_free(out, FALSE);
}

#endif /* NWA_TEST_BOLT11_H */
