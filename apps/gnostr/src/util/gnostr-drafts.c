/**
 * GnostrDrafts - NIP-37 Draft Events Implementation
 *
 * Manages draft events (kind 31234) for saving work-in-progress notes.
 */

#define G_LOG_DOMAIN "gnostr-drafts"

#include "gnostr-drafts.h"
#include "nostr_json.h"
#include <glib/gstdio.h>
#include <time.h>
#include <string.h>

/* Local storage paths */
static char *get_drafts_dir(void);
static char *get_draft_file_path(const char *d_tag);

struct _GnostrDrafts {
  GObject parent_instance;
  char *user_pubkey;     /* Current user pubkey (hex) */
  GMutex lock;           /* Thread safety */
  GHashTable *cache;     /* d_tag -> GnostrDraft* (local cache) */
};

G_DEFINE_TYPE(GnostrDrafts, gnostr_drafts, G_TYPE_OBJECT)

/* Singleton instance */
static GnostrDrafts *default_instance = NULL;

static void gnostr_drafts_finalize(GObject *obj) {
  GnostrDrafts *self = GNOSTR_DRAFTS(obj);

  g_mutex_lock(&self->lock);
  g_free(self->user_pubkey);
  self->user_pubkey = NULL;
  if (self->cache) {
    g_hash_table_destroy(self->cache);
    self->cache = NULL;
  }
  g_mutex_unlock(&self->lock);
  g_mutex_clear(&self->lock);

  G_OBJECT_CLASS(gnostr_drafts_parent_class)->finalize(obj);
}

static void gnostr_drafts_class_init(GnostrDraftsClass *klass) {
  GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
  gobject_class->finalize = gnostr_drafts_finalize;
}

static void gnostr_drafts_init(GnostrDrafts *self) {
  g_mutex_init(&self->lock);
  self->user_pubkey = NULL;
  self->cache = g_hash_table_new_full(g_str_hash, g_str_equal,
                                       g_free, (GDestroyNotify)gnostr_draft_free);
}

GnostrDrafts *gnostr_drafts_new(void) {
  return g_object_new(GNOSTR_TYPE_DRAFTS, NULL);
}

GnostrDrafts *gnostr_drafts_get_default(void) {
  if (!default_instance) {
    default_instance = gnostr_drafts_new();
  }
  return default_instance;
}

void gnostr_drafts_set_user_pubkey(GnostrDrafts *self, const char *pubkey_hex) {
  g_return_if_fail(GNOSTR_IS_DRAFTS(self));

  g_mutex_lock(&self->lock);
  g_free(self->user_pubkey);
  self->user_pubkey = g_strdup(pubkey_hex);
  g_mutex_unlock(&self->lock);

  g_message("drafts: set user pubkey: %.16s...", pubkey_hex ? pubkey_hex : "(null)");
}

/* ---- GnostrDraft lifecycle ---- */

GnostrDraft *gnostr_draft_new(void) {
  GnostrDraft *draft = g_new0(GnostrDraft, 1);
  draft->target_kind = 1; /* Default to text note */
  draft->created_at = (gint64)time(NULL);
  draft->updated_at = draft->created_at;
  return draft;
}

void gnostr_draft_free(GnostrDraft *draft) {
  if (!draft) return;
  g_clear_pointer(&draft->d_tag, g_free);
  g_clear_pointer(&draft->content, g_free);
  g_clear_pointer(&draft->subject, g_free);
  g_clear_pointer(&draft->reply_to_id, g_free);
  g_clear_pointer(&draft->root_id, g_free);
  g_clear_pointer(&draft->reply_to_pubkey, g_free);
  g_clear_pointer(&draft->quote_id, g_free);
  g_clear_pointer(&draft->quote_pubkey, g_free);
  g_clear_pointer(&draft->quote_nostr_uri, g_free);
  g_free(draft);
}

