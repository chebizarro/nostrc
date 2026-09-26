/* nsp-text.c — see nsp-text.h. */
#include "nsp-text.h"

#include <stdlib.h>
#include <string.h>

#include "nostr/nip19/nip19.h"

/* Characters that must never reach the Shell: bidi embeddings/overrides/
 * isolates and marks (text spoofing), zero-width joiners/spaces, BOM, and
 * the interlinear annotation / object replacement controls. */
static gboolean is_invisible_or_bidi(gunichar c) {
  return (c >= 0x202A && c <= 0x202E) || (c >= 0x2066 && c <= 0x2069) ||
         c == 0x200E || c == 0x200F || c == 0x061C || (c >= 0x200B && c <= 0x200D) ||
         c == 0x2060 || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) || c == 0x00AD;
}

char *nsp_text_sanitize(const char *in, gsize max_chars) {
  if (!in) return g_strdup("");
  g_autofree char *valid = g_utf8_make_valid(in, -1);
  GString *out = g_string_sized_new(MIN(strlen(valid), (gsize)1024));
  gsize chars = 0;
  gboolean pending_space = FALSE, truncated = FALSE;

  for (const char *p = valid; *p; p = g_utf8_next_char(p)) {
    gunichar c = g_utf8_get_char(p);
    if (is_invisible_or_bidi(c)) continue;
    GUnicodeType t = g_unichar_type(c);
    if (g_unichar_isspace(c) || t == G_UNICODE_CONTROL || t == G_UNICODE_LINE_SEPARATOR ||
        t == G_UNICODE_PARAGRAPH_SEPARATOR) {
      pending_space = out->len > 0;
      continue;
    }
    gsize need = pending_space ? 2 : 1;
    if (max_chars && chars + need > max_chars) {
      truncated = TRUE;
      break;
    }
    if (pending_space) {
      g_string_append_c(out, ' ');
      chars++;
      pending_space = FALSE;
    }
    g_string_append_unichar(out, c);
    chars++;
  }
  if (truncated) g_string_append(out, "\xe2\x80\xa6"); /* … */
  return g_string_free(out, FALSE);
}

char *nsp_text_relative_time(gint64 created_at, gint64 now) {
  gint64 d = now - created_at;
  if (d < 60) return g_strdup("just now");
  if (d < 3600) return g_strdup_printf("%" G_GINT64_FORMAT " min ago", d / 60);
  if (d < 86400) return g_strdup_printf("%" G_GINT64_FORMAT " h ago", d / 3600);
  if (d < 7 * 86400) return g_strdup_printf("%" G_GINT64_FORMAT " d ago", d / 86400);
  g_autoptr(GDateTime) dt = g_date_time_new_from_unix_utc(created_at);
  if (!dt) return g_strdup("");
  return g_date_time_format(dt, "%Y-%m-%d");
}

static gboolean hex_to_32(const char *hex, guint8 out[32]) {
  if (!hex || strlen(hex) != 64) return FALSE;
  for (int i = 0; i < 32; i++) {
    int hi = g_ascii_xdigit_value(hex[2 * i]), lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0) return FALSE;
    out[i] = (guint8)((hi << 4) | lo);
  }
  return TRUE;
}

char *nsp_text_short_npub(const char *pubkey_hex) {
  guint8 pk[32];
  char *bech = NULL;
  if (!hex_to_32(pubkey_hex, pk) || nostr_nip19_encode_npub(pk, &bech) != 0 || !bech)
    return NULL;
  gsize n = strlen(bech);
  char *r = n > 20 ? g_strdup_printf("%.12s\xe2\x80\xa6%s", bech, bech + n - 6) : g_strdup(bech);
  free(bech);
  return r;
}

char *nsp_text_fold(const char *s) {
  if (!s) return g_strdup("");
  g_autofree char *valid = g_utf8_make_valid(s, -1);
  g_autofree char *norm = g_utf8_normalize(valid, -1, G_NORMALIZE_ALL);
  return g_utf8_casefold(norm ? norm : valid, -1);
}

gboolean nsp_text_match_all(const char *haystack_folded, const char *const *words_folded) {
  if (!haystack_folded || !words_folded || !words_folded[0]) return FALSE;
  for (const char *const *w = words_folded; *w; w++)
    if (**w && !strstr(haystack_folded, *w)) return FALSE;
  return TRUE;
}
