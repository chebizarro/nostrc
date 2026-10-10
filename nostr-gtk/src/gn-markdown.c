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

static GnMarkdownToken *emit(GnMarkdownDocument *doc, GnMarkdownTokenKind kind,
                             const char *text, gsize len, const char *target,
                             guint style, guint level, gboolean ordered) {
  if (!len && kind != GN_MARKDOWN_SEPARATOR && kind != GN_MARKDOWN_LINE_BREAK &&
      kind != GN_MARKDOWN_TABLE_CELL && kind != GN_MARKDOWN_LIST_ITEM)
    return NULL;
  if (doc->tokens->len >= GN_MARKDOWN_MAX_TOKENS) {
    doc->truncated = TRUE;
    return NULL;
  }
  GnMarkdownToken *t = g_new0(GnMarkdownToken, 1);
  t->kind = kind;
  t->text = text ? g_strndup(text, len) : g_strdup("");
  t->target = g_strdup(target);
  t->style = style;
  t->level = level;
  t->ordered = ordered;
  g_ptr_array_add(doc->tokens, t);
  return t;
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

/* GFM delimiter runs, simplified: an opener is not followed by whitespace,
 * a closer not preceded by it, and "_" never opens or closes inside a word
 * (snake_case stays literal). "~~" is strikethrough. */
static const char *find_closer(const char *p, const char *end, guint marker) {
  const char *search_end = p + MIN((gsize)(end - p), (gsize)512);
  for (const char *q = p + marker + 1; q + marker <= search_end; q++) {
    if (memcmp(q, p, marker) != 0) continue;
    if (g_ascii_isspace(q[-1])) continue;
    if (*p == '_' && q + marker < end && g_ascii_isalnum(q[marker])) continue;
    if (marker == 1 && q + 1 < end && q[1] == *p) { q++; continue; }
    return q;
  }
  return NULL;
}

static void inline_tokens(GnMarkdownDocument *doc, const char *s, gsize n,
                          guint style, guint depth) {
  const char *p = s, *end = s + n, *plain = s;
  while (p < end && !doc->truncated) {
    const char *next = NULL;
    if (*p == '\\' && p + 1 < end && g_ascii_ispunct(p[1])) {
      emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
      plain = p + 1;
      p += 2;
      continue;
    }
    if (depth < GN_MARKDOWN_MAX_DEPTH && (*p == '*' || *p == '_' ||
        (*p == '~' && p + 1 < end && p[1] == '~'))) {
      guint marker = p + 1 < end && p[1] == *p ? 2 : 1;
      guint flag = *p == '~' ? GN_MARKDOWN_STYLE_STRIKETHROUGH
                 : marker == 2 ? GN_MARKDOWN_STYLE_STRONG : GN_MARKDOWN_STYLE_EMPHASIS;
      gboolean opens = p + marker < end && !g_ascii_isspace(p[marker]) &&
                       !(*p == '_' && p > s && g_ascii_isalnum(p[-1]));
      if (opens) next = find_closer(p, end, marker);
      if (next) {
        emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
        inline_tokens(doc, p + marker, next - p - marker, style | flag, depth + 1);
        p = next + marker;
        plain = p;
        continue;
      }
      /* An unmatched run stays literal as a whole. */
      p += marker;
      continue;
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
    gboolean image = *p == '!' && p + 1 < end && p[1] == '[';
    if (*p == '[' || image) {
      const char *open = image ? p + 1 : p;
      const char *close = memchr(open + 1, ']', end - open - 1);
      if (close && close + 1 < end && close[1] == '(') {
        const char *stop = memchr(close + 2, ')', end - close - 2);
        if (stop && stop > close + 2) {
          g_autofree char *target = g_strndup(close + 2, stop - close - 2);
          g_strstrip(target);
          emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
          /* Without a label the address itself is what is shown. */
          if (*target && close > open + 1)
            emit(doc, image ? GN_MARKDOWN_IMAGE : GN_MARKDOWN_LINK, open + 1,
                 close - open - 1, target, style, 0, FALSE);
          else if (*target)
            emit(doc, GN_MARKDOWN_TEXT, target, strlen(target), NULL, style, 0, FALSE);
          p = stop + 1;
          plain = p;
          continue;
        }
      }
    }
    gboolean nostr = FALSE;
    if (*p == '<' && p + 1 < end && uri_start(p + 1, end, &nostr)) {
      /* GFM autolink: <https://...>; the brackets are not shown. */
      const char *q = p + 1;
      while (q < end && *q != '>' && *q != '<' && !g_ascii_isspace(*q)) q++;
      if (q < end && *q == '>') {
        g_autofree char *target = g_strndup(p + 1, q - p - 1);
        emit(doc, GN_MARKDOWN_TEXT, plain, p - plain, NULL, style, 0, FALSE);
        emit(doc, nostr ? GN_MARKDOWN_NOSTR_REFERENCE : GN_MARKDOWN_RAW_URL,
             p + 1, q - p - 1, target, style, 0, FALSE);
        p = q + 1;
        plain = p;
        continue;
      }
    }
    if ((p == s || !g_ascii_isalnum(p[-1])) && uri_start(p, end, &nostr)) {
      next = p;
      while (next < end && !g_ascii_isspace(*next) && *next != '<' && *next != '>') next++;
      while (next > p && strchr(".,;!?)]}*_~", next[-1])) next--;
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

/* ---- GFM pipe tables ---------------------------------------------------- */

typedef struct {
  const char *start;
  gsize len;
} Span;

static void trim(const char **s, gsize *n) {
  while (*n && (**s == ' ' || **s == '\t')) { (*s)++; (*n)--; }
  while (*n && ((*s)[*n - 1] == ' ' || (*s)[*n - 1] == '\t')) (*n)--;
}

/* Splits a row on unescaped pipes; a leading and a trailing pipe are
 * optional. Returns the number of cells (at most max), 0 without a pipe. */
static guint split_row(const char *s, gsize n, Span *cells, guint max) {
  trim(&s, &n);
  if (!n || !memchr(s, '|', n)) return 0;
  if (*s == '|') { s++; n--; }
  if (n && s[n - 1] == '|' && (n < 2 || s[n - 2] != '\\')) n--;
  guint count = 0;
  const char *cell = s;
  for (gsize i = 0; i <= n && count < max; i++) {
    if (i + 1 < n && s[i] == '\\') { i++; continue; }
    if (i < n && s[i] != '|') continue;
    const char *c = cell;
    gsize len = (gsize)(s + MIN(i, n) - cell);
    trim(&c, &len);
    cells[count].start = c;
    cells[count].len = len;
    count++;
    cell = s + i + 1;
  }
  return count;
}

static gboolean delimiter_row(const char *s, gsize n, guint *aligns, guint *columns) {
  Span cells[GN_MARKDOWN_MAX_TABLE_COLUMNS];
  guint count = split_row(s, n, cells, GN_MARKDOWN_MAX_TABLE_COLUMNS);
  if (!count) return FALSE;
  for (guint i = 0; i < count; i++) {
    const char *c = cells[i].start;
    gsize len = cells[i].len;
    gboolean left = len && c[0] == ':', right = len > 1 && c[len - 1] == ':';
    gsize first = left ? 1 : 0, last = right ? len - 1 : len;
    if (last <= first) return FALSE;
    for (gsize j = first; j < last; j++)
      if (c[j] != '-') return FALSE;
    aligns[i] = left && right ? GN_MARKDOWN_ALIGN_CENTER
              : right ? GN_MARKDOWN_ALIGN_RIGHT
              : left ? GN_MARKDOWN_ALIGN_LEFT : GN_MARKDOWN_ALIGN_NONE;
  }
  *columns = count;
  return TRUE;
}

/* Emits one row as exactly columns cells: missing ones empty, extra dropped. */
static void emit_row(GnMarkdownDocument *doc, const char *s, gsize n, gboolean header,
                     const guint *aligns, guint columns) {
  Span cells[GN_MARKDOWN_MAX_TABLE_COLUMNS];
  guint count = split_row(s, n, cells, columns);
  if (!count) {
    trim(&s, &n);
    cells[0].start = s;
    cells[0].len = n;
    count = 1;
  }
  for (guint i = 0; i < columns; i++) {
    GnMarkdownToken *t = emit(doc, GN_MARKDOWN_TABLE_CELL,
                              i < count ? cells[i].start : NULL,
                              i < count ? cells[i].len : 0, NULL, 0, i, header);
    if (!t) return;
    t->align = aligns[i];
    t->columns = columns;
  }
}

/* ---- lists -------------------------------------------------------------- */

static gboolean list_item(GnMarkdownDocument *doc, const char *s, gsize n) {
  gsize i = 0, spaces = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) {
    spaces += s[i] == '\t' ? 4 : 1;
    i++;
  }
  gboolean ordered = FALSE;
  guint number = 0;
  if (i + 1 < n && (s[i] == '-' || s[i] == '*' || s[i] == '+') && s[i + 1] == ' ') {
    i += 2;
  } else {
    gsize digits = 0;
    while (i + digits < n && digits < 9 && g_ascii_isdigit(s[i + digits])) digits++;
    if (!digits || i + digits + 1 >= n || (s[i + digits] != '.' && s[i + digits] != ')') ||
        s[i + digits + 1] != ' ')
      return FALSE;
    for (gsize d = 0; d < digits; d++)
      number = number * 10 + (guint)(s[i + d] - '0');
    ordered = TRUE;
    i += digits + 2;
  }
  guint task = GN_MARKDOWN_TASK_NONE;
  if (i + 2 < n && s[i] == '[' && s[i + 2] == ']' && (i + 3 == n || s[i + 3] == ' ')) {
    if (s[i + 1] == ' ')
      task = GN_MARKDOWN_TASK_OPEN;
    else if (s[i + 1] == 'x' || s[i + 1] == 'X')
      task = GN_MARKDOWN_TASK_DONE;
    if (task)
      i = MIN(i + 4, n);
  }
  GnMarkdownToken *t = emit(doc, GN_MARKDOWN_LIST_ITEM, s + i, n - i, NULL, 0, number, ordered);
  if (t) {
    t->indent = MIN((guint)(spaces / 2), GN_MARKDOWN_MAX_LIST_INDENT);
    t->task = task;
  }
  return TRUE;
}

/* A code fence: ``` or ~~~ after up to three spaces. Inside one, only the
 * same character closes it. */
static gboolean fence_line(const char *s, gsize n, char open) {
  gsize i = 0;
  while (i < n && i < 3 && s[i] == ' ') i++;
  if (n - i < 3 || (s[i] != '`' && s[i] != '~') || s[i + 1] != s[i] || s[i + 2] != s[i])
    return FALSE;
  return !open || open == s[i];
}

static const char *line_bounds(const char *p, const char *end, gsize *n) {
  const char *line_end = memchr(p, '\n', end - p);
  if (!line_end) line_end = end;
  *n = line_end - p;
  if (*n && p[*n - 1] == '\r') (*n)--;
  return line_end;
}

static GnMarkdownDocument *document_new(const gchar *input, gssize length) {
  GnMarkdownDocument *doc = g_new0(GnMarkdownDocument, 1);
  doc->tokens = g_ptr_array_new_with_free_func(token_free);
  if (!input) { doc->source = g_strdup(""); return doc; }
  gsize size = length < 0 ? strlen(input) : (gsize)length;
  if (size > GN_MARKDOWN_MAX_INPUT_BYTES) {
    size = GN_MARKDOWN_MAX_INPUT_BYTES;
    doc->truncated = TRUE;
  }
  doc->source = g_utf8_make_valid(input, size);
  return doc;
}

GnMarkdownDocument *gn_markdown_parse_inline(const gchar *input, gssize length) {
  GnMarkdownDocument *doc = document_new(input, length);
  inline_tokens(doc, doc->source, strlen(doc->source), 0, 0);
  return doc;
}

/* A header row followed by a delimiter row with as many cells. */
static guint table_start(const char *s, gsize n, const char *next, const char *end,
                         guint *aligns) {
  Span header[GN_MARKDOWN_MAX_TABLE_COLUMNS];
  guint count = split_row(s, n, header, GN_MARKDOWN_MAX_TABLE_COLUMNS);
  if (!count || !next) return 0;
  gsize next_n = 0;
  guint columns = 0;
  line_bounds(next, end, &next_n);
  if (!delimiter_row(next, next_n, aligns, &columns) || columns != count) return 0;
  return columns;
}

GnMarkdownDocument *gn_markdown_parse(const gchar *input, gssize length) {
  GnMarkdownDocument *doc = document_new(input, length);
  const char *p = doc->source, *end = doc->source + strlen(doc->source);
  char fence = 0;             /* the open fence's character */
  guint table = 0;            /* columns of the table being read */
  guint aligns[GN_MARKDOWN_MAX_TABLE_COLUMNS] = { 0 };
  while (p < end && doc->tokens->len < GN_MARKDOWN_MAX_TOKENS) {
    gsize n = 0;
    const char *line_end = line_bounds(p, end, &n);
    const char *s = p;
    const char *next = line_end < end ? line_end + 1 : NULL;
    if (table) {
      /* Body rows continue until a blank line or a line without a pipe. */
      Span probe[1];
      if (split_row(s, n, probe, 1))
        emit_row(doc, s, n, FALSE, aligns, table);
      else
        table = 0;
    }
    if (table) {
      /* a body row, emitted above */
    } else if (fence_line(s, n, fence)) {
      gsize i = 0;
      while (s[i] == ' ') i++;
      fence = fence ? 0 : s[i];
    } else if (fence) {
      emit(doc, GN_MARKDOWN_CODE, s, n, NULL, 0, 0, FALSE);
    } else if (separator(s, n)) {
      emit(doc, GN_MARKDOWN_SEPARATOR, NULL, 0, NULL, 0, 0, FALSE);
    } else if ((table = table_start(s, n, next, end, aligns)) != 0) {
      emit_row(doc, s, n, TRUE, aligns, table);
      /* The delimiter row emits nothing, not even its line break. */
      gsize skipped = 0;
      line_end = line_bounds(next, end, &skipped);
    } else {
      guint level = 0;
      while (level < n && level < 6 && s[level] == '#') level++;
      if (level && level < n && s[level] == ' ') {
        emit(doc, GN_MARKDOWN_HEADING, s + level + 1, n - level - 1,
             NULL, 0, level, FALSE);
      } else if (n >= 1 && s[0] == '>' && (n == 1 || s[1] == ' ')) {
        gsize skip = n >= 2 ? 2 : 1;
        emit(doc, GN_MARKDOWN_QUOTE, s + skip, n - skip, NULL, 0, 0, FALSE);
      } else if (!list_item(doc, s, n)) {
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
