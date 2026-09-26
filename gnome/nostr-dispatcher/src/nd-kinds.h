/*
 * nd-kinds — parse and match Nostr kind specifications.
 *
 * One grammar is shared by the `X-Nostr-Kinds=` desktop-entry key and the
 * keys of `[Default Handlers]` / `[Removed Handlers]` in handlers.list:
 *
 *   token := N          exact kind, 0..65535 (NIP-01 kind range)
 *          | A-B        inclusive range, A <= B
 *          | *          wildcard: generic-viewer fallback (never matched
 *                       by nd_kind_spec_best(); see nd-registry.c)
 *   spec  := token { ';' token } [';']
 *
 * Malformed tokens are skipped (fail soft per token), never the whole spec.
 */
#ifndef ND_KINDS_H
#define ND_KINDS_H

#include <glib.h>

G_BEGIN_DECLS

#define ND_KIND_MAX 65535u

typedef struct {
  guint32 lo;
  guint32 hi;
  gboolean any; /* the `*` token */
} NdKindRange;

/* Parse one token (surrounding whitespace allowed). */
gboolean nd_kind_token_parse(const char *token, NdKindRange *out);

/* Parse a ';'-separated spec into a GArray of NdKindRange (never NULL). */
GArray *nd_kind_spec_parse(const char *spec);

gboolean nd_kind_range_contains(const NdKindRange *r, guint32 kind);

/* Specificity of a match: number of kinds covered (1 for an exact kind).
 * Smaller is more specific. */
guint32 nd_kind_range_width(const NdKindRange *r);

/* Best (narrowest) non-wildcard match for @kind in @spec. Returns FALSE if
 * nothing but (optionally) `*` matches. */
gboolean nd_kind_spec_best(GArray *spec, guint32 kind, guint32 *out_width);

/* TRUE if @spec contains the `*` token. */
gboolean nd_kind_spec_has_any(GArray *spec);

G_END_DECLS

#endif /* ND_KINDS_H */
