#define G_LOG_DOMAIN "gnostr-timeline-batch"

#include "gnostr-timeline-batch.h"
#include <string.h>

struct _GnostrTimelineBatch {
  GObject parent_instance;

  GnostrTimelineBatchKind kind;
  guint64 generation;
  GArray *entries;          /* element-type: GnostrTimelineBatchEntry */
  GPtrArray *profile_requests; /* element-type: char* */
  GArray *metadata_patches; /* element-type: GnostrTimelineMetadataPatch */
  GArray *profile_patches;  /* element-type: GnostrTimelineProfilePatch */
  GArray *delete_targets;   /* element-type: GnostrTimelineDeleteTarget */
};

G_DEFINE_TYPE(GnostrTimelineBatch, gnostr_timeline_batch, G_TYPE_OBJECT)

static void
gnostr_timeline_batch_entry_clear(gpointer data)
{
  GnostrTimelineBatchEntry *entry = data;
  g_free(entry->pubkey_hex);
  g_free(entry->content);
  g_free(entry->display_name);
  g_free(entry->handle);
  g_free(entry->avatar_url);
  g_free(entry->nip05);
  g_free(entry->root_id);
  g_free(entry->reply_id);
  g_free(entry->quoted_event_id);
  g_free(entry->reposted_event_id);
  g_free(entry->parent_pubkey);
  g_free(entry->parent_display_name);
  g_free(entry->parent_avatar_url);
  g_free(entry->parent_nip05);
  g_free(entry->quoted_pubkey);
  g_free(entry->quoted_display_name);
  g_free(entry->quoted_content);
  g_free(entry->reposted_pubkey);
  g_free(entry->reposted_display_name);
  g_free(entry->reposted_avatar_url);
  g_free(entry->reposted_nip05);
  g_free(entry->reposted_content);
  g_free(entry->zap_target);
  g_strfreev(entry->hashtags);
  g_strfreev(entry->mentions);
  g_strfreev(entry->links);
  g_strfreev(entry->media_urls);
  g_free(entry->content_warning);
  g_free(entry->relay_hint);
}

static void
gnostr_timeline_metadata_patch_clear(gpointer data)
{
  GnostrTimelineMetadataPatch *patch = data;
  g_free(patch->event_id);
}

static void
gnostr_timeline_profile_patch_clear(gpointer data)
{
  GnostrTimelineProfilePatch *patch = data;
  g_free(patch->pubkey_hex);
  g_free(patch->display_name);
  g_free(patch->handle);
  g_free(patch->avatar_url);
  g_free(patch->nip05);
}

static void
gnostr_timeline_delete_target_clear(gpointer data)
{
  GnostrTimelineDeleteTarget *target = data;
  g_free(target->target_event_id);
  g_free(target->delete_event_id);
}

static void
gnostr_timeline_batch_finalize(GObject *object)
{
  GnostrTimelineBatch *self = GNOSTR_TIMELINE_BATCH(object);

  g_clear_pointer(&self->entries, g_array_unref);
  g_clear_pointer(&self->profile_requests, g_ptr_array_unref);
  g_clear_pointer(&self->metadata_patches, g_array_unref);
  g_clear_pointer(&self->profile_patches, g_array_unref);
  g_clear_pointer(&self->delete_targets, g_array_unref);

  G_OBJECT_CLASS(gnostr_timeline_batch_parent_class)->finalize(object);
}

static void
gnostr_timeline_batch_class_init(GnostrTimelineBatchClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_timeline_batch_finalize;
}

