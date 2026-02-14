/*
 * nip84_highlights.c - NIP-84 Highlights Utilities Implementation
 */

#include "nip84_highlights.h"
#include <nostr-gobject-1.0/nostr_json.h>
#include <json.h>
#include <nostr-event.h>
#include <string.h>
#include <time.h>

GnostrHighlight *gnostr_highlight_new(void) {
  GnostrHighlight *h = g_new0(GnostrHighlight, 1);
  h->source_type = GNOSTR_HIGHLIGHT_SOURCE_NONE;
  return h;
}

void gnostr_highlight_free(GnostrHighlight *highlight) {
  if (!highlight) return;

  g_free(highlight->event_id);
  g_free(highlight->pubkey);
  g_free(highlight->highlighted_text);
  g_free(highlight->context);
  g_free(highlight->comment);
  g_free(highlight->source_event_id);
  g_free(highlight->source_a_tag);
  g_free(highlight->source_url);
  g_free(highlight->source_relay_hint);
  g_free(highlight->author_pubkey);
  g_free(highlight->author_relay_hint);
  g_free(highlight);
}

GnostrHighlight *gnostr_highlight_parse_json(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  /* Deserialize to NostrEvent using the facade */
  NostrEvent event = {0};
  if (nostr_event_deserialize(&event, event_json) != 0) {
    g_warning("NIP-84: Failed to parse event JSON");
    return NULL;
  }

  /* Verify kind */
  if (event.kind != NOSTR_KIND_HIGHLIGHT) {
    g_debug("NIP-84: Not a highlight event (kind=%d)", event.kind);
    /* Free internal event fields (stack-allocated event) */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);
    return NULL;
  }

  GnostrHighlight *h = gnostr_highlight_new();

  /* Extract event metadata */
  if (event.id) {
    h->event_id = g_strdup(event.id);
  }

  if (event.pubkey) {
    h->pubkey = g_strdup(event.pubkey);
  }

  h->created_at = event.created_at;

  /* Extract content (highlighted text) */
  if (event.content) {
    h->highlighted_text = g_strdup(event.content);
  }

  /* Parse tags using NostrTags API */
  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "context") == 0) {
        g_free(h->context);
        h->context = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "comment") == 0) {
        g_free(h->comment);
        h->comment = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "e") == 0) {
        /* Source note reference */
        h->source_type = GNOSTR_HIGHLIGHT_SOURCE_NOTE;
        g_free(h->source_event_id);
        h->source_event_id = g_strdup(tag_value);
        /* Relay hint in third position */
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            g_free(h->source_relay_hint);
            h->source_relay_hint = g_strdup(relay);
          }
        }
      }
      else if (strcmp(tag_name, "a") == 0) {
        /* Addressable event reference (articles) */
        h->source_type = GNOSTR_HIGHLIGHT_SOURCE_ARTICLE;
        g_free(h->source_a_tag);
        h->source_a_tag = g_strdup(tag_value);
        /* Relay hint in third position */
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            g_free(h->source_relay_hint);
            h->source_relay_hint = g_strdup(relay);
          }
        }
      }
      else if (strcmp(tag_name, "r") == 0) {
        /* External URL reference */
        h->source_type = GNOSTR_HIGHLIGHT_SOURCE_URL;
        g_free(h->source_url);
        h->source_url = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "p") == 0) {
        /* Author reference */
        g_free(h->author_pubkey);
        h->author_pubkey = g_strdup(tag_value);
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            g_free(h->author_relay_hint);
            h->author_relay_hint = g_strdup(relay);
          }
        }
      }
    }
  }

  /* Free internal event fields (stack-allocated event) */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);
  return h;
}

