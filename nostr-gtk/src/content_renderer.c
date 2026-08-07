/**
 * Content Renderer - NDB content block-based note rendering
 *
 * Uses nostrdb's pre-parsed content blocks instead of manual whitespace
 * tokenization. This correctly handles hashtags, mentions, URLs, and
 * invoices even when not separated by whitespace.
 */

#include "content_renderer.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

static gint parser_invocation_count;

void
gn_content_parser_reset_invocation_count(void)
{
  g_atomic_int_set(&parser_invocation_count, 0);
}

guint
gn_content_parser_get_invocation_count(void)
{
  return (guint)g_atomic_int_get(&parser_invocation_count);
}

/* nostrdb headers (with diagnostic suppression for zero-length arrays) */
#if defined(__clang__)
#  pragma clang diagnostic push
#  pragma clang diagnostic ignored "-Wpedantic"
#  pragma clang diagnostic ignored "-Wzero-length-array"
#elif defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wpedantic"
#endif
#include "nostrdb.h"
#include "block.h"
#if defined(__clang__)
#  pragma clang diagnostic pop
#elif defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif

/* Convert 32-byte binary to hex string (65-byte buffer including NUL) */
static void bytes_to_hex_str(const unsigned char *bytes, char *out) {
  for (int i = 0; i < 32; i++) {
    snprintf(out + i * 2, 3, "%02x", bytes[i]);
  }
  out[64] = '\0';
}

/* Check if URL has a media file extension */
static gboolean url_has_extension(const char *u, gsize u_len,
                                  const char *exts[], guint n_exts) {
  if (!u || u_len == 0) return FALSE;

  /* Find end of path component (before ? or #) */
  gsize path_len = u_len;
  for (gsize i = 0; i < u_len; i++) {
    if (u[i] == '?' || u[i] == '#') {
      path_len = i;
      break;
    }
  }

  gchar *path = g_ascii_strdown(u, path_len);
  gboolean result = FALSE;
  for (guint i = 0; i < n_exts; i++) {
    if (g_str_has_suffix(path, exts[i])) {
      result = TRUE;
      break;
    }
  }
  g_free(path);
  return result;
}

static gboolean is_image_url_n(const char *u, gsize len) {
  static const char *exts[] = {
    ".jpg", ".jpeg", ".png", ".gif", ".webp", ".bmp", ".svg",
    ".avif", ".ico", ".tiff", ".tif", ".heic", ".heif"
  };
  return url_has_extension(u, len, exts, G_N_ELEMENTS(exts));
}

static gboolean is_video_url_n(const char *u, gsize len) {
  static const char *exts[] = {".mp4", ".webm", ".mov", ".avi", ".mkv", ".m4v"};
  return url_has_extension(u, len, exts, G_N_ELEMENTS(exts));
}

/**
 * Format a bech32 mention for display.
 * Profile mentions: @display_name or truncated bech32
 * Event mentions: truncated bech32
 */
