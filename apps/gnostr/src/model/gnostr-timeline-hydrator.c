#define G_LOG_DOMAIN "gnostr-timeline-hydrator"

#include "gnostr-timeline-hydrator.h"

#include <string.h>

#define DEFAULT_BASE_RESERVED_HEIGHT 180.0
#define DEFAULT_CONTEXT_RESERVED_HEIGHT 48.0
#define DEFAULT_QUOTE_RESERVED_HEIGHT 96.0
#define DEFAULT_REPOST_RESERVED_HEIGHT 44.0
#define DEFAULT_MEDIA_RESERVED_HEIGHT 240.0
#define DEFAULT_LINK_PREVIEW_RESERVED_HEIGHT 120.0
#define MAX_TEXT_RESERVED_HEIGHT 240.0

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

static gboolean
str_has_suffix_with_len(const char *str,
                        gsize len,
                        const char *suffix)
{
  gsize suffix_len = strlen(suffix);
  if (!str || suffix_len > len)
    return FALSE;
  return memcmp(str + len - suffix_len, suffix, suffix_len) == 0;
}

static gboolean
str_has_media_suffix(const char *url)
{
  if (!url)
    return FALSE;

  g_autofree char *lower = g_ascii_strdown(url, -1);
  const char *q = strchr(lower, '?');
  gsize len = q ? (gsize)(q - lower) : strlen(lower);

  return (str_has_suffix_with_len(lower, len, ".jpg") ||
          str_has_suffix_with_len(lower, len, ".jpeg") ||
          str_has_suffix_with_len(lower, len, ".png") ||
          str_has_suffix_with_len(lower, len, ".gif") ||
          str_has_suffix_with_len(lower, len, ".webp") ||
          str_has_suffix_with_len(lower, len, ".mp4") ||
          str_has_suffix_with_len(lower, len, ".webm") ||
          str_has_suffix_with_len(lower, len, ".mov"));
}

static gboolean
is_nostr_bech32_token(const char *token)
{
  return token &&
    (g_str_has_prefix(token, "nostr:") ||
     g_str_has_prefix(token, "note1") ||
     g_str_has_prefix(token, "npub1") ||
     g_str_has_prefix(token, "nevent1") ||
     g_str_has_prefix(token, "nprofile1") ||
     g_str_has_prefix(token, "naddr1"));
}

static gboolean
is_http_token(const char *token)
{
  return token &&
    (g_str_has_prefix(token, "http://") ||
     g_str_has_prefix(token, "https://") ||
     g_str_has_prefix(token, "www."));
}

static gsize
trim_trailing_token_punctuation(const char *token,
                                gsize len)
{
  while (len > 0) {
    char c = token[len - 1];
    if (c == '.' || c == ',' || c == ';' || c == ':' ||
        c == '!' || c == '?' || c == ')' || c == ']' || c == '}')
      len--;
    else
      break;
  }
  return len;
}

static void
append_markup_for_token(GString *out,
                        const char *token,
                        gsize len)
{
  if (len == 0)
    return;

  gsize core_len = trim_trailing_token_punctuation(token, len);
  const char *suffix = token + core_len;
  gsize suffix_len = len - core_len;
  g_autofree char *core = g_strndup(token, core_len);

  if (core_len > 1 && core[0] == '#') {
    g_autofree char *tag = g_strdup(core + 1);
    g_autofree char *esc_tag = g_markup_escape_text(tag, -1);
    g_autofree char *esc_href = g_markup_escape_text(tag, -1);
    g_string_append_printf(out, "<a href=\"hashtag:%s\">#%s</a>", esc_href, esc_tag);
  } else if (is_http_token(core)) {
    g_autofree char *href = g_str_has_prefix(core, "www.")
      ? g_strdup_printf("https://%s", core)
      : g_strdup(core);
    g_autofree char *esc_href = g_markup_escape_text(href, -1);
    g_autofree char *esc_display = g_markup_escape_text(core, -1);
    g_string_append_printf(out, "<a href=\"%s\">%s</a>", esc_href, esc_display);
  } else if (is_nostr_bech32_token(core)) {
    g_autofree char *href = g_str_has_prefix(core, "nostr:")
      ? g_strdup(core)
      : g_strdup_printf("nostr:%s", core);
    g_autofree char *esc_href = g_markup_escape_text(href, -1);
    g_autofree char *esc_display = g_markup_escape_text(core, -1);
    g_string_append_printf(out, "<a href=\"%s\">%s</a>", esc_href, esc_display);
  } else {
    g_autofree char *escaped = g_markup_escape_text(core, -1);
    g_string_append(out, escaped);
  }

  if (suffix_len > 0) {
    g_autofree char *escaped_suffix = g_markup_escape_text(suffix, suffix_len);
    g_string_append(out, escaped_suffix);
  }
}

