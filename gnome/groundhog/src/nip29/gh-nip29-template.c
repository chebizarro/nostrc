#include "gh-nip29-template.h"

#include <nostr-kinds.h>
#include <stdlib.h>
#include <string.h>

static gboolean
fail(GError **error, GhNip29Error code, const gchar *message)
{
  g_set_error_literal(error, GH_NIP29_ERROR, code, message);
  return FALSE;
}

GStrv
gh_nip29_select_previous(const gchar *author_pubkey, const GhNip29TimelineRef *recent,
                         gsize n_recent, GError **error)
{
  if (!gh_nip29_is_hex64(author_pubkey)) {
    fail(error, GH_NIP29_ERROR_INVALID_PUBKEY, "author pubkey must be 64-char lowercase hex");
    return NULL;
  }
  if (n_recent > 0 && !recent) {
    fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "timeline references are missing");
    return NULL;
  }
  /* Only the last 50 events seen are citable; the user's own events count
   * toward that window but may not be cited. */
  gsize window = MIN(n_recent, (gsize)GH_NIP29_PREVIOUS_WINDOW);
  for (gsize i = 0; i < window; i++) {
    if (!gh_nip29_is_hex64(recent[i].event_id) || !gh_nip29_is_hex64(recent[i].pubkey)) {
      g_set_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT,
                  "timeline reference %" G_GSIZE_FORMAT " needs a hex event id and author", i);
      return NULL;
    }
  }

  GPtrArray *prefixes = g_ptr_array_new();
  for (gsize i = 0; i < window && prefixes->len < GH_NIP29_PREVIOUS_REFS; i++) {
    if (strcmp(recent[i].pubkey, author_pubkey) == 0)
      continue;
    gboolean seen = FALSE;
    for (guint j = 0; j < prefixes->len && !seen; j++)
      seen = strncmp(g_ptr_array_index(prefixes, j), recent[i].event_id,
                     GH_NIP29_PREVIOUS_PREFIX_LEN) == 0;
    if (!seen)
      g_ptr_array_add(prefixes, g_strndup(recent[i].event_id, GH_NIP29_PREVIOUS_PREFIX_LEN));
  }
  g_ptr_array_add(prefixes, NULL);
  return (GStrv)g_ptr_array_free(prefixes, FALSE);
}

/* ---- Timeline ring --------------------------------------------------------- */

struct _GhNip29Timeline {
  GArray *entries;   /* GhNip29TimelineEntry, newest first */
  GArray *refs;      /* GhNip29TimelineRef into entries; rebuilt on demand */
  gboolean refs_valid;
};

GhNip29Timeline *
gh_nip29_timeline_new(void)
{
  GhNip29Timeline *timeline = g_new0(GhNip29Timeline, 1);
  timeline->entries = g_array_sized_new(FALSE, TRUE, sizeof(GhNip29TimelineEntry),
                                        GH_NIP29_PREVIOUS_WINDOW + 1);
  timeline->refs = g_array_new(FALSE, TRUE, sizeof(GhNip29TimelineRef));
  return timeline;
}

void
gh_nip29_timeline_free(GhNip29Timeline *timeline)
{
  if (!timeline)
    return;
  g_array_unref(timeline->entries);
  g_array_unref(timeline->refs);
  g_free(timeline);
}

/* <0 when (created_at, id) sorts before entry in newest-first order. */
static gint
timeline_order(gint64 created_at, const gchar *id, const GhNip29TimelineEntry *entry)
{
  if (created_at != entry->created_at)
    return created_at > entry->created_at ? -1 : 1;
  return strcmp(id, entry->event_id);
}

