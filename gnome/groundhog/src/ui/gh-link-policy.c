#include "gh-link-policy.h"
#include "gh-diagnostics.h"
#include "gh-display-name.h"

#include <nostr/nip19/nip19.h>
#include <string.h>

/* NIP-19 entities a nostr: URI may name (NIP-21). nsec is deliberately not
 * one: a secret key must never become something to click. */
static const gchar *const nostr_entities[] = { "npub1", "nprofile1", "note1", "nevent1",
                                               "naddr1" };
static const gchar bech32_charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
/* Shorter bech32 data is not a plausible entity (npub data is 58). */
#define MIN_BECH32_DATA 6
/* Characters that end a link when they are its last one ("see https://a.b."). */
static const gchar trailing_punctuation[] = ".,:;!?'*";

void
gh_link_free(GhLink *link)
{
  if (!link)
    return;
  g_free(link->uri);
  g_free(link);
}

static gboolean
has_prefix_ci(const gchar *p, gsize len, const gchar *prefix)
{
  gsize n = strlen(prefix);
  return len >= n && g_ascii_strncasecmp(p, prefix, n) == 0;
}

/* A link starts only at the start of a word: "xhttps://…" and
 * "a.nostr:npub1…" are not links. */
static gboolean
at_word_start(const gchar *text, const gchar *p)
{
  if (p == text)
    return TRUE;
  gunichar prev = g_utf8_get_char(g_utf8_prev_char(p));
  return !g_unichar_isalnum(prev) && !strchr("_-.+/:@&=%~", (int)(prev < 0x80 ? prev : 'a'));
}

/* Characters a web address may contain as written in a message. Spaces,
 * controls, format characters (bidi overrides, zero-width marks), quotes,
 * brackets that are not ASCII and the characters RFC 3986 excludes end it. */
static gboolean
is_url_char(gunichar c)
{
  if (c < 0x80)
    return g_ascii_isgraph((gchar)c) && !strchr("<>\"`{}|\\^", (int)c);
  if (!g_unichar_isgraph(c))
    return FALSE;
  switch (g_unichar_type(c)) {
  case G_UNICODE_OPEN_PUNCTUATION:
  case G_UNICODE_CLOSE_PUNCTUATION:
  case G_UNICODE_INITIAL_PUNCTUATION:
  case G_UNICODE_FINAL_PUNCTUATION:
  case G_UNICODE_FORMAT:
  case G_UNICODE_PRIVATE_USE:
  case G_UNICODE_UNASSIGNED:
  case G_UNICODE_SURROGATE:
    return FALSE;
  default:
    return TRUE;
  }
}

static guint
count_char(const gchar *start, const gchar *end, gchar c)
{
  guint n = 0;
  for (const gchar *p = start; p < end; p++)
    n += *p == c;
  return n;
}

/* ---- web addresses ------------------------------------------------------------ */

typedef struct {
  gboolean secure;
  gchar *user_info;  /* as written, or NULL */
  gchar *ascii_host; /* lowercase ASCII (punycode for IDN) */
  gboolean ipv6;
  gchar *port;       /* digits, or NULL */
  gchar *rest;       /* path, query and fragment as written */
  gboolean idn;
} WebAddress;

static void
web_address_clear(WebAddress *web)
{
  g_free(web->user_info);
  g_free(web->ascii_host);
  g_free(web->port);
  g_free(web->rest);
  memset(web, 0, sizeof *web);
}

static gboolean
valid_ascii_host(const gchar *host)
{
  if (g_hostname_is_ip_address(host))
    return TRUE;
  gsize len = strlen(host);
  if (len == 0 || len > 253)
    return FALSE;
  g_auto(GStrv) labels = g_strsplit(host, ".", -1);
  guint n = g_strv_length(labels);
  for (guint i = 0; i < n; i++) {
    const gchar *label = labels[i];
    gsize label_len = strlen(label);
    /* One trailing dot ("example.com.") is a valid absolute name. */
    if (label_len == 0 && i == n - 1 && n > 1)
      continue;
    if (label_len == 0 || label_len > 63 || label[0] == '-' || label[label_len - 1] == '-')
      return FALSE;
    for (const gchar *c = label; *c; c++)
      if (!g_ascii_isalnum(*c) && *c != '-')
        return FALSE;
  }
  return TRUE;
}