static gchar *format_mention_display(struct nostr_bech32 *bech32,
                                     const char *bech32_str, uint32_t str_len) {
  if (!bech32) {
    return g_strndup(bech32_str, str_len);
  }

  switch (bech32->type) {
    case NOSTR_BECH32_NPUB:
    case NOSTR_BECH32_NPROFILE: {
      /* Try to resolve pubkey to display name */
      const unsigned char *pk = (bech32->type == NOSTR_BECH32_NPUB)
                                ? bech32->npub.pubkey
                                : bech32->nprofile.pubkey;
      if (pk) {
        char hex[65];
        bytes_to_hex_str(pk, hex);
        GnostrProfileMeta *meta = gnostr_profile_provider_get(hex);
        if (meta) {
          const char *name = NULL;
          if (meta->display_name && meta->display_name[0])
            name = meta->display_name;
          else if (meta->name && meta->name[0])
            name = meta->name;
          else if (meta->nip05 && meta->nip05[0])
            name = meta->nip05;

          if (name) {
            gchar *result = g_strdup_printf("@%s", name);
            gnostr_profile_meta_free(meta);
            return result;
          }
          gnostr_profile_meta_free(meta);
        }
      }
      /* Fallback: truncated bech32 */
      if (str_len > 16) {
        return g_strdup_printf("@%.*s\xe2\x80\xa6%.*s", 8, bech32_str,
                               4, bech32_str + str_len - 4);
      }
      return g_strdup_printf("@%.*s", (int)str_len, bech32_str);
    }

    case NOSTR_BECH32_NOTE:
    case NOSTR_BECH32_NEVENT:
    case NOSTR_BECH32_NADDR: {
      /* Event mention: show with note emoji + truncated bech32 */
      if (str_len > 17) {
        return g_strdup_printf("\xf0\x9f\x93\x9d%.*s\xe2\x80\xa6%.*s",
                               9, bech32_str, 4, bech32_str + str_len - 4);
      }
      return g_strdup_printf("\xf0\x9f\x93\x9d%.*s", (int)str_len, bech32_str);
    }

    default:
      return g_strndup(bech32_str, str_len);
  }
}

/* nostrc-csaf: Validate and sanitize UTF-8 strings from untrusted relay content.
 * Replaces invalid byte sequences with U+FFFD and strips dangerous zero-width
 * characters. Every string from a relay should pass through this before reaching
 * Pango or GTK label functions. */
char *
gnostr_sanitize_utf8(const char *str)
{
  if (!str) return g_strdup("");
  if (!*str) return g_strdup("");

  /* Step 1: Ensure valid UTF-8 */
  gchar *valid;
  if (g_utf8_validate(str, -1, NULL)) {
    valid = g_strdup(str);
  } else {
    valid = g_utf8_make_valid(str, -1);
  }

  /* Step 2: Strip dangerous zero-width characters */
  gnostr_strip_zwsp(valid);

  return valid;
}

/* nostrc-pgo5/pgo6: Strip zero-width and invisible Unicode characters that
 * corrupt Pango's internal layout line list (NULL entries), causing SEGV in
 * pango_layout_line_unref during gtk_widget_allocate or dispose.
 *
 * Stripped characters (UTF-8 byte sequences):
 *   U+200B  ZWS   (Zero Width Space)       = \xe2\x80\x8b
 *   U+200C  ZWNJ  (Zero Width Non-Joiner)  = \xe2\x80\x8c
 *   U+2060  WJ    (Word Joiner)            = \xe2\x81\xa0
 *   U+FEFF  BOM   (Byte Order Mark / ZWNBSP) = \xef\xbb\xbf
 *
 * NOT stripped: U+200D (ZWJ) — used in emoji sequences (family, flags).
 * Relay events can contain any of these in their text. */
char *
gnostr_strip_zwsp(char *str)
{
  if (!str) return str;
  char *src = str, *dst = str;
  while (*src) {
    unsigned char c0 = (unsigned char)src[0];
    if (c0 == 0xe2) {
      unsigned char c1 = (unsigned char)src[1];
      unsigned char c2 = (unsigned char)src[2];
      /* U+200B (ZWS) = e2 80 8b, U+200C (ZWNJ) = e2 80 8c */
      if (c1 == 0x80 && (c2 == 0x8b || c2 == 0x8c)) {
        src += 3;
        continue;
      }
      /* U+2060 (WJ) = e2 81 a0 */
      if (c1 == 0x81 && c2 == 0xa0) {
        src += 3;
        continue;
      }
    } else if (c0 == 0xef) {
      /* U+FEFF (BOM) = ef bb bf */
      if ((unsigned char)src[1] == 0xbb && (unsigned char)src[2] == 0xbf) {
        src += 3;
        continue;
      }
    }
    *dst++ = *src++;
  }
  *dst = '\0';
  return str;
}

