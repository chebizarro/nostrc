/*
 * nip54_wiki.c - NIP-54 Wiki Utilities Implementation
 */

#include "nip54_wiki.h"
#include <json-glib/json-glib.h>
#include <nostr/nip19/nip19.h>
#include <string.h>
#include <ctype.h>

/* Default reading speed in words per minute */
#define DEFAULT_WPM 200

GnostrWikiArticle *gnostr_wiki_article_new(void) {
  GnostrWikiArticle *article = g_new0(GnostrWikiArticle, 1);
  article->published_at = 0;
  article->created_at = 0;
  article->related_articles = NULL;
  article->related_count = 0;
  article->topics = NULL;
  article->topics_count = 0;
  article->fork_refs = NULL;
  article->fork_refs_count = 0;
  return article;
}

void gnostr_wiki_article_free(GnostrWikiArticle *article) {
  if (!article) return;

  g_free(article->event_id);
  g_free(article->pubkey);
  g_free(article->d_tag);
  g_free(article->title);
  g_free(article->summary);
  g_free(article->content);

  if (article->related_articles) {
    for (gsize i = 0; i < article->related_count; i++) {
      g_free(article->related_articles[i]);
    }
    g_free(article->related_articles);
  }

  if (article->topics) {
    for (gsize i = 0; i < article->topics_count; i++) {
      g_free(article->topics[i]);
    }
    g_free(article->topics);
  }

  if (article->fork_refs) {
    for (gsize i = 0; i < article->fork_refs_count; i++) {
      g_free(article->fork_refs[i]);
    }
    g_free(article->fork_refs);
  }

  g_free(article);
}

GnostrWikiArticle *gnostr_wiki_article_parse_json(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("NIP-54: Failed to parse event JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root)) {
    g_warning("NIP-54: Event is not an object");
    g_object_unref(parser);
    return NULL;
  }

  JsonObject *event = json_node_get_object(root);

  /* Verify kind is 30818 */
  gint64 kind = json_object_get_int_member(event, "kind");
  if (kind != NOSTR_KIND_WIKI) {
    g_warning("NIP-54: Expected kind 30818, got %" G_GINT64_FORMAT, kind);
    g_object_unref(parser);
    return NULL;
  }

  GnostrWikiArticle *article = gnostr_wiki_article_new();

  /* Extract event metadata */
  if (json_object_has_member(event, "id")) {
    article->event_id = g_strdup(json_object_get_string_member(event, "id"));
  }
  if (json_object_has_member(event, "pubkey")) {
    article->pubkey = g_strdup(json_object_get_string_member(event, "pubkey"));
  }
  if (json_object_has_member(event, "created_at")) {
    article->created_at = json_object_get_int_member(event, "created_at");
  }
  if (json_object_has_member(event, "content")) {
    article->content = g_strdup(json_object_get_string_member(event, "content"));
  }

  /* Parse tags */
  if (json_object_has_member(event, "tags")) {
    JsonArray *tags = json_object_get_array_member(event, "tags");
    guint n_tags = json_array_get_length(tags);

    GPtrArray *related_arr = g_ptr_array_new();
    GPtrArray *topics_arr = g_ptr_array_new();
    GPtrArray *forks_arr = g_ptr_array_new();

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
        g_free(article->d_tag);
        article->d_tag = g_strdup(tag_value);
      } else if (strcmp(tag_name, "title") == 0) {
        g_free(article->title);
        article->title = g_strdup(tag_value);
      } else if (strcmp(tag_name, "summary") == 0) {
        g_free(article->summary);
        article->summary = g_strdup(tag_value);
      } else if (strcmp(tag_name, "published_at") == 0) {
        char *endptr;
        gint64 ts = g_ascii_strtoll(tag_value, &endptr, 10);
        if (endptr != tag_value && *endptr == '\0' && ts > 0) {
          article->published_at = ts;
        }
      } else if (strcmp(tag_name, "a") == 0) {
        /* Related article reference */
        g_ptr_array_add(related_arr, g_strdup(tag_value));
      } else if (strcmp(tag_name, "t") == 0) {
        /* Topic/category tag */
        const char *topic = tag_value;
        if (*topic == '#') topic++;
        if (*topic) {
          g_ptr_array_add(topics_arr, g_strdup(topic));
        }
      } else if (strcmp(tag_name, "e") == 0) {
        /* Fork/merge reference */
        g_ptr_array_add(forks_arr, g_strdup(tag_value));
      }
    }

    /* Convert related articles array */
    article->related_count = related_arr->len;
    if (related_arr->len > 0) {
      article->related_articles = g_new0(gchar*, related_arr->len + 1);
      for (guint i = 0; i < related_arr->len; i++) {
        article->related_articles[i] = g_ptr_array_index(related_arr, i);
      }
      article->related_articles[related_arr->len] = NULL;
    }
    g_ptr_array_free(related_arr, FALSE);

    /* Convert topics array */
    article->topics_count = topics_arr->len;
    if (topics_arr->len > 0) {
      article->topics = g_new0(gchar*, topics_arr->len + 1);
      for (guint i = 0; i < topics_arr->len; i++) {
        article->topics[i] = g_ptr_array_index(topics_arr, i);
      }
      article->topics[topics_arr->len] = NULL;
    }
    g_ptr_array_free(topics_arr, FALSE);

    /* Convert fork refs array */
    article->fork_refs_count = forks_arr->len;
    if (forks_arr->len > 0) {
      article->fork_refs = g_new0(gchar*, forks_arr->len + 1);
      for (guint i = 0; i < forks_arr->len; i++) {
        article->fork_refs[i] = g_ptr_array_index(forks_arr, i);
      }
      article->fork_refs[forks_arr->len] = NULL;
    }
    g_ptr_array_free(forks_arr, FALSE);
  }

  g_object_unref(parser);
  return article;
}

