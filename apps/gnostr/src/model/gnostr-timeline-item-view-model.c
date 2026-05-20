#define G_LOG_DOMAIN "gnostr-timeline-item-view-model"

#include "gnostr-timeline-item-view-model.h"

#include <string.h>

struct _GnostrTimelineItemViewModel {
  GObject parent_instance;

  char *event_id;
  char *note_key;
  guint64 note_key_u64;
  char *pubkey;
  gint64 created_at;
  char *tie_breaker;
  gint kind;

  char *content;
  char *rendered_content;
  char *display_name;
  char *handle;
  char *avatar_url;
  char *avatar_fallback_label;
  char *nip05;
  gboolean has_profile;
  GnostrTimelineAvatarState avatar_state;

  char *root_id;
  char *reply_id;
  char *parent_pubkey;
  char *parent_display_name;
  char *parent_avatar_url;
  char *parent_nip05;
  char *parent_fallback_label;
  gboolean parent_available;
  char *quoted_event_id;
  GnostrTimelinePreviewState quote_state;
  char *quoted_pubkey;
  char *quoted_display_name;
  char *quoted_content;
  char *quoted_rendered_content;
  gint64 quoted_created_at;
  gint quoted_kind;
  char *reposted_event_id;
  GnostrTimelinePreviewState repost_state;
  char *reposted_pubkey;
  char *reposted_display_name;
  char *reposted_avatar_url;
  char *reposted_nip05;
  char *reposted_content;
  char *reposted_rendered_content;
  gint64 reposted_created_at;
  gint reposted_kind;
  char *content_warning;
  char *relay_hint;

  char **hashtags;
  char **mentions;
  char **links;
  char **media_urls;

  char *action_event_id;
  char *action_pubkey;
  gboolean action_is_own_note;
  gboolean action_logged_in;
  gboolean action_is_bookmarked;
  gboolean action_is_pinned;
  char *action_zap_target;

  guint like_count;
  gboolean is_liked;
  gboolean is_reposted;
  gboolean is_replied;
  gboolean is_zapped;
  guint repost_count;
  guint reply_count;
  guint zap_count;
  gint64 zap_total_msat;

  GnostrTimelineModerationState moderation_state;
  guint media_reservation_count;
  double media_reserved_height;
  guint link_preview_reservation_count;
  double link_preview_reserved_height;
  gboolean has_reply_context_reservation;
  gboolean has_repost_context_reservation;
  gboolean has_quote_context_reservation;
  guint context_reservation_count;
  guint quote_preview_reservation_count;
  guint repost_preview_reservation_count;
  guint footer_action_reservation_count;
  double initial_reserved_height;
  char *geometry_signature;
};

G_DEFINE_TYPE(GnostrTimelineItemViewModel,
              gnostr_timeline_item_view_model,
              G_TYPE_OBJECT)

static void
gnostr_timeline_item_view_model_finalize(GObject *object)
{
  GnostrTimelineItemViewModel *self = GNOSTR_TIMELINE_ITEM_VIEW_MODEL(object);

  g_free(self->event_id);
  g_free(self->note_key);
  g_free(self->pubkey);
  g_free(self->tie_breaker);
  g_free(self->content);
  g_free(self->rendered_content);
  g_free(self->display_name);
  g_free(self->handle);
  g_free(self->avatar_url);
  g_free(self->avatar_fallback_label);
  g_free(self->nip05);
  g_free(self->root_id);
  g_free(self->reply_id);
  g_free(self->parent_pubkey);
  g_free(self->parent_display_name);
  g_free(self->parent_avatar_url);
  g_free(self->parent_nip05);
  g_free(self->parent_fallback_label);
  g_free(self->quoted_event_id);
  g_free(self->quoted_pubkey);
  g_free(self->quoted_display_name);
  g_free(self->quoted_content);
  g_free(self->quoted_rendered_content);
  g_free(self->reposted_event_id);
  g_free(self->reposted_pubkey);
  g_free(self->reposted_display_name);
  g_free(self->reposted_avatar_url);
  g_free(self->reposted_nip05);
  g_free(self->reposted_content);
  g_free(self->reposted_rendered_content);
  g_free(self->content_warning);
  g_free(self->relay_hint);
  g_strfreev(self->hashtags);
  g_strfreev(self->mentions);
  g_strfreev(self->links);
  g_strfreev(self->media_urls);
  g_free(self->action_event_id);
  g_free(self->action_pubkey);
  g_free(self->action_zap_target);
  g_free(self->geometry_signature);

  G_OBJECT_CLASS(gnostr_timeline_item_view_model_parent_class)->finalize(object);
}