gboolean
gh_nip29_timeline_add(GhNip29Timeline *timeline, const gchar *event_id, const gchar *pubkey,
                      gint64 created_at)
{
  g_return_val_if_fail(timeline != NULL, FALSE);
  if (!gh_nip29_is_hex64(event_id) || !gh_nip29_is_hex64(pubkey) || created_at < 0)
    return FALSE;
  GArray *entries = timeline->entries;
  guint position = entries->len;
  for (guint i = 0; i < entries->len; i++) {
    const GhNip29TimelineEntry *entry = &g_array_index(entries, GhNip29TimelineEntry, i);
    if (strcmp(entry->event_id, event_id) == 0)
      return FALSE;
    if (position == entries->len && timeline_order(created_at, event_id, entry) < 0)
      position = i;
  }
  if (position >= GH_NIP29_PREVIOUS_WINDOW)
    return FALSE; /* older than everything a full window keeps */
  GhNip29TimelineEntry entry = { .created_at = created_at };
  memcpy(entry.event_id, event_id, 65);
  memcpy(entry.pubkey, pubkey, 65);
  g_array_insert_val(entries, position, entry);
  if (entries->len > GH_NIP29_PREVIOUS_WINDOW)
    g_array_set_size(entries, GH_NIP29_PREVIOUS_WINDOW);
  timeline->refs_valid = FALSE;
  return TRUE;
}

gboolean
gh_nip29_timeline_remove(GhNip29Timeline *timeline, const gchar *event_id)
{
  g_return_val_if_fail(timeline != NULL, FALSE);
  for (guint i = 0; event_id && i < timeline->entries->len; i++) {
    if (strcmp(g_array_index(timeline->entries, GhNip29TimelineEntry, i).event_id,
               event_id) == 0) {
      g_array_remove_index(timeline->entries, i);
      timeline->refs_valid = FALSE;
      return TRUE;
    }
  }
  return FALSE;
}

guint
gh_nip29_timeline_get_length(const GhNip29Timeline *timeline)
{
  g_return_val_if_fail(timeline != NULL, 0);
  return timeline->entries->len;
}

const GhNip29TimelineEntry *
gh_nip29_timeline_get_entry(const GhNip29Timeline *timeline, guint index)
{
  g_return_val_if_fail(timeline != NULL && index < timeline->entries->len, NULL);
  return &g_array_index(timeline->entries, GhNip29TimelineEntry, index);
}

const GhNip29TimelineRef *
gh_nip29_timeline_get_refs(GhNip29Timeline *timeline, gsize *n_refs)
{
  g_return_val_if_fail(timeline != NULL, NULL);
  if (!timeline->refs_valid) {
    g_array_set_size(timeline->refs, timeline->entries->len);
    for (guint i = 0; i < timeline->entries->len; i++) {
      const GhNip29TimelineEntry *entry =
        &g_array_index(timeline->entries, GhNip29TimelineEntry, i);
      GhNip29TimelineRef *ref = &g_array_index(timeline->refs, GhNip29TimelineRef, i);
      ref->event_id = entry->event_id;
      ref->pubkey = entry->pubkey;
    }
    timeline->refs_valid = TRUE;
  }
  if (n_refs)
    *n_refs = timeline->refs->len;
  return (const GhNip29TimelineRef *)(gconstpointer)timeline->refs->data;
}

/* ---- Templates ---------------------------------------------------------------- */

static gboolean
check_context(const GhNip29GroupKey *group, const GhNip29TemplateContext *context,
              GError **error)
{
  if (!group || !context)
    return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "group and template context are required");
  if (!gh_nip29_is_hex64(context->author_pubkey))
    return fail(error, GH_NIP29_ERROR_INVALID_PUBKEY, "author pubkey must be 64-char lowercase hex");
  if (context->created_at <= 0)
    return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "created_at must be positive");
  return TRUE;
}

static gboolean
check_text(const gchar *text, gboolean required, const gchar *what, GError **error)
{
  if (!text || (required && !*text)) {
    if (!required)
      return TRUE;
    g_set_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT, "%s must not be empty", what);
    return FALSE;
  }
  if (!g_utf8_validate(text, -1, NULL)) {
    g_set_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_ARGUMENT, "%s must be valid UTF-8", what);
    return FALSE;
  }
  return TRUE;
}

static gboolean
check_pubkey(const gchar *pubkey, GError **error)
{
  return gh_nip29_is_hex64(pubkey) ||
         fail(error, GH_NIP29_ERROR_INVALID_PUBKEY, "target pubkey must be 64-char lowercase hex");
}