/* Parses the http(s) address uri[0..len). web may be NULL to only validate. */
static gboolean
parse_web(const gchar *uri, gsize len, WebAddress *web)
{
  WebAddress parsed = { 0 };
  gsize scheme_len;
  if (has_prefix_ci(uri, len, "https://")) {
    parsed.secure = TRUE;
    scheme_len = 8;
  } else if (has_prefix_ci(uri, len, "http://")) {
    scheme_len = 7;
  } else {
    return FALSE;
  }
  const gchar *authority = uri + scheme_len;
  const gchar *end = uri + len;
  const gchar *authority_end = authority;
  while (authority_end < end && !strchr("/?#", *authority_end))
    authority_end++;
  if (authority_end == authority)
    return FALSE;

  const gchar *host_start = authority;
  for (const gchar *p = authority; p < authority_end; p++)
    if (*p == '@')
      host_start = p + 1;
  if (host_start > authority)
    parsed.user_info = g_strndup(authority, host_start - 1 - authority);

  gboolean ok = FALSE;
  g_autofree gchar *host = NULL;
  const gchar *port_start = NULL;
  if (host_start < authority_end && *host_start == '[') {
    const gchar *close = memchr(host_start, ']', authority_end - host_start);
    if (!close)
      goto out;
    host = g_strndup(host_start + 1, close - host_start - 1);
    if (!g_hostname_is_ip_address(host) || !strchr(host, ':'))
      goto out;
    parsed.ipv6 = TRUE;
    if (close + 1 < authority_end) {
      if (close[1] != ':')
        goto out;
      port_start = close + 2;
    }
  } else {
    const gchar *colon = NULL;
    for (const gchar *p = host_start; p < authority_end; p++)
      if (*p == ':')
        colon = p;
    const gchar *host_end = colon ? colon : authority_end;
    host = g_strndup(host_start, host_end - host_start);
    if (colon)
      port_start = colon + 1;
  }
  if (port_start) {
    gsize digits = authority_end - port_start;
    if (digits == 0 || digits > 5)
      goto out;
    for (const gchar *p = port_start; p < authority_end; p++)
      if (!g_ascii_isdigit(*p))
        goto out;
    parsed.port = g_strndup(port_start, digits);
  }
  if (!*host || !g_utf8_validate(host, -1, NULL))
    goto out;

  if (parsed.ipv6) {
    parsed.ascii_host = g_ascii_strdown(host, -1);
  } else {
    g_autofree gchar *ascii = g_hostname_to_ascii(host);
    if (!ascii)
      goto out;
    parsed.ascii_host = g_ascii_strdown(ascii, -1);
    if (!valid_ascii_host(parsed.ascii_host))
      goto out;
    parsed.idn = g_hostname_is_non_ascii(host);
    g_auto(GStrv) labels = g_strsplit(parsed.ascii_host, ".", -1);
    for (guint i = 0; labels[i] && !parsed.idn; i++)
      parsed.idn = g_str_has_prefix(labels[i], "xn--");
  }
  parsed.rest = g_strndup(authority_end, end - authority_end);
  ok = TRUE;

out:
  if (ok && web)
    *web = parsed;
  else
    web_address_clear(&parsed);
  return ok;
}

/* The length of the web link that starts at p (at an http:// or https://
 * prefix), trailing punctuation excluded, or 0 when it is not a link. */
static gsize
web_link_length(const gchar *p)
{
  gsize scheme_len = has_prefix_ci(p, strlen("https://"), "https://") ? 8 : 7;
  const gchar *end = p + scheme_len;
  while (*end) {
    gunichar c = g_utf8_get_char(end);
    if (!is_url_char(c))
      break;
    end = g_utf8_next_char(end);
  }
  while (end > p + scheme_len) {
    gchar last = end[-1];
    if (strchr(trailing_punctuation, last) && last != '\0')
      end--;
    else if (last == ')' && count_char(p, end, '(') < count_char(p, end, ')'))
      end--;
    else if (last == ']' && count_char(p, end, '[') < count_char(p, end, ']'))
      end--;
    else
      break;
  }
  return parse_web(p, end - p, NULL) ? (gsize)(end - p) : 0;
}

