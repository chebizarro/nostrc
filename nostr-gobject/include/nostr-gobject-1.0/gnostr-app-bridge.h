/*
 * gnostr-app-bridge.h — registration seam for the higher-layer app helpers
 * (publish-to-relays, NIP-11 relay-info cache, negentropy sync). These are
 * currently implemented in apps/gnostr but referenced by
 * nostr-gobject/src/gnostr-*.c files. Rather than link the app layer INTO
 * the library, the library exposes this vtable and the app installs its
 * implementations at startup (nostrc-ecrx follow-through).
 *
 * If a hook is not installed the corresponding call is a no-op (async
 * hooks complete with G_IO_ERROR_NOT_INITIALIZED; validation hooks return
 * "valid" so relay filtering is skipped).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef GNOSTR_APP_BRIDGE_H
#define GNOSTR_APP_BRIDGE_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* NostrEvent is a libnostr type; it is passed as a pointer only and is
 * fully typed by callers that include nostr-event.h. */
typedef struct _NostrEvent NostrEvent;

/* Opaque handles for the app-side relay-info cache and validation objects.
 * Only pointers travel across the bridge. */
typedef struct _GnostrRelayInfo GnostrRelayInfo;
typedef struct _GnostrRelayValidationResult GnostrRelayValidationResult;

/**
 * GnostrNegSyncStats:
 *
 * Result summary from a negentropy sync run. Defined here so both
 * apps/gnostr's producer (neg-client) and the nostr-gobject consumer
 * (gnostr-sync-service) can name the same layout without a header
 * dependency going upward. apps/gnostr's neg-client.h re-uses this type.
 */
typedef struct {
  guint local_count;
  guint rounds;
  guint events_fetched;
  gboolean in_sync;
} GnostrNegSyncStats;

/**
 * GnostrKeyInfo:
 *
 * Information about a stored key (does not contain the actual secret).
 * The definition lives here because both apps/gnostr's keystore
 * implementation and nostr-gobject's identity consumer read the fields.
 */
typedef struct {
  char *npub;
  char *label;
  gint64 created_at;
} GnostrKeyInfo;

/**
 * GnostrKeystoreError: mirror of the app-side error codes so callers
 * inside nostr-gobject can g_set_error() with the app's error quark.
 */
typedef enum {
  GNOSTR_KEYSTORE_ERROR_NOT_AVAILABLE,
  GNOSTR_KEYSTORE_ERROR_NOT_FOUND,
  GNOSTR_KEYSTORE_ERROR_ACCESS_DENIED,
  GNOSTR_KEYSTORE_ERROR_INVALID_KEY,
  GNOSTR_KEYSTORE_ERROR_STORAGE_FULL,
  GNOSTR_KEYSTORE_ERROR_FAILED
} GnostrKeystoreError;

#define GNOSTR_KEYSTORE_ERROR (gnostr_app_bridge_keystore_error_quark())
GQuark gnostr_app_bridge_keystore_error_quark(void);

/**
 * GnostrRelayPublishDoneCallback:
 * Mirror of the app-side callback type. Kept here so gnostr-*.c inside
 * libnostr_gobject can name the type without pulling in apps/gnostr headers.
 */
typedef void (*GnostrRelayPublishDoneCallback)(guint success_count,
                                                guint fail_count,
                                                gpointer user_data);

/**
 * GnostrAppBridge:
 *
 * @publish_to_relays_async: publish a signed @event to each URL in
 *   @relay_urls. Takes ownership of @event and @relay_urls, matching the
 *   app-side contract. If NULL the call reports (0, 0) synchronously.
 *
 * @relay_info_cache_get, @relay_info_free,
 * @relay_info_validate_event, @relay_info_validate_for_publishing,
 * @relay_validation_result_is_valid,
 * @relay_validation_result_format_errors,
 * @relay_validation_result_free:
 *   NIP-11 relay-info accessors. If any hook is NULL the library skips
 *   info-based filtering entirely (treats all relays as valid).
 *
 * @neg_sync_kinds_for_authors_async / @neg_sync_kinds_finish: NIP-77
 *   negentropy sync. If NULL the caller is notified with an error via the
 *   provided GAsyncReadyCallback.
 */
