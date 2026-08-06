#define G_LOG_DOMAIN "gnostr-timeline-hydrator"

#include "gnostr-timeline-hydrator.h"

#include <string.h>

struct _GnostrTimelineHydrator {
  GObject parent_instance;
  guint64 generation;
};

G_DEFINE_TYPE(GnostrTimelineHydrator, gnostr_timeline_hydrator, G_TYPE_OBJECT)

static void
gnostr_timeline_hydrator_class_init(GnostrTimelineHydratorClass *klass)
{
  (void)klass;
}

static void
gnostr_timeline_hydrator_init(GnostrTimelineHydrator *self)
{
  self->generation = 1;
}

GnostrTimelineHydrator *
gnostr_timeline_hydrator_new(guint64 generation)
{
  GnostrTimelineHydrator *self =
    g_object_new(GNOSTR_TYPE_TIMELINE_HYDRATOR, NULL);
  self->generation = generation > 0 ? generation : 1;
  return self;
}

void
gnostr_timeline_hydrator_set_generation(GnostrTimelineHydrator *self,
                                         guint64 generation)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_HYDRATOR(self));
  self->generation = generation > 0 ? generation : 1;
}

guint64
gnostr_timeline_hydrator_get_generation(GnostrTimelineHydrator *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_HYDRATOR(self), 0);
  return self->generation;
}

static void
bytes_to_hex32(const guint8 bytes[32],
               char out[65])
{
  static const char hexdigits[] = "0123456789abcdef";
  for (guint i = 0; i < 32; i++) {
    out[i * 2] = hexdigits[(bytes[i] >> 4) & 0x0f];
    out[i * 2 + 1] = hexdigits[bytes[i] & 0x0f];
  }
  out[64] = '\0';
}

static gboolean
bytes32_all_zero(const guint8 bytes[32])
{
  for (guint i = 0; i < 32; i++) {
    if (bytes[i] != 0)
      return FALSE;
  }
  return TRUE;
}

static char *
event_id_for_entry(const GnostrTimelineBatchEntry *entry)
{
  if (!bytes32_all_zero(entry->event_id)) {
    char event_id[65];
    bytes_to_hex32(entry->event_id, event_id);
    return g_strdup(event_id);
  }

  return g_strdup_printf("note-key-%" G_GUINT64_FORMAT, entry->note_key);
}

static char *
render_markup_from_content(const char *content)
{
  GnContentRenderResult *parsed =
    gn_content_parse(content ? content : "", -1, NULL, NULL);
  char *markup = g_strdup(parsed && parsed->markup ? parsed->markup : "");
  gnostr_content_render_result_free(parsed);
  return markup;
}

static gboolean
ptr_array_contains_string(GPtrArray *array,
                          const char *value)
{
  if (!array || !value)
    return FALSE;

  for (guint i = 0; i < array->len; i++) {
    if (g_strcmp0(g_ptr_array_index(array, i), value) == 0)
      return TRUE;
  }
  return FALSE;
}

static void
ptr_array_add_unique(GPtrArray *array,
                     const char *value)
{
  if (!array || !value || !*value || ptr_array_contains_string(array, value))
    return;
  g_ptr_array_add(array, g_strdup(value));
}

#define MAX_MEDIA_DESCRIPTORS 4u
#define MAX_LINK_PREVIEW_DESCRIPTORS 2u
#define MAX_EVENT_EMBED_DESCRIPTORS 3u

static void
extract_inline_metadata(const char *content,
                        GPtrArray *hashtags,
                        GPtrArray *mentions)
{
  if (!content || !*content)
    return;

  const char *p = content;
  while (*p) {
    if (*p == '#' && (p == content || !g_ascii_isalnum(*(p - 1)))) {
      const char *start = ++p;
      while (*p && (g_ascii_isalnum(*p) || *p == '_' || *p == '-'))
        p++;
      if (p > start) {
        g_autofree char *tag = g_strndup(start, (gsize)(p - start));
        ptr_array_add_unique(hashtags, tag);
      }
      continue;
    }

    if (*p == '@' && (p == content || !g_ascii_isalnum(*(p - 1)))) {
      const char *start = ++p;
      while (*p && (g_ascii_isalnum(*p) || *p == '_' || *p == '-' || *p == '.'))
        p++;
      if (p > start) {
        g_autofree char *mention = g_strndup(start, (gsize)(p - start));
        ptr_array_add_unique(mentions, mention);
      }
      continue;
    }

    p = g_utf8_next_char(p);
  }
}