static void
gnostr_timeline_batch_init(GnostrTimelineBatch *self)
{
  self->entries = g_array_new(FALSE, FALSE, sizeof(GnostrTimelineBatchEntry));
  g_array_set_clear_func(self->entries, gnostr_timeline_batch_entry_clear);
  self->profile_requests = g_ptr_array_new_with_free_func(g_free);
  self->metadata_patches = g_array_new(FALSE, FALSE, sizeof(GnostrTimelineMetadataPatch));
  g_array_set_clear_func(self->metadata_patches, gnostr_timeline_metadata_patch_clear);
  self->profile_patches = g_array_new(FALSE, FALSE, sizeof(GnostrTimelineProfilePatch));
  g_array_set_clear_func(self->profile_patches, gnostr_timeline_profile_patch_clear);
  self->delete_targets = g_array_new(FALSE, FALSE, sizeof(GnostrTimelineDeleteTarget));
  g_array_set_clear_func(self->delete_targets, gnostr_timeline_delete_target_clear);
}

GnostrTimelineBatch *
gnostr_timeline_batch_new(GnostrTimelineBatchKind kind,
                          guint64 generation)
{
  GnostrTimelineBatch *self = g_object_new(GNOSTR_TYPE_TIMELINE_BATCH, NULL);
  self->kind = kind;
  self->generation = generation;
  return self;
}

GnostrTimelineBatchKind
gnostr_timeline_batch_get_kind(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), GNOSTR_TIMELINE_BATCH_REFRESH);
  return self->kind;
}

guint64
gnostr_timeline_batch_get_generation(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), 0);
  return self->generation;
}

guint
gnostr_timeline_batch_get_n_entries(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), 0);
  return self->entries ? self->entries->len : 0;
}

const GnostrTimelineBatchEntry *
gnostr_timeline_batch_get_entry(GnostrTimelineBatch *self,
                                guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), NULL);
  g_return_val_if_fail(index < self->entries->len, NULL);
  return &g_array_index(self->entries, GnostrTimelineBatchEntry, index);
}

void
gnostr_timeline_batch_add_entry(GnostrTimelineBatch *self,
                                const GnostrTimelineBatchEntry *entry)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(self));
  g_return_if_fail(entry != NULL);

  GnostrTimelineBatchEntry copy = { 0 };
  copy.note_key = entry->note_key;
  copy.created_at = entry->created_at;
  memcpy(copy.event_id, entry->event_id, sizeof(copy.event_id));
  copy.pubkey_hex = g_strdup(entry->pubkey_hex);
  copy.content = g_strdup(entry->content);
  copy.display_name = g_strdup(entry->display_name);
  copy.handle = g_strdup(entry->handle);
  copy.avatar_url = g_strdup(entry->avatar_url);
  copy.nip05 = g_strdup(entry->nip05);
  copy.root_id = g_strdup(entry->root_id);
  copy.reply_id = g_strdup(entry->reply_id);
  copy.quoted_event_id = g_strdup(entry->quoted_event_id);
  copy.reposted_event_id = g_strdup(entry->reposted_event_id);
  copy.parent_pubkey = g_strdup(entry->parent_pubkey);
  copy.parent_display_name = g_strdup(entry->parent_display_name);
  copy.parent_avatar_url = g_strdup(entry->parent_avatar_url);
  copy.parent_nip05 = g_strdup(entry->parent_nip05);
  copy.quoted_pubkey = g_strdup(entry->quoted_pubkey);
  copy.quoted_display_name = g_strdup(entry->quoted_display_name);
  copy.quoted_content = g_strdup(entry->quoted_content);
  copy.quoted_created_at = entry->quoted_created_at;
  copy.quoted_kind = entry->quoted_kind;
  copy.quoted_resolved = entry->quoted_resolved;
  copy.reposted_pubkey = g_strdup(entry->reposted_pubkey);
  copy.reposted_display_name = g_strdup(entry->reposted_display_name);
  copy.reposted_avatar_url = g_strdup(entry->reposted_avatar_url);
  copy.reposted_nip05 = g_strdup(entry->reposted_nip05);
  copy.reposted_content = g_strdup(entry->reposted_content);
  copy.reposted_created_at = entry->reposted_created_at;
  copy.reposted_kind = entry->reposted_kind;
  copy.reposted_resolved = entry->reposted_resolved;
  copy.is_own_note = entry->is_own_note;
  copy.logged_in = entry->logged_in;
  copy.is_bookmarked = entry->is_bookmarked;
  copy.is_pinned = entry->is_pinned;
  copy.zap_target = g_strdup(entry->zap_target);
  copy.hashtags = g_strdupv(entry->hashtags);
  copy.mentions = g_strdupv(entry->mentions);
  copy.links = g_strdupv(entry->links);
  copy.media_urls = g_strdupv(entry->media_urls);
  copy.content_warning = g_strdup(entry->content_warning);
  copy.relay_hint = g_strdup(entry->relay_hint);
  copy.kind = entry->kind;
  copy.has_profile = entry->has_profile;
  copy.is_muted = entry->is_muted;

  g_array_append_val(self->entries, copy);
}

