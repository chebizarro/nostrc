#ifndef GH_EMOJI_H
#define GH_EMOJI_H
#include <gio/gio.h>
#include "nostr-event.h"
typedef struct _GhSocialLists GhSocialLists;
G_BEGIN_DECLS
/* Owned arrays of NULL-terminated full emoji tags. No I/O. */
GPtrArray *gh_emoji_tags_new(void);
gboolean gh_emoji_tag_valid(const gchar *const *tag);
gboolean gh_emoji_append(NostrEvent *event, GPtrArray *tags, GError **error);
GPtrArray *gh_emoji_tags_for_text(GPtrArray *tags, const gchar *text);
GPtrArray *gh_emoji_parse(const gchar *event_json);
/* Rebuild an unsigned event, including its canonical id, before persistence/signing. */
gchar *gh_emoji_tag_event(const gchar *json, GPtrArray *tags, gchar **out_id, GError **error);
typedef struct {
  gchar *title;
  GPtrArray *tags;
} GhEmojiSet;
void gh_emoji_set_free(GhEmojiSet *set);
/* Favorites first, then locally available sets. Never resolve remote references. */
GPtrArray *gh_emoji_catalog(GhSocialLists *lists);
G_END_DECLS
#endif