static GnContentDescriptor *
dup_content_descriptor(const GnContentDescriptor *descriptor)
{
  GnContentDescriptor *copy = g_new0(GnContentDescriptor, 1);
  copy->type = descriptor->type;
  copy->url = g_strdup(descriptor->url);
  copy->original = g_strdup(descriptor->original);
  copy->id = g_strdup(descriptor->id);
  copy->pubkey = g_strdup(descriptor->pubkey);
  copy->relay_hints = g_strdupv(descriptor->relay_hints);
  copy->width = descriptor->width;
  copy->height = descriptor->height;
  copy->thumbnail_url = g_strdup(descriptor->thumbnail_url);
  return copy;
}

static gboolean
descriptors_contain_url(const GPtrArray *descriptors,
                        const char *url)
{
  if (!descriptors || !url)
    return FALSE;
  for (guint i = 0; i < descriptors->len; i++) {
    const GnContentDescriptor *descriptor =
      g_ptr_array_index((GPtrArray *)descriptors, i);
    if (descriptor && g_strcmp0(descriptor->url, url) == 0)
      return TRUE;
  }
  return FALSE;
}

static void
append_explicit_url_descriptors(GPtrArray *descriptors,
                                char **urls,
                                GnContentDescriptorType type)
{
  if (!urls)
    return;
  for (guint i = 0; urls[i]; i++) {
    if (!*urls[i] || descriptors_contain_url(descriptors, urls[i]))
      continue;
    GnContentDescriptor *descriptor = g_new0(GnContentDescriptor, 1);
    descriptor->type = type;
    descriptor->url = g_strdup(urls[i]);
    g_ptr_array_add(descriptors, descriptor);
  }
}

static GPtrArray *
merge_content_descriptors(const GPtrArray *parsed_descriptors,
                          char **explicit_links,
                          char **explicit_media)
{
  GPtrArray *merged =
    g_ptr_array_new_with_free_func((GDestroyNotify)gn_content_descriptor_free);
  if (parsed_descriptors) {
    for (guint i = 0; i < parsed_descriptors->len; i++) {
      const GnContentDescriptor *descriptor =
        g_ptr_array_index((GPtrArray *)parsed_descriptors, i);
      if (descriptor)
        g_ptr_array_add(merged, dup_content_descriptor(descriptor));
    }
  }
  append_explicit_url_descriptors(merged, explicit_media,
                                  GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE);
  append_explicit_url_descriptors(merged, explicit_links,
                                  GN_CONTENT_DESCRIPTOR_LINK_PREVIEW);
  return merged;
}

static GPtrArray *
cap_content_descriptors(const GPtrArray *descriptors,
                        guint *out_overflow_count)
{
  GPtrArray *capped = g_ptr_array_new();
  guint media_count = 0;
  guint link_count = 0;
  guint event_count = 0;
  guint overflow_count = 0;

  if (descriptors) {
    for (guint i = 0; i < descriptors->len; i++) {
      GnContentDescriptor *descriptor =
        g_ptr_array_index((GPtrArray *)descriptors, i);
      if (!descriptor)
        continue;

      gboolean include = TRUE;
      switch (descriptor->type) {
        case GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE:
        case GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO:
          include = media_count++ < MAX_MEDIA_DESCRIPTORS;
          break;
        case GN_CONTENT_DESCRIPTOR_LINK_PREVIEW:
          include = link_count++ < MAX_LINK_PREVIEW_DESCRIPTORS;
          break;
        case GN_CONTENT_DESCRIPTOR_NOSTR_EVENT_REF:
          include = event_count++ < MAX_EVENT_EMBED_DESCRIPTORS;
          break;
        case GN_CONTENT_DESCRIPTOR_NOSTR_PROFILE_REF:
          break;
      }

      if (include)
        g_ptr_array_add(capped, descriptor);
      else
        overflow_count++;
    }
  }

  if (out_overflow_count)
    *out_overflow_count = overflow_count;
  return capped;
}

