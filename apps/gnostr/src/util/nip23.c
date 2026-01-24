/*
 * nip23.c - NIP-23 Long-form Content Utilities
 */

#include "nip23.h"
#include <json-glib/json-glib.h>
#include <nostr/nip19/nip19.h>
#include <string.h>
#include <stdio.h>

/* Default reading speed in words per minute */
#define DEFAULT_WPM 200

GnostrArticleMeta *gnostr_article_meta_new(void) {
  GnostrArticleMeta *meta = g_new0(GnostrArticleMeta, 1);
  meta->published_at = 0;
  meta->hashtags = NULL;
  meta->hashtags_count = 0;
  return meta;
}

void gnostr_article_meta_free(GnostrArticleMeta *meta) {
  if (!meta) return;

  g_free(meta->d_tag);
  g_free(meta->title);
  g_free(meta->summary);
  g_free(meta->image);
  g_free(meta->client);

  if (meta->hashtags) {
    for (gsize i = 0; i < meta->hashtags_count; i++) {
      g_free(meta->hashtags[i]);
    }
    g_free(meta->hashtags);
  }

  g_free(meta);
}

GnostrArticleMeta *gnostr_article_parse_tags(const char *tags_json) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("NIP-23: Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("NIP-23: Tags is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrArticleMeta *meta = gnostr_article_meta_new();
  GPtrArray *hashtags_arr = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++) {
    JsonNode *tag_node = json_array_get_element(tags, i);
    if (!JSON_NODE_HOLDS_ARRAY(tag_node)) continue;

    JsonArray *tag = json_node_get_array(tag_node);
    guint tag_len = json_array_get_length(tag);
    if (tag_len < 2) continue;

    const char *tag_name = json_array_get_string_element(tag, 0);
    const char *tag_value = json_array_get_string_element(tag, 1);

    if (!tag_name || !tag_value) continue;

    if (strcmp(tag_name, "d") == 0) {
      g_free(meta->d_tag);
      meta->d_tag = g_strdup(tag_value);
    } else if (strcmp(tag_name, "title") == 0) {
      g_free(meta->title);
      meta->title = g_strdup(tag_value);
    } else if (strcmp(tag_name, "summary") == 0) {
      g_free(meta->summary);
      meta->summary = g_strdup(tag_value);
    } else if (strcmp(tag_name, "image") == 0) {
      g_free(meta->image);
      meta->image = g_strdup(tag_value);
    } else if (strcmp(tag_name, "published_at") == 0) {
      char *endptr;
      gint64 ts = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && *endptr == '\0' && ts > 0) {
        meta->published_at = ts;
      }
    } else if (strcmp(tag_name, "t") == 0) {
      /* Skip leading # if present */
      const char *hashtag = tag_value;
      if (*hashtag == '#') hashtag++;
      if (*hashtag) {
        g_ptr_array_add(hashtags_arr, g_strdup(hashtag));
      }
    } else if (strcmp(tag_name, "client") == 0) {
      g_free(meta->client);
      meta->client = g_strdup(tag_value);
    }
  }

  /* Convert hashtags array */
  meta->hashtags_count = hashtags_arr->len;
  if (hashtags_arr->len > 0) {
    meta->hashtags = g_new0(gchar*, hashtags_arr->len + 1);
    for (guint i = 0; i < hashtags_arr->len; i++) {
      meta->hashtags[i] = g_ptr_array_index(hashtags_arr, i);
    }
    meta->hashtags[hashtags_arr->len] = NULL;
  }
  g_ptr_array_free(hashtags_arr, FALSE);

  g_object_unref(parser);
  return meta;
}

/* For nostrdb integration - parse from ndb_note structure */
GnostrArticleMeta *gnostr_article_parse_tags_iter(void *txn, void *ndb_note) {
  /* This would use nostrdb API to iterate tags.
   * Since we don't have direct nostrdb headers here,
   * we provide a stub that can be filled in. */
  (void)txn;
  (void)ndb_note;

  /* Caller should serialize tags to JSON and use gnostr_article_parse_tags instead */
  g_warning("NIP-23: gnostr_article_parse_tags_iter not implemented - use JSON parser");
  return NULL;
}