/* Check if a URL is http(s) */
static gboolean is_http_url_n(const char *u, uint32_t len) {
  if (len < 8) return FALSE;
  return (g_ascii_strncasecmp(u, "http://", 7) == 0 ||
          g_ascii_strncasecmp(u, "https://", 8) == 0);
}

void gn_content_descriptor_free(GnContentDescriptor *descriptor) {
  if (!descriptor) return;
  g_free(descriptor->url);
  g_free(descriptor->original);
  g_free(descriptor->id);
  g_free(descriptor->pubkey);
  g_strfreev(descriptor->relay_hints);
  g_free(descriptor->thumbnail_url);
  g_free(descriptor);
}

void gnostr_content_render_result_free(GnContentRenderResult *result) {
  if (!result) return;
  g_free(result->markup);
  g_free(result->plain_text);
  if (result->descriptors) g_ptr_array_unref(result->descriptors);
  if (result->media_urls) g_ptr_array_unref(result->media_urls);
  if (result->all_urls) g_ptr_array_unref(result->all_urls);
  g_free(result->first_nostr_ref);
  g_free(result->first_og_url);
  g_free(result);
}

typedef struct {
  gchar *mime_type;
  gint width;
  gint height;
  gchar *thumbnail_url;
} ImetaEnrichment;

static void imeta_enrichment_free(ImetaEnrichment *imeta) {
  if (!imeta) return;
  g_free(imeta->mime_type);
  g_free(imeta->thumbnail_url);
  g_free(imeta);
}

static const gchar *json_array_string_at(JsonArray *array, guint index) {
  JsonNode *node = json_array_get_element(array, index);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  return json_node_get_string(node);
}

static void parse_imeta_field(ImetaEnrichment *imeta,
                              gchar **out_url,
                              const gchar *field) {
  if (!imeta || !field) return;

  const gchar *space = strchr(field, ' ');
  if (!space || !space[1]) return;

  gsize key_len = (gsize)(space - field);
  const gchar *value = space + 1;

  if (key_len == 3 && strncmp(field, "url", 3) == 0) {
    g_free(*out_url);
    *out_url = g_strdup(value);
  } else if (key_len == 1 && field[0] == 'm') {
    g_free(imeta->mime_type);
    imeta->mime_type = g_strdup(value);
  } else if (key_len == 3 && strncmp(field, "dim", 3) == 0) {
    gint width = 0;
    gint height = 0;
    if (sscanf(value, "%dx%d", &width, &height) == 2 &&
        width > 0 && height > 0) {
      imeta->width = width;
      imeta->height = height;
    }
  } else if (key_len == 5 && strncmp(field, "thumb", 5) == 0) {
    g_free(imeta->thumbnail_url);
    imeta->thumbnail_url = g_strdup(value);
  } else if (key_len == 5 && strncmp(field, "image", 5) == 0 &&
             !imeta->thumbnail_url) {
    imeta->thumbnail_url = g_strdup(value);
  }
}

static GHashTable *parse_imeta_enrichments(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) error = NULL;
  if (!json_parser_load_from_data(parser, tags_json, -1, &error))
    return NULL;

  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_ARRAY(root))
    return NULL;

  GHashTable *by_url =
    g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                          (GDestroyNotify)imeta_enrichment_free);
  JsonArray *tags = json_node_get_array(root);
  guint tag_count = json_array_get_length(tags);

  for (guint i = 0; i < tag_count; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!tag_node || !JSON_NODE_HOLDS_ARRAY(tag_node))
      continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint fields = json_array_get_length(tag);
    const gchar *name = fields > 0 ? json_array_string_at(tag, 0) : NULL;
    if (g_strcmp0(name, "imeta") != 0)
      continue;

    ImetaEnrichment *imeta = g_new0(ImetaEnrichment, 1);
    g_autofree gchar *url = NULL;
    for (guint j = 1; j < fields; j++) {
      const gchar *field = json_array_string_at(tag, j);
      if (field)
        parse_imeta_field(imeta, &url, field);
    }

    if (url && *url)
      g_hash_table_replace(by_url, g_steal_pointer(&url), imeta);
    else
      imeta_enrichment_free(imeta);
  }

  if (g_hash_table_size(by_url) == 0) {
    g_hash_table_unref(by_url);
    return NULL;
  }
  return by_url;
}

