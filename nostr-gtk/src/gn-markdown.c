#include <nostr-gtk-1.0/gn-markdown.h>
#include <string.h>

static void token_free(gpointer data) {
  GnMarkdownToken *t = data;
  if (!t) return;
  g_free(t->text);
  g_free(t->target);
  g_free(t);
}

void gn_markdown_document_free(GnMarkdownDocument *doc) {
  if (!doc) return;
  g_ptr_array_unref(doc->tokens);
  g_free(doc->source);
  g_free(doc);
}

static void emit(GnMarkdownDocument *doc, GnMarkdownTokenKind kind,
                 const char *text, gsize len, const char *target,
                 guint style, guint level, gboolean ordered) {
  if (!len && kind != GN_MARKDOWN_SEPARATOR && kind != GN_MARKDOWN_LINE_BREAK) return;
  if (doc->tokens->len >= GN_MARKDOWN_MAX_TOKENS) {
    doc->truncated = TRUE;
    return;
  }
  GnMarkdownToken *t = g_new0(GnMarkdownToken, 1);
  t->kind = kind;
  t->text = text ? g_strndup(text, len) : g_strdup("");
  t->target = g_strdup(target);
  t->style = style;
  t->level = level;
  t->ordered = ordered;
  g_ptr_array_add(doc->tokens, t);
}

static gboolean uri_start(const char *p, const char *end, gboolean *nostr) {
  *nostr = FALSE;
  if (end - p >= 8 && g_ascii_strncasecmp(p, "https://", 8) == 0) return TRUE;
  if (end - p >= 7 && g_ascii_strncasecmp(p, "http://", 7) == 0) return TRUE;
  if (end - p >= 6 && g_ascii_strncasecmp(p, "nostr:", 6) == 0) {
    *nostr = TRUE;
    return TRUE;
  }
  return FALSE;
}

static void inline_tokens(GnMarkdownDocument *doc, const char *s, gsize n,
                          guint style, guint depth) {
  const char *p = s, *end = s + n, *plain = s;
  while (p < end && !doc->truncated) {
    const char *next = NULL;
    guint marker = 0, flag = 0;
    if (depth < GN_MARKDOWN_MAX_DEPTH && (*p == '*' || *p == '_')) {
      marker = p + 1 < end && p[1] == *p ? 2 : 1;
      flag = marker == 2 ? GN_MARKDOWN_STYLE_STRONG : GN_MARKDOWN_STYLE_EMPHASIS;
      const char *search_end = p + MIN((gsize)(end - p), (gsize)512);
      for (const char *q = p + marker; q + marker <= search_end; q++) {
        if (q > p + marker && memcmp(q, p, marker) == 0) { next = q; break; }
      }
      if (next) {
        emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
        inline_tokens(doc, p + marker, next - p - marker, style | flag, depth + 1);
        p = next + marker;
        plain = p;
        continue;
      }
    }
    if (*p == '`') {
      next = memchr(p + 1, '`', end - p - 1);
      if (next) {
        emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
        emit(doc, GN_MARKDOWN_CODE, p + 1, next - p - 1, NULL, 0, 0, FALSE);
        p = next + 1;
        plain = p;
        continue;
      }
    }
    if (*p == '[') {
      const char *close = memchr(p + 1, ']', end - p - 1);
      if (close && close + 1 < end && close[1] == '(') {
        const char *stop = memchr(close + 2, ')', end - close - 2);
        if (stop && stop > close + 2) {
          g_autofree char *target = g_strndup(close + 2, stop - close - 2);
          emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
          emit(doc, GN_MARKDOWN_LINK, p + 1, close - p - 1, target, style, 0, FALSE);
          p = stop + 1;
          plain = p;
          continue;
        }
      }
    }
    gboolean nostr = FALSE;
    if ((p == s || !g_ascii_isalnum(p[-1])) && uri_start(p, end, &nostr)) {
      next = p;
      while (next < end && !g_ascii_isspace(*next) && *next != '<' && *next != '>') next++;
      while (next > p && strchr(".,;!?)]}", next[-1])) next--;
      if (next > p) {
        g_autofree char *target = g_strndup(p, next - p);
        emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
        emit(doc, nostr ? GN_MARKDOWN_NOSTR_REFERENCE : GN_MARKDOWN_RAW_URL,
             p, next - p, target, style, 0, FALSE);
        p = next;
        plain = p;
        continue;
      }
    }
    p = g_utf8_next_char(p);
  }
  emit(doc, GN_MARKDOWN_TEXT, plain, end - plain, NULL, style, 0, FALSE);
}