static void
gnostr_timeline_item_view_model_class_init(GnostrTimelineItemViewModelClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = gnostr_timeline_item_view_model_finalize;
}

static void
gnostr_timeline_item_view_model_init(GnostrTimelineItemViewModel *self)
{
  self->avatar_state = GNOSTR_TIMELINE_AVATAR_FALLBACK;
  self->quote_state = GNOSTR_TIMELINE_PREVIEW_ABSENT;
  self->repost_state = GNOSTR_TIMELINE_PREVIEW_ABSENT;
  self->moderation_state = GNOSTR_TIMELINE_MODERATION_VISIBLE;
}

static char **
dup_strv_or_null(const char * const *values)
{
  return values ? g_strdupv((char **)values) : NULL;
}

GnostrTimelineItemViewModel *
gnostr_timeline_item_view_model_new(const GnostrTimelineItemViewModelSpec *spec)
{
  g_return_val_if_fail(spec != NULL, NULL);
  g_return_val_if_fail(spec->event_id != NULL && *spec->event_id != '\0', NULL);

  GnostrTimelineItemViewModel *self =
    g_object_new(GNOSTR_TYPE_TIMELINE_ITEM_VIEW_MODEL, NULL);

  self->event_id = g_strdup(spec->event_id);
  self->note_key = g_strdup(spec->note_key);
  self->note_key_u64 = spec->note_key_u64;
  self->pubkey = g_strdup(spec->pubkey);
  self->created_at = spec->created_at;
  self->tie_breaker = g_strdup(spec->tie_breaker ? spec->tie_breaker : spec->event_id);
  self->kind = spec->kind;

  self->content = g_strdup(spec->content ? spec->content : "");
  self->rendered_content = g_strdup(spec->rendered_content ? spec->rendered_content : self->content);
  self->display_name = g_strdup(spec->display_name);
  self->handle = g_strdup(spec->handle);
  self->avatar_url = g_strdup(spec->avatar_url);
  self->avatar_fallback_label = g_strdup(spec->avatar_fallback_label);
  self->nip05 = g_strdup(spec->nip05);
  self->has_profile = spec->has_profile;
  self->avatar_state = spec->avatar_state;

  self->root_id = g_strdup(spec->root_id);
  self->reply_id = g_strdup(spec->reply_id);
  self->parent_pubkey = g_strdup(spec->parent_pubkey);
  self->parent_display_name = g_strdup(spec->parent_display_name);
  self->parent_avatar_url = g_strdup(spec->parent_avatar_url);
  self->parent_nip05 = g_strdup(spec->parent_nip05);
  self->parent_fallback_label = g_strdup(spec->parent_fallback_label);
  self->parent_available = spec->parent_available;
  self->quoted_event_id = g_strdup(spec->quoted_event_id);
  self->quote_state = spec->quote_state;
  self->quoted_pubkey = g_strdup(spec->quoted_pubkey);
  self->quoted_display_name = g_strdup(spec->quoted_display_name);
  self->quoted_content = g_strdup(spec->quoted_content);
  self->quoted_rendered_content = g_strdup(spec->quoted_rendered_content);
  self->quoted_created_at = spec->quoted_created_at;
  self->quoted_kind = spec->quoted_kind;
  self->reposted_event_id = g_strdup(spec->reposted_event_id);
  self->repost_state = spec->repost_state;
  self->reposted_pubkey = g_strdup(spec->reposted_pubkey);
  self->reposted_display_name = g_strdup(spec->reposted_display_name);
  self->reposted_avatar_url = g_strdup(spec->reposted_avatar_url);
  self->reposted_nip05 = g_strdup(spec->reposted_nip05);
  self->reposted_content = g_strdup(spec->reposted_content);
  self->reposted_rendered_content = g_strdup(spec->reposted_rendered_content);
  self->reposted_created_at = spec->reposted_created_at;
  self->reposted_kind = spec->reposted_kind;
  self->content_warning = g_strdup(spec->content_warning);
  self->relay_hint = g_strdup(spec->relay_hint);

  self->hashtags = dup_strv_or_null(spec->hashtags);
  self->mentions = dup_strv_or_null(spec->mentions);
  self->links = dup_strv_or_null(spec->links);
  self->media_urls = dup_strv_or_null(spec->media_urls);

  self->action_event_id = g_strdup(spec->action_event_id);
  self->action_pubkey = g_strdup(spec->action_pubkey);
  self->action_is_own_note = spec->action_is_own_note;
  self->action_logged_in = spec->action_logged_in;
  self->action_is_bookmarked = spec->action_is_bookmarked;
  self->action_is_pinned = spec->action_is_pinned;
  self->action_zap_target = g_strdup(spec->action_zap_target);

  self->like_count = spec->like_count;
  self->is_liked = spec->is_liked;
  self->is_reposted = spec->is_reposted;
  self->is_replied = spec->is_replied;
  self->is_zapped = spec->is_zapped;
  self->repost_count = spec->repost_count;
  self->reply_count = spec->reply_count;
  self->zap_count = spec->zap_count;
  self->zap_total_msat = spec->zap_total_msat;

  self->moderation_state = spec->moderation_state;
  self->media_reservation_count = spec->media_reservation_count;
  self->media_reserved_height = spec->media_reserved_height;
  self->link_preview_reservation_count = spec->link_preview_reservation_count;
  self->link_preview_reserved_height = spec->link_preview_reserved_height;
  self->has_reply_context_reservation = spec->has_reply_context_reservation;
  self->has_repost_context_reservation = spec->has_repost_context_reservation;
  self->has_quote_context_reservation = spec->has_quote_context_reservation;
  self->context_reservation_count = spec->context_reservation_count;
  self->quote_preview_reservation_count = spec->quote_preview_reservation_count;
  self->repost_preview_reservation_count = spec->repost_preview_reservation_count;
  self->footer_action_reservation_count = spec->footer_action_reservation_count;
  self->initial_reserved_height = spec->initial_reserved_height;
  self->geometry_signature = g_strdup(spec->geometry_signature);

  return self;
}

