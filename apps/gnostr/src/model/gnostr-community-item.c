/*
 * gnostr-community-item.c - GObject wrapper for GnostrCommunity
 */

#include "gnostr-community-item.h"

struct _GnostrCommunityItem {
  GObject parent_instance;
  GnostrCommunity *community;
  char *a_tag;  /* cached from gnostr_community_get_a_tag() */
  gboolean joined;
};

G_DEFINE_FINAL_TYPE(GnostrCommunityItem, gnostr_community_item, G_TYPE_OBJECT)

static void
update_a_tag(GnostrCommunityItem *self)
{
  g_free(self->a_tag);
  self->a_tag = self->community ? gnostr_community_get_a_tag(self->community) : NULL;
}

static void
gnostr_community_item_finalize(GObject *obj)
{
  GnostrCommunityItem *self = GNOSTR_COMMUNITY_ITEM(obj);
  g_clear_pointer(&self->community, gnostr_community_free);
  g_clear_pointer(&self->a_tag, g_free);
  G_OBJECT_CLASS(gnostr_community_item_parent_class)->finalize(obj);
}

static void
gnostr_community_item_class_init(GnostrCommunityItemClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = gnostr_community_item_finalize;
}

static void
gnostr_community_item_init(GnostrCommunityItem *self)
{
  self->community = NULL;
  self->a_tag = NULL;
}

GnostrCommunityItem *
gnostr_community_item_new(const GnostrCommunity *community)
{
  GnostrCommunityItem *self = g_object_new(GNOSTR_TYPE_COMMUNITY_ITEM, NULL);
  if (community) {
    self->community = gnostr_community_copy(community);
    update_a_tag(self);
  }
  return self;
}

const GnostrCommunity *
gnostr_community_item_get_community(GnostrCommunityItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self), NULL);
  return self->community;
}

const char *
gnostr_community_item_get_a_tag(GnostrCommunityItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self), NULL);
  return self->a_tag;
}

const char *
gnostr_community_item_get_name(GnostrCommunityItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self), NULL);
  return self->community ? self->community->name : NULL;
}

const char *
gnostr_community_item_get_description(GnostrCommunityItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self), NULL);
  return self->community ? self->community->description : NULL;
}

void
gnostr_community_item_update(GnostrCommunityItem *self, const GnostrCommunity *community)
{
  g_return_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self));
  g_return_if_fail(community != NULL);

  g_clear_pointer(&self->community, gnostr_community_free);
  self->community = gnostr_community_copy(community);
  update_a_tag(self);
}

gboolean
gnostr_community_item_get_joined(GnostrCommunityItem *self)
{
  g_return_val_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self), FALSE);
  return self->joined;
}

void
gnostr_community_item_set_joined(GnostrCommunityItem *self, gboolean joined)
{
  g_return_if_fail(GNOSTR_IS_COMMUNITY_ITEM(self));
  self->joined = joined;
}