typedef struct {
  /* Publish */
  void (*publish_to_relays_async)(NostrEvent *event,
                                   GPtrArray *relay_urls,
                                   GnostrRelayPublishDoneCallback callback,
                                   gpointer user_data);

  /* Relay-info cache + validation */
  GnostrRelayInfo *(*relay_info_cache_get)(const gchar *relay_url);
  void (*relay_info_free)(GnostrRelayInfo *info);
  GnostrRelayValidationResult *(*relay_info_validate_event)(GnostrRelayInfo *info,
                                                             const char *content,
                                                             gint content_len,
                                                             gint tag_count,
                                                             gint64 created_at,
                                                             gssize serialized_len);
  GnostrRelayValidationResult *(*relay_info_validate_for_publishing)(GnostrRelayInfo *info);
  gboolean (*relay_validation_result_is_valid)(const GnostrRelayValidationResult *result);
  gchar *(*relay_validation_result_format_errors)(const GnostrRelayValidationResult *result);
  void (*relay_validation_result_free)(GnostrRelayValidationResult *result);

  /* Negentropy sync */
  void (*neg_sync_kinds_for_authors_async)(const char *relay_url,
                                            const int *kinds,
                                            size_t kind_count,
                                            const char * const *authors,
                                            size_t author_count,
                                            GCancellable *cancellable,
                                            GAsyncReadyCallback callback,
                                            gpointer user_data);
  gboolean (*neg_sync_kinds_finish)(GAsyncResult *result,
                                     GnostrNegSyncStats *stats_out,
                                     GError **error);

  /* Secure keystore hooks (platform-native: libsecret / macOS Keychain). */
  gboolean (*keystore_available)(void);
  gboolean (*keystore_has_key)(const char *npub);
  gboolean (*keystore_store_key)(const char *npub, const char *nsec,
                                  const char *label, GError **error);
  char    *(*keystore_retrieve_key)(const char *npub, GError **error);
  gboolean (*keystore_delete_key)(const char *npub, GError **error);
  GList   *(*keystore_list_keys)(GError **error);
  void     (*key_info_free)(GnostrKeyInfo *info);
  GQuark   (*keystore_error_quark)(void);
} GnostrAppBridge;

/**
 * gnostr_app_bridge_install:
 * @vtable: (nullable) (transfer none): the vtable to install; pass %NULL
 *          to detach.
 *
 * Installs the higher-layer app implementations. The vtable is copied.
 */
void gnostr_app_bridge_install(const GnostrAppBridge *vtable);

/* Convenience call-throughs used by nostr-gobject internals. */
void gnostr_app_bridge_publish_to_relays_async(NostrEvent *event,
                                                GPtrArray *relay_urls,
                                                GnostrRelayPublishDoneCallback callback,
                                                gpointer user_data);

GnostrRelayInfo *gnostr_app_bridge_relay_info_cache_get(const gchar *relay_url);
void gnostr_app_bridge_relay_info_free(GnostrRelayInfo *info);
GnostrRelayValidationResult *
gnostr_app_bridge_relay_info_validate_event(GnostrRelayInfo *info,
                                             const char *content,
                                             gint content_len,
                                             gint tag_count,
                                             gint64 created_at,
                                             gssize serialized_len);
GnostrRelayValidationResult *
gnostr_app_bridge_relay_info_validate_for_publishing(GnostrRelayInfo *info);
gboolean gnostr_app_bridge_relay_validation_result_is_valid(const GnostrRelayValidationResult *result);
gchar *gnostr_app_bridge_relay_validation_result_format_errors(const GnostrRelayValidationResult *result);
void gnostr_app_bridge_relay_validation_result_free(GnostrRelayValidationResult *result);

void gnostr_app_bridge_neg_sync_kinds_for_authors_async(const char *relay_url,
                                                        const int *kinds,
                                                        size_t kind_count,
                                                        const char * const *authors,
                                                        size_t author_count,
                                                        GCancellable *cancellable,
                                                        GAsyncReadyCallback callback,
                                                        gpointer user_data);
gboolean gnostr_app_bridge_neg_sync_kinds_finish(GAsyncResult *result,
                                                  GnostrNegSyncStats *stats_out,
                                                  GError **error);

/* Keystore call-throughs. Semantics mirror the app-side keystore API. */
gboolean gnostr_app_bridge_keystore_available(void);
gboolean gnostr_app_bridge_keystore_has_key(const char *npub);
gboolean gnostr_app_bridge_keystore_store_key(const char *npub,
                                               const char *nsec,
                                               const char *label,
                                               GError **error);
char    *gnostr_app_bridge_keystore_retrieve_key(const char *npub, GError **error);
gboolean gnostr_app_bridge_keystore_delete_key(const char *npub, GError **error);
GList   *gnostr_app_bridge_keystore_list_keys(GError **error);
void     gnostr_app_bridge_key_info_free(GnostrKeyInfo *info);

G_END_DECLS

#endif /* GNOSTR_APP_BRIDGE_H */