GnostrHighlight *gnostr_highlight_parse_tags(const char *tags_json,
                                               const char *content) {
  if (!tags_json || !*tags_json) return NULL;

  GnostrHighlight *h = gnostr_highlight_new();
  if (content) {
    h->highlighted_text = g_strdup(content);
  }

  /* Parse tags by wrapping in a dummy event JSON and deserializing */
  char *dummy_event = g_strdup_printf("{\"kind\":9802,\"content\":\"\",\"tags\":%s}", tags_json);
  NostrEvent event = {0};
  if (nostr_event_deserialize(&event, dummy_event) != 0) {
    g_free(dummy_event);
    gnostr_highlight_free(h);
    return NULL;
  }
  g_free(dummy_event);

  if (event.tags) {
    size_t n_tags = nostr_tags_size(event.tags);
    for (size_t i = 0; i < n_tags; i++) {
      NostrTag *tag = nostr_tags_get(event.tags, i);
      if (!tag) continue;
      size_t tag_len = nostr_tag_size(tag);
      if (tag_len < 2) continue;

      const char *tag_name = nostr_tag_get(tag, 0);
      const char *tag_value = nostr_tag_get(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (strcmp(tag_name, "context") == 0) {
        g_free(h->context);
        h->context = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "comment") == 0) {
        g_free(h->comment);
        h->comment = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "e") == 0) {
        h->source_type = GNOSTR_HIGHLIGHT_SOURCE_NOTE;
        g_free(h->source_event_id);
        h->source_event_id = g_strdup(tag_value);
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            g_free(h->source_relay_hint);
            h->source_relay_hint = g_strdup(relay);
          }
        }
      }
      else if (strcmp(tag_name, "a") == 0) {
        h->source_type = GNOSTR_HIGHLIGHT_SOURCE_ARTICLE;
        g_free(h->source_a_tag);
        h->source_a_tag = g_strdup(tag_value);
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            g_free(h->source_relay_hint);
            h->source_relay_hint = g_strdup(relay);
          }
        }
      }
      else if (strcmp(tag_name, "r") == 0) {
        h->source_type = GNOSTR_HIGHLIGHT_SOURCE_URL;
        g_free(h->source_url);
        h->source_url = g_strdup(tag_value);
      }
      else if (strcmp(tag_name, "p") == 0) {
        g_free(h->author_pubkey);
        h->author_pubkey = g_strdup(tag_value);
        if (tag_len >= 3) {
          const char *relay = nostr_tag_get(tag, 2);
          if (relay && *relay) {
            g_free(h->author_relay_hint);
            h->author_relay_hint = g_strdup(relay);
          }
        }
      }
    }
  }

  /* Free internal event fields (stack-allocated event) */
  free(event.id);
  free(event.pubkey);
  free(event.content);
  free(event.sig);
  if (event.tags) nostr_tags_free(event.tags);
  return h;
}

char *gnostr_highlight_build_event_json(const char *highlighted_text,
                                         const char *context,
                                         const char *comment,
                                         const char *source_event_id,
                                         const char *source_a_tag,
                                         const char *source_url,
                                         const char *author_pubkey,
                                         const char *relay_hint) {
  if (!highlighted_text || !*highlighted_text) {
    g_warning("NIP-84: Cannot create highlight without text");
    return NULL;
  }

  /* Build the unsigned event using NostrEvent API */
  NostrEvent event = {0};
  event.kind = NOSTR_KIND_HIGHLIGHT;
  event.created_at = (int64_t)time(NULL);
  event.content = (char *)highlighted_text; /* Will be copied by serialize */

  /* Create tags array */
  NostrTags *tags = nostr_tags_new(0);

  /* Add context tag if provided */
  if (context && *context) {
    NostrTag *context_tag = nostr_tag_new("context", NULL);
    nostr_tag_append(context_tag, context);
    nostr_tags_append(tags, context_tag);
  }

  /* Add source reference - only one type allowed */
  if (source_event_id && *source_event_id) {
    /* Note reference (e tag) */
    NostrTag *e_tag = nostr_tag_new("e", NULL);
    nostr_tag_append(e_tag, source_event_id);
    if (relay_hint && *relay_hint) {
      nostr_tag_append(e_tag, relay_hint);
      nostr_tag_append(e_tag, "mention");
    }
    nostr_tags_append(tags, e_tag);
  }
  else if (source_a_tag && *source_a_tag) {
    /* Addressable event reference (a tag) */
    NostrTag *a_tag = nostr_tag_new("a", NULL);
    nostr_tag_append(a_tag, source_a_tag);
    if (relay_hint && *relay_hint) {
      nostr_tag_append(a_tag, relay_hint);
      nostr_tag_append(a_tag, "mention");
    }
    nostr_tags_append(tags, a_tag);
  }
  else if (source_url && *source_url) {
    /* External URL reference (r tag) */
    NostrTag *r_tag = nostr_tag_new("r", NULL);
    nostr_tag_append(r_tag, source_url);
    nostr_tags_append(tags, r_tag);
  }

  /* Add author reference if provided */
  if (author_pubkey && *author_pubkey) {
    NostrTag *p_tag = nostr_tag_new("p", NULL);
    nostr_tag_append(p_tag, author_pubkey);
    if (relay_hint && *relay_hint) {
      nostr_tag_append(p_tag, relay_hint);
    }
    nostr_tags_append(tags, p_tag);
  }

  /* Add comment tag if provided */
  if (comment && *comment) {
    NostrTag *comment_tag = nostr_tag_new("comment", NULL);
    nostr_tag_append(comment_tag, comment);
    nostr_tags_append(tags, comment_tag);
  }

  event.tags = tags;

  /* Serialize the event */
  char *event_json = nostr_event_serialize(&event);

  /* Clean up tags (event doesn't own them, we allocated them) */
  nostr_tags_free(tags);

  return event_json;
}

char *gnostr_highlight_build_from_note(const char *highlighted_text,
                                        const char *context,
                                        const char *comment,
                                        const char *note_event_id,
                                        const char *note_author_pubkey,
                                        const char *relay_hint) {
  return gnostr_highlight_build_event_json(
    highlighted_text,
    context,
    comment,
    note_event_id,    /* source_event_id */
    NULL,             /* source_a_tag */
    NULL,             /* source_url */
    note_author_pubkey,
    relay_hint
  );
}