void
gnostr_timeline_batch_add_note_full(GnostrTimelineBatch *self,
                                    uint64_t note_key,
                                    gint64 created_at,
                                    const uint8_t event_id[32],
                                    const char *pubkey_hex,
                                    const char *content,
                                    const char *display_name,
                                    const char *handle,
                                    const char *avatar_url,
                                    const char *nip05,
                                    const char *root_id,
                                    const char *reply_id,
                                    const char *quoted_event_id,
                                    const char *reposted_event_id,
                                    const char * const *hashtags,
                                    gint kind,
                                    gboolean has_profile)
{
  GnostrTimelineBatchEntry entry = {
    .note_key = note_key,
    .created_at = created_at,
    .kind = kind,
    .has_profile = has_profile,
  };
  if (event_id)
    memcpy(entry.event_id, event_id, sizeof(entry.event_id));
  entry.pubkey_hex = (char *)pubkey_hex;
  entry.content = (char *)content;
  entry.display_name = (char *)display_name;
  entry.handle = (char *)handle;
  entry.avatar_url = (char *)avatar_url;
  entry.nip05 = (char *)nip05;
  entry.root_id = (char *)root_id;
  entry.reply_id = (char *)reply_id;
  entry.quoted_event_id = (char *)quoted_event_id;
  entry.reposted_event_id = (char *)reposted_event_id;
  entry.hashtags = (char **)hashtags;
  gnostr_timeline_batch_add_entry(self, &entry);
}

void
gnostr_timeline_batch_add_note(GnostrTimelineBatch *self,
                               uint64_t note_key,
                               gint64 created_at,
                               const uint8_t event_id[32],
                               const char *pubkey_hex,
                               const char *content,
                               const char *display_name,
                               const char *handle,
                               const char *avatar_url,
                               const char *nip05,
                               const char *root_id,
                               const char *reply_id,
                               gint kind,
                               gboolean has_profile)
{
  gnostr_timeline_batch_add_note_full(self, note_key, created_at, event_id,
                                      pubkey_hex, content, display_name, handle,
                                      avatar_url, nip05, root_id, reply_id,
                                      NULL, NULL, NULL, kind, has_profile);
}

void
gnostr_timeline_batch_add_profile_request(GnostrTimelineBatch *self,
                                          const char *pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(self));
  if (!pubkey_hex || !*pubkey_hex)
    return;

  for (guint i = 0; i < self->profile_requests->len; i++) {
    const char *existing = g_ptr_array_index(self->profile_requests, i);
    if (g_strcmp0(existing, pubkey_hex) == 0)
      return;
  }

  g_ptr_array_add(self->profile_requests, g_strdup(pubkey_hex));
}

guint
gnostr_timeline_batch_get_n_profile_requests(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), 0);
  return self->profile_requests ? self->profile_requests->len : 0;
}

const char *
gnostr_timeline_batch_get_profile_request(GnostrTimelineBatch *self,
                                          guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), NULL);
  g_return_val_if_fail(index < self->profile_requests->len, NULL);
  return g_ptr_array_index(self->profile_requests, index);
}

