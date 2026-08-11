#ifndef CONCORD_COMMUNITIES_PLUGIN_H
#define CONCORD_COMMUNITIES_PLUGIN_H
#include <glib-object.h>
G_BEGIN_DECLS
#define CONCORD_TYPE_COMMUNITIES_PLUGIN (concord_communities_plugin_get_type())
G_DECLARE_FINAL_TYPE(ConcordCommunitiesPlugin, concord_communities_plugin,
                     CONCORD, COMMUNITIES_PLUGIN, GObject)
G_END_DECLS
#endif
