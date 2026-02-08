/**
 * NIP-32 Labeling Support for gnostr
 *
 * Implements kind 1985 label events for categorizing/tagging content.
 * nostrc-3nj: Migrated from jansson to NostrJsonInterface
 */

#include "nip32_labels.h"
#include "../storage_ndb.h"
#include "nostr_json.h"
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
static gboolean parse_label_tag_cb(gsize index, const gchar *element_json, gpointer user_data) {
  (void)index;
  LabelParseContext *ctx = (LabelParseContext *)user_data;

  /* Each element should be an array (a tag) */
  if (!element_json || !gnostr_json_is_array_str(element_json)) return TRUE;

  /* Get tag length */
  size_t tag_len = 0;
  tag_len = gnostr_json_get_array_length(element_json, NULL, NULL);
  if (tag_len < 0 || tag_len < 2) {
    return TRUE;  /* Skip invalid tags */
  }

  /* Get tag name and value */
  char *tag_name = NULL;
  char *tag_value = NULL;
  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE;
  }
  tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  if (!tag_value) {
    free(tag_name);
    return TRUE;
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
      if ((ns_from_tag = gnostr_json_get_array_string(element_json, NULL, 2, NULL)) != NULL && ns_from_tag && *ns_from_tag) {
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
  return TRUE;  /* Continue iteration */
}

GPtrArray *gnostr_nip32_parse_label_event(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  /* Validate JSON */
  if (!gnostr_json_is_valid(event_json)) {
    g_warning("[NIP-32] Failed to parse label event JSON");
    return NULL;
  }

  /* Verify this is a kind 1985 event */
  int kind = gnostr_json_get_int(event_json, "kind", NULL);
  if (kind != NOSTR_KIND_LABEL) {
    return NULL;
  }

  /* Get event metadata */
  char *label_author = NULL;
  label_author = gnostr_json_get_string(event_json, "pubkey", NULL);

  int64_t created_at = 0;
  created_at = gnostr_json_get_int64(event_json, "created_at", NULL);

  /* Get tags array */
  char *tags_raw = NULL;
  tags_raw = gnostr_json_get_raw(event_json, "tags", NULL);
  if (!tags_raw) {
    free(label_author);
    return NULL;
  }

  if (!gnostr_json_is_array_str(tags_raw)) {
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

  gnostr_json_array_foreach_root(tags_raw, parse_label_tag_cb, &ctx);

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

  /* nostrc-3nj: Use GNostrJsonBuilder for JSON construction */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  if (!builder) return NULL;

  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NOSTR_KIND_LABEL);

  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* L tag (namespace) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "L");
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* l tag (label with namespace) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "l");
  gnostr_json_builder_add_string(builder, label);
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* e tag (event reference) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "e");
  gnostr_json_builder_add_string(builder, event_id_hex);
  gnostr_json_builder_end_array(builder);

  /* p tag (event author - recommended) */
  if (event_pubkey_hex && strlen(event_pubkey_hex) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, event_pubkey_hex);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);  /* End tags */
  gnostr_json_builder_end_object(builder);

  char *json_str = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  return json_str;
}

char *gnostr_nip32_build_profile_label_event_json(const char *namespace,
                                                   const char *label,
                                                   const char *pubkey_hex) {
  if (!namespace || !label || !pubkey_hex) return NULL;

  /* nostrc-3nj: Use GNostrJsonBuilder for JSON construction */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  if (!builder) return NULL;

  gnostr_json_builder_begin_object(builder);

  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, NOSTR_KIND_LABEL);

  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, (int64_t)time(NULL));

  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, "");

  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* L tag (namespace) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "L");
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* l tag (label with namespace) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "l");
  gnostr_json_builder_add_string(builder, label);
  gnostr_json_builder_add_string(builder, namespace);
  gnostr_json_builder_end_array(builder);

  /* p tag (pubkey reference) */
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_string(builder, "p");
  gnostr_json_builder_add_string(builder, pubkey_hex);
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_end_array(builder);  /* End tags */
  gnostr_json_builder_end_object(builder);

  char *json_str = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

  return json_str;
}

uint64_t gnostr_nip32_subscribe_labels(const char **event_ids, size_t count) {
  if (!event_ids || count == 0) return 0;

  /* nostrc-3nj: Use GNostrJsonBuilder for filter construction */
  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  if (!builder) return 0;

  gnostr_json_builder_begin_object(builder);

  /* kinds array */
  gnostr_json_builder_set_key(builder, "kinds");
  gnostr_json_builder_begin_array(builder);
  gnostr_json_builder_add_int(builder, NOSTR_KIND_LABEL);
  gnostr_json_builder_end_array(builder);

  /* #e array (event IDs) */
  gnostr_json_builder_set_key(builder, "#e");
  gnostr_json_builder_begin_array(builder);
  for (size_t i = 0; i < count; i++) {
    if (event_ids[i]) {
      gnostr_json_builder_add_string(builder, event_ids[i]);
    }
  }
  gnostr_json_builder_end_array(builder);

  gnostr_json_builder_set_key(builder, "limit");
  gnostr_json_builder_add_int(builder, 100);

  gnostr_json_builder_end_object(builder);

  char *filter_json = gnostr_json_builder_finish(builder);
  g_object_unref(builder);

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
