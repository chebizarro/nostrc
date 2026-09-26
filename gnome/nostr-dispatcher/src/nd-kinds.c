/* nd-kinds.c — see nd-kinds.h for the grammar. */
#include "nd-kinds.h"

#include <string.h>

static gboolean parse_kind_number(const char *s, guint32 *out) {
  if (!s || !*s) return FALSE;
  guint64 v = 0;
  /* g_ascii_string_to_unsigned rejects signs, whitespace and trailing junk. */
  if (!g_ascii_string_to_unsigned(s, 10, 0, ND_KIND_MAX, &v, NULL))
    return FALSE;
  *out = (guint32)v;
  return TRUE;
}

gboolean nd_kind_token_parse(const char *token, NdKindRange *out) {
  if (!token || !out) return FALSE;
  g_autofree char *t = g_strstrip(g_strdup(token));
  if (!*t) return FALSE;

  if (strcmp(t, "*") == 0) {
    out->lo = 0;
    out->hi = ND_KIND_MAX;
    out->any = TRUE;
    return TRUE;
  }

  char *dash = strchr(t, '-');
  if (dash) {
    *dash = '\0';
    guint32 lo = 0, hi = 0;
    if (!parse_kind_number(g_strstrip(t), &lo) ||
        !parse_kind_number(g_strstrip(dash + 1), &hi) || lo > hi)
      return FALSE;
    out->lo = lo;
    out->hi = hi;
    out->any = FALSE;
    return TRUE;
  }

  guint32 k = 0;
  if (!parse_kind_number(t, &k)) return FALSE;
  out->lo = out->hi = k;
  out->any = FALSE;
  return TRUE;
}

GArray *nd_kind_spec_parse(const char *spec) {
  GArray *arr = g_array_new(FALSE, FALSE, sizeof(NdKindRange));
  if (!spec) return arr;
  g_auto(GStrv) parts = g_strsplit(spec, ";", -1);
  for (guint i = 0; parts[i]; i++) {
    if (!*g_strstrip(parts[i])) continue;
    NdKindRange r;
    if (nd_kind_token_parse(parts[i], &r))
      g_array_append_val(arr, r);
    else
      g_debug("nostr-dispatcher: ignoring malformed kind token '%s'", parts[i]);
  }
  return arr;
}

gboolean nd_kind_range_contains(const NdKindRange *r, guint32 kind) {
  return r && kind >= r->lo && kind <= r->hi;
}

guint32 nd_kind_range_width(const NdKindRange *r) {
  return r ? r->hi - r->lo + 1 : G_MAXUINT32;
}

gboolean nd_kind_spec_best(GArray *spec, guint32 kind, guint32 *out_width) {
  gboolean found = FALSE;
  guint32 best = G_MAXUINT32;
  for (guint i = 0; spec && i < spec->len; i++) {
    const NdKindRange *r = &g_array_index(spec, NdKindRange, i);
    if (r->any || !nd_kind_range_contains(r, kind)) continue;
    guint32 w = nd_kind_range_width(r);
    if (!found || w < best) {
      best = w;
      found = TRUE;
    }
  }
  if (found && out_width) *out_width = best;
  return found;
}

gboolean nd_kind_spec_has_any(GArray *spec) {
  for (guint i = 0; spec && i < spec->len; i++)
    if (g_array_index(spec, NdKindRange, i).any) return TRUE;
  return FALSE;
}
