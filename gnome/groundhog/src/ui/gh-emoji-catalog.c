#include "gh-emoji.h"
#include "gh-social-lists.h"
#include "nostr-kinds.h"
#include <glib/gi18n.h>

void
gh_emoji_set_free(GhEmojiSet *set)
{
  g_free(set->title);
  g_ptr_array_unref(set->tags);
  g_free(set);
}

GPtrArray *
gh_emoji_catalog(GhSocialLists *lists)
{
  GPtrArray *sets = g_ptr_array_new_with_free_func((GDestroyNotify)gh_emoji_set_free);
  GNostrListService *service = lists ? gh_social_lists_get_service(lists) : NULL;
  if (!service) return sets;
  for (guint pass = 0; pass < 2; pass++) {
    for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(service)); i++) {
      g_autoptr(GNostrListDocument) snapshot = g_list_model_get_item(G_LIST_MODEL(service), i);
      gint kind = gnostr_list_document_get_kind(snapshot);
      if (kind != (pass == 0 ? NOSTR_KIND_EMOJI_LIST : NOSTR_KIND_EMOJI_SETS)) continue;
      const NostrListDocument *doc = gnostr_list_document_get_codec(snapshot);
      GhEmojiSet *set = g_new0(GhEmojiSet, 1);
      set->title = g_strdup(pass == 0 ? _("Favorites") : gnostr_list_document_get_identifier(snapshot));
      set->tags = gh_emoji_tags_new();
      for (guint priv = 0; priv < 2; priv++) {
        for (guint j = 0; j < nostr_nip51_document_tag_count(doc, priv); j++) {
          const NostrListTag *tag = nostr_nip51_document_tag(doc, priv, j);
          const char *key = nostr_nip51_tag_field(tag, 0);
          if (!g_strcmp0(key, "emoji")) {
            const gchar *fields[] = { "emoji", nostr_nip51_tag_field(tag, 1),
              nostr_nip51_tag_field(tag, 2), nostr_nip51_tag_field(tag, 3), NULL };
            if (gh_emoji_tag_valid(fields) && set->tags->len < 256)
              g_ptr_array_add(set->tags, g_strdupv((gchar **)fields));
          } else if (pass && !g_strcmp0(key, "title")) {
            g_free(set->title);
            set->title = g_strdup(nostr_nip51_tag_field(tag, 1));
          }
        }
      }
      if (set->tags->len) g_ptr_array_add(sets, set);
      else gh_emoji_set_free(set);
    }
  }
  return sets;
}