GnostrWikiArticle *gnostr_wiki_article_parse_tags(const char *tags_json,
                                                    const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, tags_json, -1, &error)) {
    g_warning("NIP-54: Failed to parse tags JSON: %s", error->message);
    g_error_free(error);
    g_object_unref(parser);
    return NULL;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_ARRAY(root)) {
    g_warning("NIP-54: Tags is not an array");
    g_object_unref(parser);
    return NULL;
  }

  JsonArray *tags = json_node_get_array(root);
  guint n_tags = json_array_get_length(tags);

  GnostrWikiArticle *article = gnostr_wiki_article_new();
  article->content = g_strdup(content);

  GPtrArray *related_arr = g_ptr_array_new();
  GPtrArray *topics_arr = g_ptr_array_new();
  GPtrArray *forks_arr = g_ptr_array_new();

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
      g_free(article->d_tag);
      article->d_tag = g_strdup(tag_value);
    } else if (strcmp(tag_name, "title") == 0) {
      g_free(article->title);
      article->title = g_strdup(tag_value);
    } else if (strcmp(tag_name, "summary") == 0) {
      g_free(article->summary);
      article->summary = g_strdup(tag_value);
    } else if (strcmp(tag_name, "published_at") == 0) {
      char *endptr;
      gint64 ts = g_ascii_strtoll(tag_value, &endptr, 10);
      if (endptr != tag_value && *endptr == '\0' && ts > 0) {
        article->published_at = ts;
      }
    } else if (strcmp(tag_name, "a") == 0) {
      g_ptr_array_add(related_arr, g_strdup(tag_value));
    } else if (strcmp(tag_name, "t") == 0) {
      const char *topic = tag_value;
      if (*topic == '#') topic++;
      if (*topic) {
        g_ptr_array_add(topics_arr, g_strdup(topic));
      }
    } else if (strcmp(tag_name, "e") == 0) {
      g_ptr_array_add(forks_arr, g_strdup(tag_value));
    }
  }

  /* Convert arrays */
  article->related_count = related_arr->len;
  if (related_arr->len > 0) {
    article->related_articles = g_new0(gchar*, related_arr->len + 1);
    for (guint i = 0; i < related_arr->len; i++) {
      article->related_articles[i] = g_ptr_array_index(related_arr, i);
    }
  }
  g_ptr_array_free(related_arr, FALSE);

  article->topics_count = topics_arr->len;
  if (topics_arr->len > 0) {
    article->topics = g_new0(gchar*, topics_arr->len + 1);
    for (guint i = 0; i < topics_arr->len; i++) {
      article->topics[i] = g_ptr_array_index(topics_arr, i);
    }
  }
  g_ptr_array_free(topics_arr, FALSE);

  article->fork_refs_count = forks_arr->len;
  if (forks_arr->len > 0) {
    article->fork_refs = g_new0(gchar*, forks_arr->len + 1);
    for (guint i = 0; i < forks_arr->len; i++) {
      article->fork_refs[i] = g_ptr_array_index(forks_arr, i);
    }
  }
  g_ptr_array_free(forks_arr, FALSE);

  g_object_unref(parser);
  return article;
}

