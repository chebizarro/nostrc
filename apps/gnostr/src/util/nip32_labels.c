/**
 * NIP-32 Labeling Support for gnostr
 *
 * Implements kind 1985 label events for categorizing/tagging content.
 * nostrc-3nj: Migrated from jansson to NostrJsonInterface
 */

#include "nip32_labels.h"
#include "../storage_ndb.h"
#include <json.h>
#include <string.h>
#include <time.h>

/* Predefined labels for quick access */
static const GnostrPredefinedLabel predefined_labels[] = {
  { NIP32_NS_UGC, "good", "Good Content" },
  { NIP32_NS_UGC, "interesting", "Interesting" },
  { NIP32_NS_UGC, "informative", "Informative" },
  { NIP32_NS_UGC, "funny", "Funny" },
  { NIP32_NS_UGC, "spam", "Spam" },
  { NIP32_NS_UGC, "nsfw", "NSFW" },
  { NIP32_NS_QUALITY, "high", "High Quality" },
  { NIP32_NS_QUALITY, "low", "Low Quality" },
  { "topic", "bitcoin", "Bitcoin" },
  { "topic", "nostr", "Nostr" },
  { "topic", "lightning", "Lightning" },
  { "topic", "tech", "Technology" },
  { "topic", "news", "News" },
  { "topic", "art", "Art" },
  { "topic", "music", "Music" },
  { NULL, NULL, NULL }  /* Terminator */
};

const GnostrPredefinedLabel *gnostr_nip32_get_predefined_labels(void) {
  return predefined_labels;
}

void gnostr_label_free(gpointer label) {
  GnostrLabel *l = (GnostrLabel *)label;
  if (!l) return;
  g_free(l->namespace);
  g_free(l->label);
  g_free(l->event_id_hex);
  g_free(l->pubkey_hex);
  g_free(l->label_author);
  g_free(l);
}

void gnostr_event_labels_free(gpointer event_labels) {
  GnostrEventLabels *el = (GnostrEventLabels *)event_labels;
  if (!el) return;
  g_free(el->event_id_hex);
  if (el->labels) {
    g_ptr_array_unref(el->labels);
  }
  g_free(el);
}

/* nostrc-3nj: Context for tag iteration */
typedef struct {
  GPtrArray *labels;
  char *current_namespace;
  char *event_id;
  char *pubkey;
  char *label_author;
  gint64 created_at;
} LabelParseContext;

/* nostrc-3nj: Callback for iterating tags array */
static bool parse_label_tag_cb(size_t index, const char *element_json, void *user_data) {
  (void)index;
  LabelParseContext *ctx = (LabelParseContext *)user_data;

  /* Each element should be an array (a tag) */
  if (!element_json || !nostr_json_is_array_str(element_json)) return true;

  /* Get tag length */
  size_t tag_len = 0;
  if (nostr_json_get_array_length(element_json, NULL, &tag_len) != 0 || tag_len < 2) {
    return true;  /* Skip invalid tags */
  }

  /* Get tag name and value */
  char *tag_name = NULL;
  char *tag_value = NULL;
  if (nostr_json_get_array_string(element_json, NULL, 0, &tag_name) != 0 || !tag_name) {
    return true;
  }
  if (nostr_json_get_array_string(element_json, NULL, 1, &tag_value) != 0 || !tag_value) {
    free(tag_name);
    return true;
  }

  if (strcmp(tag_name, "L") == 0) {
    /* Namespace tag */
    g_free(ctx->current_namespace);
    ctx->current_namespace = g_strdup(tag_value);
  } else if (strcmp(tag_name, "l") == 0) {
    /* Label tag - may have namespace in 3rd element */
    const char *label_namespace = ctx->current_namespace;
    char *ns_from_tag = NULL;
    if (tag_len >= 3) {
      if (nostr_json_get_array_string(element_json, NULL, 2, &ns_from_tag) == 0 && ns_from_tag && *ns_from_tag) {
        label_namespace = ns_from_tag;
      }
    }

    /* Create label entry */
    GnostrLabel *l = g_new0(GnostrLabel, 1);
    l->namespace = g_strdup(label_namespace);
    l->label = g_strdup(tag_value);
    l->event_id_hex = ctx->event_id ? g_strdup(ctx->event_id) : NULL;
    l->pubkey_hex = ctx->pubkey ? g_strdup(ctx->pubkey) : NULL;
    l->label_author = ctx->label_author ? g_strdup(ctx->label_author) : NULL;
    l->created_at = ctx->created_at;
    g_ptr_array_add(ctx->labels, l);

    free(ns_from_tag);
  } else if (strcmp(tag_name, "e") == 0) {
    /* Event reference */
    g_free(ctx->event_id);
    ctx->event_id = g_strdup(tag_value);
  } else if (strcmp(tag_name, "p") == 0) {
    /* Pubkey reference */
    g_free(ctx->pubkey);
    ctx->pubkey = g_strdup(tag_value);
  }

  free(tag_name);
  free(tag_value);
  return true;  /* Continue iteration */
}