static char *
render_markup_from_content(const char *content)
{
  if (!content || !*content)
    return g_strdup("");

  GString *out = g_string_new(NULL);
  const char *p = content;
  while (*p) {
    if (g_ascii_isspace(*p)) {
      g_string_append_c(out, *p++);
      continue;
    }

    const char *start = p;
    while (*p && !g_ascii_isspace(*p))
      p++;
    append_markup_for_token(out, start, (gsize)(p - start));
  }

  return g_string_free(out, FALSE);
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

static char *
trim_url_token(const char *start,
               gsize len)
{
  while (len > 0) {
    char c = start[len - 1];
    if (c == '.' || c == ',' || c == ';' || c == ':' || c == ')' || c == ']' || c == '}' || c == '!' || c == '?')
      len--;
    else
      break;
  }

  return len > 0 ? g_strndup(start, len) : NULL;
}

static void
extract_content_tokens(const char *content,
                       GPtrArray *hashtags,
                       GPtrArray *mentions,
                       GPtrArray *links,
                       GPtrArray *media)
{
  if (!content || !*content)
    return;

  const char *p = content;
  while (*p) {
    if (g_str_has_prefix(p, "https://") || g_str_has_prefix(p, "http://")) {
      const char *start = p;
      while (*p && !g_ascii_isspace(*p) && *p != '<' && *p != '>' && *p != '"' && *p != '\'')
        p++;
      g_autofree char *url = trim_url_token(start, (gsize)(p - start));
      if (url && *url) {
        if (str_has_media_suffix(url))
          ptr_array_add_unique(media, url);
        else
          ptr_array_add_unique(links, url);
      }
      continue;
    }

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

static guint
strv_length(char **values)
{
  if (!values)
    return 0;
  guint n = 0;
  while (values[n])
    n++;
  return n;
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
  g_autofree char *rendered_content = render_markup_from_content(entry->content);
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
  seed_ptr_array_from_strv(links, entry->links);
  seed_ptr_array_from_strv(media, entry->media_urls);
  extract_content_tokens(entry->content, hashtags, mentions, links, media);

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

  guint media_count = strv_length(media_v);
  guint link_count = strv_length(links_v);
  gboolean has_reply_context = (entry->root_id && *entry->root_id) || (entry->reply_id && *entry->reply_id);
  gboolean has_quote_context = quote_state != GNOSTR_TIMELINE_PREVIEW_ABSENT;
  gboolean has_repost_context = (entry->kind == 6) || repost_state != GNOSTR_TIMELINE_PREVIEW_ABSENT;
  gboolean has_content_warning = entry->content_warning && *entry->content_warning;

  guint content_len = entry->content ? (guint)g_utf8_strlen(entry->content, -1) : 0;
  double text_reserved = MIN(MAX_TEXT_RESERVED_HEIGHT, 24.0 + ((content_len + 79u) / 80u) * 22.0);
  double media_reserved = media_count * DEFAULT_MEDIA_RESERVED_HEIGHT;
  double link_reserved = link_count * DEFAULT_LINK_PREVIEW_RESERVED_HEIGHT;
  double initial_reserved = DEFAULT_BASE_RESERVED_HEIGHT + text_reserved + media_reserved + link_reserved;
  if (has_reply_context)
    initial_reserved += DEFAULT_CONTEXT_RESERVED_HEIGHT;
  if (has_quote_context)
    initial_reserved += DEFAULT_QUOTE_RESERVED_HEIGHT;
  if (has_repost_context)
    initial_reserved += DEFAULT_REPOST_RESERVED_HEIGHT;
  if (has_content_warning)
    initial_reserved += DEFAULT_CONTEXT_RESERVED_HEIGHT;

  g_autofree char *geometry_signature = g_strdup_printf("vm-v1:k%d:r%d:q%d:p%d:m%u:l%u:cw%d",
                                                        entry->kind,
                                                        has_reply_context,
                                                        has_quote_context,
                                                        has_repost_context,
                                                        media_count,
                                                        link_count,
                                                        has_content_warning);

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
    .avatar_state = (entry->avatar_url && *entry->avatar_url) ? GNOSTR_TIMELINE_AVATAR_URL : GNOSTR_TIMELINE_AVATAR_FALLBACK,
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
    .action_event_id = event_id,
    .action_pubkey = entry->pubkey_hex,
    .action_is_own_note = entry->is_own_note,
    .action_logged_in = entry->logged_in,
    .action_is_bookmarked = entry->is_bookmarked,
    .action_is_pinned = entry->is_pinned,
    .action_zap_target = entry->zap_target ? entry->zap_target : entry->pubkey_hex,
    .moderation_state = entry->is_muted ? GNOSTR_TIMELINE_MODERATION_MUTED :
      (has_content_warning ? GNOSTR_TIMELINE_MODERATION_CONTENT_WARNING : GNOSTR_TIMELINE_MODERATION_VISIBLE),
    .media_reservation_count = media_count,
    .media_reserved_height = media_reserved,
    .link_preview_reservation_count = link_count,
    .link_preview_reserved_height = link_reserved,
    .has_reply_context_reservation = has_reply_context,
    .has_repost_context_reservation = has_repost_context,
    .has_quote_context_reservation = has_quote_context,
    .context_reservation_count = (has_reply_context ? 1u : 0u),
    .quote_preview_reservation_count = (has_quote_context ? 1u : 0u),
    .repost_preview_reservation_count = (has_repost_context ? 1u : 0u),
    .footer_action_reservation_count = 1u,
    .initial_reserved_height = initial_reserved,
    .geometry_signature = geometry_signature,
  };

  return gnostr_timeline_item_view_model_new(&spec);
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
  (void)cancellable;
  GnostrTimelineHydrator *self = GNOSTR_TIMELINE_HYDRATOR(source_object);
  HydrateTaskData *data = task_data;

  if (data->generation != gnostr_timeline_hydrator_get_generation(self) ||
      data->generation != gnostr_timeline_batch_get_generation(data->batch)) {
    g_task_return_pointer(task, NULL, NULL);
    return;
  }

  GPtrArray *items = gnostr_timeline_hydrator_hydrate_batch(self, data->batch);
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
  data->generation = gnostr_timeline_batch_get_generation(batch);

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

  return g_task_propagate_pointer(G_TASK(result), error);
}
