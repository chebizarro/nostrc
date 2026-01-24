/**
 * GnostrDrafts - NIP-37 Draft Events Implementation
 *
 * Manages draft events (kind 31234) for saving work-in-progress notes.
 */

#define G_LOG_DOMAIN "gnostr-drafts"

#include "gnostr-drafts.h"
#include <jansson.h>
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
  g_free(draft->d_tag);
  g_free(draft->content);
  g_free(draft->subject);
  g_free(draft->reply_to_id);
  g_free(draft->root_id);
  g_free(draft->reply_to_pubkey);
  g_free(draft->quote_id);
  g_free(draft->quote_pubkey);
  g_free(draft->quote_nostr_uri);
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

  json_t *obj = json_object();

  /* Required fields */
  json_object_set_new(obj, "kind", json_integer(draft->target_kind));
  json_object_set_new(obj, "content", json_string(draft->content ? draft->content : ""));
  json_object_set_new(obj, "created_at", json_integer(draft->created_at));

  /* Build tags array */
  json_t *tags = json_array();

  /* NIP-14: Subject tag */
  if (draft->subject && *draft->subject) {
    json_t *subject_tag = json_array();
    json_array_append_new(subject_tag, json_string("subject"));
    json_array_append_new(subject_tag, json_string(draft->subject));
    json_array_append_new(tags, subject_tag);
  }

  /* NIP-10: Reply context */
  if (draft->root_id && strlen(draft->root_id) == 64) {
    json_t *root_tag = json_array();
    json_array_append_new(root_tag, json_string("e"));
    json_array_append_new(root_tag, json_string(draft->root_id));
    json_array_append_new(root_tag, json_string("")); /* relay hint */
    json_array_append_new(root_tag, json_string("root"));
    json_array_append_new(tags, root_tag);
  }

  if (draft->reply_to_id && strlen(draft->reply_to_id) == 64 &&
      (!draft->root_id || strcmp(draft->reply_to_id, draft->root_id) != 0)) {
    json_t *reply_tag = json_array();
    json_array_append_new(reply_tag, json_string("e"));
    json_array_append_new(reply_tag, json_string(draft->reply_to_id));
    json_array_append_new(reply_tag, json_string("")); /* relay hint */
    json_array_append_new(reply_tag, json_string("reply"));
    json_array_append_new(tags, reply_tag);
  }

  if (draft->reply_to_pubkey && strlen(draft->reply_to_pubkey) == 64) {
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(draft->reply_to_pubkey));
    json_array_append_new(tags, p_tag);
  }

  /* NIP-18: Quote context */
  if (draft->quote_id && strlen(draft->quote_id) == 64) {
    json_t *q_tag = json_array();
    json_array_append_new(q_tag, json_string("q"));
    json_array_append_new(q_tag, json_string(draft->quote_id));
    json_array_append_new(q_tag, json_string("")); /* relay hint */
    json_array_append_new(tags, q_tag);
  }

  if (draft->quote_pubkey && strlen(draft->quote_pubkey) == 64) {
    json_t *p_tag = json_array();
    json_array_append_new(p_tag, json_string("p"));
    json_array_append_new(p_tag, json_string(draft->quote_pubkey));
    json_array_append_new(tags, p_tag);
  }

  /* NIP-36: Content warning */
  if (draft->is_sensitive) {
    json_t *cw_tag = json_array();
    json_array_append_new(cw_tag, json_string("content-warning"));
    json_array_append_new(cw_tag, json_string(""));
    json_array_append_new(tags, cw_tag);
  }

  json_object_set_new(obj, "tags", tags);

  /* Draft metadata (not part of the actual event, but useful for drafts) */
  json_t *meta = json_object();
  if (draft->d_tag) {
    json_object_set_new(meta, "d_tag", json_string(draft->d_tag));
  }
  json_object_set_new(meta, "updated_at", json_integer(draft->updated_at));
  if (draft->quote_nostr_uri) {
    json_object_set_new(meta, "quote_nostr_uri", json_string(draft->quote_nostr_uri));
  }
  json_object_set_new(obj, "_draft_meta", meta);

  char *result = json_dumps(obj, JSON_COMPACT);
  json_decref(obj);
  return result;
}

