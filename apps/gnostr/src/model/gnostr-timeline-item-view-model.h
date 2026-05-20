#ifndef GNOSTR_TIMELINE_ITEM_VIEW_MODEL_H
#define GNOSTR_TIMELINE_ITEM_VIEW_MODEL_H

#include <glib-object.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_ITEM_VIEW_MODEL \
  (gnostr_timeline_item_view_model_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineItemViewModel,
                     gnostr_timeline_item_view_model,
                     GNOSTR, TIMELINE_ITEM_VIEW_MODEL,
                     GObject)

typedef enum {
  GNOSTR_TIMELINE_AVATAR_FALLBACK,
  GNOSTR_TIMELINE_AVATAR_URL,
} GnostrTimelineAvatarState;

typedef enum {
  GNOSTR_TIMELINE_MODERATION_VISIBLE,
  GNOSTR_TIMELINE_MODERATION_CONTENT_WARNING,
  GNOSTR_TIMELINE_MODERATION_MUTED,
} GnostrTimelineModerationState;

typedef enum {
  GNOSTR_TIMELINE_PREVIEW_ABSENT,
  GNOSTR_TIMELINE_PREVIEW_MISSING,
  GNOSTR_TIMELINE_PREVIEW_RESOLVED,
} GnostrTimelinePreviewState;

typedef struct {
  const char *event_id;
  const char *note_key;
  guint64 note_key_u64;
  const char *pubkey;
  gint64 created_at;
  const char *tie_breaker;
  gint kind;

  const char *content;
  const char *rendered_content;
  const char *display_name;
  const char *handle;
  const char *avatar_url;
  const char *avatar_fallback_label;
  const char *nip05;
  gboolean has_profile;
  GnostrTimelineAvatarState avatar_state;

  const char *root_id;
  const char *reply_id;
  const char *parent_pubkey;
  const char *parent_display_name;
  const char *parent_avatar_url;
  const char *parent_nip05;
  const char *parent_fallback_label;
  gboolean parent_available;
  const char *quoted_event_id;
  GnostrTimelinePreviewState quote_state;
  const char *quoted_pubkey;
  const char *quoted_display_name;
  const char *quoted_content;
  const char *quoted_rendered_content;
  gint64 quoted_created_at;
  gint quoted_kind;
  const char *reposted_event_id;
  GnostrTimelinePreviewState repost_state;
  const char *reposted_pubkey;
  const char *reposted_display_name;
  const char *reposted_avatar_url;
  const char *reposted_nip05;
  const char *reposted_content;
  const char *reposted_rendered_content;
  gint64 reposted_created_at;
  gint reposted_kind;
  const char *content_warning;
  const char *relay_hint;

  const char * const *hashtags;
  const char * const *mentions;
  const char * const *links;
  const char * const *media_urls;

  const char *action_event_id;
  const char *action_pubkey;
  gboolean action_is_own_note;
  gboolean action_logged_in;
  gboolean action_is_bookmarked;
  gboolean action_is_pinned;
  const char *action_zap_target;

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
  const char *geometry_signature;
} GnostrTimelineItemViewModelSpec;

GnostrTimelineItemViewModel *
gnostr_timeline_item_view_model_new(const GnostrTimelineItemViewModelSpec *spec);

GnostrTimelineItemViewModel *
gnostr_timeline_item_view_model_copy_with_profile(GnostrTimelineItemViewModel *self,
                                                  const char *display_name,
                                                  const char *handle,
                                                  const char *avatar_url,
                                                  const char *nip05,
                                                  gboolean has_profile);

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
                                                       gint64 zap_total_msat);

const char *gnostr_timeline_item_view_model_get_event_id(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_note_key(GnostrTimelineItemViewModel *self);
guint64     gnostr_timeline_item_view_model_get_note_key_u64(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_pubkey(GnostrTimelineItemViewModel *self);
gint64      gnostr_timeline_item_view_model_get_created_at(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_tie_breaker(GnostrTimelineItemViewModel *self);
gint        gnostr_timeline_item_view_model_get_kind(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_content(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_rendered_content(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_display_name(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_handle(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_avatar_url(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_avatar_fallback_label(GnostrTimelineItemViewModel *self);
GnostrTimelineAvatarState gnostr_timeline_item_view_model_get_avatar_state(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_nip05(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_has_profile(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_root_id(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reply_id(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_parent_pubkey(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_parent_display_name(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_parent_avatar_url(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_parent_nip05(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_parent_fallback_label(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_parent_available(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_quoted_event_id(GnostrTimelineItemViewModel *self);
GnostrTimelinePreviewState gnostr_timeline_item_view_model_get_quote_state(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_quoted_pubkey(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_quoted_display_name(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_quoted_content(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_quoted_rendered_content(GnostrTimelineItemViewModel *self);
gint64      gnostr_timeline_item_view_model_get_quoted_created_at(GnostrTimelineItemViewModel *self);
gint        gnostr_timeline_item_view_model_get_quoted_kind(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_event_id(GnostrTimelineItemViewModel *self);
GnostrTimelinePreviewState gnostr_timeline_item_view_model_get_repost_state(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_pubkey(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_display_name(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_avatar_url(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_nip05(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_content(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_reposted_rendered_content(GnostrTimelineItemViewModel *self);
gint64      gnostr_timeline_item_view_model_get_reposted_created_at(GnostrTimelineItemViewModel *self);
gint        gnostr_timeline_item_view_model_get_reposted_kind(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_content_warning(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_relay_hint(GnostrTimelineItemViewModel *self);
const char * const *gnostr_timeline_item_view_model_get_hashtags(GnostrTimelineItemViewModel *self);
const char * const *gnostr_timeline_item_view_model_get_mentions(GnostrTimelineItemViewModel *self);
const char * const *gnostr_timeline_item_view_model_get_links(GnostrTimelineItemViewModel *self);
const char * const *gnostr_timeline_item_view_model_get_media_urls(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_action_event_id(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_action_pubkey(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_action_is_own_note(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_action_logged_in(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_action_is_bookmarked(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_action_is_pinned(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_action_zap_target(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_like_count(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_is_liked(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_is_reposted(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_is_replied(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_is_zapped(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_repost_count(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_reply_count(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_zap_count(GnostrTimelineItemViewModel *self);
gint64      gnostr_timeline_item_view_model_get_zap_total_msat(GnostrTimelineItemViewModel *self);
GnostrTimelineModerationState gnostr_timeline_item_view_model_get_moderation_state(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_media_reservation_count(GnostrTimelineItemViewModel *self);
double      gnostr_timeline_item_view_model_get_media_reserved_height(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_link_preview_reservation_count(GnostrTimelineItemViewModel *self);
double      gnostr_timeline_item_view_model_get_link_preview_reserved_height(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_has_reply_context_reservation(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_has_repost_context_reservation(GnostrTimelineItemViewModel *self);
gboolean    gnostr_timeline_item_view_model_get_has_quote_context_reservation(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_context_reservation_count(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_quote_preview_reservation_count(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_repost_preview_reservation_count(GnostrTimelineItemViewModel *self);
guint       gnostr_timeline_item_view_model_get_footer_action_reservation_count(GnostrTimelineItemViewModel *self);
double      gnostr_timeline_item_view_model_get_initial_reserved_height(GnostrTimelineItemViewModel *self);
const char *gnostr_timeline_item_view_model_get_geometry_signature(GnostrTimelineItemViewModel *self);

gint gnostr_timeline_item_view_model_compare(GnostrTimelineItemViewModel *a,
                                             GnostrTimelineItemViewModel *b);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_ITEM_VIEW_MODEL_H */