typedef struct {
  NostrTags *tags;
  GStrv previous;
  gboolean ok;
} TemplateBuild;

static void
template_add(TemplateBuild *build, NostrTag *tag)
{
  if (!build->ok || !tag) {
    nostr_tag_free(tag);
    build->ok = FALSE;
    return;
  }
  gsize before = nostr_tags_size(build->tags);
  nostr_tags_append(build->tags, tag);
  if (nostr_tags_size(build->tags) != before + 1) {
    nostr_tag_free(tag);
    build->ok = FALSE;
  }
}

/* Called after all caller input is validated; only allocation can fail
 * between here and template_finish(). */
static gboolean
template_begin(TemplateBuild *build, const GhNip29GroupKey *group,
               const GhNip29TemplateContext *context, GError **error)
{
  memset(build, 0, sizeof(*build));
  build->previous = gh_nip29_select_previous(context->author_pubkey, context->recent,
                                             context->n_recent, error);
  if (!build->previous)
    return FALSE;
  build->tags = nostr_tags_new(0);
  build->ok = build->tags != NULL;
  template_add(build, nostr_tag_new("h", gh_nip29_group_key_get_group_id(group), NULL));
  return TRUE;
}

static gchar *
template_finish(TemplateBuild *build, const GhNip29TemplateContext *context, gint kind,
                const gchar *content, GError **error)
{
  if (build->previous[0]) {
    NostrTag *tag = nostr_tag_new("previous", NULL);
    for (gsize i = 0; tag && build->previous[i]; i++)
      nostr_tag_append(tag, build->previous[i]);
    template_add(build, tag);
  }
  g_clear_pointer(&build->previous, g_strfreev);

  gchar *json = NULL;
  NostrEvent *event = build->ok ? nostr_event_new() : NULL;
  if (event) {
    nostr_event_set_kind(event, kind);
    nostr_event_set_pubkey(event, context->author_pubkey);
    nostr_event_set_created_at(event, context->created_at);
    nostr_event_set_content(event, content ? content : "");
    nostr_event_set_tags(event, g_steal_pointer(&build->tags));
    char *raw = nostr_event_serialize_compact(event);
    nostr_event_free(event);
    if (raw) {
      json = g_strdup(raw);
      free(raw);
    }
  }
  if (build->tags)
    nostr_tags_free(build->tags);
  build->tags = NULL;
  if (!json)
    fail(error, GH_NIP29_ERROR_FAILED, "could not serialize the NIP-29 event template");
  return json;
}

gchar *
gh_nip29_template_chat(const GhNip29GroupKey *group, const GhNip29TemplateContext *context,
                       const gchar *text, GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_text(text, TRUE, "chat text", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE, text, error);
}

gchar *
gh_nip29_template_join_request(const GhNip29GroupKey *group,
                               const GhNip29TemplateContext *context, const gchar *reason,
                               const gchar *invite_code, GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_text(reason, FALSE, "reason", error) ||
      !check_text(invite_code, FALSE, "invite code", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  if (invite_code && *invite_code)
    template_add(&build, nostr_tag_new("code", invite_code, NULL));
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_JOIN_REQUEST, reason, error);
}

gchar *
gh_nip29_template_leave_request(const GhNip29GroupKey *group,
                                const GhNip29TemplateContext *context, const gchar *reason,
                                GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_text(reason, FALSE, "reason", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_LEAVE_REQUEST, reason, error);
}

gchar *
gh_nip29_template_put_user(const GhNip29GroupKey *group, const GhNip29TemplateContext *context,
                           const gchar *pubkey, const gchar *const *roles, const gchar *reason,
                           GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_pubkey(pubkey, error) ||
      !check_text(reason, FALSE, "reason", error))
    return NULL;
  for (gsize i = 0; roles && roles[i]; i++) {
    if (!check_text(roles[i], TRUE, "role", error))
      return NULL;
  }
  if (!template_begin(&build, group, context, error))
    return NULL;
  NostrTag *tag = nostr_tag_new("p", pubkey, NULL);
  for (gsize i = 0; tag && roles && roles[i]; i++) {
    gboolean seen = FALSE;
    for (gsize j = 0; j < i && !seen; j++)
      seen = strcmp(roles[j], roles[i]) == 0;
    if (!seen)
      nostr_tag_append(tag, roles[i]);
  }
  template_add(&build, tag);
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_ADD_USER, reason, error);
}

