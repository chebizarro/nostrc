#include "gh-mls-imeta.h"

#include "gh-store.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <string.h>

gchar *
gh_mls_imeta_file_id(const MarmotMediaReference *ref, const gchar *group_id_hex, guint64 epoch)
{
  g_return_val_if_fail(ref != NULL, NULL);
  return gh_store_mls_media_file_id(group_id_hex, epoch, ref->ciphertext_sha256,
                                    ref->plaintext_sha256, ref->nonce, ref->media_type,
                                    ref->filename);
}

static GhMessageAttachment *
describe(const MarmotMediaReference *ref, const gchar *group_id_hex, gboolean has_epoch,
         guint64 epoch)
{
  GhMessageAttachment *a = g_new0(GhMessageAttachment, 1);
  a->media_type = g_strdup(ref->media_type);
  a->filename = g_strdup(ref->filename);
  /* dim is "<w>x<h>", a hint only (a card's size before download). */
  if (ref->dim) {
    guint64 w = 0, h = 0;
    gchar *end = NULL;
    w = g_ascii_strtoull(ref->dim, &end, 10);
    if (end && *end == 'x')
      h = g_ascii_strtoull(end + 1, &end, 10);
    if (end && !*end && w > 0 && h > 0 && w <= G_MAXUINT16 && h <= G_MAXUINT16) {
      a->width = (guint)w;
      a->height = (guint)h;
    }
  }
  a->file_id = has_epoch ? gh_mls_imeta_file_id(ref, group_id_hex, epoch) : NULL;
  return a;
}

void
gh_mls_imeta_describe(GhMessage *message, const gchar *group_id_hex, gboolean has_epoch,
                      guint64 epoch)
{
  g_return_if_fail(GH_IS_MESSAGE(message) && gh_message_is_mls(message));
  if (has_epoch)
    gh_message_set_mls_epoch(message, epoch);
  const gchar *json = gh_message_get_rumor_json(message);
  NostrEvent *event = json ? nostr_event_new() : NULL;
  if (!event)
    return;
  g_autoptr(GPtrArray) out =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_message_attachment_free);
  guint rejected = 0;
  if (nostr_event_deserialize_unsigned(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK) {
    NostrTags *tags = nostr_event_get_tags(event);
    for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
      NostrTag *t = nostr_tags_get(tags, i);
      if (!t || nostr_tag_size(t) < 1 || g_strcmp0(nostr_tag_get(t, 0), "imeta") != 0)
        continue;
      if (out->len >= GH_MLS_IMETA_MAX_ATTACHMENTS) {
        rejected++;
        continue;
      }
      gsize n = nostr_tag_size(t);
      g_autofree const char **fields = g_new0(const char *, n + 1);
      for (gsize k = 0; k < n; k++)
        fields[k] = nostr_tag_get(t, k) ? nostr_tag_get(t, k) : "";
      MarmotMediaReference ref;
      memset(&ref, 0, sizeof ref);
      if (marmot_media_imeta_parse(fields, NULL, n, &ref) == MARMOT_OK) {
        g_ptr_array_add(out, describe(&ref, group_id_hex, has_epoch, epoch));
        marmot_media_reference_clear(&ref);
      } else {
        rejected++;
      }
    }
  }
  nostr_event_free(event);
  gh_message_set_attachments(message, out, rejected);
}

GStrv
gh_mls_imeta_dup_file_ids(GhMessage *message)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  guint n = gh_message_get_n_attachments(message);
  g_autoptr(GPtrArray) ids = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < n; i++) {
    const GhMessageAttachment *a = gh_message_get_attachment(message, i);
    gboolean seen = !a->file_id;
    for (guint k = 0; !seen && k < ids->len; k++)
      seen = g_str_equal(g_ptr_array_index(ids, k), a->file_id);
    if (!seen)
      g_ptr_array_add(ids, g_strdup(a->file_id));
  }
  if (ids->len == 0)
    return NULL;
  g_ptr_array_add(ids, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&ids), FALSE);
}