static GnContentDescriptorType classify_url(const char *url,
                                            gsize len,
                                            const ImetaEnrichment *imeta) {
  if (imeta && imeta->mime_type) {
    if (g_ascii_strncasecmp(imeta->mime_type, "image/", 6) == 0)
      return GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE;
    if (g_ascii_strncasecmp(imeta->mime_type, "video/", 6) == 0)
      return GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO;
  }
  if (is_image_url_n(url, len))
    return GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE;
  if (is_video_url_n(url, len))
    return GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO;
  return GN_CONTENT_DESCRIPTOR_LINK_PREVIEW;
}

static GnContentDescriptor *descriptor_for_url(const gchar *url,
                                               GnContentDescriptorType type,
                                               const ImetaEnrichment *imeta) {
  GnContentDescriptor *descriptor = g_new0(GnContentDescriptor, 1);
  descriptor->type = type;
  descriptor->url = g_strdup(url);
  if (imeta) {
    descriptor->width = imeta->width;
    descriptor->height = imeta->height;
    descriptor->thumbnail_url = g_strdup(imeta->thumbnail_url);
  }
  return descriptor;
}

static gchar *bytes_to_hex_dup(const unsigned char *bytes) {
  if (!bytes) return NULL;
  gchar *hex = g_malloc(65);
  bytes_to_hex_str(bytes, hex);
  return hex;
}

static gchar **copy_relay_hints(const struct ndb_relays *relays) {
  if (!relays || relays->num_relays <= 0)
    return NULL;

  guint count = MIN((guint)relays->num_relays, (guint)NDB_MAX_RELAYS);
  gchar **hints = g_new0(gchar *, count + 1);
  for (guint i = 0; i < count; i++) {
    const struct ndb_str_block *relay = &relays->relays[i];
    if (relay->str && relay->len > 0)
      hints[i] = g_strndup(relay->str, relay->len);
    else
      hints[i] = g_strdup("");
  }
  return hints;
}

static GnContentDescriptor *descriptor_for_bech32(struct nostr_bech32 *bech32,
                                                  const gchar *original) {
  if (!bech32) return NULL;

  GnContentDescriptor *descriptor = g_new0(GnContentDescriptor, 1);
  descriptor->original = g_strdup(original);

  switch (bech32->type) {
    case NOSTR_BECH32_NOTE:
      descriptor->type = GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF;
      descriptor->id = bytes_to_hex_dup(bech32->note.event_id);
      break;
    case NOSTR_BECH32_NEVENT:
      descriptor->type = GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF;
      descriptor->id = bytes_to_hex_dup(bech32->nevent.event_id);
      descriptor->pubkey = bytes_to_hex_dup(bech32->nevent.pubkey);
      descriptor->relay_hints = copy_relay_hints(&bech32->nevent.relays);
      break;
    case NOSTR_BECH32_NADDR:
      descriptor->type = GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF;
      if (bech32->naddr.identifier.str && bech32->naddr.identifier.len > 0)
        descriptor->id = g_strndup(bech32->naddr.identifier.str,
                                   bech32->naddr.identifier.len);
      descriptor->pubkey = bytes_to_hex_dup(bech32->naddr.pubkey);
      descriptor->relay_hints = copy_relay_hints(&bech32->naddr.relays);
      break;
    case NOSTR_BECH32_NPUB:
      descriptor->type = GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF;
      descriptor->pubkey = bytes_to_hex_dup(bech32->npub.pubkey);
      break;
    case NOSTR_BECH32_NPROFILE:
      descriptor->type = GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF;
      descriptor->pubkey = bytes_to_hex_dup(bech32->nprofile.pubkey);
      descriptor->relay_hints = copy_relay_hints(&bech32->nprofile.relays);
      break;
    default:
      gn_content_descriptor_free(descriptor);
      return NULL;
  }

  return descriptor;
}