/* ---- nostr: URIs -------------------------------------------------------------- */

/* The length of the nostr: URI that starts at p, or 0. */
static gsize
nostr_link_length(const gchar *p)
{
  const gchar *q = p + strlen("nostr:");
  for (guint i = 0; i < G_N_ELEMENTS(nostr_entities); i++) {
    gsize hrp = strlen(nostr_entities[i]);
    if (strncmp(q, nostr_entities[i], hrp) != 0)
      continue;
    const gchar *end = q + hrp;
    while (*end && strchr(bech32_charset, *end))
      end++;
    if ((gsize)(end - q) - hrp < MIN_BECH32_DATA)
      return 0;
    /* The entity must end the word: "nostr:npub1…xyzB" is not one. */
    gunichar next = g_utf8_get_char(end);
    if (*end && (g_unichar_isalnum(next) || next == '_'))
      return 0;
    return end - p;
  }
  return 0;
}

/* ---- public ------------------------------------------------------------------- */

static gsize
link_at(const gchar *p, GhLinkKind *kind)
{
  gsize available = strnlen(p, 8);
  if (has_prefix_ci(p, available, "https://") || has_prefix_ci(p, available, "http://")) {
    *kind = GH_LINK_KIND_WEB;
    return web_link_length(p);
  }
  if (has_prefix_ci(p, available, "nostr:")) {
    *kind = GH_LINK_KIND_NOSTR;
    return nostr_link_length(p);
  }
  return 0;
}

GPtrArray *
gh_link_policy_find_links(const gchar *text)
{
  GPtrArray *links = g_ptr_array_new_with_free_func((GDestroyNotify)gh_link_free);
  if (!text || !g_utf8_validate(text, -1, NULL))
    return links;
  for (const gchar *p = text; *p;) {
    GhLinkKind kind = GH_LINK_KIND_WEB;
    gsize length = at_word_start(text, p) ? link_at(p, &kind) : 0;
    if (length == 0) {
      p = g_utf8_next_char(p);
      continue;
    }
    GhLink *link = g_new0(GhLink, 1);
    link->start = p - text;
    link->end = link->start + length;
    link->kind = kind;
    link->uri = g_strndup(p, length);
    g_ptr_array_add(links, link);
    p += length;
  }
  return links;
}

static void
append_escaped(GString *out, const gchar *text, gsize length)
{
  g_autofree gchar *part = g_strndup(text, length);
  g_autofree gchar *escaped = g_markup_escape_text(part, -1);
  g_string_append(out, escaped);
}

gchar *
gh_link_policy_to_markup(const gchar *text)
{
  if (!text)
    return g_strdup("");
  if (!g_utf8_validate(text, -1, NULL)) {
    gh_diagnostics_record_default(GH_DIAGNOSTIC_COMPONENT_UI,
                                  GH_DIAGNOSTIC_EVENT_RENDER_FALLBACK,
                                  GH_DIAGNOSTIC_RESULT_FAILED);
    g_autofree gchar *valid = g_utf8_make_valid(text, -1);
    return g_markup_escape_text(valid, -1);
  }
  g_autoptr(GPtrArray) links = gh_link_policy_find_links(text);
  GString *out = g_string_sized_new(strlen(text) + 16);
  gsize position = 0;
  for (guint i = 0; i < links->len; i++) {
    GhLink *link = g_ptr_array_index(links, i);
    append_escaped(out, text + position, link->start - position);
    g_autofree gchar *uri = g_markup_escape_text(link->uri, -1);
    g_string_append_printf(out, "<a href=\"%s\">%s</a>", uri, uri);
    position = link->end;
  }
  append_escaped(out, text + position, strlen(text + position));
  return g_string_free(out, FALSE);
}

