/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-migration.c - see gn-nip29-migration.h (nostrc-7n4t)
 */
#include "gn-nip29-migration.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

#define KIND_SIMPLE_GROUPS 10009

void
gn_nip29_relocation_free(GnNip29Relocation *relocation)
{
  if (!relocation)
    return;
  g_free(relocation->relay_url);
  g_clear_pointer(&relocation->pubkeys, g_ptr_array_unref);
  g_free(relocation);
}

/* "WSS://Relay.Example:443/" -> "wss://relay.example" (path case kept). */
static gchar *
normalize_relay(const char *url)
{
  if (!url)
    return NULL;
  g_autofree gchar *s = g_strstrip(g_strdup(url));
  gsize len = strlen(s);
  while (len > 0 && s[len - 1] == '/')
    s[--len] = '\0';
  const char *sep = strstr(s, "://");
  const char *host = sep ? sep + 3 : s;
  const char *path = strchr(host, '/');
  gsize head = path ? (gsize)(path - s) : len;
  g_autofree gchar *lower = g_ascii_strdown(s, (gssize)head);
  const char *dflt = g_str_has_prefix(lower, "wss://") ? ":443"
                   : g_str_has_prefix(lower, "ws://") ? ":80" : NULL;
  if (dflt && g_str_has_suffix(lower, dflt))
    lower[strlen(lower) - strlen(dflt)] = '\0';
  return g_strconcat(lower, path ? path : "", NULL);
}

gboolean
gn_nip29_relay_url_equal(const char *a, const char *b)
{
  g_autofree gchar *na = normalize_relay(a);
  g_autofree gchar *nb = normalize_relay(b);
  return na && nb && *na && g_strcmp0(na, nb) == 0;
}

static gboolean
is_trusted(const char * const *trusted, const char *pubkey)
{
  for (gsize i = 0; trusted && trusted[i]; i++)
    if (g_ascii_strcasecmp(trusted[i], pubkey) == 0)
      return TRUE;
  return FALSE;
}

GPtrArray *
gn_nip29_find_relocations(const char         *group_id,
                          const char         *current_relay,
                          const char * const *trusted_pubkeys,
                          GPtrArray          *event_jsons,
                          guint              *out_staying)
{
  GPtrArray *out = g_ptr_array_new_with_free_func((GDestroyNotify)gn_nip29_relocation_free);
  if (out_staying)
    *out_staying = 0;
  if (!group_id || !*group_id || !event_jsons)
    return out;

  /* Newest verified kind:10009 per trusted author (replaceable kind). */
  GHashTable *newest = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                             (GDestroyNotify)nostr_event_free);
  for (guint i = 0; i < event_jsons->len; i++) {
    const char *json = g_ptr_array_index(event_jsons, i);
    NostrEvent *ev = nostr_event_new();
    if (!json || nostr_event_deserialize_compact(ev, json, NULL) != 1 ||
        nostr_event_get_kind(ev) != KIND_SIMPLE_GROUPS ||
        !nostr_event_get_pubkey(ev) ||
        !is_trusted(trusted_pubkeys, nostr_event_get_pubkey(ev)) ||
        !nostr_event_check_signature(ev)) {
      nostr_event_free(ev);
      continue;
    }
    g_autofree gchar *author = g_ascii_strdown(nostr_event_get_pubkey(ev), -1);
    NostrEvent *have = g_hash_table_lookup(newest, author);
    if (have && nostr_event_get_created_at(have) >= nostr_event_get_created_at(ev)) {
      nostr_event_free(ev);
      continue;
    }
    g_hash_table_replace(newest, g_steal_pointer(&author), ev);
  }

  /* Relays other than the current one that each list names for the group. */
  GHashTableIter it;
  gpointer key, value;
  g_hash_table_iter_init(&it, newest);
  while (g_hash_table_iter_next(&it, &key, &value)) {
    NostrTags *tags = nostr_event_get_tags(value);
    gboolean stays = FALSE;
    for (size_t t = 0; tags && t < nostr_tags_size(tags); t++) {
      NostrTag *tag = nostr_tags_get(tags, t);
      if (nostr_tag_size(tag) < 3 || g_strcmp0(nostr_tag_get(tag, 0), "group") != 0 ||
          g_strcmp0(nostr_tag_get(tag, 1), group_id) != 0)
        continue;
      const char *relay = nostr_tag_get(tag, 2);
      if (!relay || !*relay)
        continue;
      if (gn_nip29_relay_url_equal(relay, current_relay)) {
        stays = TRUE;
        continue;
      }
      GnNip29Relocation *hit = NULL;
      for (guint r = 0; r < out->len && !hit; r++) {
        GnNip29Relocation *cand = g_ptr_array_index(out, r);
        if (gn_nip29_relay_url_equal(cand->relay_url, relay))
          hit = cand;
      }
      if (!hit) {
        hit = g_new0(GnNip29Relocation, 1);
        hit->relay_url = g_strdup(relay);
        hit->pubkeys = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(out, hit);
      }
      gboolean counted = FALSE;
      for (guint p = 0; p < hit->pubkeys->len && !counted; p++)
        counted = g_strcmp0(g_ptr_array_index(hit->pubkeys, p), key) == 0;
      if (!counted)
        g_ptr_array_add(hit->pubkeys, g_strdup(key));
    }
    if (stays && out_staying)
      (*out_staying)++;
  }
  g_hash_table_unref(newest);

  /* Most authors first; insertion sort keeps ties in first-seen order. */
  for (guint i = 1; i < out->len; i++) {
    for (guint j = i; j > 0; j--) {
      GnNip29Relocation *a = g_ptr_array_index(out, j - 1);
      GnNip29Relocation *b = g_ptr_array_index(out, j);
      if (b->pubkeys->len <= a->pubkeys->len)
        break;
      out->pdata[j - 1] = b;
      out->pdata[j] = a;
    }
  }
  return out;
}