static void
fill_spec_from_vm(GnostrTimelineItemViewModel *self,
                  GnostrTimelineItemViewModelSpec *spec)
{
  memset(spec, 0, sizeof(*spec));
  spec->event_id = self->event_id;
  spec->note_key = self->note_key;
  spec->note_key_u64 = self->note_key_u64;
  spec->pubkey = self->pubkey;
  spec->created_at = self->created_at;
  spec->tie_breaker = self->tie_breaker;
  spec->kind = self->kind;
  spec->content = self->content;
  spec->rendered_content = self->rendered_content;
  spec->display_name = self->display_name;
  spec->handle = self->handle;
  spec->avatar_url = self->avatar_url;
  spec->avatar_fallback_label = self->avatar_fallback_label;
  spec->nip05 = self->nip05;
  spec->has_profile = self->has_profile;
  spec->avatar_state = self->avatar_state;
  spec->root_id = self->root_id;
  spec->reply_id = self->reply_id;
  spec->parent_pubkey = self->parent_pubkey;
  spec->parent_display_name = self->parent_display_name;
  spec->parent_avatar_url = self->parent_avatar_url;
  spec->parent_nip05 = self->parent_nip05;
  spec->parent_fallback_label = self->parent_fallback_label;
  spec->parent_available = self->parent_available;
  spec->quoted_event_id = self->quoted_event_id;
  spec->quote_state = self->quote_state;
  spec->quoted_pubkey = self->quoted_pubkey;
  spec->quoted_display_name = self->quoted_display_name;
  spec->quoted_content = self->quoted_content;
  spec->quoted_rendered_content = self->quoted_rendered_content;
  spec->quoted_created_at = self->quoted_created_at;
  spec->quoted_kind = self->quoted_kind;
  spec->reposted_event_id = self->reposted_event_id;
  spec->repost_state = self->repost_state;
  spec->reposted_pubkey = self->reposted_pubkey;
  spec->reposted_display_name = self->reposted_display_name;
  spec->reposted_avatar_url = self->reposted_avatar_url;
  spec->reposted_nip05 = self->reposted_nip05;
  spec->reposted_content = self->reposted_content;
  spec->reposted_rendered_content = self->reposted_rendered_content;
  spec->reposted_created_at = self->reposted_created_at;
  spec->reposted_kind = self->reposted_kind;
  spec->content_warning = self->content_warning;
  spec->relay_hint = self->relay_hint;
  spec->hashtags = (const char * const *)self->hashtags;
  spec->mentions = (const char * const *)self->mentions;
  spec->links = (const char * const *)self->links;
  spec->media_urls = (const char * const *)self->media_urls;
  spec->action_event_id = self->action_event_id;
  spec->action_pubkey = self->action_pubkey;
  spec->action_is_own_note = self->action_is_own_note;
  spec->action_logged_in = self->action_logged_in;
  spec->action_is_bookmarked = self->action_is_bookmarked;
  spec->action_is_pinned = self->action_is_pinned;
  spec->action_zap_target = self->action_zap_target;
  spec->like_count = self->like_count;
  spec->is_liked = self->is_liked;
  spec->is_reposted = self->is_reposted;
  spec->is_replied = self->is_replied;
  spec->is_zapped = self->is_zapped;
  spec->repost_count = self->repost_count;
  spec->reply_count = self->reply_count;
  spec->zap_count = self->zap_count;
  spec->zap_total_msat = self->zap_total_msat;
  spec->moderation_state = self->moderation_state;
  spec->media_reservation_count = self->media_reservation_count;
  spec->media_reserved_height = self->media_reserved_height;
  spec->link_preview_reservation_count = self->link_preview_reservation_count;
  spec->link_preview_reserved_height = self->link_preview_reserved_height;
  spec->has_reply_context_reservation = self->has_reply_context_reservation;
  spec->has_repost_context_reservation = self->has_repost_context_reservation;
  spec->has_quote_context_reservation = self->has_quote_context_reservation;
  spec->context_reservation_count = self->context_reservation_count;
  spec->quote_preview_reservation_count = self->quote_preview_reservation_count;
  spec->repost_preview_reservation_count = self->repost_preview_reservation_count;
  spec->footer_action_reservation_count = self->footer_action_reservation_count;
  spec->initial_reserved_height = self->initial_reserved_height;
  spec->geometry_signature = self->geometry_signature;
}