static void
collect_descriptor_urls(const GPtrArray *descriptors,
                        GPtrArray *links,
                        GPtrArray *media)
{
  if (!descriptors)
    return;

  for (guint i = 0; i < descriptors->len; i++) {
    const GnContentDescriptor *descriptor =
      g_ptr_array_index((GPtrArray *)descriptors, i);
    if (!descriptor || !descriptor->url)
      continue;
    if (descriptor->type == GN_CONTENT_DESCRIPTOR_MEDIA_IMAGE ||
        descriptor->type == GN_CONTENT_DESCRIPTOR_MEDIA_VIDEO)
      ptr_array_add_unique(media, descriptor->url);
    else if (descriptor->type == GN_CONTENT_DESCRIPTOR_LINK_PREVIEW)
      ptr_array_add_unique(links, descriptor->url);
  }
}

static char **
finish_strv(GPtrArray *array)
{
  if (!array || array->len == 0) {
    if (array)
      g_ptr_array_unref(array);
    return NULL;
  }
  g_ptr_array_add(array, NULL);
  return (char **)g_ptr_array_free(array, FALSE);
}

static void
seed_ptr_array_from_strv(GPtrArray *array,
                         char **values)
{
  if (!array || !values)
    return;

  for (guint i = 0; values[i]; i++)
    ptr_array_add_unique(array, values[i]);
}

static char *
author_fallback_label(const char *pubkey)
{
  if (pubkey && strlen(pubkey) >= 8)
    return g_strdup_printf("%.8s...", pubkey);
  if (pubkey && *pubkey)
    return g_strdup(pubkey);
  return g_strdup("unknown");
}

static char *
avatar_fallback_label_for_author(const char *display,
                                 const char *handle,
                                 const char *pubkey)
{
  const char *source = (display && *display) ? display : ((handle && *handle) ? handle : pubkey);
  if (!source || !*source)
    return g_strdup("?");

  gunichar ch = g_utf8_get_char(source);
  ch = g_unichar_toupper(ch);
  char buf[8] = {0};
  gint len = g_unichar_to_utf8(ch, buf);
  buf[len] = '\0';
  return g_strdup(buf);
}

static char *
content_snippet(const char *content)
{
  if (!content || !*content)
    return NULL;

  const guint max_chars = 180;
  if (g_utf8_strlen(content, -1) <= max_chars)
    return g_strdup(content);

  const char *end = g_utf8_offset_to_pointer(content, max_chars);
  return g_strdup_printf("%.*s…", (int)(end - content), content);
}

static GnostrTimelinePreviewState
preview_state_for(const char *event_id,
                  gboolean resolved,
                  const char *content,
                  const char *pubkey,
                  gint64 created_at)
{
  if (!event_id || !*event_id)
    return GNOSTR_TIMELINE_PREVIEW_ABSENT;
  if (resolved || content || pubkey || created_at > 0)
    return GNOSTR_TIMELINE_PREVIEW_RESOLVED;
  return GNOSTR_TIMELINE_PREVIEW_MISSING;
}

