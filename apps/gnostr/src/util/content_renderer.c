/**
 * Content Renderer - NDB content block-based note rendering
 *
 * Uses nostrdb's pre-parsed content blocks instead of manual whitespace
 * tokenization. This correctly handles hashtags, mentions, URLs, and
 * invoices even when not separated by whitespace.
 */

#include "content_renderer.h"
#include "../storage_ndb.h"
#include "../ui/gnostr-profile-provider.h"
#include "nostr_nip19.h"
#include <string.h>

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

char *gnostr_render_content_markup(const char *content, int content_len) {
  if (!content || !*content) return g_strdup("");

  if (content_len < 0) content_len = (int)strlen(content);

  /* Parse content into blocks on-the-fly */
  storage_ndb_blocks *blocks = storage_ndb_parse_content_blocks(content, content_len);
  if (!blocks) {
    /* Fallback: escape the whole string */
    return g_markup_escape_text(content, content_len);
  }

  GString *out = g_string_new("");
  struct ndb_block_iterator iter;
  struct ndb_block *block;

  ndb_blocks_iterate_start(content, blocks, &iter);

  while ((block = ndb_blocks_iterate_next(&iter)) != NULL) {
    enum ndb_block_type btype = ndb_get_block_type(block);

    switch (btype) {
      case BLOCK_TEXT: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        /* nostrc-dct8: Sanitize invalid UTF-8 before Pango markup */
        g_autofree gchar *text = g_strndup(ptr, len);
        if (!g_utf8_validate(text, -1, NULL)) {
          gchar *valid = g_utf8_make_valid(text, -1);
          g_free(text);
          text = g_steal_pointer(&valid);
        }
        gchar *escaped = g_markup_escape_text(text, -1);
        g_string_append(out, escaped);
        g_free(escaped);
        break;
      }

      case BLOCK_HASHTAG: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        gchar *tag = g_strndup(ptr, len);
        /* Validate UTF-8 before inserting into Pango markup */
        if (!g_utf8_validate(tag, -1, NULL)) {
          /* Invalid UTF-8: render as plain text with replacement char */
          gchar *valid = g_utf8_make_valid(tag, -1);
          gchar *esc = g_markup_escape_text(valid, -1);
          g_string_append_printf(out, "#%s", esc);
          g_free(esc);
          g_free(valid);
        } else {
          gchar *esc = g_markup_escape_text(tag, -1);
          g_string_append_printf(out, "<a href=\"hashtag:%s\">#%s</a>", esc, esc);
          g_free(esc);
        }
        g_free(tag);
        break;
      }

      case BLOCK_URL: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        gchar *url = g_strndup(ptr, len);

        /* For www. URLs, use https:// in href */
        gchar *href = g_str_has_prefix(url, "www.")
                       ? g_strdup_printf("https://%s", url)
                       : g_strdup(url);
        gchar *esc_href = g_markup_escape_text(href, -1);

        /* Truncate display if too long */
        gchar *display;
        if (len > 40) {
          display = g_strdup_printf("%.35s...", url);
        } else {
          display = g_strdup(url);
        }
        gchar *esc_display = g_markup_escape_text(display, -1);

        g_string_append_printf(out,
          "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);

        g_free(esc_display);
        g_free(display);
        g_free(esc_href);
        g_free(href);
        g_free(url);
        break;
      }

      case BLOCK_MENTION_BECH32: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *str_ptr = ndb_str_block_ptr(sb);
        uint32_t str_len = ndb_str_block_len(sb);
        struct nostr_bech32 *bech32 = ndb_bech32_block(block);

        /* Build nostr: href from the raw bech32 string */
        gchar *bech32_str = g_strndup(str_ptr, str_len);
        gchar *href = g_strdup_printf("nostr:%s", bech32_str);
        gchar *esc_href = g_markup_escape_text(href, -1);

        /* Format display name */
        gchar *display = format_mention_display(bech32, str_ptr, str_len);
        gchar *esc_display = g_markup_escape_text(display, -1);

        g_string_append_printf(out,
          "<a href=\"%s\" title=\"%s\">%s</a>", esc_href, esc_href, esc_display);

        g_free(esc_display);
        g_free(display);
        g_free(esc_href);
        g_free(href);
        g_free(bech32_str);
        break;
      }

      case BLOCK_INVOICE: {
        struct ndb_str_block *sb = ndb_block_str(block);
        const char *ptr = ndb_str_block_ptr(sb);
        uint32_t len = ndb_str_block_len(sb);
        gchar *inv_str = g_strndup(ptr, len);
        gchar *esc = g_markup_escape_text(inv_str, -1);

        /* Display as lightning link with truncated invoice */
        gchar *display;
        if (len > 20) {
          display = g_strdup_printf("\xe2\x9a\xa1%.12s\xe2\x80\xa6", inv_str);
        } else {
          display = g_strdup_printf("\xe2\x9a\xa1%s", inv_str);
        }
        gchar *esc_display = g_markup_escape_text(display, -1);
        g_string_append_printf(out,
          "<a href=\"lightning:%s\">%s</a>", esc, esc_display);

        g_free(esc_display);
        g_free(display);
        g_free(esc);
        g_free(inv_str);
        break;
      }

      case BLOCK_MENTION_INDEX: {
        /* Legacy #[N] style mention - render as-is for now */
        struct ndb_str_block *sb = ndb_block_str(block);
        if (sb) {
          const char *ptr = ndb_str_block_ptr(sb);
          uint32_t len = ndb_str_block_len(sb);
          gchar *escaped = g_markup_escape_text(ptr, len);
          g_string_append(out, escaped);
          g_free(escaped);
        }
        break;
      }

      default:
        break;
    }
  }

  storage_ndb_blocks_free(blocks);

  gchar *result = g_string_free(out, FALSE);
  return (result && *result) ? result : (g_free(result), g_strdup(""));
}

GPtrArray *gnostr_extract_media_urls(const char *content, int content_len) {
  if (!content || !*content) return NULL;

  if (content_len < 0) content_len = (int)strlen(content);

  storage_ndb_blocks *blocks = storage_ndb_parse_content_blocks(content, content_len);
  if (!blocks) return NULL;

  GPtrArray *urls = NULL;
  struct ndb_block_iterator iter;
  struct ndb_block *block;

  ndb_blocks_iterate_start(content, blocks, &iter);

  while ((block = ndb_blocks_iterate_next(&iter)) != NULL) {
    if (ndb_get_block_type(block) != BLOCK_URL) continue;

    struct ndb_str_block *sb = ndb_block_str(block);
    const char *ptr = ndb_str_block_ptr(sb);
    uint32_t len = ndb_str_block_len(sb);

    /* Only http(s) URLs */
    if (len < 8) continue;
    if (g_ascii_strncasecmp(ptr, "http://", 7) != 0 &&
        g_ascii_strncasecmp(ptr, "https://", 8) != 0) continue;

    if (is_image_url_n(ptr, len) || is_video_url_n(ptr, len)) {
      if (!urls) urls = g_ptr_array_new_with_free_func(g_free);
      g_ptr_array_add(urls, g_strndup(ptr, len));
    }
  }

  storage_ndb_blocks_free(blocks);
  return urls;
}