GnostrDraft *gnostr_draft_copy(const GnostrDraft *draft) {
  if (!draft) return NULL;

  GnostrDraft *copy = g_new0(GnostrDraft, 1);
  copy->d_tag = g_strdup(draft->d_tag);
  copy->target_kind = draft->target_kind;
  copy->content = g_strdup(draft->content);
  copy->subject = g_strdup(draft->subject);
  copy->reply_to_id = g_strdup(draft->reply_to_id);
  copy->root_id = g_strdup(draft->root_id);
  copy->reply_to_pubkey = g_strdup(draft->reply_to_pubkey);
  copy->quote_id = g_strdup(draft->quote_id);
  copy->quote_pubkey = g_strdup(draft->quote_pubkey);
  copy->quote_nostr_uri = g_strdup(draft->quote_nostr_uri);
  copy->created_at = draft->created_at;
  copy->updated_at = draft->updated_at;
  copy->is_sensitive = draft->is_sensitive;
  return copy;
}

char *gnostr_draft_generate_d_tag(void) {
  /* Generate a unique identifier: timestamp + random bytes */
  guint64 ts = (guint64)g_get_real_time();
  guint32 rand = g_random_int();
  return g_strdup_printf("draft-%lu-%08x", (unsigned long)ts, rand);
}

/* ---- JSON serialization ---- */

char *gnostr_draft_to_json(const GnostrDraft *draft) {
  if (!draft) return NULL;

  GNostrJsonBuilder *builder = gnostr_json_builder_new();
  gnostr_json_builder_begin_object(builder);

  /* Required fields */
  gnostr_json_builder_set_key(builder, "kind");
  gnostr_json_builder_add_int(builder, draft->target_kind);

  gnostr_json_builder_set_key(builder, "content");
  gnostr_json_builder_add_string(builder, draft->content ? draft->content : "");

  gnostr_json_builder_set_key(builder, "created_at");
  gnostr_json_builder_add_int64(builder, draft->created_at);

  /* Build tags array */
  gnostr_json_builder_set_key(builder, "tags");
  gnostr_json_builder_begin_array(builder);

  /* NIP-14: Subject tag */
  if (draft->subject && *draft->subject) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "subject");
    gnostr_json_builder_add_string(builder, draft->subject);
    gnostr_json_builder_end_array(builder);
  }

  /* NIP-10: Reply context */
  if (draft->root_id && strlen(draft->root_id) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, draft->root_id);
    gnostr_json_builder_add_string(builder, ""); /* relay hint */
    gnostr_json_builder_add_string(builder, "root");
    gnostr_json_builder_end_array(builder);
  }

  if (draft->reply_to_id && strlen(draft->reply_to_id) == 64 &&
      (!draft->root_id || strcmp(draft->reply_to_id, draft->root_id) != 0)) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "e");
    gnostr_json_builder_add_string(builder, draft->reply_to_id);
    gnostr_json_builder_add_string(builder, ""); /* relay hint */
    gnostr_json_builder_add_string(builder, "reply");
    gnostr_json_builder_end_array(builder);
  }

  if (draft->reply_to_pubkey && strlen(draft->reply_to_pubkey) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, draft->reply_to_pubkey);
    gnostr_json_builder_end_array(builder);
  }

  /* NIP-18: Quote context */
  if (draft->quote_id && strlen(draft->quote_id) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "q");
    gnostr_json_builder_add_string(builder, draft->quote_id);
    gnostr_json_builder_add_string(builder, ""); /* relay hint */
    gnostr_json_builder_end_array(builder);
  }

  if (draft->quote_pubkey && strlen(draft->quote_pubkey) == 64) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "p");
    gnostr_json_builder_add_string(builder, draft->quote_pubkey);
    gnostr_json_builder_end_array(builder);
  }

  /* NIP-36: Content warning */
  if (draft->is_sensitive) {
    gnostr_json_builder_begin_array(builder);
    gnostr_json_builder_add_string(builder, "content-warning");
    gnostr_json_builder_add_string(builder, "");
    gnostr_json_builder_end_array(builder);
  }

  gnostr_json_builder_end_array(builder);  /* end tags */

  /* Draft metadata (not part of the actual event, but useful for drafts) */
  gnostr_json_builder_set_key(builder, "_draft_meta");
  gnostr_json_builder_begin_object(builder);

  if (draft->d_tag) {
    gnostr_json_builder_set_key(builder, "d_tag");
    gnostr_json_builder_add_string(builder, draft->d_tag);
  }
  gnostr_json_builder_set_key(builder, "updated_at");
  gnostr_json_builder_add_int64(builder, draft->updated_at);

  if (draft->quote_nostr_uri) {
    gnostr_json_builder_set_key(builder, "quote_nostr_uri");
    gnostr_json_builder_add_string(builder, draft->quote_nostr_uri);
  }

  gnostr_json_builder_end_object(builder);  /* end _draft_meta */

  gnostr_json_builder_end_object(builder);  /* end root */

  char *result = gnostr_json_builder_finish(builder);
  g_object_unref(builder);
  return result;
}