static void append_display_text(GString *markup,
                                GString *plain_text,
                                const char *text,
                                gssize len) {
  if (!text || len == 0) return;
  gchar *escaped = g_markup_escape_text(text, len);
  g_string_append(markup, escaped);
  g_free(escaped);
  if (len < 0)
    g_string_append(plain_text, text);
  else
    g_string_append_len(plain_text, text, len);
}

static void append_profile_mention(GString *markup,
                                   GString *plain_text,
                                   const char *href,
                                   const char *display) {
  if (!href || !*href || !display) return;

  g_autofree gchar *escaped_href = g_markup_escape_text(href, -1);
  g_autofree gchar *escaped_display = g_markup_escape_text(display, -1);
  g_string_append_printf(markup, "<a href=\"%s\">%s</a>",
                         escaped_href, escaped_display);
  g_string_append(plain_text, display);
}

/* Persisted block offsets address the exact stored content bytes, not a
 * sanitized copy. Sanitize only after slicing so removed/replaced UTF-8 bytes
 * cannot shift later offsets or split a multi-byte sequence by remapping. */
static gchar *sanitize_block_slice(const char *text, gsize len) {
  g_autofree gchar *slice = g_strndup(text, len);
  return gnostr_sanitize_utf8(slice);
}

static void append_sanitized_block_slice(GString *markup,
                                         GString *plain_text,
                                         const char *text,
                                         gsize len) {
  if (!text || len == 0) return;
  g_autofree gchar *safe = sanitize_block_slice(text, len);
  append_display_text(markup, plain_text, safe, -1);
}

static void populate_legacy_fields(GnContentRenderResult *result) {
  if (!result || !result->descriptors) return;

  for (guint i = 0; i < result->descriptors->len; i++) {
    GnContentDescriptor *descriptor =
      g_ptr_array_index(result->descriptors, i);
    if (!descriptor) continue;

    switch (descriptor->type) {
      case GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE:
      case GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO:
        if (!result->media_urls)
          result->media_urls = g_ptr_array_new_with_free_func(g_free);
        if (!result->all_urls)
          result->all_urls = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(result->media_urls, g_strdup(descriptor->url));
        g_ptr_array_add(result->all_urls, g_strdup(descriptor->url));
        break;
      case GN_CONTENT_DESCRIPTOR_LINK_PREVIEW:
        if (!result->all_urls)
          result->all_urls = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(result->all_urls, g_strdup(descriptor->url));
        if (!result->first_og_url)
          result->first_og_url = g_strdup(descriptor->url);
        break;
      case GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF:
        if (!result->first_nostr_ref)
          result->first_nostr_ref = g_strdup(descriptor->original);
        break;
      case GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF:
        break;
    }
  }
}

static GnContentDescriptor *fallback_descriptor_for_ref(const gchar *ref) {
  const gchar *payload = g_str_has_prefix(ref, "nostr:") ? ref + 6 : ref;
  GnContentDescriptorType type;

  if (g_str_has_prefix(payload, "note1") ||
      g_str_has_prefix(payload, "nevent1") ||
      g_str_has_prefix(payload, "naddr1"))
    type = GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF;
  else if (g_str_has_prefix(payload, "npub1") ||
           g_str_has_prefix(payload, "nprofile1"))
    type = GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF;
  else
    return NULL;

  GnContentDescriptor *descriptor = g_new0(GnContentDescriptor, 1);
  descriptor->type = type;
  descriptor->original = g_str_has_prefix(ref, "nostr:") ?
    g_strdup(ref) : g_strdup_printf("nostr:%s", ref);
  descriptor->id = g_strdup(payload);
  return descriptor;
}