GnostrTimelineItemViewModel *
gnostr_timeline_hydrator_hydrate_entry(GnostrTimelineHydrator *self,
                                       const GnostrTimelineBatchEntry *entry)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_HYDRATOR(self), NULL);
  g_return_val_if_fail(entry != NULL, NULL);

  g_autofree char *event_id = event_id_for_entry(entry);
  g_autofree char *note_key = g_strdup_printf("%" G_GUINT64_FORMAT, entry->note_key);
  GnContentRenderResult *parsed_content =
    gn_content_parse(entry->content ? entry->content : "", -1, NULL, NULL);
  g_autofree char *rendered_content =
    g_strdup(parsed_content && parsed_content->markup ? parsed_content->markup : "");
  GPtrArray *descriptor_candidates =
    merge_content_descriptors(parsed_content ? parsed_content->descriptors : NULL,
                              entry->links,
                              entry->media_urls);
  guint descriptor_overflow_count = 0;
  GPtrArray *content_descriptors =
    cap_content_descriptors(descriptor_candidates, &descriptor_overflow_count);
  g_autofree char *parent_fallback = author_fallback_label(entry->parent_pubkey);
  g_autofree char *quoted_snippet = content_snippet(entry->quoted_content);
  g_autofree char *quoted_rendered = render_markup_from_content(quoted_snippet);
  g_autofree char *reposted_snippet = content_snippet(entry->reposted_content);
  g_autofree char *reposted_rendered = render_markup_from_content(reposted_snippet);
  GnostrTimelinePreviewState quote_state = preview_state_for(entry->quoted_event_id,
                                                             entry->quoted_resolved,
                                                             entry->quoted_content,
                                                             entry->quoted_pubkey,
                                                             entry->quoted_created_at);
  GnostrTimelinePreviewState repost_state = preview_state_for(entry->reposted_event_id,
                                                              entry->reposted_resolved,
                                                              entry->reposted_content,
                                                              entry->reposted_pubkey,
                                                              entry->reposted_created_at);

  GPtrArray *hashtags = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *mentions = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *links = g_ptr_array_new_with_free_func(g_free);
  GPtrArray *media = g_ptr_array_new_with_free_func(g_free);
  seed_ptr_array_from_strv(hashtags, entry->hashtags);
  seed_ptr_array_from_strv(mentions, entry->mentions);
  extract_inline_metadata(entry->content, hashtags, mentions);
  collect_descriptor_urls(content_descriptors, links, media);

  g_auto(GStrv) hashtags_v = finish_strv(hashtags);
  g_auto(GStrv) mentions_v = finish_strv(mentions);
  g_auto(GStrv) links_v = finish_strv(links);
  g_auto(GStrv) media_v = finish_strv(media);

  g_autofree char *display_fallback = NULL;
  const char *display = entry->display_name;
  if ((!display || !*display) && (!entry->handle || !*entry->handle))
    display_fallback = author_fallback_label(entry->pubkey_hex);

  const char *effective_display = (display && *display) ? display : display_fallback;
  g_autofree char *avatar_fallback =
    avatar_fallback_label_for_author(effective_display, entry->handle, entry->pubkey_hex);

  gboolean has_content_warning = entry->content_warning && *entry->content_warning;

  GnostrTimelineItemViewModelSpec spec = {
    .event_id = event_id,
    .note_key = note_key,
    .note_key_u64 = entry->note_key,
    .pubkey = entry->pubkey_hex,
    .created_at = entry->created_at,
    .tie_breaker = event_id,
    .kind = entry->kind,
    .content = entry->content,
    .rendered_content = rendered_content,
    .display_name = effective_display,
    .handle = entry->handle,
    .avatar_url = entry->avatar_url,
    .avatar_fallback_label = avatar_fallback,
    .nip05 = entry->nip05,
    .has_profile = entry->has_profile,
    .root_id = entry->root_id,
    .reply_id = entry->reply_id,
    .parent_pubkey = entry->parent_pubkey,
    .parent_display_name = entry->parent_display_name,
    .parent_avatar_url = entry->parent_avatar_url,
    .parent_nip05 = entry->parent_nip05,
    .parent_fallback_label = parent_fallback,
    .parent_available = entry->parent_pubkey || entry->parent_display_name || entry->parent_avatar_url || entry->parent_nip05,
    .quoted_event_id = entry->quoted_event_id,
    .quote_state = quote_state,
    .quoted_pubkey = entry->quoted_pubkey,
    .quoted_display_name = entry->quoted_display_name,
    .quoted_content = quoted_snippet,
    .quoted_rendered_content = quote_state == GNOSTR_TIMELINE_PREVIEW_RESOLVED ? quoted_rendered : NULL,
    .quoted_created_at = entry->quoted_created_at,
    .quoted_kind = entry->quoted_kind,
    .reposted_event_id = entry->reposted_event_id,
    .repost_state = repost_state,
    .reposted_pubkey = entry->reposted_pubkey,
    .reposted_display_name = entry->reposted_display_name,
    .reposted_avatar_url = entry->reposted_avatar_url,
    .reposted_nip05 = entry->reposted_nip05,
    .reposted_content = reposted_snippet,
    .reposted_rendered_content = repost_state == GNOSTR_TIMELINE_PREVIEW_RESOLVED ? reposted_rendered : NULL,
    .reposted_created_at = entry->reposted_created_at,
    .reposted_kind = entry->reposted_kind,
    .content_warning = entry->content_warning,
    .relay_hint = entry->relay_hint,
    .hashtags = (const char * const *)hashtags_v,
    .mentions = (const char * const *)mentions_v,
    .links = (const char * const *)links_v,
    .media_urls = (const char * const *)media_v,
    .content_descriptors = content_descriptors,
    .descriptor_overflow_count = descriptor_overflow_count,
    .action_event_id = event_id,
    .action_pubkey = entry->pubkey_hex,
    .action_is_own_note = entry->is_own_note,
    .action_logged_in = entry->logged_in,
    .action_is_bookmarked = entry->is_bookmarked,
    .action_is_pinned = entry->is_pinned,
    .action_zap_target = entry->zap_target ? entry->zap_target : entry->pubkey_hex,
    .moderation_state = entry->is_muted ? GNOSTR_TIMELINE_MODERATION_MUTED :
      (has_content_warning ? GNOSTR_TIMELINE_MODERATION_CONTENT_WARNING : GNOSTR_TIMELINE_MODERATION_VISIBLE),
  };

  g_autofree char *geometry_signature =
    gnostr_timeline_item_view_model_spec_recompute_derived_fields(&spec);
  (void)geometry_signature;
  GnostrTimelineItemViewModel *view_model =
    gnostr_timeline_item_view_model_new(&spec);
  g_ptr_array_unref(content_descriptors);
  g_ptr_array_unref(descriptor_candidates);
  gnostr_content_render_result_free(parsed_content);
  return view_model;
}

