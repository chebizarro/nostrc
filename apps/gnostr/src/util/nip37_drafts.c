/*
 * nip37_drafts.c - NIP-37 Draft Events utility library
 *
 * Implementation of NIP-37 draft event parsing and building functions.
 */

#define G_LOG_DOMAIN "nip37-drafts"

#include "nip37_drafts.h"
#include <jansson.h>
#include <string.h>
#include <time.h>

/* ============== Draft Lifecycle ============== */

GnostrNip37Draft *
gnostr_nip37_draft_new(void)
{
  GnostrNip37Draft *draft = g_new0(GnostrNip37Draft, 1);
  draft->target_kind = 0;
  draft->created_at = (gint64)time(NULL);
  return draft;
}

void
gnostr_nip37_draft_free(GnostrNip37Draft *draft)
{
  if (!draft) return;
  g_free(draft->draft_id);
  g_free(draft->draft_json);
  g_free(draft->edit_event_id);
  g_free(draft->edit_addr);
  g_free(draft);
}

/* ============== Parsing ============== */

/**
 * Extract kind from an inner event JSON string.
 */
static gint
extract_kind_from_json(const gchar *json_str)
{
  if (!json_str || !*json_str) return 0;

  json_error_t error;
  json_t *root = json_loads(json_str, 0, &error);
  if (!root) return 0;

  json_t *kind_val = json_object_get(root, "kind");
  gint kind = 0;
  if (kind_val && json_is_integer(kind_val)) {
    kind = (gint)json_integer_value(kind_val);
  }

  json_decref(root);
  return kind;
}

gboolean
gnostr_nip37_is_draft_event(const gchar *event_json)
{
  if (!event_json || !*event_json) return FALSE;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) return FALSE;

  json_t *kind_val = json_object_get(root, "kind");
  gboolean is_draft = FALSE;
  if (kind_val && json_is_integer(kind_val)) {
    is_draft = (json_integer_value(kind_val) == NIP37_KIND_DRAFT);
  }

  json_decref(root);
  return is_draft;
}

GnostrNip37Draft *
gnostr_nip37_draft_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  json_error_t error;
  json_t *root = json_loads(event_json, 0, &error);
  if (!root) {
    g_warning("nip37: failed to parse event JSON: %s", error.text);
    return NULL;
  }

  /* Verify kind is 31234 */
  json_t *kind_val = json_object_get(root, "kind");
  if (!kind_val || json_integer_value(kind_val) != NIP37_KIND_DRAFT) {
    g_debug("nip37: event is not a draft (kind != 31234)");
    json_decref(root);
    return NULL;
  }

  GnostrNip37Draft *draft = gnostr_nip37_draft_new();

  /* Extract created_at */
  json_t *created_val = json_object_get(root, "created_at");
  if (created_val && json_is_integer(created_val)) {
    draft->created_at = json_integer_value(created_val);
  }

  /* Extract content (the inner draft event JSON) */
  json_t *content_val = json_object_get(root, "content");
  if (content_val && json_is_string(content_val)) {
    draft->draft_json = g_strdup(json_string_value(content_val));
  }

  /* Parse tags for draft metadata */
  json_t *tags = json_object_get(root, "tags");
  if (tags && json_is_array(tags)) {
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_value = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "d") == 0) {
        /* Draft identifier */
        g_free(draft->draft_id);
        draft->draft_id = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "k") == 0) {
        /* Target kind */
        draft->target_kind = (gint)g_ascii_strtoll(tag_value, NULL, 10);
      } else if (g_strcmp0(tag_name, "e") == 0 && !draft->edit_event_id) {
        /* Event being edited (first "e" tag) */
        draft->edit_event_id = g_strdup(tag_value);
      } else if (g_strcmp0(tag_name, "a") == 0 && !draft->edit_addr) {
        /* Addressable event being edited (first "a" tag) */
        draft->edit_addr = g_strdup(tag_value);
      }
    }
  }

  json_decref(root);

  /* Validate: must have draft_id (d tag) */
  if (!draft->draft_id || !*draft->draft_id) {
    g_debug("nip37: draft event missing 'd' tag");
    gnostr_nip37_draft_free(draft);
    return NULL;
  }

  /* If target_kind not set via "k" tag, try to extract from inner event */
  if (draft->target_kind == 0 && draft->draft_json) {
    draft->target_kind = extract_kind_from_json(draft->draft_json);
  }

  g_debug("nip37: parsed draft id=%s kind=%d edit_event=%s edit_addr=%s",
          draft->draft_id,
          draft->target_kind,
          draft->edit_event_id ? draft->edit_event_id : "(none)",
          draft->edit_addr ? draft->edit_addr : "(none)");

  return draft;
}