static gboolean fallback_ref_starts_at(const char *p,
                                       const char *content,
                                       const char *end) {
  if (p != content && (g_ascii_isalnum(p[-1]) || p[-1] == '_'))
    return FALSE;

  const char *payload = p;
  if (p + 6 <= end && strncmp(p, "nostr:", 6) == 0)
    payload += 6;

  return (payload + 5 <= end && g_str_has_prefix(payload, "note1")) ||
    (payload + 7 <= end && g_str_has_prefix(payload, "nevent1")) ||
    (payload + 6 <= end && g_str_has_prefix(payload, "naddr1")) ||
    (payload + 5 <= end && g_str_has_prefix(payload, "npub1")) ||
    (payload + 9 <= end && g_str_has_prefix(payload, "nprofile1"));
}

static gboolean fallback_trailing_punctuation(char c) {
  return c == '.' || c == ',' || c == ';' || c == '!' ||
    c == ')' || c == ']' || c == '}' || c == '\'' || c == '"';
}

static void parse_fallback_text(const char *content,
                                GHashTable *imeta_by_url,
                                GnContentRenderResult *result,
                                GString *markup,
                                GString *plain_text) {
  const char *p = content;
  const char *end = content + strlen(content);

  while (p < end) {
    const char *token_end = p;
    gboolean is_url =
      (p + 7 <= end && g_ascii_strncasecmp(p, "http://", 7) == 0) ||
      (p + 8 <= end && g_ascii_strncasecmp(p, "https://", 8) == 0);
    gboolean is_ref = fallback_ref_starts_at(p, content, end);

    if (is_url || is_ref) {
      while (token_end < end && !g_ascii_isspace(*token_end))
        token_end++;
      while (token_end > p && fallback_trailing_punctuation(token_end[-1]))
        token_end--;
      if (token_end == p) {
        const char *next = g_utf8_next_char(p);
        append_display_text(markup, plain_text, p, next - p);
        p = next;
        continue;
      }
      gsize token_len = (gsize)(token_end - p);
      g_autofree gchar *token = g_strndup(p, token_len);

      if (is_url) {
        ImetaEnrichment *imeta = imeta_by_url ?
          g_hash_table_lookup(imeta_by_url, token) : NULL;
        GnContentDescriptorType type =
          classify_url(token, token_len, imeta);
        g_ptr_array_add(result->descriptors,
                        descriptor_for_url(token, type, imeta));
        if (type == GN_CONTENT_DESCRIPTOR_LINK_PREVIEW) {
          g_autofree gchar *display = token_len > 40 ?
            g_strdup_printf("%.35s...", token) : g_strdup(token);
          append_display_text(markup, plain_text, display, -1);
        }
      } else {
        GnContentDescriptor *descriptor =
          fallback_descriptor_for_ref(token);
        if (descriptor) {
          gboolean profile =
            descriptor->type == GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF;
          g_ptr_array_add(result->descriptors, descriptor);
          if (profile)
            append_profile_mention(markup, plain_text,
                                   descriptor->original, token);
        } else {
          append_display_text(markup, plain_text, token, -1);
        }
      }
      p = token_end;
      continue;
    }

    const char *next = g_utf8_next_char(p);
    append_display_text(markup, plain_text, p, next - p);
    p = next;
  }
}

