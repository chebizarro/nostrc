#ifndef GN_COMMUNIKEYS_COMMUNITY_SERVICE_H
#define GN_COMMUNIKEYS_COMMUNITY_SERVICE_H
#include <gio/gio.h>
#include <glib-object.h>
#include <gnostr-plugin-api.h>
#include <nip_communikeys.h>
G_BEGIN_DECLS
#define GN_TYPE_COMMUNIKEYS_COMMUNITY_SERVICE (gn_communikeys_community_service_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunityService, gn_communikeys_community_service,
                     GN, COMMUNIKEYS_COMMUNITY_SERVICE, GObject)
GnCommunikeysCommunityService *gn_communikeys_community_service_new(GnostrPluginContext *context);
GListModel *gn_communikeys_community_service_get_model(GnCommunikeysCommunityService *self);
void gn_communikeys_community_service_refresh(GnCommunikeysCommunityService *self);
void gn_communikeys_community_service_shutdown(GnCommunikeysCommunityService *self);
void gn_communikeys_community_service_publish_exclusive_async(
    GnCommunikeysCommunityService *self, const char *community_pubkey,
    int kind, const char *content, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean gn_communikeys_community_service_publish_exclusive_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result, GError **error);
void gn_communikeys_community_service_publish_target_async(
    GnCommunikeysCommunityService *self,
    const nostr_communikeys_targeted_publication_t *publication,
    GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data);
gboolean gn_communikeys_community_service_publish_target_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result, GError **error);
G_END_DECLS
#endif
