/*
 * gnostr-app-bridge.c — implementation of the higher-layer registration
 * seam. See gnostr-app-bridge.h. All state is file-static; installation
 * happens once at app startup on the main thread.
 *
 * When a hook is NULL, the library gracefully degrades:
 *  - publish: reports (0, 0) synchronously to the callback (mirrors the
 *    "no relays succeeded" outcome) and unrefs/frees the transferred
 *    inputs, honoring the original ownership contract.
 *  - relay-info cache: returns NULL, which upstream call-sites already
 *    treat as "no cached info; do not filter".
 *  - relay validation result helpers: an app-installed hook always owns
 *    the result object it minted; without a hook, the library never mints
 *    one and never sees one, so these are pure passthroughs to app hooks.
 *  - neg-sync: reports G_IO_ERROR_NOT_INITIALIZED through a GTask.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "gnostr-app-bridge.h"

#include <glib.h>
#include <gio/gio.h>
#include <string.h>

/* NostrEvent free lives in libnostr, not visible from the include set of
 * this TU; forward-declare only what we call. */
extern void nostr_event_free(NostrEvent *event);

static GnostrAppBridge s_bridge = { 0 };

void
gnostr_app_bridge_install(const GnostrAppBridge *vtable)
{
  if (vtable) {
    s_bridge = *vtable;
  } else {
    memset(&s_bridge, 0, sizeof(s_bridge));
  }
}

/* --- Publish --- */

void
gnostr_app_bridge_publish_to_relays_async(NostrEvent *event,
                                           GPtrArray *relay_urls,
                                           GnostrRelayPublishDoneCallback callback,
                                           gpointer user_data)
{
  if (s_bridge.publish_to_relays_async) {
    s_bridge.publish_to_relays_async(event, relay_urls, callback, user_data);
    return;
  }
  /* Honor the "takes ownership" contract even without an installed hook. */
  if (event) nostr_event_free(event);
  if (relay_urls) g_ptr_array_free(relay_urls, TRUE);
  if (callback) callback(0, 0, user_data);
}

/* --- Relay info cache + validation --- */

GnostrRelayInfo *
gnostr_app_bridge_relay_info_cache_get(const gchar *relay_url)
{
  return s_bridge.relay_info_cache_get ? s_bridge.relay_info_cache_get(relay_url) : NULL;
}

void
gnostr_app_bridge_relay_info_free(GnostrRelayInfo *info)
{
  if (s_bridge.relay_info_free) s_bridge.relay_info_free(info);
}

GnostrRelayValidationResult *
gnostr_app_bridge_relay_info_validate_event(GnostrRelayInfo *info,
                                             const char *content,
                                             gint content_len,
                                             gint tag_count,
                                             gint64 created_at,
                                             gssize serialized_len)
{
  if (s_bridge.relay_info_validate_event) {
    return s_bridge.relay_info_validate_event(info, content, content_len,
                                              tag_count, created_at,
                                              serialized_len);
  }
  return NULL;
}

GnostrRelayValidationResult *
gnostr_app_bridge_relay_info_validate_for_publishing(GnostrRelayInfo *info)
{
  return s_bridge.relay_info_validate_for_publishing
      ? s_bridge.relay_info_validate_for_publishing(info)
      : NULL;
}

gboolean
gnostr_app_bridge_relay_validation_result_is_valid(const GnostrRelayValidationResult *result)
{
  /* Without a hook, treat a NULL result as "valid" so upstream filtering
   * is skipped (matches the app-side semantic for a missing cache entry). */
  if (!s_bridge.relay_validation_result_is_valid) return result == NULL;
  return s_bridge.relay_validation_result_is_valid(result);
}

gchar *
gnostr_app_bridge_relay_validation_result_format_errors(const GnostrRelayValidationResult *result)
{
  return s_bridge.relay_validation_result_format_errors
      ? s_bridge.relay_validation_result_format_errors(result)
      : NULL;
}

void
gnostr_app_bridge_relay_validation_result_free(GnostrRelayValidationResult *result)
{
  if (s_bridge.relay_validation_result_free) {
    s_bridge.relay_validation_result_free(result);
  }
}

/* --- Negentropy sync --- */