gchar *
gh_link_policy_to_mention_markup(const gchar *text, const gchar *account_pubkey)
{
  if (!text || !g_utf8_validate(text, -1, NULL))
    return gh_link_policy_to_markup(text);
  g_autoptr(GPtrArray) links = gh_link_policy_find_links(text);
  GString *out = g_string_sized_new(strlen(text) + 16);
  gsize position = 0;
  for (guint i = 0; i < links->len; i++) {
    GhLink *link = g_ptr_array_index(links, i);
    append_escaped(out, text + position, link->start - position);
    g_autofree gchar *uri = g_markup_escape_text(link->uri, -1);
    guint8 key[32];
    if (link->kind == GH_LINK_KIND_NOSTR &&
        g_str_has_prefix(link->uri, "nostr:npub1") &&
        nostr_nip19_decode_npub(link->uri + strlen("nostr:"), key) == 0) {
      gchar hex[65];
      for (guint j = 0; j < sizeof key; j++)
        g_snprintf(hex + 2 * j, sizeof hex - 2 * j, "%02x", key[j]);
      g_autofree gchar *name = gh_display_name_for(hex);
      g_autofree gchar *escaped = g_markup_escape_text(name, -1);
      gboolean own = g_strcmp0(hex, account_pubkey) == 0;
      g_string_append_printf(out, own ? "<a href=\"%s\"><b>@%s</b></a>"
                                      : "<a href=\"%s\">@%s</a>", uri, escaped);
    } else {
      g_string_append_printf(out, "<a href=\"%s\">%s</a>", uri, uri);
    }
    position = link->end;
  }
  append_escaped(out, text + position, strlen(text + position));
  return g_string_free(out, FALSE);
}

static void
append_markdown_inline(GString *out, const gchar *text, guint style,
                       const gchar *account_pubkey, gboolean linkify)
{
  g_autofree gchar *inner = linkify
    ? gh_link_policy_to_mention_markup(text, account_pubkey)
    : g_markup_escape_text(text ? text : "", -1);
  if (style & GN_MARKDOWN_STYLE_STRONG)
    g_string_append(out, "<b>");
  if (style & GN_MARKDOWN_STYLE_EMPHASIS)
    g_string_append(out, "<i>");
  g_string_append(out, inner);
  if (style & GN_MARKDOWN_STYLE_EMPHASIS)
    g_string_append(out, "</i>");
  if (style & GN_MARKDOWN_STYLE_STRONG)
    g_string_append(out, "</b>");
}

gchar *
gh_link_policy_format_markdown(const GnMarkdownDocument *document,
                                       const gchar *account_pubkey)
{
  if (!document || !document->tokens)
    return g_strdup("");
  GString *out = g_string_sized_new(document->source ? strlen(document->source) + 64 : 64);
  for (guint i = 0; i < document->tokens->len; i++) {
    const GnMarkdownToken *token = g_ptr_array_index(document->tokens, i);
    const gchar *value = token->text ? token->text : "";
    switch (token->kind) {
    case GN_MARKDOWN_TEXT:
      append_markdown_inline(out, value, token->style, account_pubkey, TRUE);
      break;
    case GN_MARKDOWN_CODE:
      g_string_append(out, "<tt>");
      append_markdown_inline(out, value, 0, account_pubkey, FALSE);
      g_string_append(out, "</tt>");
      break;
    case GN_MARKDOWN_LINK:
      /* A label can say anything, so it must never disguise the destination. */
      append_markdown_inline(out, value, token->style, account_pubkey, FALSE);
      g_string_append(out, " (");
      append_markdown_inline(out, token->target, 0, account_pubkey, TRUE);
      g_string_append_c(out, ')');
      break;
    case GN_MARKDOWN_RAW_URL:
    case GN_MARKDOWN_NOSTR_REFERENCE:
      append_markdown_inline(out, value, token->style, account_pubkey, TRUE);
      break;
    case GN_MARKDOWN_HEADING:
      g_string_append(out, "<b>");
      append_markdown_inline(out, value, 0, account_pubkey, TRUE);
      g_string_append(out, "</b>");
      break;
    case GN_MARKDOWN_LIST_ITEM:
      g_string_append(out, token->ordered ? "1. " : "• ");
      append_markdown_inline(out, value, 0, account_pubkey, TRUE);
      break;
    case GN_MARKDOWN_QUOTE:
      g_string_append(out, "│ ");
      append_markdown_inline(out, value, 0, account_pubkey, TRUE);
      break;
    case GN_MARKDOWN_SEPARATOR:
      g_string_append(out, "────────");
      break;
    case GN_MARKDOWN_LINE_BREAK:
      g_string_append_c(out, '\n');
      break;
    }
  }
  return g_string_free(out, FALSE);
}

