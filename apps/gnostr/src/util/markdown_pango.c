/*
 * markdown_pango.c - Simple Markdown to Pango Markup Converter
 *
 * NIP-23 Long-form Content support for gnostr GTK4 client.
 */

#include "markdown_pango.h"
#include <string.h>
#include <ctype.h>

/* Helper: escape special Pango/XML characters */
static char *escape_pango_text(const char *text, gsize len) {
  if (!text) return g_strdup("");
  if (len == 0) len = strlen(text);

  GString *escaped = g_string_sized_new(len + 32);

  for (gsize i = 0; i < len && text[i]; i++) {
    switch (text[i]) {
      case '&':
        g_string_append(escaped, "&amp;");
        break;
      case '<':
        g_string_append(escaped, "&lt;");
        break;
      case '>':
        g_string_append(escaped, "&gt;");
        break;
      case '\'':
        g_string_append(escaped, "&apos;");
        break;
      case '"':
        g_string_append(escaped, "&quot;");
        break;
      default:
        g_string_append_c(escaped, text[i]);
    }
  }

  return g_string_free(escaped, FALSE);
}

/* Helper: check if character is markdown formatting delimiter */
static gboolean is_format_delimiter(char c) {
  return c == '*' || c == '_' || c == '`' || c == '[' || c == '!';
}

/* Helper: count consecutive matching characters */
static int count_consecutive(const char *str, char target) {
  int count = 0;
  while (str[count] == target) count++;
  return count;
}

/* Helper: find closing delimiter, respecting escapes */
static const char *find_closing(const char *start, const char *delim, gsize delim_len) {
  const char *p = start;

  while (*p) {
    if (*p == '\\' && *(p + 1)) {
      p += 2;  /* Skip escaped character */
      continue;
    }
    if (strncmp(p, delim, delim_len) == 0) {
      return p;
    }
    p++;
  }
  return NULL;
}

/* nostrc-csaf: Helper to escape and append a single UTF-8 character.
 * Unlike escape_pango_text(p, 1) which splits multi-byte sequences,
 * this properly handles the full character width. */
static inline void
append_escaped_utf8_char(GString *out, const char *p, const char **next_out)
{
  const char *next = g_utf8_next_char(p);
  gsize byte_len = next - p;
  char *escaped = escape_pango_text(p, byte_len);
  g_string_append(out, escaped);
  g_free(escaped);
  *next_out = next;
}

