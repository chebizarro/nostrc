#define G_LOG_DOMAIN "gnostr-mute-filter"

#include "mute_filter.h"

gboolean
gnostr_mute_filter_should_hide_fields(GNostrMuteList *mute_list,
                                      const char *pubkey_hex,
                                      const char *content,
                                      char * const *hashtags)
{
  if (!mute_list)
    return FALSE;

  if (pubkey_hex && gnostr_mute_list_is_pubkey_muted(mute_list, pubkey_hex))
    return TRUE;

  if (content && gnostr_mute_list_contains_muted_word(mute_list, content))
    return TRUE;

  if (hashtags) {
    for (guint i = 0; hashtags[i]; i++) {
      if (gnostr_mute_list_is_hashtag_muted(mute_list, hashtags[i]))
        return TRUE;
    }
  }

  return FALSE;
}
