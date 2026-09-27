/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-events.c - see gn-nip29-events.h
 */
#include "gn-nip29-events.h"

#include <json-glib/json-glib.h>
#include <nostr-kinds.h>
#include <string.h>

static gboolean
set(const char *s)
{
  return s != NULL && s[0] != '\0';
}

static void
tag1(JsonBuilder *b, const char *name)
{
  json_builder_begin_array(b);
  json_builder_add_string_value(b, name);
  json_builder_end_array(b);
}

static void
tag2(JsonBuilder *b, const char *name, const char *value)
{
  json_builder_begin_array(b);
  json_builder_add_string_value(b, name);
  json_builder_add_string_value(b, value);
  json_builder_end_array(b);
}

/* {"kind":k,"created_at":t,"content":"","tags":[["h",id], ...]}; the
 * caller appends tags between begin and finish. */
static JsonBuilder *
begin(int kind, const char *group_id, gint64 created_at)
{
  JsonBuilder *b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kind");
  json_builder_add_int_value(b, kind);
  json_builder_set_member_name(b, "created_at");
  json_builder_add_int_value(b, created_at);
  json_builder_set_member_name(b, "content");
  json_builder_add_string_value(b, "");
  json_builder_set_member_name(b, "tags");
  json_builder_begin_array(b);
  tag2(b, "h", group_id);
  return b;
}

static gchar *
finish(JsonBuilder *b)
{
  json_builder_end_array(b);
  json_builder_end_object(b);
  JsonNode *root = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_root(gen, root);
  gchar *json = json_generator_to_data(gen, NULL);
  json_node_unref(root);
  g_object_unref(b);
  return json;
}

gboolean
gn_nip29_metadata_is_empty(const GnNip29Metadata *md)
{
  return md == NULL ||
         (!set(md->name) && !set(md->about) && !set(md->picture) && !set(md->banner) &&
          !set(md->parent) && !md->is_private && !md->is_restricted && !md->is_hidden &&
          !md->is_closed);
}

gchar *
gn_nip29_build_create_group_json(const char *group_id, gint64 created_at)
{
  g_return_val_if_fail(set(group_id), NULL);
  return finish(begin(NOSTR_KIND_SIMPLE_GROUP_CREATE_GROUP, group_id, created_at));
}

gchar *
gn_nip29_build_edit_metadata_json(const char            *group_id,
                                  const GnNip29Metadata *md,
                                  gint64                 created_at)
{
  g_return_val_if_fail(set(group_id) && md != NULL, NULL);
  JsonBuilder *b = begin(NOSTR_KIND_SIMPLE_GROUP_EDIT_METADATA, group_id, created_at);
  if (set(md->name)) tag2(b, "name", md->name);
  if (set(md->about)) tag2(b, "about", md->about);
  if (set(md->picture)) tag2(b, "picture", md->picture);
  if (set(md->banner)) tag2(b, "banner", md->banner);
  if (md->is_private) tag1(b, "private");
  if (md->is_restricted) tag1(b, "restricted");
  if (md->is_hidden) tag1(b, "hidden");
  if (md->is_closed) tag1(b, "closed");
  /* At most one parent (NIP-29 subgroups). */
  if (set(md->parent)) tag2(b, "parent", md->parent);
  return finish(b);
}

static gboolean
is_hex64(const char *s)
{
  if (!s || strlen(s) != 64) return FALSE;
  for (const char *p = s; *p; p++)
    if (!g_ascii_isxdigit(*p)) return FALSE;
  return TRUE;
}

gchar *
gn_nip29_build_update_pin_list_json(const char         *group_id,
                                    const char * const *refs,
                                    gint64              created_at)
{
  g_return_val_if_fail(set(group_id), NULL);
  JsonBuilder *b = begin(NOSTR_KIND_SIMPLE_GROUP_UPDATE_PIN_LIST, group_id, created_at);
  for (gsize i = 0; refs && refs[i]; i++) {
    if (is_hex64(refs[i])) {
      g_autofree char *lower = g_ascii_strdown(refs[i], -1);
      tag2(b, "e", lower);
    } else if (strchr(refs[i], ':')) {
      tag2(b, "a", refs[i]);
    }
  }
  return finish(b);
}