gchar *
gh_nip29_template_remove_user(const GhNip29GroupKey *group,
                              const GhNip29TemplateContext *context, const gchar *pubkey,
                              const gchar *reason, GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_pubkey(pubkey, error) ||
      !check_text(reason, FALSE, "reason", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  template_add(&build, nostr_tag_new("p", pubkey, NULL));
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_REMOVE_USER, reason, error);
}

static gboolean
check_related_group(const GhNip29GroupKey *group, const gchar *id, const gchar *what,
                    GError **error)
{
  g_autoptr(GhNip29GroupKey) related =
    gh_nip29_group_key_new(gh_nip29_group_key_get_relay_url(group), id, NULL);
  if (related && g_strcmp0(id, gh_nip29_group_key_get_group_id(group)) != 0)
    return TRUE;
  g_set_error(error, GH_NIP29_ERROR, GH_NIP29_ERROR_INVALID_GROUP_ID,
              "%s must be another valid group id", what);
  return FALSE;
}

static gboolean
check_metadata(const GhNip29GroupKey *group, const GhNip29Metadata *metadata, GError **error)
{
  if (!metadata)
    return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "metadata is required");
  if (!check_text(metadata->name, FALSE, "name", error) ||
      !check_text(metadata->about, FALSE, "about", error) ||
      !check_text(metadata->picture, FALSE, "picture", error) ||
      !check_text(metadata->banner, FALSE, "banner", error))
    return FALSE;
  if (metadata->n_supported_kinds > 0 && !metadata->supported_kinds)
    return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "supported kinds are missing");
  for (gsize i = 0; metadata->has_supported_kinds && i < metadata->n_supported_kinds; i++) {
    if (metadata->supported_kinds[i] < 0 || metadata->supported_kinds[i] > 65535)
      return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "supported kinds must be 0-65535");
  }
  if (metadata->parent && !check_related_group(group, metadata->parent, "parent", error))
    return FALSE;
  for (gsize i = 0; metadata->children && metadata->children[i]; i++) {
    if (!check_related_group(group, metadata->children[i], "child", error))
      return FALSE;
  }
  static const gchar *const modelled[] = {
    "d", "h", "previous", "name", "picture", "banner", "about", "private", "restricted",
    "hidden", "closed", "livekit", "supported_kinds", "parent", "child", NULL,
  };
  guint n_extra = metadata->extra_tags ? metadata->extra_tags->len : 0;
  if (n_extra > GH_NIP29_MAX_EXTRA_TAGS)
    return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "too many extra metadata tags");
  for (guint i = 0; i < n_extra; i++) {
    const gchar *const *tag = g_ptr_array_index(metadata->extra_tags, i);
    guint n = tag ? g_strv_length((GStrv)tag) : 0;
    if (n == 0 || !*tag[0] || g_strv_contains(modelled, tag[0]) ||
        n > GH_NIP29_MAX_EXTRA_TAG_VALUES + 1)
      return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT,
                  "an extra metadata tag needs a name the model does not own");
    for (guint j = 0; j < n; j++) {
      if (strlen(tag[j]) > GH_NIP29_MAX_EXTRA_TAG_VALUE_BYTES ||
          !g_utf8_validate(tag[j], -1, NULL))
        return fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT,
                    "an extra metadata tag value is too long or not UTF-8");
    }
  }
  return TRUE;
}

static void
add_optional(TemplateBuild *build, const gchar *key, const gchar *value)
{
  if (value)
    template_add(build, nostr_tag_new(key, value, NULL));
}

static void
add_flag(TemplateBuild *build, const gchar *key, gboolean set)
{
  if (set)
    template_add(build, nostr_tag_new(key, NULL));
}

