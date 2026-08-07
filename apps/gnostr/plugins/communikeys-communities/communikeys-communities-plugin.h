#ifndef COMMUNIKEYS_COMMUNITIES_PLUGIN_H
#define COMMUNIKEYS_COMMUNITIES_PLUGIN_H
#include <glib-object.h>
G_BEGIN_DECLS
#define COMMUNIKEYS_TYPE_COMMUNITIES_PLUGIN (communikeys_communities_plugin_get_type())
G_DECLARE_FINAL_TYPE(CommunikeysCommunitiesPlugin,
                     communikeys_communities_plugin,
                     COMMUNIKEYS, COMMUNITIES_PLUGIN, GObject)
G_END_DECLS
#endif
