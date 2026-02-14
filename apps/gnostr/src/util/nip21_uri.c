/**
 * NIP-21: nostr: URI Scheme Implementation
 *
 * This module implements parsing and building of nostr: URIs as defined
 * in NIP-21. It delegates to the NIP-19 functions for bech32 encoding
 * and decoding.
 */

#include "nip21_uri.h"
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <string.h>
#include <ctype.h>

/* The nostr: URI prefix */
#define NOSTR_URI_PREFIX "nostr:"
#define NOSTR_URI_PREFIX_LEN 6

/* Forward declarations */
static gchar *url_decode(const gchar *str);

/**
 * URL-decode a string.
 * Handles %XX escape sequences.
 */
static gchar *url_decode(const gchar *str) {
  if (!str) return NULL;

  gsize len = strlen(str);
  gchar *decoded = g_malloc(len + 1);
  gsize j = 0;

  for (gsize i = 0; i < len; i++) {
    if (str[i] == '%' && i + 2 < len &&
        g_ascii_isxdigit(str[i + 1]) && g_ascii_isxdigit(str[i + 2])) {
      gchar hex[3] = { str[i + 1], str[i + 2], '\0' };
      decoded[j++] = (gchar)g_ascii_strtoull(hex, NULL, 16);
      i += 2;
    } else if (str[i] == '+') {
      decoded[j++] = ' ';
    } else {
      decoded[j++] = str[i];
    }
  }
  decoded[j] = '\0';

  return decoded;
}

/**
 * Check if string starts with nostr: prefix (case-insensitive).
 */
static gboolean has_nostr_prefix(const gchar *str) {
  if (!str || strlen(str) < NOSTR_URI_PREFIX_LEN) return FALSE;
  return g_ascii_strncasecmp(str, NOSTR_URI_PREFIX, NOSTR_URI_PREFIX_LEN) == 0;
}

/**
 * Extract the bech32 part from a nostr: URI.
 * Handles URL-encoded URIs.
 */
static gchar *extract_bech32(const gchar *uri) {
  if (!uri) return NULL;

  /* First try URL decoding in case the URI is encoded */
  gchar *decoded = url_decode(uri);
  if (!decoded) return NULL;

  /* Check for nostr: prefix */
  if (!has_nostr_prefix(decoded)) {
    g_free(decoded);
    return NULL;
  }

  /* Extract the bech32 part (after "nostr:") */
  gchar *bech32 = g_strdup(decoded + NOSTR_URI_PREFIX_LEN);
  g_free(decoded);

  /* Trim any trailing whitespace or URL fragments */
  if (bech32) {
    gchar *end = bech32;
    while (*end && !g_ascii_isspace(*end) && *end != '#' && *end != '?') {
      end++;
    }
    *end = '\0';
  }

  return bech32;
}

const gchar *gnostr_uri_type_to_string(GnostrUriType type) {
  switch (type) {
    case GNOSTR_URI_TYPE_NPUB:     return "npub";
    case GNOSTR_URI_TYPE_NOTE:     return "note";
    case GNOSTR_URI_TYPE_NPROFILE: return "nprofile";
    case GNOSTR_URI_TYPE_NEVENT:   return "nevent";
    case GNOSTR_URI_TYPE_NADDR:    return "naddr";
    default:                       return "unknown";
  }
}

void gnostr_uri_free(GnostrUri *uri) {
  if (!uri) return;

  g_free(uri->raw_bech32);
  g_free(uri->pubkey_hex);
  g_free(uri->event_id_hex);
  g_free(uri->d_tag);
  g_free(uri->author_hex);

  if (uri->relays) {
    g_strfreev(uri->relays);
  }

  g_free(uri);
}

gboolean gnostr_uri_is_valid(const gchar *uri_string) {
  if (!uri_string) return FALSE;

  /* URL-decode and check prefix */
  gchar *decoded = url_decode(uri_string);
  if (!decoded) return FALSE;

  gboolean has_prefix = has_nostr_prefix(decoded);
  g_free(decoded);

  if (!has_prefix) return FALSE;

  /* Extract bech32 and do a quick inspection */
  gchar *bech32 = extract_bech32(uri_string);
  if (!bech32 || strlen(bech32) < 10) {
    g_free(bech32);
    return FALSE;
  }

  /* Check for valid HRP using NIP-19 inspect */
  GNostrBech32Type type = gnostr_nip19_inspect(bech32);
  g_free(bech32);

  return (type != GNOSTR_BECH32_UNKNOWN);
}

/**
 * Helper to copy relays from a GNostrNip19 into a GnostrUri.
 */
static void copy_relays_to_uri(GnostrUri *result, GNostrNip19 *n19) {
  const gchar *const *relays = gnostr_nip19_get_relays(n19);
  if (!relays || !relays[0]) return;

  gsize count = g_strv_length((gchar **)relays);
  result->relay_count = count;
  result->relays = g_new0(gchar *, count + 1);
  for (gsize i = 0; i < count; i++) {
    result->relays[i] = g_strdup(relays[i]);
  }
}