gchar *
gh_nip29_template_edit_metadata(const GhNip29GroupKey *group,
                                const GhNip29TemplateContext *context,
                                const GhNip29Metadata *metadata, const gchar *reason,
                                GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_metadata(group, metadata, error) ||
      !check_text(reason, FALSE, "reason", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  /* Field order follows the NIP-29 kind:39000 example. */
  add_optional(&build, "name", metadata->name);
  add_optional(&build, "picture", metadata->picture);
  add_optional(&build, "banner", metadata->banner);
  add_optional(&build, "about", metadata->about);
  add_flag(&build, "private", metadata->is_private);
  add_flag(&build, "restricted", metadata->is_restricted);
  add_flag(&build, "hidden", metadata->is_hidden);
  add_flag(&build, "closed", metadata->is_closed);
  add_flag(&build, "livekit", metadata->has_livekit);
  if (metadata->has_supported_kinds) {
    NostrTag *tag = nostr_tag_new("supported_kinds", NULL);
    for (gsize i = 0; tag && i < metadata->n_supported_kinds; i++) {
      gchar kind[8];
      g_snprintf(kind, sizeof(kind), "%d", metadata->supported_kinds[i]);
      nostr_tag_append(tag, kind);
    }
    template_add(&build, tag);
  }
  add_optional(&build, "parent", metadata->parent);
  for (gsize i = 0; metadata->children && metadata->children[i]; i++)
    template_add(&build, nostr_tag_new("child", metadata->children[i], NULL));
  /* Relay-specific and newer fields the relay published, verbatim. */
  for (guint i = 0; metadata->extra_tags && i < metadata->extra_tags->len; i++) {
    const gchar *const *extra = g_ptr_array_index(metadata->extra_tags, i);
    NostrTag *tag = nostr_tag_new(extra[0], NULL);
    for (guint j = 1; tag && extra[j]; j++)
      nostr_tag_append(tag, extra[j]);
    template_add(&build, tag);
  }
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_EDIT_METADATA, reason, error);
}

gchar *
gh_nip29_template_delete_event(const GhNip29GroupKey *group,
                               const GhNip29TemplateContext *context, const gchar *event_id,
                               const gchar *reason, GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error))
    return NULL;
  if (!gh_nip29_is_hex64(event_id)) {
    fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "event id must be 64-char lowercase hex");
    return NULL;
  }
  if (!check_text(reason, FALSE, "reason", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  template_add(&build, nostr_tag_new("e", event_id, NULL));
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_DELETE_EVENT, reason, error);
}

gchar *
gh_nip29_template_reaction(const GhNip29GroupKey *group,
                           const GhNip29TemplateContext *context,
                           const gchar *target_event_id,
                           const gchar *target_pubkey,
                           const gchar *target_kind_str,
                           const gchar *emoji, GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) ||
      !check_text(emoji, TRUE, "emoji", error))
    return NULL;
  if (!gh_nip29_is_hex64(target_event_id)) {
    fail(error, GH_NIP29_ERROR_INVALID_ARGUMENT, "target event id must be 64-char lowercase hex");
    return NULL;
  }
  if (!check_pubkey(target_pubkey, error))
    return NULL;
  if (!check_text(target_kind_str, TRUE, "target kind", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  template_add(&build, nostr_tag_new("e", target_event_id, NULL));
  template_add(&build, nostr_tag_new("p", target_pubkey, NULL));
  template_add(&build, nostr_tag_new("k", target_kind_str, NULL));
  return template_finish(&build, context, 7, emoji, error);
}

gchar *
gh_nip29_template_create_invite(const GhNip29GroupKey *group,
                                const GhNip29TemplateContext *context, const gchar *code,
                                const gchar *reason, GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_text(code, TRUE, "invite code", error) ||
      !check_text(reason, FALSE, "reason", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  template_add(&build, nostr_tag_new("code", code, NULL));
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_CREATE_INVITE, reason, error);
}

gchar *
gh_nip29_template_create_group(const GhNip29GroupKey *group,
                               const GhNip29TemplateContext *context, const gchar *reason,
                               GError **error)
{
  TemplateBuild build;
  if (!check_context(group, context, error) || !check_text(reason, FALSE, "reason", error) ||
      !template_begin(&build, group, context, error))
    return NULL;
  return template_finish(&build, context, NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP, reason, error);
}
