/**
 * NIP-32 Labeling Support for gnostr
 *
 * Implements kind 1985 label events for categorizing/tagging content.
 */

#include "nip32_labels.h"
#include "../storage_ndb.h"
#include <jansson.h>
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

GPtrArray *gnostr_nip32_parse_label_event(const char *event_json) {
  if (!event_json || !*event_json) return NULL;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("[NIP-32] Failed to parse label event JSON: %s", error.text);
    return NULL;
  }

  /* Verify this is a kind 1985 event */
  json_t *kind_obj = json_object_get(root, "kind");
  if (!json_is_integer(kind_obj) || json_integer_value(kind_obj) != NOSTR_KIND_LABEL) {
    json_decref(root);
    return NULL;
  }

  /* Get event metadata */
  json_t *pubkey_obj = json_object_get(root, "pubkey");
  const char *label_author = json_is_string(pubkey_obj) ? json_string_value(pubkey_obj) : NULL;

  json_t *created_at_obj = json_object_get(root, "created_at");
  gint64 created_at = json_is_integer(created_at_obj) ? json_integer_value(created_at_obj) : 0;

  /* Parse tags */
  json_t *tags = json_object_get(root, "tags");
  if (!json_is_array(tags)) {
    json_decref(root);
    return NULL;
  }

  GPtrArray *labels = g_ptr_array_new_with_free_func(gnostr_label_free);
  char *current_namespace = NULL;
  char *event_id = NULL;
  char *pubkey = NULL;

  size_t tag_count = json_array_size(tags);
  for (size_t i = 0; i < tag_count; i++) {
    json_t *tag = json_array_get(tags, i);
    if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

    const char *tag_name = json_string_value(json_array_get(tag, 0));
    const char *tag_value = json_string_value(json_array_get(tag, 1));
    if (!tag_name || !tag_value) continue;

    if (strcmp(tag_name, "L") == 0) {
      /* Namespace tag */
      g_free(current_namespace);
      current_namespace = g_strdup(tag_value);
    } else if (strcmp(tag_name, "l") == 0) {
      /* Label tag - may have namespace in 3rd element */
      const char *label_namespace = current_namespace;
      if (json_array_size(tag) >= 3) {
        const char *ns = json_string_value(json_array_get(tag, 2));
        if (ns && *ns) {
          label_namespace = ns;
        }
      }

      /* Create label entry */
      GnostrLabel *l = g_new0(GnostrLabel, 1);
      l->namespace = g_strdup(label_namespace);
      l->label = g_strdup(tag_value);
      l->event_id_hex = event_id ? g_strdup(event_id) : NULL;
      l->pubkey_hex = pubkey ? g_strdup(pubkey) : NULL;
      l->label_author = label_author ? g_strdup(label_author) : NULL;
      l->created_at = created_at;
      g_ptr_array_add(labels, l);
    } else if (strcmp(tag_name, "e") == 0) {
      /* Event reference */
      g_free(event_id);
      event_id = g_strdup(tag_value);
    } else if (strcmp(tag_name, "p") == 0) {
      /* Pubkey reference */
      g_free(pubkey);
      pubkey = g_strdup(tag_value);
    }
  }

  g_free(current_namespace);
  g_free(event_id);
  g_free(pubkey);
  json_decref(root);

  if (labels->len == 0) {
    g_ptr_array_unref(labels);
    return NULL;
  }

  return labels;
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

  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(NOSTR_KIND_LABEL));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string(""));

  /* Build tags array */
  json_t *tags = json_array();

  /* L tag (namespace) */
  json_t *L_tag = json_array();
  json_array_append_new(L_tag, json_string("L"));
  json_array_append_new(L_tag, json_string(namespace));
  json_array_append(tags, L_tag);
  json_decref(L_tag);

  /* l tag (label with namespace) */
  json_t *l_tag = json_array();
  json_array_append_new(l_tag, json_string("l"));
  json_array_append_new(l_tag, json_string(label));
  json_array_append_new(l_tag, json_string(namespace));
  json_array_append(tags, l_tag);
  json_decref(l_tag);

  /* e tag (event reference) */
  json_t *e_tag = json_array();
  json_array_append_new(e_tag, json_string("e"));
  json_array_append_new(e_tag, json_string(event_id_hex));
  json_array_append(tags, e_tag);
  json_decref(e_tag);

  /* p tag (event author - recommended) */
  if (event_pubkey_hex && strlen(event_pubkey_hex) == 64) {
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(event_pubkey_hex));
    json_array_append(tags, p_tag);
    json_decref(p_tag);
  }

  json_object_set_new(event_obj, "tags", tags);

  char *json_str = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  return json_str;
}

char *gnostr_nip32_build_profile_label_event_json(const char *namespace,
                                                   const char *label,
                                                   const char *pubkey_hex) {
  if (!namespace || !label || !pubkey_hex) return NULL;

  json_t *event_obj = json_object();
  json_object_set_new(event_obj, "kind", json_integer(NOSTR_KIND_LABEL));
  json_object_set_new(event_obj, "created_at", json_integer((json_int_t)time(NULL)));
  json_object_set_new(event_obj, "content", json_string(""));

  /* Build tags array */
  json_t *tags = json_array();

  /* L tag (namespace) */
  json_t *L_tag = json_array();
  json_array_append_new(L_tag, json_string("L"));
  json_array_append_new(L_tag, json_string(namespace));
  json_array_append(tags, L_tag);
  json_decref(L_tag);

  /* l tag (label with namespace) */
  json_t *l_tag = json_array();
  json_array_append_new(l_tag, json_string("l"));
  json_array_append_new(l_tag, json_string(label));
  json_array_append_new(l_tag, json_string(namespace));
  json_array_append(tags, l_tag);
  json_decref(l_tag);

  /* p tag (pubkey reference) */
  json_t *p_tag = json_array();
  json_array_append_new(p_tag, json_string("p"));
  json_array_append_new(p_tag, json_string(pubkey_hex));
  json_array_append(tags, p_tag);
  json_decref(p_tag);

  json_object_set_new(event_obj, "tags", tags);

  char *json_str = json_dumps(event_obj, JSON_COMPACT);
  json_decref(event_obj);

  return json_str;
}

uint64_t gnostr_nip32_subscribe_labels(const char **event_ids, size_t count) {
  if (!event_ids || count == 0) return 0;

  /* Build JSON array of event IDs */
  json_t *e_array = json_array();
  for (size_t i = 0; i < count; i++) {
    if (event_ids[i]) {
      json_array_append_new(e_array, json_string(event_ids[i]));
    }
  }

  json_t *filter = json_object();
  json_object_set_new(filter, "kinds", json_pack("[i]", NOSTR_KIND_LABEL));
  json_object_set_new(filter, "#e", e_array);
  json_object_set_new(filter, "limit", json_integer(100));

  char *filter_json = json_dumps(filter, JSON_COMPACT);
  json_decref(filter);

  if (!filter_json) return 0;

  uint64_t subid = storage_ndb_subscribe(filter_json);
  g_free(filter_json);

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