static GnContentRenderResult *gn_content_parse_internal(const char *content,
                                                 int content_len,
                                                 const char *tags_json,
                                                 storage_ndb_blocks *provided_blocks,
                                                 GError **error) {
  if (!content) {
    g_set_error_literal(error, NOSTR_GTK_ERROR, NOSTR_GTK_ERROR_INVALID_INPUT,
                        "Content string is NULL");
    return NULL;
  }

  if (content_len < 0)
    content_len = (int)strlen(content);
  else
    content_len = (int)strnlen(content, (gsize)content_len);

  g_autofree gchar *bounded_content = g_strndup(content, content_len);
  g_autofree gchar *safe_content = provided_blocks ? NULL :
    gnostr_sanitize_utf8(bounded_content);
  if (!provided_blocks && !safe_content)
    safe_content = g_strdup("");

  GnContentRenderResult *result = g_new0(GnContentRenderResult, 1);
  result->descriptors =
    g_ptr_array_new_with_free_func((GDestroyNotify)gn_content_descriptor_free);

  if (!*(provided_blocks ? bounded_content : safe_content)) {
    result->markup = g_strdup("");
    result->plain_text = g_strdup("");
    return result;
  }

  g_autoptr(GHashTable) imeta_by_url = parse_imeta_enrichments(tags_json);
  const char *block_content = provided_blocks ? bounded_content : safe_content;
  storage_ndb_blocks *blocks = provided_blocks ? provided_blocks :
    storage_ndb_parse_content_blocks(safe_content, (int)strlen(safe_content));
  gboolean owns_blocks = provided_blocks == NULL;
  GString *markup = g_string_new("");
  GString *plain_text = g_string_new("");

  if (!blocks) {
    result->used_block_fallback = TRUE;
    parse_fallback_text(safe_content, imeta_by_url, result, markup, plain_text);
    goto finish;
  }

  const char *content_end = block_content + strlen(block_content);
  struct ndb_block_iterator iter;
  struct ndb_block *block;
  ndb_blocks_iterate_start(block_content, blocks, &iter);

#define VALIDATE_BLOCK_RANGE(ptr_, len_) \
  do { \
    if ((ptr_) == NULL || (ptr_) < block_content || (ptr_) > content_end || \
        (gsize)(content_end - (ptr_)) < (gsize)(len_)) { \
      g_warning("content_renderer: invalid block range; falling back type=%d len=%u", \
                btype, (guint)(len_)); \
      goto fallback_from_blocks; \
    } \
  } while (0)

  while ((block = ndb_blocks_iterate_next(&iter)) != NULL) {
    enum ndb_block_type btype = ndb_get_block_type(block);

    switch (btype) {
      case BLOCK_TEXT: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        VALIDATE_BLOCK_RANGE(ptr, len);
        append_sanitized_block_slice(markup, plain_text, ptr, len);
        break;
      }

      case BLOCK_HASHTAG: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        VALIDATE_BLOCK_RANGE(ptr, len);
        append_display_text(markup, plain_text, "#", 1);
        append_sanitized_block_slice(markup, plain_text, ptr, len);
        break;
      }

      case BLOCK_URL: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        VALIDATE_BLOCK_RANGE(ptr, len);
        g_autofree gchar *url = sanitize_block_slice(ptr, len);

        if (!is_http_url_n(ptr, len)) {
          append_display_text(markup, plain_text, url, -1);
          break;
        }

        ImetaEnrichment *imeta = imeta_by_url ?
          g_hash_table_lookup(imeta_by_url, url) : NULL;
        GnContentDescriptorType type = classify_url(ptr, len, imeta);
        g_ptr_array_add(result->descriptors,
                        descriptor_for_url(url, type, imeta));

        if (type == GN_CONTENT_DESCRIPTOR_LINK_PREVIEW) {
          g_autofree gchar *display = len > 40 ?
            g_strdup_printf("%.35s...", url) : g_strdup(url);
          append_display_text(markup, plain_text, display, -1);
        }
        break;
      }

      case BLOCK_MENTION_BECH32: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *str_ptr = ndb_str_block_ptr(sb);
        uint32_t str_len = ndb_str_block_len(sb);
        struct nostr_bech32 *bech32 = ndb_bech32_block(block);
        VALIDATE_BLOCK_RANGE(str_ptr, str_len);

        g_autofree gchar *bech32_str = g_strndup(str_ptr, str_len);
        g_autofree gchar *original =
          g_strdup_printf("nostr:%s", bech32_str);
        GnContentDescriptor *descriptor =
          descriptor_for_bech32(bech32, original);

        if (descriptor) {
          gboolean profile =
            descriptor->type == GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF;
          g_ptr_array_add(result->descriptors, descriptor);
          if (!profile)
            break;
        }

        g_autofree gchar *display =
          format_mention_display(bech32, str_ptr, str_len);
        if (descriptor &&
            descriptor->type == GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF)
          append_profile_mention(markup, plain_text,
                                 descriptor->original, display);
        else
          append_display_text(markup, plain_text, display, -1);
        break;
      }

      case BLOCK_INVOICE: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        VALIDATE_BLOCK_RANGE(ptr, len);
        g_autofree gchar *invoice = g_strndup(ptr, len);
        g_autofree gchar *display = len > 20 ?
          g_strdup_printf("\xe2\x9a\xa1%.12s\xe2\x80\xa6", invoice) :
          g_strdup_printf("\xe2\x9a\xa1%s", invoice);
        append_display_text(markup, plain_text, display, -1);
        break;
      }

      case BLOCK_MENTION_INDEX: {
        struct ndb_str_block *sb = ndb_block_str(block);
        if (sb) {
          const char *ptr = ndb_str_block_ptr(sb);
          uint32_t len = ndb_str_block_len(sb);
          VALIDATE_BLOCK_RANGE(ptr, len);
          append_sanitized_block_slice(markup, plain_text, ptr, len);
        }
        break;
      }

      default:
        break;
    }
  }