/* Context for parsing draft tags */
typedef struct {
  GnostrDraft *draft;
} DraftTagParseCtx;

static gboolean parse_draft_tag_cb(gsize idx, const gchar *element_json, gpointer user_data) {
  (void)idx;
  DraftTagParseCtx *ctx = (DraftTagParseCtx *)user_data;

  if (!gnostr_json_is_array_str(element_json)) return TRUE;

  char *tag_name = NULL;
  char *tag_val = NULL;

  if ((tag_name = gnostr_json_get_array_string(element_json, NULL, 0, NULL)) == NULL ||
      (tag_val = gnostr_json_get_array_string(element_json, NULL, 1, NULL)) == NULL) {
    g_free(tag_name);
    g_free(tag_val);
    return TRUE;
  }

  if (!tag_name || !tag_val) {
    g_free(tag_name);
    g_free(tag_val);
    return TRUE;
  }

  if (strcmp(tag_name, "subject") == 0) {
    ctx->draft->subject = tag_val;
    tag_val = NULL;
  } else if (strcmp(tag_name, "e") == 0) {
    /* Check marker (element 3) */
    char *marker = NULL;
    marker = gnostr_json_get_array_string(element_json, NULL, 3, NULL);

    if (marker && strcmp(marker, "root") == 0) {
      ctx->draft->root_id = tag_val;
      tag_val = NULL;
    } else if (marker && strcmp(marker, "reply") == 0) {
      ctx->draft->reply_to_id = tag_val;
      tag_val = NULL;
    } else if (!ctx->draft->reply_to_id) {
      /* Legacy: no marker, treat as reply */
      ctx->draft->reply_to_id = tag_val;
      tag_val = NULL;
    }
    g_free(marker);
  } else if (strcmp(tag_name, "p") == 0) {
    if (!ctx->draft->reply_to_pubkey) {
      ctx->draft->reply_to_pubkey = tag_val;
      tag_val = NULL;
    } else if (!ctx->draft->quote_pubkey) {
      ctx->draft->quote_pubkey = tag_val;
      tag_val = NULL;
    }
  } else if (strcmp(tag_name, "q") == 0) {
    ctx->draft->quote_id = tag_val;
    tag_val = NULL;
  } else if (strcmp(tag_name, "content-warning") == 0) {
    ctx->draft->is_sensitive = TRUE;
  }

  g_free(tag_name);
  g_free(tag_val);
  return TRUE;
}

GnostrDraft *gnostr_draft_from_json(const char *json_str) {
  if (!json_str) return NULL;

  if (!gnostr_json_is_valid(json_str)) {
    g_warning("drafts: failed to parse draft JSON");
    return NULL;
  }

  GnostrDraft *draft = gnostr_draft_new();

  /* Kind */
  int kind_val = 0;
  kind_val = gnostr_json_get_int(json_str, "kind", NULL);
  /* Note: check error if 0 is a valid value */
  {
    draft->target_kind = kind_val;
  }

  /* Content */
  char *content_val = NULL;
  content_val = gnostr_json_get_string(json_str, "content", NULL);
  if (content_val) {
    draft->content = content_val;
  }

  /* Created at */
  int64_t created_val = 0;
  if ((created_val = gnostr_json_get_int64(json_str, "created_at", NULL), TRUE)) {
    draft->created_at = created_val;
  }

  /* Parse tags using callback */
  char *tags_json = NULL;
  tags_json = gnostr_json_get_raw(json_str, "tags", NULL);
  if (tags_json) {
    DraftTagParseCtx ctx = { .draft = draft };
    gnostr_json_array_foreach_root(tags_json, parse_draft_tag_cb, &ctx);
    g_free(tags_json);
  }

  /* Draft metadata from _draft_meta object */
  char *d_tag_val = NULL;
  if ((d_tag_val = gnostr_json_get_string_at(json_str, "_draft_meta", "d_tag", NULL)) != NULL ) {
    draft->d_tag = d_tag_val;
  }

  gint64 updated_val = gnostr_json_get_int64_at(json_str, "_draft_meta", "updated_at", NULL);
  if (updated_val != 0) {
    draft->updated_at = updated_val;
  }

  char *quote_uri_val = NULL;
  if ((quote_uri_val = gnostr_json_get_string_at(json_str, "_draft_meta", "quote_nostr_uri", NULL)) != NULL ) {
    draft->quote_nostr_uri = quote_uri_val;
  }

  return draft;
}