/* Process inline markdown elements */
static void process_inline(GString *out, const char *line, gsize len) {
  const char *p = line;
  const char *end = line + len;

  while (p < end && *p) {
    /* Handle escaped characters */
    if (*p == '\\' && p + 1 < end) {
      /* nostrc-csaf: advance by full UTF-8 char after backslash */
      const char *next;
      append_escaped_utf8_char(out, p + 1, &next);
      p = next;
      continue;
    }

    /* Bold with ** */
    if (p + 1 < end && p[0] == '*' && p[1] == '*') {
      const char *close = find_closing(p + 2, "**", 2);
      if (close && close < end) {
        g_string_append(out, "<b>");
        process_inline(out, p + 2, close - (p + 2));
        g_string_append(out, "</b>");
        p = close + 2;
        continue;
      }
    }

    /* Bold with __ */
    if (p + 1 < end && p[0] == '_' && p[1] == '_') {
      const char *close = find_closing(p + 2, "__", 2);
      if (close && close < end) {
        g_string_append(out, "<b>");
        process_inline(out, p + 2, close - (p + 2));
        g_string_append(out, "</b>");
        p = close + 2;
        continue;
      }
    }

    /* Italic with * (single) */
    if (*p == '*' && (p + 1 >= end || p[1] != '*')) {
      const char *close = find_closing(p + 1, "*", 1);
      if (close && close < end && close > p + 1) {
        g_string_append(out, "<i>");
        process_inline(out, p + 1, close - (p + 1));
        g_string_append(out, "</i>");
        p = close + 1;
        continue;
      }
    }

    /* Italic with _ (single) */
    if (*p == '_' && (p + 1 >= end || p[1] != '_')) {
      const char *close = find_closing(p + 1, "_", 1);
      if (close && close < end && close > p + 1) {
        g_string_append(out, "<i>");
        process_inline(out, p + 1, close - (p + 1));
        g_string_append(out, "</i>");
        p = close + 1;
        continue;
      }
    }

    /* Inline code with ` */
    if (*p == '`') {
      const char *close = find_closing(p + 1, "`", 1);
      if (close && close < end) {
        g_string_append(out, "<tt>");
        char *code_escaped = escape_pango_text(p + 1, close - (p + 1));
        g_string_append(out, code_escaped);
        g_free(code_escaped);
        g_string_append(out, "</tt>");
        p = close + 1;
        continue;
      }
    }

    /* Links: [text](url) */
    if (*p == '[') {
      const char *text_end = strchr(p + 1, ']');
      if (text_end && text_end + 1 < end && *(text_end + 1) == '(') {
        const char *url_end = strchr(text_end + 2, ')');
        if (url_end && url_end <= end) {
          gsize text_len = text_end - (p + 1);
          gsize url_len = url_end - (text_end + 2);

          char *text = g_strndup(p + 1, text_len);
          char *url = g_strndup(text_end + 2, url_len);
          char *text_escaped = escape_pango_text(text, 0);
          char *url_escaped = escape_pango_text(url, 0);

          g_string_append_printf(out, "<a href=\"%s\">%s</a>", url_escaped, text_escaped);

          g_free(text);
          g_free(url);
          g_free(text_escaped);
          g_free(url_escaped);

          p = url_end + 1;
          continue;
        }
      }
    }

    /* Images: ![alt](url) - render as link since Pango can't do images */
    if (*p == '!' && p + 1 < end && *(p + 1) == '[') {
      const char *alt_end = strchr(p + 2, ']');
      if (alt_end && alt_end + 1 < end && *(alt_end + 1) == '(') {
        const char *url_end = strchr(alt_end + 2, ')');
        if (url_end && url_end <= end) {
          gsize alt_len = alt_end - (p + 2);
          gsize url_len = url_end - (alt_end + 2);

          char *alt = alt_len > 0 ? g_strndup(p + 2, alt_len) : g_strdup("[Image]");
          char *url = g_strndup(alt_end + 2, url_len);
          char *alt_escaped = escape_pango_text(alt, 0);
          char *url_escaped = escape_pango_text(url, 0);

          g_string_append_printf(out, "<a href=\"%s\">%s</a>", url_escaped, alt_escaped);

          g_free(alt);
          g_free(url);
          g_free(alt_escaped);
          g_free(url_escaped);

          p = url_end + 1;
          continue;
        }
      }
    }

    /* Nostr mentions: nostr:npub1... or nostr:note1... */
    if (strncmp(p, "nostr:", 6) == 0) {
      const char *entity_start = p + 6;
      const char *entity_end = entity_start;

      /* Find end of bech32 entity */
      while (entity_end < end && (g_ascii_isalnum(*entity_end) || *entity_end == '1')) {
        entity_end++;
      }

      if (entity_end > entity_start) {
        gsize entity_len = entity_end - p;
        char *entity = g_strndup(p, entity_len);
        char *entity_escaped = escape_pango_text(entity, 0);

        /* Create a clickable link for nostr entities */
        g_string_append_printf(out, "<a href=\"%s\">%s</a>", entity_escaped, entity_escaped);

        g_free(entity);
        g_free(entity_escaped);
        p = entity_end;
        continue;
      }
    }

    /* Regular character — escape full UTF-8 codepoint (nostrc-csaf) */
    const char *next;
    append_escaped_utf8_char(out, p, &next);
    p = next;
  }
}