static void
neg_report_not_initialized(GAsyncReadyCallback callback, gpointer user_data)
{
  GTask *task = g_task_new(NULL, NULL, callback, user_data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "gnostr app bridge (neg-sync) not installed");
  g_object_unref(task);
}

void
gnostr_app_bridge_neg_sync_kinds_for_authors_async(const char *relay_url,
                                                    const int *kinds,
                                                    size_t kind_count,
                                                    const char * const *authors,
                                                    size_t author_count,
                                                    GCancellable *cancellable,
                                                    GAsyncReadyCallback callback,
                                                    gpointer user_data)
{
  if (s_bridge.neg_sync_kinds_for_authors_async) {
    s_bridge.neg_sync_kinds_for_authors_async(relay_url, kinds, kind_count,
                                              authors, author_count,
                                              cancellable, callback, user_data);
    return;
  }
  if (callback) neg_report_not_initialized(callback, user_data);
}

gboolean
gnostr_app_bridge_neg_sync_kinds_finish(GAsyncResult *result,
                                         GnostrNegSyncStats *stats_out,
                                         GError **error)
{
  if (s_bridge.neg_sync_kinds_finish) {
    return s_bridge.neg_sync_kinds_finish(result, stats_out, error);
  }
  /* Task produced by our not-initialized path carries only an error. */
  if (G_IS_TASK(result)) {
    (void)g_task_propagate_pointer(G_TASK(result), error);
    return FALSE;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                      "gnostr app bridge (neg-sync) not installed");
  return FALSE;
}

/* --- Keystore --- */

GQuark
gnostr_app_bridge_keystore_error_quark(void)
{
  /* If the app has installed a real quark, use it so error matching works
   * against the app's GNOSTR_KEYSTORE_ERROR domain. Otherwise fall back to
   * a locally-registered quark so the library never crashes. */
  if (s_bridge.keystore_error_quark) return s_bridge.keystore_error_quark();
  return g_quark_from_static_string("gnostr-keystore-error-quark");
}

gboolean
gnostr_app_bridge_keystore_available(void)
{
  return s_bridge.keystore_available ? s_bridge.keystore_available() : FALSE;
}

gboolean
gnostr_app_bridge_keystore_has_key(const char *npub)
{
  return s_bridge.keystore_has_key ? s_bridge.keystore_has_key(npub) : FALSE;
}

static void
keystore_not_installed(GError **error)
{
  g_set_error_literal(error, gnostr_app_bridge_keystore_error_quark(),
                      GNOSTR_KEYSTORE_ERROR_NOT_AVAILABLE,
                      "gnostr keystore bridge not installed");
}

gboolean
gnostr_app_bridge_keystore_store_key(const char *npub, const char *nsec,
                                      const char *label, GError **error)
{
  if (s_bridge.keystore_store_key) {
    return s_bridge.keystore_store_key(npub, nsec, label, error);
  }
  keystore_not_installed(error);
  return FALSE;
}

char *
gnostr_app_bridge_keystore_retrieve_key(const char *npub, GError **error)
{
  if (s_bridge.keystore_retrieve_key) {
    return s_bridge.keystore_retrieve_key(npub, error);
  }
  keystore_not_installed(error);
  return NULL;
}

gboolean
gnostr_app_bridge_keystore_delete_key(const char *npub, GError **error)
{
  if (s_bridge.keystore_delete_key) {
    return s_bridge.keystore_delete_key(npub, error);
  }
  keystore_not_installed(error);
  return FALSE;
}

GList *
gnostr_app_bridge_keystore_list_keys(GError **error)
{
  if (s_bridge.keystore_list_keys) {
    return s_bridge.keystore_list_keys(error);
  }
  keystore_not_installed(error);
  return NULL;
}

void
gnostr_app_bridge_key_info_free(GnostrKeyInfo *info)
{
  if (s_bridge.key_info_free) {
    s_bridge.key_info_free(info);
  } else if (info) {
    /* The library never mints these without an installed hook, so a NULL
     * hook only fires if a caller hand-crafts one. Free the fields we
     * know about. */
    g_free(info->npub);
    g_free(info->label);
    g_free(info);
  }
}
