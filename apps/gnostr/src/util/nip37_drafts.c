/*
 * nip37_drafts.c - NIP-37 Draft Events utility library
 *
 * Implementation of NIP-37 draft event parsing and building functions.
 */

#define G_LOG_DOMAIN "nip37-drafts"

#include "nip37_drafts.h"
#include <nostr-gobject-1.0/nostr_json.h>
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

  int kind = gnostr_json_get_int(json_str, "kind", NULL);
  return kind;
}

gboolean
gnostr_nip37_is_draft_event(const gchar *event_json)
{
  if (!event_json || !*event_json) return FALSE;

  int kind = gnostr_json_get_int(event_json, "kind", NULL);
  return kind == NIP37_KIND_DRAFT;
}

/* Callback context for parsing draft tags */
typedef struct {
  GnostrNip37Draft *draft;
} DraftParseCtx;

static gboolean
nip37_tag_callback(gsize index, const gchar *element_json, gpointer user_data)
{
  (void)index;
  DraftParseCtx *ctx = user_data;

  /* Each element is a tag array like ["d", "value"] */
  char *tag_name = NULL;
  char *tag_value = NULL;

  tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL);
  if (!tag_name) {
    return TRUE; /* Continue iteration */
  }

  tag_value = gnostr_json_get_array_string(element_json, NULL, 1, NULL);
  if (!tag_value) {
    free(tag_name);
    return TRUE;
  }

  if (g_strcmp0(tag_name, "d") == 0) {
    g_free(ctx->draft->draft_id);
    ctx->draft->draft_id = g_strdup(tag_value);
  } else if (g_strcmp0(tag_name, "k") == 0) {
    ctx->draft->target_kind = (gint)g_ascii_strtoll(tag_value, NULL, 10);
  } else if (g_strcmp0(tag_name, "e") == 0 && !ctx->draft->edit_event_id) {
    ctx->draft->edit_event_id = g_strdup(tag_value);
  } else if (g_strcmp0(tag_name, "a") == 0 && !ctx->draft->edit_addr) {
    ctx->draft->edit_addr = g_strdup(tag_value);
  }

  free(tag_name);
  free(tag_value);
  return TRUE; /* Continue iteration */
}

GnostrNip37Draft *
gnostr_nip37_draft_parse(const gchar *event_json)
{
  if (!event_json || !*event_json) return NULL;

  if (!gnostr_json_is_valid(event_json)) {
    g_warning("nip37: failed to parse event JSON");
    return NULL;
  }

  /* Verify kind is 31234 */
  int kind = gnostr_json_get_int(event_json, "kind", NULL);
  if (kind != NIP37_KIND_DRAFT) {
    g_debug("nip37: event is not a draft (kind != 31234)");
    return NULL;
  }

  GnostrNip37Draft *draft = gnostr_nip37_draft_new();

  /* Extract created_at */
  int64_t created_at = 0;
  if ((created_at = gnostr_json_get_int64(event_json, "created_at", NULL), TRUE)) {
    draft->created_at = created_at;
  }

  /* Extract content (the inner draft event JSON) */
  char *content = NULL;
  content = gnostr_json_get_string(event_json, "content", NULL);
  if (content) {
    draft->draft_json = g_strdup(content);
    free(content);
  }

  /* Parse tags for draft metadata */
  DraftParseCtx ctx = { .draft = draft };
  gnostr_json_array_foreach(event_json, "tags", nip37_tag_callback, &ctx);

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

  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_array(builder);

  /* "d" tag - required */
  if (draft->draft_id && *draft->draft_id) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "d");
    gnostr_json_builder_add_string(builder, draft->draft_id);
    gnostr_json_builder_end_array(builder);
  }

  /* "k" tag - target kind */
  if (draft->target_kind > 0) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "k");
    gchar *kind_str = g_strdup_printf("%d", draft->target_kind);
    gnostr_json_builder_add_string(builder, kind_str);
    g_free(kind_str);
    gnostr_json_builder_end_array(builder);
  }

  /* "e" tag - event being edited */
  if (draft->edit_event_id && *draft->edit_event_id) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, draft->edit_event_id);
    gnostr_json_builder_end_array(builder);
  }

  /* "a" tag - addressable event being edited */
  if (draft->edit_addr && *draft->edit_addr) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "a");
    gnostr_json_builder_add_string(builder, draft->edit_addr);
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);

  char *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);
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