#undef VALIDATE_BLOCK_RANGE
  if (owns_blocks)
    storage_ndb_blocks_free(blocks);
  goto finish;

fallback_from_blocks:
#undef VALIDATE_BLOCK_RANGE
  if (owns_blocks)
    storage_ndb_blocks_free(blocks);
  g_string_truncate(markup, 0);
  g_string_truncate(plain_text, 0);
  g_ptr_array_set_size(result->descriptors, 0);
  result->used_block_fallback = TRUE;
  if (!safe_content)
    safe_content = gnostr_sanitize_utf8(bounded_content);
  parse_fallback_text(safe_content ? safe_content : "", imeta_by_url,
                      result, markup, plain_text);

finish:
  result->markup = g_string_free(markup, FALSE);
  result->plain_text = g_string_free(plain_text, FALSE);
  gnostr_strip_zwsp(result->markup);
  gnostr_strip_zwsp(result->plain_text);
  populate_legacy_fields(result);
  return result;
}

GnContentRenderResult *gn_content_parse(const char *content,
                                        int content_len,
                                        const char *tags_json,
                                        GError **error) {
  g_atomic_int_inc(&parser_invocation_count);
  return gn_content_parse_internal(content, content_len, tags_json, NULL, error);
}

GnContentRenderResult *gn_content_parse_with_blocks(const char *content,
                                                    int content_len,
                                                    const char *tags_json,
                                                    struct ndb_blocks *blocks,
                                                    GError **error) {
  g_atomic_int_inc(&parser_invocation_count);
  if (!blocks)
    return gn_content_parse_internal(content, content_len, tags_json, NULL, error);
  return gn_content_parse_internal(content, content_len, tags_json, blocks, error);
}

GnContentRenderResult *gnostr_render_content(const char *content,
                                             int content_len,
                                             GError **error) {
  return gn_content_parse(content, content_len, NULL, error);
}

/* Convenience wrappers — delegate to unified gnostr_render_content() */

char *gnostr_render_content_markup(const char *content, int content_len,
                                    GError **error) {
  GnContentRenderResult *res = gnostr_render_content(content, content_len, error);
  if (!res) return NULL;
  gchar *markup = g_strdup(res->markup);
  gnostr_content_render_result_free(res);
  return markup;
}

GPtrArray *gnostr_extract_media_urls(const char *content, int content_len,
                                      GError **error) {
  GnContentRenderResult *res = gnostr_render_content(content, content_len, error);
  if (!res) return NULL;
  GPtrArray *urls = res->media_urls ? g_ptr_array_ref(res->media_urls) : NULL;
  gnostr_content_render_result_free(res);
  return urls;
}