gboolean gnostr_wiki_is_wiki_article(int kind) {
  return kind == NOSTR_KIND_WIKI;
}

GnostrWikiRelatedArticle *gnostr_wiki_parse_a_tag(const char *a_tag) {
  if (!a_tag || !*a_tag) return NULL;

  /* Format: "kind:pubkey:d-tag" */
  char **parts = g_strsplit(a_tag, ":", 3);
  if (!parts || !parts[0] || !parts[1] || !parts[2]) {
    g_strfreev(parts);
    return NULL;
  }

  /* Parse kind */
  char *endptr;
  long kind = strtol(parts[0], &endptr, 10);
  if (*endptr != '\0' || kind <= 0 || kind > 65535) {
    g_strfreev(parts);
    return NULL;
  }

  /* Validate pubkey (64 hex chars) */
  size_t pubkey_len = strlen(parts[1]);
  if (pubkey_len != 64) {
    g_strfreev(parts);
    return NULL;
  }

  for (size_t i = 0; i < 64; i++) {
    if (!g_ascii_isxdigit(parts[1][i])) {
      g_strfreev(parts);
      return NULL;
    }
  }

  GnostrWikiRelatedArticle *related = g_new0(GnostrWikiRelatedArticle, 1);
  related->kind = (int)kind;
  related->pubkey = g_strdup(parts[1]);
  related->d_tag = g_strdup(parts[2]);
  related->relay_hint = NULL;

  g_strfreev(parts);
  return related;
}

void gnostr_wiki_related_article_free(GnostrWikiRelatedArticle *related) {
  if (!related) return;

  g_free(related->pubkey);
  g_free(related->d_tag);
  g_free(related->relay_hint);
  g_free(related);
}

char *gnostr_wiki_build_a_tag(const char *pubkey_hex, const char *d_tag) {
  if (!pubkey_hex || !d_tag) return NULL;

  return g_strdup_printf("%d:%s:%s", NOSTR_KIND_WIKI, pubkey_hex, d_tag);
}

char *gnostr_wiki_build_naddr(const char *pubkey_hex,
                               const char *d_tag,
                               const char **relays) {
  if (!pubkey_hex || !d_tag) return NULL;

  NostrEntityPointer cfg = {
    .kind = NOSTR_KIND_WIKI,
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

char *gnostr_wiki_normalize_slug(const char *title) {
  if (!title || !*title) return g_strdup("");

  GString *slug = g_string_sized_new(strlen(title));
  gboolean prev_hyphen = FALSE;

  for (const char *p = title; *p; p++) {
    gunichar c = g_utf8_get_char(p);

    if (g_unichar_isalnum(c)) {
      g_string_append_unichar(slug, g_unichar_tolower(c));
      prev_hyphen = FALSE;
    } else if (g_unichar_isspace(c) || c == '-' || c == '_') {
      if (!prev_hyphen && slug->len > 0) {
        g_string_append_c(slug, '-');
        prev_hyphen = TRUE;
      }
    }
    /* Skip other characters */

    p = g_utf8_next_char(p) - 1;  /* -1 because loop will increment */
  }

  /* Remove trailing hyphen */
  if (slug->len > 0 && slug->str[slug->len - 1] == '-') {
    g_string_truncate(slug, slug->len - 1);
  }

  return g_string_free(slug, FALSE);
}

char *gnostr_wiki_build_event_json(const char *d_tag,
                                    const char *title,
                                    const char *summary,
                                    const char *content,
                                    const char **related_articles,
                                    const char **topics) {
  if (!d_tag || !content) return NULL;

  JsonBuilder *builder = json_builder_new();

  json_builder_begin_object(builder);

  /* kind */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, NOSTR_KIND_WIKI);

  /* content */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, content);

  /* tags */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* d tag (required) */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "d");
  json_builder_add_string_value(builder, d_tag);
  json_builder_end_array(builder);

  /* title tag */
  if (title && *title) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "title");
    json_builder_add_string_value(builder, title);
    json_builder_end_array(builder);
  }

  /* summary tag */
  if (summary && *summary) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "summary");
    json_builder_add_string_value(builder, summary);
    json_builder_end_array(builder);
  }

  /* published_at tag */
  json_builder_begin_array(builder);
  json_builder_add_string_value(builder, "published_at");
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  gchar *ts_str = g_strdup_printf("%" G_GINT64_FORMAT, now);
  json_builder_add_string_value(builder, ts_str);
  g_free(ts_str);
  json_builder_end_array(builder);

  /* Related article tags */
  if (related_articles) {
    for (int i = 0; related_articles[i]; i++) {
      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "a");
      json_builder_add_string_value(builder, related_articles[i]);
      json_builder_end_array(builder);
    }
  }

  /* Topic tags */
  if (topics) {
    for (int i = 0; topics[i]; i++) {
      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "t");
      json_builder_add_string_value(builder, topics[i]);
      json_builder_end_array(builder);
    }
  }

  json_builder_end_array(builder);  /* end tags */
  json_builder_end_object(builder);

  JsonGenerator *generator = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(generator, root);

  gchar *json_str = json_generator_to_data(generator, NULL);

  json_node_unref(root);
  g_object_unref(generator);
  g_object_unref(builder);

  return json_str;
}