static gint
sort_vm_ptrs_cb(gconstpointer a,
                gconstpointer b)
{
  GnostrTimelineItemViewModel *vm_a = *(GnostrTimelineItemViewModel * const *)a;
  GnostrTimelineItemViewModel *vm_b = *(GnostrTimelineItemViewModel * const *)b;
  return gnostr_timeline_item_view_model_compare(vm_a, vm_b);
}

static void
dedup_sorted_items(GPtrArray *items)
{
  GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < items->len;) {
    GnostrTimelineItemViewModel *vm = g_ptr_array_index(items, i);
    const char *event_id = gnostr_timeline_item_view_model_get_event_id(vm);
    if (event_id && *event_id && g_hash_table_contains(seen, event_id)) {
      g_ptr_array_remove_index(items, i);
      continue;
    }
    if (event_id && *event_id)
      g_hash_table_add(seen, (gpointer)event_id);
    i++;
  }
  g_hash_table_destroy(seen);
}

GPtrArray *
gnostr_timeline_hydrator_hydrate_batch(GnostrTimelineHydrator *self,
                                        GnostrTimelineBatch *batch)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_HYDRATOR(self), NULL);
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_BATCH(batch), NULL);

  guint64 batch_generation = gnostr_timeline_batch_get_generation(batch);
  if (batch_generation != self->generation) {
    g_debug("[HYDRATOR] Dropping stale %s batch gen=%" G_GUINT64_FORMAT " current=%" G_GUINT64_FORMAT,
            gnostr_timeline_batch_kind_to_string(gnostr_timeline_batch_get_kind(batch)),
            batch_generation,
            self->generation);
    return NULL;
  }

  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
  guint n_entries = gnostr_timeline_batch_get_n_entries(batch);
  for (guint i = 0; i < n_entries; i++) {
    const GnostrTimelineBatchEntry *entry = gnostr_timeline_batch_get_entry(batch, i);
    if (!entry)
      continue;
    GnostrTimelineItemViewModel *vm = gnostr_timeline_hydrator_hydrate_entry(self, entry);
    if (vm)
      g_ptr_array_add(items, vm);
  }

  g_ptr_array_sort(items, sort_vm_ptrs_cb);
  dedup_sorted_items(items);
  return items;
}