GnostrTimelineItemViewModel *
gnostr_timeline_item_view_model_copy_with_profile(GnostrTimelineItemViewModel *self,
                                                  const char *display_name,
                                                  const char *handle,
                                                  const char *avatar_url,
                                                  const char *nip05,
                                                  gboolean has_profile)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL);

  GnostrTimelineItemViewModelSpec spec;
  fill_spec_from_vm(self, &spec);
  spec.display_name = display_name ? display_name : self->display_name;
  spec.handle = handle ? handle : self->handle;
  spec.avatar_url = avatar_url ? avatar_url : self->avatar_url;
  spec.nip05 = nip05 ? nip05 : self->nip05;
  spec.has_profile = has_profile;
  spec.avatar_state = (spec.avatar_url && *spec.avatar_url) ?
    GNOSTR_TIMELINE_AVATAR_URL : GNOSTR_TIMELINE_AVATAR_FALLBACK;
  return gnostr_timeline_item_view_model_new(&spec);
}

GnostrTimelineItemViewModel *
gnostr_timeline_item_view_model_copy_with_interactions(GnostrTimelineItemViewModel *self,
                                                       gboolean has_like_count,
                                                       guint like_count,
                                                       gboolean has_is_liked,
                                                       gboolean is_liked,
                                                       gboolean has_repost_count,
                                                       guint repost_count,
                                                       gboolean has_reply_count,
                                                       guint reply_count,
                                                       gboolean has_zap_count,
                                                       guint zap_count,
                                                       gboolean has_zap_total_msat,
                                                       gint64 zap_total_msat)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL);

  GnostrTimelineItemViewModelSpec spec;
  fill_spec_from_vm(self, &spec);
  if (has_like_count)
    spec.like_count = like_count;
  if (has_is_liked)
    spec.is_liked = is_liked;
  if (has_repost_count)
    spec.repost_count = repost_count;
  if (has_reply_count)
    spec.reply_count = reply_count;
  if (has_zap_count)
    spec.zap_count = zap_count;
  if (has_zap_total_msat)
    spec.zap_total_msat = zap_total_msat;
  return gnostr_timeline_item_view_model_new(&spec);
}

