#include "gh-emoji.h"

#include <nostr/nip30/nip30.h>
#include "nostr-kinds.h"
#include <glib/gi18n.h>
#include <string.h>

GPtrArray *
gh_emoji_tags_new(void)
{
  return g_ptr_array_new_with_free_func((GDestroyNotify)g_strfreev);
}

gboolean
gh_emoji_tag_valid(const gchar *const *tag)
{
  guint n = tag ? g_strv_length((gchar **)tag) : 0;
  if ((n != 3 && n != 4) || g_strcmp0(tag[0], "emoji") ||
      !nostr_nip30_valid_shortcode(tag[1]) || strlen(tag[1]) + 2 > 64)
    return FALSE;
  NostrEvent *event = nostr_event_new();
  gboolean valid = event && nostr_nip30_add_emoji_full(event, tag[1], tag[2],
                                                       n == 4 ? tag[3] : NULL) == 0;
  if (event) nostr_event_free(event);
  return valid;
}

gboolean
gh_emoji_append(NostrEvent *event, GPtrArray *tags, GError **error)
{
  if (tags && tags->len > 256) goto invalid;
  for (guint i = 0; tags && i < tags->len; i++)
    if (!gh_emoji_tag_valid(g_ptr_array_index(tags, i))) goto invalid;
  for (guint i = 0; tags && i < tags->len; i++) {
    const gchar *const *tag = g_ptr_array_index(tags, i);
    if (nostr_nip30_add_emoji_full(event, tag[1], tag[2], tag[3]) != 0) goto invalid;
  }
  return TRUE;
invalid:
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid custom emoji tags");
  return FALSE;
}

GPtrArray *
gh_emoji_tags_for_text(GPtrArray *tags, const gchar *text)
{
  GPtrArray *result = gh_emoji_tags_new();
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; tags && i < tags->len && result->len < 256; i++) {
    gchar **tag = g_ptr_array_index(tags, i);
    if (!gh_emoji_tag_valid((const gchar *const *)tag)) continue;
    g_autofree gchar *token = g_strdup_printf(":%s:", tag[1]);
    if (text && strstr(text, token) && g_hash_table_add(seen, tag[1]))
      g_ptr_array_add(result, g_strdupv(tag));
  }
  return result;
}

GPtrArray *
gh_emoji_parse(const gchar *json)
{
  GPtrArray *result = gh_emoji_tags_new();
  NostrEvent *event = nostr_event_new();
  if (json && event && nostr_event_deserialize_compact(event, json, NULL) == 1) {
    NostrNip30EmojiTag entries[256];
    size_t n = 0;
    if (nostr_nip30_parse_full(event, entries, G_N_ELEMENTS(entries), &n) == 0) {
      for (size_t i = 0; i < n; i++) {
        const gchar *fields[] = { "emoji", entries[i].shortcode, entries[i].url,
                                  entries[i].set_address, NULL };
        if (gh_emoji_tag_valid(fields)) g_ptr_array_add(result, g_strdupv((gchar **)fields));
      }
    }
  }
  if (event) nostr_event_free(event);
  return result;
}

gchar *
gh_emoji_tag_event(const gchar *json, GPtrArray *tags, gchar **out_id, GError **error)
{
  NostrEvent *event = nostr_event_new();
  gchar *result = NULL;
  if (!json || !event || nostr_event_deserialize_compact(event, json, NULL) != 1 ||
      (event->sig && *event->sig)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Expected an unsigned emoji event");
    goto done;
  }
  if (!gh_emoji_append(event, tags, error)) goto done;
  gchar id[65];
  if (nostr_event_compute_id(event, id) != NOSTR_EVENT_VALIDATION_OK) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not identify emoji event");
    goto done;
  }
  /* Signer templates omit id; stored rumors carry their canonical id. */
  if (event->id) {
    free(event->id);
    event->id = strdup(id);
  }
  char *serialized = nostr_event_serialize_compact(event);
  result = g_strdup(serialized);
  free(serialized);
  if (result && out_id) *out_id = g_strdup(id);
done:
  if (event) nostr_event_free(event);
  return result;
}