int gnostr_wiki_estimate_reading_time(const char *content,
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

static char *generate_anchor(const char *text) {
  if (!text || !*text) return g_strdup("");

  GString *anchor = g_string_sized_new(strlen(text));
  gboolean prev_hyphen = FALSE;

  for (const char *p = text; *p; p++) {
    if (g_ascii_isalnum(*p)) {
      g_string_append_c(anchor, g_ascii_tolower(*p));
      prev_hyphen = FALSE;
    } else if (g_ascii_isspace(*p) || *p == '-' || *p == '_') {
      if (!prev_hyphen && anchor->len > 0) {
        g_string_append_c(anchor, '-');
        prev_hyphen = TRUE;
      }
    }
  }

  /* Remove trailing hyphen */
  if (anchor->len > 0 && anchor->str[anchor->len - 1] == '-') {
    g_string_truncate(anchor, anchor->len - 1);
  }

  return g_string_free(anchor, FALSE);
}

void gnostr_wiki_heading_free(GnostrWikiHeading *heading) {
  if (!heading) return;

  g_free(heading->text);
  g_free(heading->anchor);
  g_free(heading);
}

GPtrArray *gnostr_wiki_extract_table_of_contents(const char *markdown) {
  if (!markdown || !*markdown) return NULL;

  GPtrArray *toc = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_wiki_heading_free);

  const char *p = markdown;

  while (*p) {
    /* Find start of line */
    if (p == markdown || *(p - 1) == '\n') {
      /* Check for ATX-style heading (# Heading) */
      if (*p == '#') {
        int level = 0;
        const char *hash_start = p;

        while (*p == '#' && level < 6) {
          level++;
          p++;
        }

        /* Must have space after # and valid level */
        if (level > 0 && level <= 6 && g_ascii_isspace(*p)) {
          /* Skip whitespace */
          while (*p && g_ascii_isspace(*p) && *p != '\n') p++;

          /* Find end of heading text */
          const char *text_start = p;
          while (*p && *p != '\n') p++;
          const char *text_end = p;

          /* Trim trailing # and whitespace */
          while (text_end > text_start && (*(text_end - 1) == '#' || g_ascii_isspace(*(text_end - 1)))) {
            text_end--;
          }

          if (text_end > text_start) {
            GnostrWikiHeading *heading = g_new0(GnostrWikiHeading, 1);
            heading->level = level;
            heading->text = g_strndup(text_start, text_end - text_start);
            heading->anchor = generate_anchor(heading->text);
            g_ptr_array_add(toc, heading);
          }
        } else {
          /* Not a valid heading, restore position */
          p = hash_start;
        }
      }
    }

    if (*p) p++;
  }

  if (toc->len == 0) {
    g_ptr_array_unref(toc);
    return NULL;
  }

  return toc;
}
