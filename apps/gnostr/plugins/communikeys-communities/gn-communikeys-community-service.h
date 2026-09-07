#ifndef GN_COMMUNIKEYS_COMMUNITY_SERVICE_H
#define GN_COMMUNIKEYS_COMMUNITY_SERVICE_H

#include <gio/gio.h>
#include <glib-object.h>
#include <gnostr-plugin-api.h>
#include <nip_communikeys.h>
#include "model/gn-communikeys-community-item.h"

G_BEGIN_DECLS

/*
 * Communikeys V2 community service.
 *
 * Every community is identified by its exact definition address
 * "32222:<ownerPubkey>:<communityId>". Two branches sharing a community ID
 * are distinct communities and appear as separate model rows; the service
 * never deduplicates on community ID (V2 §Same-ID Branches).
 */

typedef enum {
  GN_COMMUNIKEYS_UPDATE_DEFINITION = 1u << 0,
  GN_COMMUNIKEYS_UPDATE_ACL = 1u << 1,
  GN_COMMUNIKEYS_UPDATE_EXCLUSIVE = 1u << 2,
  GN_COMMUNIKEYS_UPDATE_TARGETED = 1u << 3
} GnCommunikeysUpdateFlags;

#define GN_TYPE_COMMUNIKEYS_COMMUNITY_SERVICE (gn_communikeys_community_service_get_type())
G_DECLARE_FINAL_TYPE(GnCommunikeysCommunityService,
                     gn_communikeys_community_service,
                     GN, COMMUNIKEYS_COMMUNITY_SERVICE, GObject)

GnCommunikeysCommunityService *gn_communikeys_community_service_new(
    GnostrPluginContext *context);
/* Deterministic no-network constructor used by the plugin's service tests. */
GnCommunikeysCommunityService *gn_communikeys_community_service_new_offline(
    const char *user_pubkey);

GListModel *gn_communikeys_community_service_get_model(
    GnCommunikeysCommunityService *self);
GnCommunikeysCommunityItem *gn_communikeys_community_service_lookup_community(
    GnCommunikeysCommunityService *self, const char *definition_address);
GListModel *gn_communikeys_community_service_get_messages(
    GnCommunikeysCommunityService *self, const char *definition_address);
GListModel *gn_communikeys_community_service_get_targets(
    GnCommunikeysCommunityService *self, const char *definition_address);
const char *gn_communikeys_community_service_get_current_pubkey(
    GnCommunikeysCommunityService *self);

gboolean gn_communikeys_community_service_author_can_publish(
    GnCommunikeysCommunityService *self, const char *definition_address,
    int kind, const char *author_pubkey);

/* Verifies and ingests one complete signed event JSON. */
gboolean gn_communikeys_community_service_ingest_event(
    GnCommunikeysCommunityService *self, const char *event_json);

void gn_communikeys_community_service_refresh(
    GnCommunikeysCommunityService *self);
void gn_communikeys_community_service_refresh_community(
    GnCommunikeysCommunityService *self, const char *definition_address);
void gn_communikeys_community_service_shutdown(
    GnCommunikeysCommunityService *self);

/* Publishes a kind-9/11 community-exclusive event. The definition address
 * resolves branch authority and relays; the emitted event carries
 * h = <communityId> (the two are NOT interchangeable). */
void gn_communikeys_community_service_publish_exclusive_async(
    GnCommunikeysCommunityService *self, const char *definition_address,
    int kind, const char *content, GCancellable *cancellable,
    GAsyncReadyCallback callback, gpointer user_data);
gboolean gn_communikeys_community_service_publish_exclusive_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result, GError **error);

/* Publishes a kind-30222 targeting wrapper. The signer is the CURATOR and
 * need not be the original publication's author; admission depends on the
 * original author's section grants in every targeted branch. */
void gn_communikeys_community_service_publish_target_async(
    GnCommunikeysCommunityService *self,
    const nostr_communikeys_targeted_publication_t *publication,
    GCancellable *cancellable, GAsyncReadyCallback callback,
    gpointer user_data);
gboolean gn_communikeys_community_service_publish_target_finish(
    GnCommunikeysCommunityService *self, GAsyncResult *result, GError **error);

G_END_DECLS
#endif