GnostrDraft *gnostr_draft_from_json(const char *json_str) {
  if (!json_str) return NULL;

  json_error_t error;
  json_t *obj = json_loads(json_str, 0, &error);
  if (!obj) {
    g_warning("drafts: failed to parse draft JSON: %s", error.text);
    return NULL;
  }

  GnostrDraft *draft = gnostr_draft_new();

  /* Kind */
  json_t *kind_val = json_object_get(obj, "kind");
  if (json_is_integer(kind_val)) {
    draft->target_kind = (int)json_integer_value(kind_val);
  }

  /* Content */
  json_t *content_val = json_object_get(obj, "content");
  if (json_is_string(content_val)) {
    draft->content = g_strdup(json_string_value(content_val));
  }

  /* Created at */
  json_t *created_val = json_object_get(obj, "created_at");
  if (json_is_integer(created_val)) {
    draft->created_at = json_integer_value(created_val);
  }

  /* Parse tags */
  json_t *tags = json_object_get(obj, "tags");
  if (json_is_array(tags)) {
    size_t i;
    json_t *tag;
    json_array_foreach(tags, i, tag) {
      if (!json_is_array(tag) || json_array_size(tag) < 2) continue;

      const char *tag_name = json_string_value(json_array_get(tag, 0));
      const char *tag_val = json_string_value(json_array_get(tag, 1));
      if (!tag_name || !tag_val) continue;

      if (strcmp(tag_name, "subject") == 0) {
        draft->subject = g_strdup(tag_val);
      } else if (strcmp(tag_name, "e") == 0) {
        /* Check marker */
        const char *marker = NULL;
        if (json_array_size(tag) >= 4) {
          marker = json_string_value(json_array_get(tag, 3));
        }
        if (marker && strcmp(marker, "root") == 0) {
          draft->root_id = g_strdup(tag_val);
        } else if (marker && strcmp(marker, "reply") == 0) {
          draft->reply_to_id = g_strdup(tag_val);
        } else if (!draft->reply_to_id) {
          /* Legacy: no marker, treat as reply */
          draft->reply_to_id = g_strdup(tag_val);
        }
      } else if (strcmp(tag_name, "p") == 0) {
        if (!draft->reply_to_pubkey) {
          draft->reply_to_pubkey = g_strdup(tag_val);
        } else if (!draft->quote_pubkey) {
          draft->quote_pubkey = g_strdup(tag_val);
        }
      } else if (strcmp(tag_name, "q") == 0) {
        draft->quote_id = g_strdup(tag_val);
      } else if (strcmp(tag_name, "content-warning") == 0) {
        draft->is_sensitive = TRUE;
      }
    }
  }

  /* Draft metadata */
  json_t *meta = json_object_get(obj, "_draft_meta");
  if (json_is_object(meta)) {
    json_t *d_tag_val = json_object_get(meta, "d_tag");
    if (json_is_string(d_tag_val)) {
      draft->d_tag = g_strdup(json_string_value(d_tag_val));
    }
    json_t *updated_val = json_object_get(meta, "updated_at");
    if (json_is_integer(updated_val)) {
      draft->updated_at = json_integer_value(updated_val);
    }
    json_t *quote_uri_val = json_object_get(meta, "quote_nostr_uri");
    if (json_is_string(quote_uri_val)) {
      draft->quote_nostr_uri = g_strdup(json_string_value(quote_uri_val));
    }
  }

  json_decref(obj);
  return draft;
}

/* ---- Local storage ---- */

static char *get_drafts_dir(void) {
  const char *data_dir = g_get_user_data_dir();
  return g_build_filename(data_dir, "gnostr", "drafts", NULL);
}

static char *get_draft_file_path(const char *d_tag) {
  char *dir = get_drafts_dir();
  /* Sanitize d_tag for filesystem */
  char *safe_tag = g_strdup(d_tag);
  for (char *p = safe_tag; *p; p++) {
    if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
  }
  char *path = g_build_filename(dir, safe_tag, NULL);
  g_free(dir);
  g_free(safe_tag);
  return path;
}

static gboolean ensure_drafts_dir(void) {
  char *dir = get_drafts_dir();
  int result = g_mkdir_with_parents(dir, 0700);
  g_free(dir);
  return result == 0;
}

static gboolean save_draft_to_file(const GnostrDraft *draft) {
  if (!draft || !draft->d_tag) return FALSE;

  if (!ensure_drafts_dir()) {
    g_warning("drafts: failed to create drafts directory");
    return FALSE;
  }

  char *json = gnostr_draft_to_json(draft);
  if (!json) return FALSE;

  char *path = get_draft_file_path(draft->d_tag);
  GError *error = NULL;
  gboolean ok = g_file_set_contents(path, json, -1, &error);

  if (!ok) {
    g_warning("drafts: failed to save draft to %s: %s", path, error->message);
    g_error_free(error);
  } else {
    g_message("drafts: saved draft to %s", path);
  }

  g_free(path);
  g_free(json);
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

  char *dir = get_drafts_dir();
  GDir *gdir = g_dir_open(dir, 0, NULL);
  if (!gdir) {
    g_free(dir);
    return result;
  }

  const char *name;
  while ((name = g_dir_read_name(gdir)) != NULL) {
    char *path = g_build_filename(dir, name, NULL);
    GnostrDraft *draft = load_draft_from_file(path);
    if (draft) {
      g_ptr_array_add(result, draft);
    }
    g_free(path);
  }

  g_dir_close(gdir);
  g_free(dir);

  g_message("drafts: loaded %u drafts from local storage", result->len);
  return result;
}

gboolean gnostr_drafts_delete_local(GnostrDrafts *self, const char *d_tag) {
  g_return_val_if_fail(GNOSTR_IS_DRAFTS(self), FALSE);
  g_return_val_if_fail(d_tag != NULL, FALSE);

  char *path = get_draft_file_path(d_tag);
  int result = g_unlink(path);
  g_free(path);

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
  if (ctx->draft) gnostr_draft_free(ctx->draft);
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

  /* TODO: Optionally publish to relays with NIP-44 encryption
   * This requires signer support for NIP-44 encryption which is
   * not yet fully integrated into the unified signer service.
   * For now, drafts are stored locally only.
   */

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

  /* TODO: Publish blanked event to relays to signal deletion
   * This requires signer integration.
   */

  if (callback) {
    callback(self, local_ok, local_ok ? NULL : "Draft not found", user_data);
  }
}

/* ---- Load from relays (stub) ---- */

void gnostr_drafts_load_from_relays_async(GnostrDrafts *self,
                                           GnostrDraftsLoadCallback callback,
                                           gpointer user_data) {
  g_return_if_fail(GNOSTR_IS_DRAFTS(self));

  /* TODO: Implement relay fetch for kind 31234 events
   * This requires:
   * 1. Subscribing to kind 31234 events from user's relays
   * 2. NIP-44 decryption of content
   * 3. Merging with local drafts
   *
   * For now, just return local drafts.
   */
  GPtrArray *drafts = gnostr_drafts_load_local(self);

  if (callback) {
    callback(self, drafts, NULL, user_data);
  }
}
