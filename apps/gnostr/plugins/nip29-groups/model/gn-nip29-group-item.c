/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-item.c - GObject representing a NIP-29 group
 */

#include "gn-nip29-group-item.h"
#include <string.h>

struct _GnNip29GroupItem
{
  GObject parent_instance;

  gchar   *key;
  gchar   *relay_url;
  gchar   *group_id;
  gchar   *alias;
  gchar   *name;
  gchar   *picture;
  gchar   *about;

  gboolean is_private;
  gboolean is_restricted;
  gboolean is_hidden;
  gboolean is_closed;

  gboolean admins_loaded;
  gboolean members_loaded;
  gboolean members_may_be_partial;
  gboolean roles_loaded;

  guint    admin_count;
  guint    member_count;
  guint    message_count;
};

G_DEFINE_TYPE(GnNip29GroupItem, gn_nip29_group_item, G_TYPE_OBJECT)

static void
gn_nip29_group_item_finalize(GObject *object)
{
  GnNip29GroupItem *self = GN_NIP29_GROUP_ITEM(object);

  g_free(self->key);
  g_free(self->relay_url);
  g_free(self->group_id);
  g_free(self->alias);
  g_free(self->name);
  g_free(self->picture);
  g_free(self->about);

  G_OBJECT_CLASS(gn_nip29_group_item_parent_class)->finalize(object);
}

static void
gn_nip29_group_item_class_init(GnNip29GroupItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gn_nip29_group_item_finalize;
}

static void
gn_nip29_group_item_init(GnNip29GroupItem *self)
{
}

GnNip29GroupItem *
gn_nip29_group_item_new(const char *key,
                        const char *relay_url,
                        const char *group_id,
                        const char *alias,
                        const char *name,
                        const char *picture,
                        const char *about,
                        gboolean    is_private,
                        gboolean    is_restricted,
                        gboolean    is_hidden,
                        gboolean    is_closed,
                        gboolean    admins_loaded,
                        gboolean    members_loaded,
                        gboolean    members_may_be_partial,
                        gboolean    roles_loaded,
                        guint       admin_count,
                        guint       member_count,
                        guint       message_count)
{
  GnNip29GroupItem *self = g_object_new(GN_TYPE_NIP29_GROUP_ITEM, NULL);

  self->key = g_strdup(key);
  self->relay_url = g_strdup(relay_url);
  self->group_id = g_strdup(group_id);
  self->alias = g_strdup(alias);
  self->name = g_strdup(name);
  self->picture = g_strdup(picture);
  self->about = g_strdup(about);
  self->is_private = is_private;
  self->is_restricted = is_restricted;
  self->is_hidden = is_hidden;
  self->is_closed = is_closed;
  self->admins_loaded = admins_loaded;
  self->members_loaded = members_loaded;
  self->members_may_be_partial = members_may_be_partial;
  self->roles_loaded = roles_loaded;
  self->admin_count = admin_count;
  self->member_count = member_count;
  self->message_count = message_count;

  return self;
}

const char *gn_nip29_group_item_get_key(GnNip29GroupItem *self)
{ return self->key; }

const char *gn_nip29_group_item_get_relay_url(GnNip29GroupItem *self)
{ return self->relay_url; }

const char *gn_nip29_group_item_get_group_id(GnNip29GroupItem *self)
{ return self->group_id; }

const char *gn_nip29_group_item_get_alias(GnNip29GroupItem *self)
{ return self->alias; }

const char *gn_nip29_group_item_get_name(GnNip29GroupItem *self)
{ return self->name; }

const char *gn_nip29_group_item_get_picture(GnNip29GroupItem *self)
{ return self->picture; }

const char *gn_nip29_group_item_get_about(GnNip29GroupItem *self)
{ return self->about; }

const char *
gn_nip29_group_item_get_display_name(GnNip29GroupItem *self)
{
  if (self->name != NULL && self->name[0] != '\0')
    return self->name;
  if (self->alias != NULL && self->alias[0] != '\0')
    return self->alias;
  return self->group_id;
}

gboolean gn_nip29_group_item_get_is_private(GnNip29GroupItem *self)
{ return self->is_private; }

gboolean gn_nip29_group_item_get_is_restricted(GnNip29GroupItem *self)
{ return self->is_restricted; }

gboolean gn_nip29_group_item_get_is_hidden(GnNip29GroupItem *self)
{ return self->is_hidden; }

gboolean gn_nip29_group_item_get_is_closed(GnNip29GroupItem *self)
{ return self->is_closed; }

gboolean gn_nip29_group_item_get_admins_loaded(GnNip29GroupItem *self)
{ return self->admins_loaded; }

gboolean gn_nip29_group_item_get_members_loaded(GnNip29GroupItem *self)
{ return self->members_loaded; }

gboolean gn_nip29_group_item_get_members_may_be_partial(GnNip29GroupItem *self)
{ return self->members_may_be_partial; }

gboolean gn_nip29_group_item_get_roles_loaded(GnNip29GroupItem *self)
{ return self->roles_loaded; }

guint gn_nip29_group_item_get_admin_count(GnNip29GroupItem *self)
{ return self->admin_count; }

guint gn_nip29_group_item_get_member_count(GnNip29GroupItem *self)
{ return self->member_count; }

guint gn_nip29_group_item_get_message_count(GnNip29GroupItem *self)
{ return self->message_count; }