char *gnostr_highlight_build_from_article(const char *highlighted_text,
                                           const char *context,
                                           const char *comment,
                                           int article_kind,
                                           const char *article_pubkey,
                                           const char *article_d_tag,
                                           const char *relay_hint) {
  if (!article_pubkey || !article_d_tag) {
    g_warning("NIP-84: Cannot create article highlight without pubkey and d-tag");
    return NULL;
  }

  /* Build the "a" tag value: kind:pubkey:d-tag */
  char *a_tag_value = g_strdup_printf("%d:%s:%s",
                                       article_kind,
                                       article_pubkey,
                                       article_d_tag);

  char *result = gnostr_highlight_build_event_json(
    highlighted_text,
    context,
    comment,
    NULL,             /* source_event_id */
    a_tag_value,      /* source_a_tag */
    NULL,             /* source_url */
    article_pubkey,
    relay_hint
  );

  g_free(a_tag_value);
  return result;
}

char *gnostr_highlight_build_from_url(const char *highlighted_text,
                                       const char *context,
                                       const char *comment,
                                       const char *url) {
  return gnostr_highlight_build_event_json(
    highlighted_text,
    context,
    comment,
    NULL,   /* source_event_id */
    NULL,   /* source_a_tag */
    url,    /* source_url */
    NULL,   /* author_pubkey */
    NULL    /* relay_hint */
  );
}

char *gnostr_highlight_get_source_description(const GnostrHighlight *highlight) {
  if (!highlight) return g_strdup("Unknown source");

  switch (highlight->source_type) {
    case GNOSTR_HIGHLIGHT_SOURCE_NOTE:
      if (highlight->source_event_id) {
        /* Truncate event ID for display */
        return g_strdup_printf("From note %s...",
                               g_strndup(highlight->source_event_id, 8));
      }
      return g_strdup("From a note");

    case GNOSTR_HIGHLIGHT_SOURCE_ARTICLE:
      if (highlight->source_a_tag) {
        /* Parse a-tag to extract d-tag for display */
        char **parts = g_strsplit(highlight->source_a_tag, ":", 3);
        if (parts && parts[2]) {
          char *desc = g_strdup_printf("From article \"%s\"", parts[2]);
          g_strfreev(parts);
          return desc;
        }
        g_strfreev(parts);
      }
      return g_strdup("From an article");

    case GNOSTR_HIGHLIGHT_SOURCE_URL:
      if (highlight->source_url) {
        /* Show domain for URL */
        GUri *uri = g_uri_parse(highlight->source_url, G_URI_FLAGS_NONE, NULL);
        if (uri) {
          const char *host = g_uri_get_host(uri);
          char *desc = g_strdup_printf("From %s", host ? host : highlight->source_url);
          g_uri_unref(uri);
          return desc;
        }
        return g_strdup_printf("From %s", highlight->source_url);
      }
      return g_strdup("From a URL");

    case GNOSTR_HIGHLIGHT_SOURCE_NONE:
    default:
      return g_strdup("Unknown source");
  }
}

char *gnostr_highlight_extract_context(const char *full_text,
                                        gsize selection_start,
                                        gsize selection_end,
                                        gsize context_chars) {
  if (!full_text || selection_start >= selection_end) return NULL;

  gsize text_len = strlen(full_text);
  if (selection_end > text_len) return NULL;

  /* Calculate context boundaries */
  gsize context_start = (selection_start > context_chars)
                        ? selection_start - context_chars
                        : 0;
  gsize context_end = (selection_end + context_chars < text_len)
                      ? selection_end + context_chars
                      : text_len;

  /* Try to find natural break points (sentence boundaries) */
  /* Look for sentence start before selection */
  if (context_start > 0) {
    const char *p = full_text + context_start;
    while (p > full_text && *p != '.' && *p != '!' && *p != '?' && *p != '\n') {
      p--;
    }
    if (p > full_text) {
      /* Skip the punctuation and any whitespace */
      p++;
      while (*p && g_ascii_isspace(*p)) p++;
      context_start = p - full_text;
    }
  }

  /* Look for sentence end after selection */
  if (context_end < text_len) {
    const char *p = full_text + context_end;
    while (*p && *p != '.' && *p != '!' && *p != '?' && *p != '\n') {
      p++;
    }
    if (*p) {
      p++; /* Include the punctuation */
      context_end = p - full_text;
    }
  }

  /* Extract the context */
  gsize context_len = context_end - context_start;
  char *context = g_strndup(full_text + context_start, context_len);

  /* Trim leading/trailing whitespace */
  char *trimmed = g_strstrip(context);
  if (trimmed != context) {
    char *result = g_strdup(trimmed);
    g_free(context);
    return result;
  }

  return context;
}