gboolean gnostr_article_is_article(int kind) {
  return kind == NOSTR_KIND_LONG_FORM || kind == NOSTR_KIND_LONG_FORM_DRAFT;
}

/* Helper: convert hex string to bytes */
static gboolean hex_to_bytes(const char *hex, uint8_t *out, size_t len) {
  if (!hex || strlen(hex) != len * 2) return FALSE;

  for (size_t i = 0; i < len; i++) {
    unsigned int byte;
    if (sscanf(hex + i * 2, "%2x", &byte) != 1) return FALSE;
    out[i] = (uint8_t)byte;
  }
  return TRUE;
}

char *gnostr_article_build_naddr(int kind, const char *pubkey_hex,
                                  const char *d_tag, const char **relays) {
  if (!pubkey_hex || !d_tag) return NULL;

  NostrEntityPointer cfg = {
    .kind = kind,
    .public_key = (char *)pubkey_hex,
    .identifier = (char *)d_tag,
    .relays = (char **)relays,
    .relays_count = 0
  };

  /* Count relays if provided */
  if (relays) {
    while (relays[cfg.relays_count]) cfg.relays_count++;
  }

  NostrPointer *ptr = NULL;
  if (nostr_pointer_from_naddr_config(&cfg, &ptr) != 0 || !ptr) {
    return NULL;
  }

  char *encoded = NULL;
  int result = nostr_pointer_to_bech32(ptr, &encoded);
  nostr_pointer_free(ptr);

  if (result != 0) {
    return NULL;
  }

  return encoded;
}

char *gnostr_article_build_a_tag(int kind, const char *pubkey_hex,
                                  const char *d_tag) {
  if (!pubkey_hex || !d_tag) return NULL;

  return g_strdup_printf("%d:%s:%s", kind, pubkey_hex, d_tag);
}

gboolean gnostr_article_parse_a_tag(const char *a_tag,
                                     int *out_kind,
                                     char **out_pubkey,
                                     char **out_d_tag) {
  if (!a_tag || !*a_tag) return FALSE;

  /* Format: "kind:pubkey:d-tag" */
  char **parts = g_strsplit(a_tag, ":", 3);
  if (!parts || !parts[0] || !parts[1] || !parts[2]) {
    g_strfreev(parts);
    return FALSE;
  }

  /* Parse kind */
  char *endptr;
  long kind = strtol(parts[0], &endptr, 10);
  if (*endptr != '\0' || kind <= 0 || kind > 65535) {
    g_strfreev(parts);
    return FALSE;
  }

  /* Validate pubkey (64 hex chars) */
  size_t pubkey_len = strlen(parts[1]);
  if (pubkey_len != 64) {
    g_strfreev(parts);
    return FALSE;
  }

  for (size_t i = 0; i < 64; i++) {
    if (!g_ascii_isxdigit(parts[1][i])) {
      g_strfreev(parts);
      return FALSE;
    }
  }

  /* Output results */
  if (out_kind) *out_kind = (int)kind;
  if (out_pubkey) *out_pubkey = g_strdup(parts[1]);
  if (out_d_tag) *out_d_tag = g_strdup(parts[2]);

  g_strfreev(parts);
  return TRUE;
}

int gnostr_article_estimate_reading_time(const char *content,
                                          int words_per_minute) {
  if (!content || !*content) return 0;

  if (words_per_minute <= 0) {
    words_per_minute = DEFAULT_WPM;
  }

  /* Count words by finding transitions from whitespace to non-whitespace */
  int word_count = 0;
  gboolean in_word = FALSE;

  for (const char *p = content; *p; p++) {
    if (g_ascii_isspace(*p)) {
      in_word = FALSE;
    } else if (!in_word) {
      in_word = TRUE;
      word_count++;
    }
  }

  /* Round up to nearest minute */
  int minutes = (word_count + words_per_minute - 1) / words_per_minute;
  return minutes > 0 ? minutes : 1;
}
