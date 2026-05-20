#ifndef GNOSTR_TIMELINE_BATCH_H
#define GNOSTR_TIMELINE_BATCH_H

#include <glib-object.h>
#include <stdint.h>

G_BEGIN_DECLS

#define GNOSTR_TYPE_TIMELINE_BATCH (gnostr_timeline_batch_get_type())
G_DECLARE_FINAL_TYPE(GnostrTimelineBatch, gnostr_timeline_batch, GNOSTR, TIMELINE_BATCH, GObject)

typedef enum {
  GNOSTR_TIMELINE_BATCH_REFRESH,
  GNOSTR_TIMELINE_BATCH_LIVE_HEAD,
  GNOSTR_TIMELINE_BATCH_PAGE_OLDER,
  GNOSTR_TIMELINE_BATCH_PAGE_NEWER,
  GNOSTR_TIMELINE_BATCH_DELETE,
  GNOSTR_TIMELINE_BATCH_PROFILE_PATCH,
  GNOSTR_TIMELINE_BATCH_METADATA_PATCH,
} GnostrTimelineBatchKind;

typedef struct {
  uint64_t note_key;
  gint64   created_at;
  uint8_t  event_id[32];
  char    *pubkey_hex;
  char    *content;
  char    *display_name;
  char    *handle;
  char    *avatar_url;
  char    *nip05;
  char    *root_id;
  char    *reply_id;
  char    *quoted_event_id;
  char    *reposted_event_id;
  char    *parent_pubkey;
  char    *parent_display_name;
  char    *parent_avatar_url;
  char    *parent_nip05;
  char    *quoted_pubkey;
  char    *quoted_display_name;
  char    *quoted_content;
  gint64   quoted_created_at;
  gint     quoted_kind;
  gboolean quoted_resolved;
  char    *reposted_pubkey;
  char    *reposted_display_name;
  char    *reposted_avatar_url;
  char    *reposted_nip05;
  char    *reposted_content;
  gint64   reposted_created_at;
  gint     reposted_kind;
  gboolean reposted_resolved;
  gboolean is_own_note;
  gboolean logged_in;
  gboolean is_bookmarked;
  gboolean is_pinned;
  char    *zap_target;
  char   **hashtags;
  char   **mentions;
  char   **links;
  char   **media_urls;
  char    *content_warning;
  char    *relay_hint;
  gint     kind;
  gboolean has_profile;
  gboolean is_muted;
} GnostrTimelineBatchEntry;

typedef struct {
  char    *event_id;
  gboolean has_like_count;
  guint    like_count;
  gboolean has_is_liked;
  gboolean is_liked;
  gboolean has_zap_count;
  guint    zap_count;
  gboolean has_zap_total_msat;
  gint64   zap_total_msat;
  gboolean has_repost_count;
  guint    repost_count;
  gboolean has_reply_count;
  guint    reply_count;
} GnostrTimelineMetadataPatch;

typedef struct {
  char *pubkey_hex;
  char *display_name;
  char *handle;
  char *avatar_url;
  char *nip05;
} GnostrTimelineProfilePatch;

typedef struct {
  char *target_event_id;
  char *delete_event_id;
} GnostrTimelineDeleteTarget;

GnostrTimelineBatch *gnostr_timeline_batch_new(GnostrTimelineBatchKind kind,
                                               guint64 generation);

GnostrTimelineBatchKind gnostr_timeline_batch_get_kind(GnostrTimelineBatch *self);
guint64 gnostr_timeline_batch_get_generation(GnostrTimelineBatch *self);
guint gnostr_timeline_batch_get_n_entries(GnostrTimelineBatch *self);
const GnostrTimelineBatchEntry *gnostr_timeline_batch_get_entry(GnostrTimelineBatch *self,
                                                                guint index);

void gnostr_timeline_batch_add_entry(GnostrTimelineBatch *self,
                                     const GnostrTimelineBatchEntry *entry);
void gnostr_timeline_batch_add_note(GnostrTimelineBatch *self,
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
                                    gboolean has_profile);
void gnostr_timeline_batch_add_note_full(GnostrTimelineBatch *self,
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
                                         gboolean has_profile);

void gnostr_timeline_batch_add_profile_request(GnostrTimelineBatch *self,
                                               const char *pubkey_hex);
guint gnostr_timeline_batch_get_n_profile_requests(GnostrTimelineBatch *self);
const char *gnostr_timeline_batch_get_profile_request(GnostrTimelineBatch *self,
                                                      guint index);

void gnostr_timeline_batch_add_metadata_patch(GnostrTimelineBatch *self,
                                              const GnostrTimelineMetadataPatch *patch);
guint gnostr_timeline_batch_get_n_metadata_patches(GnostrTimelineBatch *self);
const GnostrTimelineMetadataPatch *gnostr_timeline_batch_get_metadata_patch(GnostrTimelineBatch *self,
                                                                            guint index);

void gnostr_timeline_batch_add_profile_patch(GnostrTimelineBatch *self,
                                             const GnostrTimelineProfilePatch *patch);
guint gnostr_timeline_batch_get_n_profile_patches(GnostrTimelineBatch *self);
const GnostrTimelineProfilePatch *gnostr_timeline_batch_get_profile_patch(GnostrTimelineBatch *self,
                                                                          guint index);

void gnostr_timeline_batch_add_delete_target(GnostrTimelineBatch *self,
                                             const GnostrTimelineDeleteTarget *target);
guint gnostr_timeline_batch_get_n_delete_targets(GnostrTimelineBatch *self);
const GnostrTimelineDeleteTarget *gnostr_timeline_batch_get_delete_target(GnostrTimelineBatch *self,
                                                                          guint index);

const char *gnostr_timeline_batch_kind_to_string(GnostrTimelineBatchKind kind);

G_END_DECLS

#endif /* GNOSTR_TIMELINE_BATCH_H */
