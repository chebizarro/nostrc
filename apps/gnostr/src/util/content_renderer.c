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

/* nostrc-pgo5: Strip ZWS (U+200B = \xe2\x80\x8b) from strings to prevent
 * Pango layout corruption.  ZWS in Pango markup corrupts the internal line
 * list (NULL entries) causing SEGV in pango_layout_line_unref during
 * gtk_widget_allocate.  Relay events can contain actual ZWS characters in
 * their text, so we must strip them after rendering. */
char *
gnostr_strip_zwsp(char *str)
{
  if (!str) return str;
  char *src = str, *dst = str;
  while (*src) {
    if ((unsigned char)src[0] == 0xe2 &&
        (unsigned char)src[1] == 0x80 &&
        (unsigned char)src[2] == 0x8b) {
      src += 3;
    } else {
      *dst++ = *src++;
    }
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

void gnostr_content_render_result_free(GnContentRenderResult *result) {
  if (!result) return;
  g_free(result->markup);
  if (result->media_urls) g_ptr_array_unref(result->media_urls);
  if (result->all_urls) g_ptr_array_unref(result->all_urls);
  g_free(result->first_nostr_ref);
  g_free(result->first_og_url);
  g_free(result);
}

GnContentRenderResult *gnostr_render_content(const char *content, int content_len) {
  GnContentRenderResult *res = g_new0(GnContentRenderResult, 1);

  if (!content || !*content) {
    res->markup = g_strdup("");
    return res;
  }

  if (content_len < 0) content_len = (int)strlen(content);

  storage_ndb_blocks *blocks = storage_ndb_parse_content_blocks(content, content_len);
  if (!blocks) {
    res->markup = gnostr_strip_zwsp(g_markup_escape_text(content, content_len));
    return res;
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
        if (!g_utf8_validate(tag, -1, NULL)) {
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

        /* Collect URL metadata during this single pass */
        if (is_http_url_n(ptr, len)) {
          /* all_urls: every http(s) URL */
          if (!res->all_urls)
            res->all_urls = g_ptr_array_new_with_free_func(g_free);
          g_ptr_array_add(res->all_urls, g_strdup(url));

          gboolean is_media = is_image_url_n(ptr, len) || is_video_url_n(ptr, len);
          if (is_media) {
            if (!res->media_urls)
              res->media_urls = g_ptr_array_new_with_free_func(g_free);
            g_ptr_array_add(res->media_urls, g_strdup(url));
          } else if (!res->first_og_url) {
            res->first_og_url = g_strdup(url);
          }
        }

        /* Render markup */
        gchar *href = g_str_has_prefix(url, "www.")
                       ? g_strdup_printf("https://%s", url)
                       : g_strdup(url);
        gchar *esc_href = g_markup_escape_text(href, -1);

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

        gchar *bech32_str = g_strndup(str_ptr, str_len);
        gchar *href = g_strdup_printf("nostr:%s", bech32_str);
        gchar *esc_href = g_markup_escape_text(href, -1);

        /* Collect first nostr: ref for NIP-21 embed */
        if (!res->first_nostr_ref && bech32) {
          switch (bech32->type) {
            case NOSTR_BECH32_NOTE:
            case NOSTR_BECH32_NEVENT:
            case NOSTR_BECH32_NADDR:
            case NOSTR_BECH32_NPUB:
            case NOSTR_BECH32_NPROFILE:
              res->first_nostr_ref = g_strdup(href); /* "nostr:..." */
              break;
            default:
              break;
          }
        }

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

  gchar *markup = g_string_free(out, FALSE);
  gnostr_strip_zwsp(markup);
  res->markup = (markup && *markup) ? markup : (g_free(markup), g_strdup(""));
  return res;
}

/* Convenience wrappers — delegate to unified gnostr_render_content() */

char *gnostr_render_content_markup(const char *content, int content_len) {
  GnContentRenderResult *res = gnostr_render_content(content, content_len);
  gchar *markup = g_strdup(res->markup);
  gnostr_content_render_result_free(res);
  return markup;
}

GPtrArray *gnostr_extract_media_urls(const char *content, int content_len) {
  GnContentRenderResult *res = gnostr_render_content(content, content_len);
  GPtrArray *urls = res->media_urls ? g_ptr_array_ref(res->media_urls) : NULL;
  gnostr_content_render_result_free(res);
  return urls;
}