void
gnostr_timeline_batch_add_metadata_patch(GnostrTimelineBatch *self,
                                         const GnostrTimelineMetadataPatch *patch)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(self));
  g_return_if_fail(patch != NULL);
  if (!patch->event_id || !*patch->event_id)
    return;

  GnostrTimelineMetadataPatch copy = *patch;
  copy.event_id = g_strdup(patch->event_id);
  g_array_append_val(self->metadata_patches, copy);
}

guint
gnostr_timeline_batch_get_n_metadata_patches(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), 0);
  return self->metadata_patches ? self->metadata_patches->len : 0;
}

const GnostrTimelineMetadataPatch *
gnostr_timeline_batch_get_metadata_patch(GnostrTimelineBatch *self,
                                         guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), NULL);
  g_return_val_if_fail(index < self->metadata_patches->len, NULL);
  return &g_array_index(self->metadata_patches, GnostrTimelineMetadataPatch, index);
}

void
gnostr_timeline_batch_add_profile_patch(GnostrTimelineBatch *self,
                                        const GnostrTimelineProfilePatch *patch)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(self));
  g_return_if_fail(patch != NULL);
  if (!patch->pubkey_hex || !*patch->pubkey_hex)
    return;

  GnostrTimelineProfilePatch copy = { 0 };
  copy.pubkey_hex = g_strdup(patch->pubkey_hex);
  copy.display_name = g_strdup(patch->display_name);
  copy.handle = g_strdup(patch->handle);
  copy.avatar_url = g_strdup(patch->avatar_url);
  copy.nip05 = g_strdup(patch->nip05);
  g_array_append_val(self->profile_patches, copy);
}

guint
gnostr_timeline_batch_get_n_profile_patches(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), 0);
  return self->profile_patches ? self->profile_patches->len : 0;
}

const GnostrTimelineProfilePatch *
gnostr_timeline_batch_get_profile_patch(GnostrTimelineBatch *self,
                                        guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), NULL);
  g_return_val_if_fail(index < self->profile_patches->len, NULL);
  return &g_array_index(self->profile_patches, GnostrTimelineProfilePatch, index);
}

void
gnostr_timeline_batch_add_delete_target(GnostrTimelineBatch *self,
                                        const GnostrTimelineDeleteTarget *target)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(self));
  g_return_if_fail(target != NULL);
  if (!target->target_event_id || !*target->target_event_id)
    return;

  GnostrTimelineDeleteTarget copy = { 0 };
  copy.target_event_id = g_strdup(target->target_event_id);
  copy.delete_event_id = g_strdup(target->delete_event_id);
  g_array_append_val(self->delete_targets, copy);
}

guint
gnostr_timeline_batch_get_n_delete_targets(GnostrTimelineBatch *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), 0);
  return self->delete_targets ? self->delete_targets->len : 0;
}

const GnostrTimelineDeleteTarget *
gnostr_timeline_batch_get_delete_target(GnostrTimelineBatch *self,
                                        guint index)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(self), NULL);
  g_return_val_if_fail(index < self->delete_targets->len, NULL);
  return &g_array_index(self->delete_targets, GnostrTimelineDeleteTarget, index);
}

const char *
gnostr_timeline_batch_kind_to_string(GnostrTimelineBatchKind kind)
{
  switch (kind) {
    case GNOSTR_TIMELINE_BATCH_REFRESH: return "refresh";
    case GNOSTR_TIMELINE_BATCH_LIVE_HEAD: return "live-head";
    case GNOSTR_TIMELINE_BATCH_PAGE_OLDER: return "page-older";
    case GNOSTR_TIMELINE_BATCH_PAGE_NEWER: return "page-newer";
    case GNOSTR_TIMELINE_BATCH_DELETE: return "delete";
    case GNOSTR_TIMELINE_BATCH_PROFILE_PATCH: return "profile-patch";
    case GNOSTR_TIMELINE_BATCH_METADATA_PATCH: return "metadata-patch";
    default: return "unknown";
  }
}