/* ---- Local storage ---- */

static char *get_drafts_dir(void) {
  const char *data_dir = g_get_user_data_dir();
  return g_build_filename(data_dir, "gnostr", "drafts", NULL);
}

static char *get_draft_file_path(const char *d_tag) {
  g_autofree char *dir = get_drafts_dir();
  /* Sanitize d_tag for filesystem */
  g_autofree char *safe_tag = g_strdup(d_tag);
  for (char *p = safe_tag; *p; p++) {
    if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
  }
  return g_build_filename(dir, safe_tag, NULL);
}

static gboolean ensure_drafts_dir(void) {
  g_autofree char *dir = get_drafts_dir();
  return g_mkdir_with_parents(dir, 0700) == 0;
}

static gboolean save_draft_to_file(const GnostrDraft *draft) {
  if (!draft || !draft->d_tag) return FALSE;

  if (!ensure_drafts_dir()) {
    g_warning("drafts: failed to create drafts directory");
    return FALSE;
  }

  g_autofree char *json = gnostr_draft_to_json(draft);
  if (!json) return FALSE;

  g_autofree char *path = get_draft_file_path(draft->d_tag);
  GError *error = NULL;
  gboolean ok = g_file_set_contents(path, json, -1, &error);

  if (!ok) {
    g_warning("drafts: failed to save draft to %s: %s", path, error->message);
    g_error_free(error);
  } else {
    g_message("drafts: saved draft to %s", path);
  }

  return ok;
}

static GnostrDraft *load_draft_from_file(const char *path) {
  char *contents = NULL;
  gsize length = 0;
  GError *error = NULL;

  if (!g_file_get_contents(path, &contents, &length, &error)) {
    g_warning("drafts: failed to read %s: %s", path, error->message);
    g_error_free(error);
    return NULL;
  }

  GnostrDraft *draft = gnostr_draft_from_json(contents);
  g_free(contents);
  return draft;
}

GPtrArray *gnostr_drafts_load_local(GnostrDrafts *self) {
  g_return_val_if_fail(GNOSTR_IS_DRAFTS(self), NULL);

  GPtrArray *result = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_draft_free);

  g_autofree char *dir = get_drafts_dir();
  GDir *gdir = g_dir_open(dir, 0, NULL);
  if (!gdir) {
    return result;
  }

  const char *name;
  while ((name = g_dir_read_name(gdir)) != NULL) {
    g_autofree char *path = g_build_filename(dir, name, NULL);
    GnostrDraft *draft = load_draft_from_file(path);
    if (draft) {
      g_ptr_array_add(result, draft);
    }
  }

  g_dir_close(gdir);

  g_message("drafts: loaded %u drafts from local storage", result->len);
  return result;
}

gboolean gnostr_drafts_delete_local(GnostrDrafts *self, const char *d_tag) {
  g_return_val_if_fail(GNOSTR_IS_DRAFTS(self), FALSE);
  g_return_val_if_fail(d_tag != NULL, FALSE);

  g_autofree char *path = get_draft_file_path(d_tag);
  int result = g_unlink(path);

  if (result == 0) {
    g_message("drafts: deleted local draft: %s", d_tag);
    return TRUE;
  }
  return FALSE;
}

