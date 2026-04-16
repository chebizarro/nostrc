/* gnostr-filter-set-predefined-services.c — bind predefined filter sets
 * to the live follow / bookmark / mute list singletons.
 *
 * SPDX-License-Identifier: MIT
 *
 * Split out of gnostr-filter-set-predefined.c so the pure builders can
 * be unit-tested without linking the bookmark / follow-list / mute-list
 * services.
 *
 * nostrc-yg8j.3.
 */

#define G_LOG_DOMAIN "gnostr-filter-set-predefined"

#include "gnostr-filter-set-predefined.h"

#include "util/bookmarks.h"
#include "util/follow_list.h"

/* The mute list lives in nostr-gobject and uses its own header. */
#include "nostr-gobject-1.0/gnostr-mute-list.h"

#include <string.h>

/* Convert a count-out array from bookmarks/mute list accessors into a
 * NULL-terminated strv. Returns NULL when empty. */
static gchar **
cstr_array_to_strv(const char **src, size_t count)
{
  if (!src || count == 0) return NULL;
  gchar **out = g_new0(gchar *, count + 1);
  for (size_t i = 0; i < count; i++)
    out[i] = g_strdup(src[i]);
  out[count] = NULL;
  return out;
}

void
gnostr_filter_set_predefined_refresh_from_services(
    GnostrFilterSetManager *manager,
    const gchar *user_pubkey_hex)
{
  g_return_if_fail(GNOSTR_IS_FILTER_SET_MANAGER(manager));

  /* --- Follow list (NIP-02) ---------------------------------------- */
  g_auto(GStrv) follows = NULL;
  if (user_pubkey_hex && *user_pubkey_hex)
    follows = gnostr_follow_list_get_pubkeys_cached(user_pubkey_hex);

  /* --- Bookmarks (NIP-51) ------------------------------------------ */
  g_auto(GStrv) bookmarks = NULL;
  {
    GnostrBookmarks *bm = gnostr_bookmarks_get_default();
    size_t n = 0;
    const char **raw = gnostr_bookmarks_get_event_ids(bm, &n);
    bookmarks = cstr_array_to_strv(raw, n);
    /* gnostr_bookmarks_get_event_ids returns (transfer container) —
     * strings are owned by the bookmarks object, but the outer array
     * itself is allocated for us. */
    g_free((gpointer)raw);
  }

  /* --- Mute list (NIP-51) ------------------------------------------ */
  g_auto(GStrv) muted = NULL;
  {
    GNostrMuteList *ml = gnostr_mute_list_get_default();
    size_t n = 0;
    const char **raw = gnostr_mute_list_get_pubkeys(ml, &n);
    muted = cstr_array_to_strv(raw, n);
    g_free((gpointer)raw);
  }

  gnostr_filter_set_predefined_install(
      manager,
      (const gchar * const *)follows,
      (const gchar * const *)bookmarks,
      (const gchar * const *)muted);
}