gchar *
gh_link_policy_to_markdown_markup(const gchar *text, const gchar *account_pubkey)
{
  g_autoptr(GnMarkdownDocument) document = gn_markdown_parse(text, -1);
  if (document->truncated) {
    g_autofree gchar *valid = g_utf8_make_valid(text ? text : "", -1);
    return g_markup_escape_text(valid, -1);
  }
  return gh_link_policy_format_markdown(document, account_pubkey);
}

/* Parses uri as exactly one web link, as the scanner would have found it. */
static gboolean
parse_whole_web_link(const gchar *uri, WebAddress *web)
{
  gsize len = strlen(uri);
  GhLinkKind kind = GH_LINK_KIND_NOSTR;
  return link_at(uri, &kind) == len && kind == GH_LINK_KIND_WEB && parse_web(uri, len, web);
}

static gchar *
escape_non_ascii(const gchar *text)
{
  /* Keeps every ASCII character valid in a URI, and existing %XX escapes. */
  return g_uri_escape_string(text, "!#$%&'()*+,/:;=?@[]~", FALSE);
}

GhLinkAction
gh_link_policy_classify(const gchar *uri, GhLinkConfirmReasons *reasons, gchar **open_uri)
{
  if (reasons)
    *reasons = GH_LINK_CONFIRM_NONE;
  if (open_uri)
    *open_uri = NULL;
  if (!uri || !*uri || !g_utf8_validate(uri, -1, NULL))
    return GH_LINK_ACTION_REFUSE;

  GhLinkKind kind = GH_LINK_KIND_WEB;
  if (link_at(uri, &kind) == strlen(uri) && kind == GH_LINK_KIND_NOSTR)
    return GH_LINK_ACTION_NOSTR;

  WebAddress web = { 0 };
  if (!parse_whole_web_link(uri, &web))
    return GH_LINK_ACTION_REFUSE;
  GhLinkConfirmReasons why = GH_LINK_CONFIRM_NONE;
  if (!web.secure)
    why |= GH_LINK_CONFIRM_INSECURE;
  if (web.idn)
    why |= GH_LINK_CONFIRM_IDN;
  if (web.user_info)
    why |= GH_LINK_CONFIRM_USER_INFO;
  if (open_uri) {
    GString *out = g_string_new(web.secure ? "https://" : "http://");
    if (web.user_info) {
      g_autofree gchar *user_info = escape_non_ascii(web.user_info);
      g_string_append_printf(out, "%s@", user_info);
    }
    g_string_append_printf(out, web.ipv6 ? "[%s]" : "%s", web.ascii_host);
    if (web.port)
      g_string_append_printf(out, ":%s", web.port);
    g_autofree gchar *rest = escape_non_ascii(web.rest);
    g_string_append(out, rest);
    *open_uri = g_string_free(out, FALSE);
  }
  if (reasons)
    *reasons = why;
  web_address_clear(&web);
  return why ? GH_LINK_ACTION_CONFIRM : GH_LINK_ACTION_OPEN;
}

gchar *
gh_link_policy_dup_host(const gchar *uri)
{
  WebAddress web = { 0 };
  if (!uri || !g_utf8_validate(uri, -1, NULL) || !parse_whole_web_link(uri, &web))
    return NULL;
  gchar *host = g_steal_pointer(&web.ascii_host);
  web_address_clear(&web);
  return host;
}

gboolean
gh_link_policy_can_preview(const gchar *uri)
{
  WebAddress web = { 0 };
  if (!uri || !g_utf8_validate(uri, -1, NULL) || !parse_whole_web_link(uri, &web))
    return FALSE;
  gboolean ok = web.secure && !web.user_info;
  web_address_clear(&web);
  return ok;
}

gchar *
gh_link_policy_dup_preview_uri(const gchar *text)
{
  g_autoptr(GPtrArray) links = gh_link_policy_find_links(text);
  for (guint i = 0; i < links->len; i++) {
    GhLink *link = g_ptr_array_index(links, i);
    if (link->kind == GH_LINK_KIND_WEB && gh_link_policy_can_preview(link->uri))
      return g_strdup(link->uri);
  }
  return NULL;
}