/* ============== Building ============== */

gchar *
gnostr_nip37_draft_build_tags(const GnostrNip37Draft *draft)
{
  if (!draft) return g_strdup("[]");

  json_t *tags = json_array();

  /* "d" tag - required */
  if (draft->draft_id && *draft->draft_id) {
    json_t *d_tag = json_array();
    json_array_append_new(d_tag, json_string("d"));
    json_array_append_new(d_tag, json_string(draft->draft_id));
    json_array_append_new(tags, d_tag);
  }

  /* "k" tag - target kind */
  if (draft->target_kind > 0) {
    json_t *k_tag = json_array();
    json_array_append_new(k_tag, json_string("k"));
    gchar *kind_str = g_strdup_printf("%d", draft->target_kind);
    json_array_append_new(k_tag, json_string(kind_str));
    g_free(kind_str);
    json_array_append_new(tags, k_tag);
  }

  /* "e" tag - event being edited */
  if (draft->edit_event_id && *draft->edit_event_id) {
    json_t *e_tag = json_array();
    json_array_append_new(e_tag, json_string("e"));
    json_array_append_new(e_tag, json_string(draft->edit_event_id));
    json_array_append_new(tags, e_tag);
  }

  /* "a" tag - addressable event being edited */
  if (draft->edit_addr && *draft->edit_addr) {
    json_t *a_tag = json_array();
    json_array_append_new(a_tag, json_string("a"));
    json_array_append_new(a_tag, json_string(draft->edit_addr));
    json_array_append_new(tags, a_tag);
  }

  gchar *result = json_dumps(tags, JSON_COMPACT);
  json_decref(tags);
  return result;
}

/* ============== Content Access ============== */

const gchar *
gnostr_nip37_draft_get_content(const GnostrNip37Draft *draft)
{
  if (!draft) return NULL;
  return draft->draft_json;
}

void
gnostr_nip37_draft_set_content(GnostrNip37Draft *draft, const gchar *content_json)
{
  if (!draft) return;

  g_free(draft->draft_json);
  draft->draft_json = g_strdup(content_json);

  /* Try to extract target_kind from the inner event */
  if (content_json && *content_json) {
    gint extracted_kind = extract_kind_from_json(content_json);
    if (extracted_kind > 0) {
      draft->target_kind = extracted_kind;
    }
  }
}

gint
gnostr_nip37_draft_get_target_kind(const GnostrNip37Draft *draft)
{
  if (!draft) return 0;

  /* Return explicit k tag value if set */
  if (draft->target_kind > 0) {
    return draft->target_kind;
  }

  /* Try to extract from inner event JSON */
  if (draft->draft_json && *draft->draft_json) {
    return extract_kind_from_json(draft->draft_json);
  }

  return 0;
}

/* ============== Utilities ============== */

gchar *
gnostr_nip37_draft_generate_id(void)
{
  /* Generate a unique identifier: timestamp + random bytes */
  guint64 ts = (guint64)g_get_real_time();
  guint32 rand_val = g_random_int();
  return g_strdup_printf("draft-%lu-%08x", (unsigned long)ts, rand_val);
}