#define GET_STR(name, field) \
const char *gnostr_timeline_item_view_model_get_##name(GnostrTimelineItemViewModel *self) \
{ \
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL); \
  return self->field; \
}

GET_STR(event_id, event_id)
GET_STR(note_key, note_key)
GET_STR(pubkey, pubkey)
GET_STR(tie_breaker, tie_breaker)
GET_STR(content, content)
GET_STR(rendered_content, rendered_content)
GET_STR(display_name, display_name)
GET_STR(handle, handle)
GET_STR(avatar_url, avatar_url)
GET_STR(avatar_fallback_label, avatar_fallback_label)
GET_STR(nip05, nip05)
GET_STR(root_id, root_id)
GET_STR(reply_id, reply_id)
GET_STR(parent_pubkey, parent_pubkey)
GET_STR(parent_display_name, parent_display_name)
GET_STR(parent_avatar_url, parent_avatar_url)
GET_STR(parent_nip05, parent_nip05)
GET_STR(parent_fallback_label, parent_fallback_label)
GET_STR(quoted_event_id, quoted_event_id)
GET_STR(quoted_pubkey, quoted_pubkey)
GET_STR(quoted_display_name, quoted_display_name)
GET_STR(quoted_content, quoted_content)
GET_STR(quoted_rendered_content, quoted_rendered_content)
GET_STR(reposted_event_id, reposted_event_id)
GET_STR(reposted_pubkey, reposted_pubkey)
GET_STR(reposted_display_name, reposted_display_name)
GET_STR(reposted_avatar_url, reposted_avatar_url)
GET_STR(reposted_nip05, reposted_nip05)
GET_STR(reposted_content, reposted_content)
GET_STR(reposted_rendered_content, reposted_rendered_content)
GET_STR(action_event_id, action_event_id)
GET_STR(action_pubkey, action_pubkey)
GET_STR(action_zap_target, action_zap_target)
GET_STR(content_warning, content_warning)
GET_STR(relay_hint, relay_hint)
GET_STR(geometry_signature, geometry_signature)

#undef GET_STR

guint64 gnostr_timeline_item_view_model_get_note_key_u64(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->note_key_u64;
}

gint64 gnostr_timeline_item_view_model_get_created_at(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->created_at;
}

gint gnostr_timeline_item_view_model_get_kind(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 1);
  return self->kind;
}

GnostrTimelineAvatarState gnostr_timeline_item_view_model_get_avatar_state(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), GNOSTR_TIMELINE_AVATAR_FALLBACK);
  return self->avatar_state;
}

gboolean gnostr_timeline_item_view_model_get_has_profile(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->has_profile;
}

gboolean gnostr_timeline_item_view_model_get_parent_available(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->parent_available;
}

GnostrTimelinePreviewState gnostr_timeline_item_view_model_get_quote_state(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), GNOSTR_TIMELINE_PREVIEW_ABSENT);
  return self->quote_state;
}

gint64 gnostr_timeline_item_view_model_get_quoted_created_at(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->quoted_created_at;
}

gint gnostr_timeline_item_view_model_get_quoted_kind(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 1);
  return self->quoted_kind;
}

GnostrTimelinePreviewState gnostr_timeline_item_view_model_get_repost_state(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), GNOSTR_TIMELINE_PREVIEW_ABSENT);
  return self->repost_state;
}

gint64 gnostr_timeline_item_view_model_get_reposted_created_at(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->reposted_created_at;
}