/* Process a single line of markdown */
static void process_line(GString *out, const char *line, gsize len, gboolean *in_code_block) {
  if (!line || len == 0) {
    g_string_append(out, "\n");
    return;
  }

  /* Skip leading whitespace for analysis but preserve for code */
  const char *trimmed = line;
  while (*trimmed && g_ascii_isspace(*trimmed) && (trimmed - line) < (gssize)len) {
    trimmed++;
  }
  gsize trimmed_len = len - (trimmed - line);

  /* Code block toggle (```) */
  if (trimmed_len >= 3 && strncmp(trimmed, "```", 3) == 0) {
    *in_code_block = !(*in_code_block);
    if (*in_code_block) {
      g_string_append(out, "<tt>");
    } else {
      g_string_append(out, "</tt>\n");
    }
    return;
  }

  /* Inside code block - just escape and add */
  if (*in_code_block) {
    char *escaped = escape_pango_text(line, len);
    g_string_append(out, escaped);
    g_string_append(out, "\n");
    g_free(escaped);
    return;
  }

  /* Horizontal rule */
  if (trimmed_len >= 3) {
    gboolean is_hr = TRUE;
    char hr_char = trimmed[0];
    if (hr_char == '-' || hr_char == '*' || hr_char == '_') {
      for (gsize i = 0; i < trimmed_len; i++) {
        if (trimmed[i] != hr_char && !g_ascii_isspace(trimmed[i])) {
          is_hr = FALSE;
          break;
        }
      }
      if (is_hr) {
        g_string_append(out, "\n<span alpha=\"50%\">---</span>\n");
        return;
      }
    }
  }

  /* Headings (# to ######) */
  if (trimmed[0] == '#') {
    int level = count_consecutive(trimmed, '#');
    if (level >= 1 && level <= 6 && (trimmed_len <= (gsize)level || g_ascii_isspace(trimmed[level]))) {
      const char *heading_text = trimmed + level;
      while (*heading_text && g_ascii_isspace(*heading_text)) heading_text++;
      gsize heading_len = trimmed_len - (heading_text - trimmed);

      /* Size based on heading level */
      const char *size_attr;
      switch (level) {
        case 1: size_attr = "xx-large"; break;
        case 2: size_attr = "x-large"; break;
        case 3: size_attr = "large"; break;
        case 4: size_attr = "medium"; break;
        default: size_attr = "medium"; break;
      }

      g_string_append_printf(out, "\n<span size=\"%s\" weight=\"bold\">", size_attr);
      process_inline(out, heading_text, heading_len);
      g_string_append(out, "</span>\n");
      return;
    }
  }

  /* Blockquote (>) */
  if (trimmed[0] == '>') {
    const char *quote_text = trimmed + 1;
    while (*quote_text && g_ascii_isspace(*quote_text)) quote_text++;
    gsize quote_len = trimmed_len - (quote_text - trimmed);

    g_string_append(out, "<span alpha=\"80%\" style=\"italic\">");
    process_inline(out, quote_text, quote_len);
    g_string_append(out, "</span>\n");
    return;
  }

  /* Unordered list (- or *) */
  if ((trimmed[0] == '-' || trimmed[0] == '*') && trimmed_len > 1 && g_ascii_isspace(trimmed[1])) {
    const char *item_text = trimmed + 2;
    gsize item_len = trimmed_len - 2;

    g_string_append(out, "  \342\200\242 "); /* Unicode bullet */
    process_inline(out, item_text, item_len);
    g_string_append(out, "\n");
    return;
  }

  /* Ordered list (1. 2. etc) */
  if (g_ascii_isdigit(trimmed[0])) {
    const char *dot = trimmed;
    while (*dot && g_ascii_isdigit(*dot)) dot++;
    if (*dot == '.' && dot + 1 < trimmed + trimmed_len && g_ascii_isspace(*(dot + 1))) {
      const char *item_text = dot + 2;
      gsize item_len = trimmed_len - (item_text - trimmed);

      gsize num_len = dot - trimmed;
      g_string_append_printf(out, "  %.*s. ", (int)num_len, trimmed);
      process_inline(out, item_text, item_len);
      g_string_append(out, "\n");
      return;
    }
  }

  /* Regular paragraph line */
  process_inline(out, line, len);
  g_string_append(out, "\n");
}