static gboolean separator(const char *s, gsize n) {
  char mark = 0;
  guint count = 0;
  for (gsize i = 0; i < n; i++) {
    if (s[i] == ' ' || s[i] == '\t') continue;
    if (!mark) mark = s[i];
    if (s[i] != mark || (mark != '-' && mark != '*' && mark != '_')) return FALSE;
    count++;
  }
  return count >= 3;
}

GnMarkdownDocument *gn_markdown_parse(const gchar *input, gssize length) {
  GnMarkdownDocument *doc = g_new0(GnMarkdownDocument, 1);
  doc->tokens = g_ptr_array_new_with_free_func(token_free);
  if (!input) { doc->source = g_strdup(""); return doc; }
  gsize size = length < 0 ? strlen(input) : (gsize)length;
  if (size > GN_MARKDOWN_MAX_INPUT_BYTES) {
    size = GN_MARKDOWN_MAX_INPUT_BYTES;
    doc->truncated = TRUE;
  }
  doc->source = g_utf8_make_valid(input, size);
  const char *p = doc->source, *end = doc->source + strlen(doc->source);
  gboolean fence = FALSE;
  while (p < end && doc->tokens->len < GN_MARKDOWN_MAX_TOKENS) {
    const char *line_end = memchr(p, '\n', end - p);
    if (!line_end) line_end = end;
    const char *s = p;
    gsize n = line_end - p;
    if (n && s[n - 1] == '\r') n--;
    if (n >= 3 && memcmp(s, "```", 3) == 0) {
      fence = !fence;
    } else if (fence) {
      emit(doc, GN_MARKDOWN_CODE, s, n, NULL, 0, 0, FALSE);
    } else if (separator(s, n)) {
      emit(doc, GN_MARKDOWN_SEPARATOR, NULL, 0, NULL, 0, 0, FALSE);
    } else {
      guint level = 0;
      while (level < n && level < 6 && s[level] == '#') level++;
      if (level && level < n && s[level] == ' ') {
        emit(doc, GN_MARKDOWN_HEADING, s + level + 1, n - level - 1,
             NULL, 0, level, FALSE);
      } else if (n >= 2 && s[0] == '>' && s[1] == ' ') {
        emit(doc, GN_MARKDOWN_QUOTE, s + 2, n - 2, NULL, 0, 0, FALSE);
      } else if (n >= 2 && (s[0] == '-' || s[0] == '*' || s[0] == '+') && s[1] == ' ') {
        emit(doc, GN_MARKDOWN_LIST_ITEM, s + 2, n - 2, NULL, 0, 0, FALSE);
      } else {
        gsize digits = 0;
        while (digits < n && digits < 9 && g_ascii_isdigit(s[digits])) digits++;
        if (digits && digits + 1 < n && s[digits] == '.' && s[digits + 1] == ' ')
          emit(doc, GN_MARKDOWN_LIST_ITEM, s + digits + 2, n - digits - 2,
               NULL, 0, 0, TRUE);
        else
          inline_tokens(doc, s, n, 0, 0);
      }
    }
    if (line_end < end)
      emit(doc, GN_MARKDOWN_LINE_BREAK, "\n", 1, NULL, 0, 0, FALSE);
    p = line_end < end ? line_end + 1 : end;
  }
  if (p < end) doc->truncated = TRUE;
  return doc;
}