gint gnostr_timeline_item_view_model_get_reposted_kind(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 1);
  return self->reposted_kind;
}

const char * const *gnostr_timeline_item_view_model_get_hashtags(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL);
  return (const char * const *)self->hashtags;
}

const char * const *gnostr_timeline_item_view_model_get_mentions(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL);
  return (const char * const *)self->mentions;
}

const char * const *gnostr_timeline_item_view_model_get_links(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL);
  return (const char * const *)self->links;
}

const char * const *gnostr_timeline_item_view_model_get_media_urls(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), NULL);
  return (const char * const *)self->media_urls;
}

gboolean gnostr_timeline_item_view_model_get_action_is_own_note(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->action_is_own_note;
}

gboolean gnostr_timeline_item_view_model_get_action_logged_in(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->action_logged_in;
}

gboolean gnostr_timeline_item_view_model_get_action_is_bookmarked(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->action_is_bookmarked;
}

gboolean gnostr_timeline_item_view_model_get_action_is_pinned(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->action_is_pinned;
}

guint gnostr_timeline_item_view_model_get_like_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->like_count;
}

gboolean gnostr_timeline_item_view_model_get_is_liked(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->is_liked;
}

gboolean gnostr_timeline_item_view_model_get_is_reposted(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->is_reposted;
}

gboolean gnostr_timeline_item_view_model_get_is_replied(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->is_replied;
}

gboolean gnostr_timeline_item_view_model_get_is_zapped(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->is_zapped;
}

guint gnostr_timeline_item_view_model_get_repost_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->repost_count;
}

guint gnostr_timeline_item_view_model_get_reply_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->reply_count;
}

guint gnostr_timeline_item_view_model_get_zap_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->zap_count;
}

gint64 gnostr_timeline_item_view_model_get_zap_total_msat(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->zap_total_msat;
}

GnostrTimelineModerationState gnostr_timeline_item_view_model_get_moderation_state(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), GNOSTR_TIMELINE_MODERATION_VISIBLE);
  return self->moderation_state;
}

guint gnostr_timeline_item_view_model_get_media_reservation_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->media_reservation_count;
}

double gnostr_timeline_item_view_model_get_media_reserved_height(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0.0);
  return self->media_reserved_height;
}

guint gnostr_timeline_item_view_model_get_link_preview_reservation_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->link_preview_reservation_count;
}

double gnostr_timeline_item_view_model_get_link_preview_reserved_height(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0.0);
  return self->link_preview_reserved_height;
}

gboolean gnostr_timeline_item_view_model_get_has_reply_context_reservation(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->has_reply_context_reservation;
}

gboolean gnostr_timeline_item_view_model_get_has_repost_context_reservation(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->has_repost_context_reservation;
}

gboolean gnostr_timeline_item_view_model_get_has_quote_context_reservation(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), FALSE);
  return self->has_quote_context_reservation;
}

guint gnostr_timeline_item_view_model_get_context_reservation_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->context_reservation_count;
}

guint gnostr_timeline_item_view_model_get_quote_preview_reservation_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->quote_preview_reservation_count;
}

guint gnostr_timeline_item_view_model_get_repost_preview_reservation_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->repost_preview_reservation_count;
}

guint gnostr_timeline_item_view_model_get_footer_action_reservation_count(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0);
  return self->footer_action_reservation_count;
}

double gnostr_timeline_item_view_model_get_initial_reserved_height(GnostrTimelineItemViewModel *self)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(self), 0.0);
  return self->initial_reserved_height;
}

gint
gnostr_timeline_item_view_model_compare(GnostrTimelineItemViewModel *a,
                                         GnostrTimelineItemViewModel *b)
{
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(a), 0);
  g_return_val_if_fail(GNOSTR_IS_TIMELINE_ITEM_VIEW_MODEL(b), 0);

  if (a->created_at > b->created_at)
    return -1;
  if (a->created_at < b->created_at)
    return 1;

  gint tie = g_strcmp0(a->tie_breaker, b->tie_breaker);
  if (tie != 0)
    return tie;

  return g_strcmp0(a->event_id, b->event_id);
}