char *markdown_to_pango(const char *markdown, gsize max_length) {
  if (!markdown || !*markdown) {
    return g_strdup("");
  }

  /* nostrc-csaf: Sanitize UTF-8 first. Relay content can contain invalid
   * byte sequences that corrupt Pango if passed through as-is. */
  g_autofree gchar *safe_md = NULL;
  if (!g_utf8_validate(markdown, -1, NULL)) {
    safe_md = g_utf8_make_valid(markdown, -1);
    markdown = safe_md;
  }

  GString *out = g_string_sized_new(strlen(markdown) + 256);
  gboolean in_code_block = FALSE;

  const char *p = markdown;
  const char *line_start = p;

  while (*p) {
    if (*p == '\n') {
      process_line(out, line_start, p - line_start, &in_code_block);
      line_start = p + 1;
    }
    p++;
  }

  /* Process last line if not ending with newline */
  if (line_start < p) {
    process_line(out, line_start, p - line_start, &in_code_block);
  }

  /* Close unclosed code block */
  if (in_code_block) {
    g_string_append(out, "</tt>");
  }

  /* Truncate if needed */
  if (max_length > 0 && out->len > max_length) {
    g_string_truncate(out, max_length);
    g_string_append(out, "...");
  }

  return g_string_free(out, FALSE);
}

/* nostrc-csaf: Helper to process a span of text with proper UTF-8 advancement.
 * Used inside bold/italic/link-text spans. */
static inline const char *
append_span_text_utf8(GString *out, const char *p, const char *limit,
                      gsize *char_count, gsize max_chars, gboolean *prev_space)
{
  while (p < limit && *char_count < max_chars) {
    if (g_ascii_isspace(*p)) {
      if (!*prev_space) {
        g_string_append_c(out, ' ');
        (*char_count)++;
      }
      *prev_space = TRUE;
      p++;
    } else {
      const char *next;
      append_escaped_utf8_char(out, p, &next);
      (*char_count)++;
      *prev_space = FALSE;
      p = next;
    }
  }
  return p;
}

char *markdown_to_pango_summary(const char *markdown, gsize max_chars) {
  if (!markdown || !*markdown) {
    return g_strdup("");
  }

  /* nostrc-csaf: Sanitize UTF-8 first. Relay content can contain invalid
   * byte sequences that corrupt Pango if passed through as-is. */
  g_autofree gchar *safe_md = NULL;
  if (!g_utf8_validate(markdown, -1, NULL)) {
    safe_md = g_utf8_make_valid(markdown, -1);
    markdown = safe_md;
  }

  /* For summaries, we do a simplified conversion:
   * - Keep bold and italic
   * - Strip heading markers but keep text
   * - Convert links to plain text
   * - Collapse whitespace
   * nostrc-csaf: All character advancement uses g_utf8_next_char to
   * avoid splitting multi-byte UTF-8 sequences.
   */
  GString *out = g_string_sized_new(max_chars + 64);
  const char *p = markdown;
  gboolean prev_space = FALSE;
  gsize char_count = 0;

  while (*p && char_count < max_chars) {
    /* Skip code fences */
    if (strncmp(p, "```", 3) == 0) {
      p += 3;
      /* Skip to end of code fence */
      const char *end = strstr(p, "```");
      if (end) {
        p = end + 3;
        continue;
      }
    }

    /* Skip heading markers */
    if (*p == '#') {
      while (*p == '#') p++;
      while (*p && g_ascii_isspace(*p)) p++;
      continue;
    }

    /* Handle ** and __ (bold) */
    if ((p[0] == '*' && p[1] == '*') || (p[0] == '_' && p[1] == '_')) {
      g_string_append(out, "<b>");
      char delim[3] = { p[0], p[1], 0 };
      p += 2;
      const char *close = strstr(p, delim);
      if (close) {
        p = append_span_text_utf8(out, p, close, &char_count, max_chars, &prev_space);
        p = close + 2;
      }
      g_string_append(out, "</b>");
      continue;
    }

    /* Handle single * and _ (italic) */
    if ((*p == '*' || *p == '_') && p[1] != *p) {
      g_string_append(out, "<i>");
      char delim = *p;
      p++;
      /* Find closing delimiter */
      const char *close = strchr(p, delim);
      if (close) {
        p = append_span_text_utf8(out, p, close, &char_count, max_chars, &prev_space);
        if (*p == delim) p++;
      }
      g_string_append(out, "</i>");
      continue;
    }

    /* Skip link/image markup but keep text */
    if (*p == '[' || (*p == '!' && *(p+1) == '[')) {
      if (*p == '!') p++;
      const char *text_end = strchr(p + 1, ']');
      if (text_end && *(text_end + 1) == '(') {
        const char *url_end = strchr(text_end + 2, ')');
        if (url_end) {
          /* Output just the link text with proper UTF-8 handling */
          const char *text = p + 1;
          append_span_text_utf8(out, text, text_end, &char_count, max_chars, &prev_space);
          p = url_end + 1;
          continue;
        }
      }
    }

    /* Collapse whitespace */
    if (g_ascii_isspace(*p)) {
      if (!prev_space) {
        g_string_append_c(out, ' ');
        char_count++;
      }
      prev_space = TRUE;
      p++;
      continue;
    }

    /* Regular character — advance by full UTF-8 codepoint */
    const char *next;
    append_escaped_utf8_char(out, p, &next);
    char_count++;
    prev_space = FALSE;
    p = next;
  }

  /* Add ellipsis if truncated */
  if (char_count >= max_chars && *p) {
    g_string_append(out, "…");
  }

  return g_string_free(out, FALSE);
}

