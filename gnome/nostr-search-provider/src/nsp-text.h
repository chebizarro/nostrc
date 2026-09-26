/*
 * nsp-text — display-string helpers for GNOME Shell result metas.
 *
 * Escaping contract (verified against gnome-shell 46, js/ui/search.js and
 * js/misc/util.js): the Shell renders `name` as plain St.Label text and
 * passes `description` (first line only) through its Highlighter, which
 * calls GLib.markup_escape_text() itself before wrapping matched terms in
 * <b>. So metas must NOT be markup-escaped here: doing so would show
 * "&amp;" to the user. The notify daemon escapes because GNotification
 * bodies ARE markup; this module deliberately differs.
 *
 * What we do instead is sanitize: every string that reaches the Shell is
 * valid UTF-8, single-line, free of C0/C1 controls, bidi overrides and
 * zero-width characters (spoofing), whitespace-collapsed and truncated by
 * code points.
 */
#ifndef NSP_TEXT_H
#define NSP_TEXT_H

#include <glib.h>

G_BEGIN_DECLS

#define NSP_NOTE_SNIPPET_CHARS 80
#define NSP_NAME_MAX_CHARS 64

/* Sanitize @in (any bytes, may be NULL) as described above and truncate
 * to at most @max_chars code points, appending U+2026 when truncated.
 * @max_chars == 0 means no limit. Never returns NULL. */
char *nsp_text_sanitize(const char *in, gsize max_chars);

/* Compact relative time of @created_at (unix seconds) against @now:
 * "just now", "5 min ago", "3 h ago", "2 d ago", else "YYYY-MM-DD"
 * (UTC). Future timestamps render as "just now". */
char *nsp_text_relative_time(gint64 created_at, gint64 now);

/* "npub1abcdefgh…uvwxyz" for a 64-hex pubkey; NULL on bad input. */
char *nsp_text_short_npub(const char *pubkey_hex);

/* Case-folded, NFKD-normalized copy for term matching. */
char *nsp_text_fold(const char *s);

/* TRUE if every word in @words_folded occurs in @haystack_folded. An
 * empty/NULL word list matches nothing. */
gboolean nsp_text_match_all(const char *haystack_folded, const char *const *words_folded);

G_END_DECLS

#endif /* NSP_TEXT_H */