typedef struct {
  GnostrTimelineBatch *batch;
  guint64 generation;
} HydrateTaskData;

static void
hydrate_task_data_free(HydrateTaskData *data)
{
  if (!data)
    return;
  g_clear_object(&data->batch);
  g_free(data);
}

static void
hydrate_batch_thread(GTask *task,
                     gpointer source_object,
                     gpointer task_data,
                     GCancellable *cancellable)
{
  GnostrTimelineHydrator *self = GNOSTR_TIMELINE_HYDRATOR(source_object);
  HydrateTaskData *data = task_data;

  if (g_task_return_error_if_cancelled(task))
    return;
  if (data->generation != gnostr_timeline_batch_get_generation(data->batch)) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }

  GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
  guint n_entries = gnostr_timeline_batch_get_n_entries(data->batch);
  for (guint i = 0; i < n_entries; i++) {
    if (g_cancellable_is_cancelled(cancellable)) {
      g_ptr_array_unref(items);
      g_task_return_error_if_cancelled(task);
      return;
    }
    const GnostrTimelineBatchEntry *entry =
        gnostr_timeline_batch_get_entry(data->batch, i);
    if (!entry)
      continue;
    GnostrTimelineItemViewModel *vm =
        gnostr_timeline_hydrator_hydrate_entry(self, entry);
    if (vm)
      g_ptr_array_add(items, vm);
  }
  if (g_cancellable_is_cancelled(cancellable)) {
    g_ptr_array_unref(items);
    g_task_return_error_if_cancelled(task);
    return;
  }

  g_ptr_array_sort(items, sort_vm_ptrs_cb);
  dedup_sorted_items(items);
  g_task_return_pointer(task, items, (GDestroyNotify)g_ptr_array_unref);
}

void
gnostr_timeline_hydrator_hydrate_batch_async(GnostrTimelineHydrator *self,
                                             GnostrTimelineBatch *batch,
                                             GCancellable *cancellable,
                                             GAsyncReadyCallback callback,
                                             gpointer user_data)
{
  g_return_if_fail(GNOSTR_IS_TIMELINE_HYDRATOR(self));
  g_return_if_fail(GNOSTR_IS_TIMELINE_BATCH(batch));

  HydrateTaskData *data = g_new0(HydrateTaskData, 1);
  data->batch = g_object_ref(batch);
  /* Generation is main-context state: capture it before worker dispatch and do
   * not read the mutable field from the worker thread. */
  data->generation = self->generation;

  GTask *task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_task_data(task, data, (GDestroyNotify)hydrate_task_data_free);
  g_task_run_in_thread(task, hydrate_batch_thread);
  g_object_unref(task);
}

GPtrArray *
gnostr_timeline_hydrator_hydrate_batch_finish(GnostrTimelineHydrator *self,
                                              GAsyncResult *result,
                                              GError **error)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_HYDRATOR(self), NULL);
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);

  GTask *task = G_TASK(result);
  GPtrArray *items = g_task_propagate_pointer(task, error);
  if (!items)
    return NULL;

  /* finish() runs on the dispatching main context.  Do not publish worker
   * results after a query generation change. */
  HydrateTaskData *data = g_task_get_task_data(task);
  if (!data || data->generation != self->generation) {
    g_ptr_array_unref(items);
    return NULL;
  }
  return items;
}
