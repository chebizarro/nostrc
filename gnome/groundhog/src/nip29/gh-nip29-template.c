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
