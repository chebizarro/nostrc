/*
 * gnostr-signer-bridge-install.c
 *
 * nostrc-ecrx: wire apps/gnostr's implementations into nostr-gobject's
 * signer and app-hooks bridges. This is the seam that lets the shared
 * libnostr_gobject.so link without any symbols from the app layer:
 * gnostr-*.c inside the library calls through function pointers we
 * install here at app startup.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <nostr-gobject-1.0/gnostr-signer-bridge.h>
#include <nostr-gobject-1.0/gnostr-app-bridge.h>

#include "gnostr-signer-service.h"
#include "signer_ipc.h"
#include "../util/relay_info.h"
#include "../util/utils.h"
#include "../util/keystore.h"
#include "../sync/neg-client.h"

#include "gnostr-signer-bridge-install.h"

/* --- Signer bridge adapters --- */

static gboolean
sb_is_available(void)
{
  return gnostr_signer_service_is_available(gnostr_signer_service_get_default());
}

static void
sb_sign_event_async(const char *event_json,
                     GCancellable *cancellable,
                     GAsyncReadyCallback callback,
                     gpointer user_data)
{
  /* current_user / app_id are ignored by the existing implementation
   * (see gnostr-signer-service.c); pass placeholders so we can reuse it. */
  gnostr_sign_event_async(event_json, "", "gnostr", cancellable, callback, user_data);
}

static gboolean
sb_sign_event_finish(GAsyncResult *res,
                      char **out_signed_event,
                      GError **error)
{
  return gnostr_sign_event_finish(res, out_signed_event, error);
}

static NostrOrgNostrSigner *
sb_proxy_get(GError **error)
{
  /* NostrSignerProxy is a typedef for NostrOrgNostrSigner in signer_ipc.h,
   * so this is a straight type-identity conversion. */
  return (NostrOrgNostrSigner *) gnostr_signer_proxy_get(error);
}

/* --- App bridge adapters --- */

static void
ab_publish_to_relays_async(NostrEvent *event,
                            GPtrArray *relay_urls,
                            GnostrRelayPublishDoneCallback callback,
                            gpointer user_data)
{
  gnostr_publish_to_relays_async(event, relay_urls, callback, user_data);
}

static GnostrRelayInfo *
ab_relay_info_cache_get(const gchar *relay_url)
{
  return gnostr_relay_info_cache_get(relay_url);
}

static void
ab_relay_info_free(GnostrRelayInfo *info)
{
  gnostr_relay_info_free(info);
}

static GnostrRelayValidationResult *
ab_relay_info_validate_event(GnostrRelayInfo *info,
                              const char *content,
                              gint content_len,
                              gint tag_count,
                              gint64 created_at,
                              gssize serialized_len)
{
  return gnostr_relay_info_validate_event(info, content, content_len,
                                          tag_count, created_at,
                                          serialized_len);
}

static GnostrRelayValidationResult *
ab_relay_info_validate_for_publishing(GnostrRelayInfo *info)
{
  return gnostr_relay_info_validate_for_publishing(info);
}

static gboolean
ab_relay_validation_result_is_valid(const GnostrRelayValidationResult *result)
{
  return gnostr_relay_validation_result_is_valid(result);
}

static gchar *
ab_relay_validation_result_format_errors(const GnostrRelayValidationResult *result)
{
  return gnostr_relay_validation_result_format_errors(result);
}

static void
ab_relay_validation_result_free(GnostrRelayValidationResult *result)
{
  gnostr_relay_validation_result_free(result);
}

static void
ab_neg_sync_kinds_for_authors_async(const char *relay_url,
                                     const int *kinds,
                                     size_t kind_count,
                                     const char * const *authors,
                                     size_t author_count,
                                     GCancellable *cancellable,
                                     GAsyncReadyCallback callback,
                                     gpointer user_data)
{
  gnostr_neg_sync_kinds_for_authors_async(relay_url, kinds, kind_count,
                                          authors, author_count,
                                          cancellable, callback, user_data);
}

static gboolean
ab_neg_sync_kinds_finish(GAsyncResult *result,
                          GnostrNegSyncStats *stats_out,
                          GError **error)
{
  return gnostr_neg_sync_kinds_finish(result, stats_out, error);
}

/* Keystore adapters. The app's GnostrKeyInfo has the same layout as the
 * bridge's (the app header now typedefs from the bridge), so casts are
 * unnecessary. */
static gboolean ab_keystore_available(void)
{ return gnostr_keystore_available(); }

static gboolean ab_keystore_has_key(const char *npub)
{ return gnostr_keystore_has_key(npub); }

static gboolean ab_keystore_store_key(const char *npub, const char *nsec,
                                       const char *label, GError **error)
{ return gnostr_keystore_store_key(npub, nsec, label, error); }

static char *ab_keystore_retrieve_key(const char *npub, GError **error)
{ return gnostr_keystore_retrieve_key(npub, error); }

static gboolean ab_keystore_delete_key(const char *npub, GError **error)
{ return gnostr_keystore_delete_key(npub, error); }

static GList *ab_keystore_list_keys(GError **error)
{ return gnostr_keystore_list_keys(error); }

static void ab_key_info_free(GnostrKeyInfo *info)
{ gnostr_key_info_free(info); }

static GQuark ab_keystore_error_quark(void)
{ return gnostr_keystore_error_quark(); }

/* --- Installer --- */

void
gnostr_signer_bridge_install_default(void)
{
  static const GnostrSignerBridge signer_vtable = {
    .is_available      = sb_is_available,
    .sign_event_async  = sb_sign_event_async,
    .sign_event_finish = sb_sign_event_finish,
    .proxy_get         = sb_proxy_get,
  };
  gnostr_signer_bridge_install(&signer_vtable);

  static const GnostrAppBridge app_vtable = {
    .publish_to_relays_async               = ab_publish_to_relays_async,
    .relay_info_cache_get                  = ab_relay_info_cache_get,
    .relay_info_free                       = ab_relay_info_free,
    .relay_info_validate_event             = ab_relay_info_validate_event,
    .relay_info_validate_for_publishing    = ab_relay_info_validate_for_publishing,
    .relay_validation_result_is_valid      = ab_relay_validation_result_is_valid,
    .relay_validation_result_format_errors = ab_relay_validation_result_format_errors,
    .relay_validation_result_free          = ab_relay_validation_result_free,
    .neg_sync_kinds_for_authors_async      = ab_neg_sync_kinds_for_authors_async,
    .neg_sync_kinds_finish                 = ab_neg_sync_kinds_finish,
    .keystore_available                    = ab_keystore_available,
    .keystore_has_key                      = ab_keystore_has_key,
    .keystore_store_key                    = ab_keystore_store_key,
    .keystore_retrieve_key                 = ab_keystore_retrieve_key,
    .keystore_delete_key                   = ab_keystore_delete_key,
    .keystore_list_keys                    = ab_keystore_list_keys,
    .key_info_free                         = ab_key_info_free,
    .keystore_error_quark                  = ab_keystore_error_quark,
  };
  gnostr_app_bridge_install(&app_vtable);
}