char *markdown_extract_first_image(const char *markdown) {
  if (!markdown) return NULL;

  const char *p = markdown;

  while (*p) {
    /* Look for ![alt](url) pattern */
    if (*p == '!' && *(p + 1) == '[') {
      const char *alt_end = strchr(p + 2, ']');
      if (alt_end && *(alt_end + 1) == '(') {
        const char *url_start = alt_end + 2;
        const char *url_end = strchr(url_start, ')');
        if (url_end) {
          return g_strndup(url_start, url_end - url_start);
        }
      }
    }
    p++;
  }

  return NULL;
}

char *markdown_strip_to_plain(const char *markdown, gsize max_length) {
  if (!markdown || !*markdown) {
    return g_strdup("");
  }

  GString *out = g_string_sized_new(max_length > 0 ? max_length : strlen(markdown));
  const char *p = markdown;
  gboolean prev_space = FALSE;
  gboolean in_code = FALSE;

  while (*p) {
    /* Code fence toggle */
    if (strncmp(p, "```", 3) == 0) {
      in_code = !in_code;
      p += 3;
      continue;
    }

    if (in_code) {
      if (*p == '\n') {
        if (!prev_space) {
          g_string_append_c(out, ' ');
          prev_space = TRUE;
        }
      } else if (!g_ascii_isspace(*p)) {
        g_string_append_c(out, *p);
        prev_space = FALSE;
      }
      p++;
      continue;
    }

    /* Skip markdown characters */
    if (*p == '#' || *p == '*' || *p == '_' || *p == '`' || *p == '>' || *p == '[' || *p == ']' || *p == '(' || *p == ')' || *p == '!') {
      p++;
      continue;
    }

    /* Collapse whitespace */
    if (g_ascii_isspace(*p)) {
      if (!prev_space) {
        g_string_append_c(out, ' ');
        prev_space = TRUE;
      }
    } else {
      g_string_append_c(out, *p);
      prev_space = FALSE;
    }
    p++;

    if (max_length > 0 && out->len >= max_length) {
      break;
    }
  }

  if (max_length > 0 && out->len >= max_length) {
    g_string_append(out, "...");
  }

  return g_string_free(out, FALSE);
}