GPtrArray *gnostr_nip32_parse_label_event(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  /* Validate JSON */
  if (!nostr_json_is_valid(event_json)) {
    g_warning("[NIP-32] Failed to parse label event JSON");
    return NULL;
  }

  /* Verify this is a kind 1985 event */
  int kind = 0;
  if (nostr_json_get_int(event_json, "kind", &kind) != 0 || kind != NOSTR_KIND_LABEL) {
    return NULL;
  }

  /* Get event metadata */
  char *label_author = NULL;
  nostr_json_get_string(event_json, "pubkey", &label_author);

  int64_t created_at = 0;
  nostr_json_get_int64(event_json, "created_at", &created_at);

  /* Get tags array */
  char *tags_raw = NULL;
  if (nostr_json_get_raw(event_json, "tags", &tags_raw) != 0 || !tags_raw) {
    free(label_author);
    return NULL;
  }

  if (!nostr_json_is_array_str(tags_raw)) {
    free(tags_raw);
    free(label_author);
    return NULL;
  }

  /* Parse tags using iteration */
  LabelParseContext ctx = {
    .labels = g_ptr_array_new_with_free_func(gnostr_label_free),
    .current_namespace = NULL,
    .event_id = NULL,
    .pubkey = NULL,
    .label_author = label_author,
    .created_at = created_at
  };

  nostr_json_array_foreach_root(tags_raw, parse_label_tag_cb, &ctx);

  /* Cleanup */
  free(tags_raw);
  g_free(ctx.current_namespace);
  g_free(ctx.event_id);
  g_free(ctx.pubkey);
  free(label_author);

  if (ctx.labels->len == 0) {
    g_ptr_array_unref(ctx.labels);
    return NULL;
  }

  return ctx.labels;
}

GnostrEventLabels *gnostr_nip32_get_labels_for_event(const char *event_id_hex) {
  if (!event_id_hex || strlen(event_id_hex) != 64) return NULL;

  /* Build filter for kind 1985 events that reference this event */
  char *filter_json = g_strdup_printf(
    "{\"kinds\":[%d],\"#e\":[\"%s\"],\"limit\":50}",
    NOSTR_KIND_LABEL, event_id_hex);

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0) {
    g_free(filter_json);
    return NULL;
  }

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  storage_ndb_end_query(txn);
  g_free(filter_json);

  if (rc != 0 || count == 0 || !results) {
    return NULL;
  }

  GnostrEventLabels *event_labels = g_new0(GnostrEventLabels, 1);
  event_labels->event_id_hex = g_strdup(event_id_hex);
  event_labels->labels = g_ptr_array_new_with_free_func(gnostr_label_free);

  for (int i = 0; i < count; i++) {
    if (!results[i]) continue;
    GPtrArray *parsed = gnostr_nip32_parse_label_event(results[i]);
    if (parsed) {
      for (guint j = 0; j < parsed->len; j++) {
        GnostrLabel *l = g_ptr_array_index(parsed, j);
        if (l) {
          /* Transfer ownership */
          g_ptr_array_add(event_labels->labels, l);
        }
      }
      /* Don't free the labels, just the array container */
      g_ptr_array_free(parsed, FALSE);
    }
  }

  storage_ndb_free_results(results, count);

  if (event_labels->labels->len == 0) {
    gnostr_event_labels_free(event_labels);
    return NULL;
  }

  return event_labels;
}

GPtrArray *gnostr_nip32_get_labels_by_user(const char *pubkey_hex) {
  if (!pubkey_hex || strlen(pubkey_hex) != 64) return NULL;

  /* Build filter for kind 1985 events by this author */
  char *filter_json = g_strdup_printf(
    "{\"kinds\":[%d],\"authors\":[\"%s\"],\"limit\":100}",
    NOSTR_KIND_LABEL, pubkey_hex);

  void *txn = NULL;
  if (storage_ndb_begin_query_retry(&txn, 3, 10) != 0) {
    g_free(filter_json);
    return NULL;
  }

  char **results = NULL;
  int count = 0;
  int rc = storage_ndb_query(txn, filter_json, &results, &count);
  storage_ndb_end_query(txn);
  g_free(filter_json);

  if (rc != 0 || count == 0 || !results) {
    return NULL;
  }

  GPtrArray *all_labels = g_ptr_array_new_with_free_func(gnostr_label_free);

  for (int i = 0; i < count; i++) {
    if (!results[i]) continue;
    GPtrArray *parsed = gnostr_nip32_parse_label_event(results[i]);
    if (parsed) {
      for (guint j = 0; j < parsed->len; j++) {
        GnostrLabel *l = g_ptr_array_index(parsed, j);
        if (l) {
          g_ptr_array_add(all_labels, l);
        }
      }
      g_ptr_array_free(parsed, FALSE);
    }
  }

  storage_ndb_free_results(results, count);

  if (all_labels->len == 0) {
    g_ptr_array_unref(all_labels);
    return NULL;
  }

  return all_labels;
}