GnostrUri *gnostr_uri_parse(const gchar *uri) {
  if (!uri) return NULL;

  /* Extract the bech32 part */
  gchar *bech32 = extract_bech32(uri);
  if (!bech32 || strlen(bech32) < 10) {
    g_free(bech32);
    return NULL;
  }

  /* Decode once using the unified GNostrNip19 API */
  g_autoptr(GNostrNip19) n19 = gnostr_nip19_decode(bech32, NULL);
  if (!n19) {
    g_free(bech32);
    return NULL;
  }

  GNostrBech32Type b32_type = gnostr_nip19_get_entity_type(n19);

  /* Create the result structure */
  GnostrUri *result = g_new0(GnostrUri, 1);
  result->raw_bech32 = bech32;
  result->kind = -1;  /* -1 indicates not specified */

  switch (b32_type) {
    case GNOSTR_BECH32_NPUB:
      result->type = GNOSTR_URI_TYPE_NPUB;
      result->pubkey_hex = g_strdup(gnostr_nip19_get_pubkey(n19));
      break;

    case GNOSTR_BECH32_NOTE:
      result->type = GNOSTR_URI_TYPE_NOTE;
      result->event_id_hex = g_strdup(gnostr_nip19_get_event_id(n19));
      break;

    case GNOSTR_BECH32_NPROFILE:
      result->type = GNOSTR_URI_TYPE_NPROFILE;
      result->pubkey_hex = g_strdup(gnostr_nip19_get_pubkey(n19));
      copy_relays_to_uri(result, n19);
      break;

    case GNOSTR_BECH32_NEVENT: {
      result->type = GNOSTR_URI_TYPE_NEVENT;
      result->event_id_hex = g_strdup(gnostr_nip19_get_event_id(n19));
      const gchar *author = gnostr_nip19_get_author(n19);
      if (author) {
        result->author_hex = g_strdup(author);
      }
      gint kind = gnostr_nip19_get_kind(n19);
      if (kind > 0) {
        result->kind = kind;
      }
      copy_relays_to_uri(result, n19);
      break;
    }

    case GNOSTR_BECH32_NADDR:
      result->type = GNOSTR_URI_TYPE_NADDR;
      result->pubkey_hex = g_strdup(gnostr_nip19_get_pubkey(n19));
      result->kind = gnostr_nip19_get_kind(n19);
      result->d_tag = g_strdup(gnostr_nip19_get_identifier(n19));
      copy_relays_to_uri(result, n19);
      break;

    default:
      gnostr_uri_free(result);
      return NULL;
  }

  return result;
}

gchar *gnostr_uri_build_npub(const gchar *pubkey_hex) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return NULL;

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_npub(pubkey_hex, NULL);
  if (!n19) return NULL;

  return g_strconcat(NOSTR_URI_PREFIX, gnostr_nip19_get_bech32(n19), NULL);
}

gchar *gnostr_uri_build_note(const gchar *event_id_hex) {
  if (!event_id_hex || strlen(event_id_hex) != 64) return NULL;

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_note(event_id_hex, NULL);
  if (!n19) return NULL;

  return g_strconcat(NOSTR_URI_PREFIX, gnostr_nip19_get_bech32(n19), NULL);
}

gchar *gnostr_uri_build_nprofile(const gchar *pubkey_hex,
                                  const gchar *const *relays,
                                  gsize relay_count) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return NULL;

  (void)relay_count; /* relays is NULL-terminated; count is unused */

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_nprofile(pubkey_hex, relays, NULL);
  if (!n19) return NULL;

  return g_strconcat(NOSTR_URI_PREFIX, gnostr_nip19_get_bech32(n19), NULL);
}

gchar *gnostr_uri_build_nevent(const gchar *event_id_hex,
                                const gchar *const *relays,
                                gsize relay_count,
                                const gchar *author_hex,
                                gint kind) {
  if (!event_id_hex || strlen(event_id_hex) != 64) return NULL;
  if (author_hex && strlen(author_hex) != 64) return NULL;

  (void)relay_count; /* relays is NULL-terminated; count is unused */

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_nevent(event_id_hex, relays,
                                                            author_hex, kind, NULL);
  if (!n19) return NULL;

  return g_strconcat(NOSTR_URI_PREFIX, gnostr_nip19_get_bech32(n19), NULL);
}

gchar *gnostr_uri_build_naddr(const gchar *pubkey_hex,
                               gint kind,
                               const gchar *d_tag,
                               const gchar *const *relays,
                               gsize relay_count) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return NULL;
  if (!d_tag) return NULL;
  if (kind <= 0) return NULL;

  (void)relay_count; /* relays is NULL-terminated; count is unused */

  g_autoptr(GNostrNip19) n19 = gnostr_nip19_encode_naddr(d_tag, pubkey_hex, kind,
                                                           relays, NULL);
  if (!n19) return NULL;

  return g_strconcat(NOSTR_URI_PREFIX, gnostr_nip19_get_bech32(n19), NULL);
}

const gchar *gnostr_uri_get_bech32(const GnostrUri *uri) {
  return uri ? uri->raw_bech32 : NULL;
}

gchar *gnostr_uri_to_string(const GnostrUri *uri) {
  if (!uri || !uri->raw_bech32) return NULL;
  return g_strconcat(NOSTR_URI_PREFIX, uri->raw_bech32, NULL);
}