/* ---- Save draft async ---- */

typedef struct {
  GnostrDrafts *drafts;
  GnostrDraft *draft;
  GnostrDraftsCallback callback;
  gpointer user_data;
} SaveContext;

static void save_context_free(SaveContext *ctx) {
  if (!ctx) return;
  g_clear_pointer(&ctx->draft, gnostr_draft_free);
  g_free(ctx);
}

void gnostr_drafts_save_async(GnostrDrafts *self,
                               GnostrDraft *draft,
                               GnostrDraftsCallback callback,
                               gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_DRAFTS(self));
  g_return_if_fail(draft != NULL);

  /* Generate d_tag if not set */
  if (!draft->d_tag) {
    draft->d_tag = gnostr_draft_generate_d_tag();
  }

  /* Update timestamp */
  draft->updated_at = (gint64)time(NULL);

  /* Save locally first (synchronously for simplicity) */
  if (!save_draft_to_file(draft)) {
    if (callback) {
      callback(self, FALSE, "Failed to save draft locally", user_data);
    }
    return;
  }

  /* Update cache */
  g_mutex_lock(&self->lock);
  g_hash_table_replace(self->cache, g_strdup(draft->d_tag), gnostr_draft_copy(draft));
  g_mutex_unlock(&self->lock);

  /* Relay sync requires nostrc-n44s (NIP-44 signer integration) */

  if (callback) {
    callback(self, TRUE, NULL, user_data);
  }
}

/* ---- Delete draft async ---- */

void gnostr_drafts_delete_async(GnostrDrafts *self,
                                 const char *d_tag,
                                 GnostrDraftsCallback callback,
                                 gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_DRAFTS(self));
  g_return_if_fail(d_tag != NULL);

  /* Delete locally */
  gboolean local_ok = gnostr_drafts_delete_local(self, d_tag);

  /* Remove from cache */
  g_mutex_lock(&self->lock);
  g_hash_table_remove(self->cache, d_tag);
  g_mutex_unlock(&self->lock);

  /* Relay deletion requires nostrc-n44s (NIP-44 signer integration) */

  if (callback) {
    callback(self, local_ok, local_ok ? NULL : "Draft not found", user_data);
  }
}

/* ---- Load from relays with strategy ---- */

void gnostr_drafts_load_with_strategy_async(GnostrDrafts *self,
                                             GnostrDraftsMergeStrategy strategy,
                                             GnostrDraftsLoadCallback callback,
                                             gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_DRAFTS(self));

  g_message("drafts: load with strategy %d", strategy);

  /* Strategy-specific behavior */
  switch (strategy) {
  case GNOSTR_DRAFTS_MERGE_LOCAL_WINS:
    /* Only load from local storage, skip relay fetch */
    g_message("drafts: LOCAL_WINS - loading local only");
    break;

  case GNOSTR_DRAFTS_MERGE_REMOTE_WINS:
    /* When relay fetch is implemented, would clear local first */
    g_message("drafts: REMOTE_WINS - relay fetch not yet implemented, using local");
    break;

  case GNOSTR_DRAFTS_MERGE_UNION:
    /* When relay fetch is implemented, would merge local + remote */
    g_message("drafts: UNION - relay fetch not yet implemented, using local");
    break;

  case GNOSTR_DRAFTS_MERGE_LATEST:
  default:
    /* When relay fetch is implemented, would keep newest per d-tag */
    g_message("drafts: LATEST - relay fetch not yet implemented, using local");
    break;
  }

  /* For now, all strategies fall back to local loading.
   * Relay fetch requires nostrc-n44s (NIP-44 signer integration). */
  GPtrArray *drafts = gnostr_drafts_load_local(self);

  if (callback) {
    callback(self, drafts, NULL, user_data);
  }
}

void gnostr_drafts_load_from_relays_async(GnostrDrafts *self,
                                           GnostrDraftsLoadCallback callback,
                                           gpointer user_data) {
  gnostr_drafts_load_with_strategy_async(self, GNOSTR_DRAFTS_MERGE_LATEST, callback, user_data);
}