char *gnostr_nip32_build_label_event_json(const char *namespace,
                                           const char *label,
                                           const char *event_id_hex,
                                           const char *event_pubkey_hex) {
  if (!namespace || !label || !event_id_hex) return NULL;

  /* nostrc-3nj: Use NostrJsonBuilder for JSON construction */
  NostrJsonBuilder *builder = nostr_json_builder_new();
  if (!builder) return NULL;

  nostr_json_builder_begin_object(builder);

  nostr_json_builder_set_key(builder, "kind");
  nostr_json_builder_add_int(builder, NOSTR_KIND_LABEL);

  nostr_json_builder_set_key(builder, "created_at");
  nostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  nostr_json_builder_set_key(builder, "content");
  nostr_json_builder_add_string(builder, "");

  nostr_json_builder_set_key(builder, "tags");
  nostr_json_builder_begin_array(builder);

  /* L tag (namespace) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "L");
  nostr_json_builder_add_string(builder, namespace);
  nostr_json_builder_end_array(builder);

  /* l tag (label with namespace) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "l");
  nostr_json_builder_add_string(builder, label);
  nostr_json_builder_add_string(builder, namespace);
  nostr_json_builder_end_array(builder);

  /* e tag (event reference) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "e");
  nostr_json_builder_add_string(builder, event_id_hex);
  nostr_json_builder_end_array(builder);

  /* p tag (event author - recommended) */
  if (event_pubkey_hex && strlen(event_pubkey_hex) == 64) {
    nostr_json_builder_begin_array(builder);
    nostr_json_builder_add_string(builder, "p");
    nostr_json_builder_add_string(builder, event_pubkey_hex);
    nostr_json_builder_end_array(builder);
  }

  nostr_json_builder_end_array(builder);  /* End tags */
  nostr_json_builder_end_object(builder);

  char *json_str = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  return json_str;
}

char *gnostr_nip32_build_profile_label_event_json(const char *namespace,
                                                   const char *label,
                                                   const char *pubkey_hex) {
  if (!namespace || !label || !pubkey_hex) return NULL;

  /* nostrc-3nj: Use NostrJsonBuilder for JSON construction */
  NostrJsonBuilder *builder = nostr_json_builder_new();
  if (!builder) return NULL;

  nostr_json_builder_begin_object(builder);

  nostr_json_builder_set_key(builder, "kind");
  nostr_json_builder_add_int(builder, NOSTR_KIND_LABEL);

  nostr_json_builder_set_key(builder, "created_at");
  nostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  nostr_json_builder_set_key(builder, "content");
  nostr_json_builder_add_string(builder, "");

  nostr_json_builder_set_key(builder, "tags");
  nostr_json_builder_begin_array(builder);

  /* L tag (namespace) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "L");
  nostr_json_builder_add_string(builder, namespace);
  nostr_json_builder_end_array(builder);

  /* l tag (label with namespace) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "l");
  nostr_json_builder_add_string(builder, label);
  nostr_json_builder_add_string(builder, namespace);
  nostr_json_builder_end_array(builder);

  /* p tag (pubkey reference) */
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_string(builder, "p");
  nostr_json_builder_add_string(builder, pubkey_hex);
  nostr_json_builder_end_array(builder);

  nostr_json_builder_end_array(builder);  /* End tags */
  nostr_json_builder_end_object(builder);

  char *json_str = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  return json_str;
}

uint64_t gnostr_nip32_subscribe_labels(const char **event_ids, size_t count) {
  if (!event_ids || count == 0) return 0;

  /* nostrc-3nj: Use NostrJsonBuilder for filter construction */
  NostrJsonBuilder *builder = nostr_json_builder_new();
  if (!builder) return 0;

  nostr_json_builder_begin_object(builder);

  /* kinds array */
  nostr_json_builder_set_key(builder, "kinds");
  nostr_json_builder_begin_array(builder);
  nostr_json_builder_add_int(builder, NOSTR_KIND_LABEL);
  nostr_json_builder_end_array(builder);

  /* #e array (event IDs) */
  nostr_json_builder_set_key(builder, "#e");
  nostr_json_builder_begin_array(builder);
  for (size_t i = 0; i < count; i++) {
    if (event_ids[i]) {
      nostr_json_builder_add_string(builder, event_ids[i]);
    }
  }
  nostr_json_builder_end_array(builder);

  nostr_json_builder_set_key(builder, "limit");
  nostr_json_builder_add_int(builder, 100);

  nostr_json_builder_end_object(builder);

  char *filter_json = nostr_json_builder_finish(builder);
  nostr_json_builder_free(builder);

  if (!filter_json) return 0;

  uint64_t subid = storage_ndb_subscribe(filter_json);
  free(filter_json);

  return subid;
}

char *gnostr_nip32_format_label(const GnostrLabel *label) {
  if (!label || !label->label) return NULL;

  /* For well-known namespaces, just show the label */
  if (label->namespace &&
      (strcmp(label->namespace, NIP32_NS_UGC) == 0 ||
       strcmp(label->namespace, "topic") == 0)) {
    return g_strdup(label->label);
  }

  /* For other namespaces, show namespace:label */
  if (label->namespace && *label->namespace) {
    return g_strdup_printf("%s:%s", label->namespace, label->label);
  }

  return g_strdup(label->label);
}
