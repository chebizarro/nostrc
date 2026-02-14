/* gir_stubs.c — Stub implementations of app-level symbols referenced by
 * nostr-gobject but defined in the gnostr application.  These stubs exist
 * solely so the GIR shared wrapper can link and load for g-ir-scanner.
 * They are NEVER used at runtime.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <glib.h>
#include <gio/gio.h>
#include <stddef.h>

/* ── Forward declarations (avoid pulling in app headers) ───────────── */
typedef struct _GnostrKeyInfo GnostrKeyInfo;
typedef struct _NostrSignerProxy NostrSignerProxy;
typedef struct _GnostrSignerService GnostrSignerService;
typedef struct _GnostrNegSyncStats GnostrNegSyncStats;
typedef struct _NostrEvent NostrEvent;
typedef struct _GnostrRelayInfo GnostrRelayInfo;
typedef struct _GnostrRelayValidationResult GnostrRelayValidationResult;
typedef void (*GnostrRelayPublishDoneCallback)(gpointer user_data);

/* ── Keystore stubs ────────────────────────────────────────────────── */
void gnostr_key_info_free(GnostrKeyInfo *info) { (void)info; }
gboolean gnostr_keystore_available(void) { return FALSE; }
gboolean gnostr_keystore_store_key(const char *npub, const char *nsec,
                                    const char *label, GError **error)
{ (void)npub; (void)nsec; (void)label; (void)error; return FALSE; }
gboolean gnostr_keystore_retrieve_key(const char *npub, GError **error)
{ (void)npub; (void)error; return FALSE; }
gboolean gnostr_keystore_delete_key(const char *npub, GError **error)
{ (void)npub; (void)error; return FALSE; }
GList *gnostr_keystore_list_keys(GError **error)
{ (void)error; return NULL; }
gboolean gnostr_keystore_has_key(const char *npub)
{ (void)npub; return FALSE; }
GQuark gnostr_keystore_error_quark(void) { return 0; }

/* ── Signer service stubs ──────────────────────────────────────────── */
void gnostr_sign_event_async(const char *event_json, const char *current_user,
                              const char *app_id, GCancellable *cancellable,
                              GAsyncReadyCallback callback, gpointer user_data)
{ (void)event_json; (void)current_user; (void)app_id;
  (void)cancellable; (void)callback; (void)user_data; }
gboolean gnostr_sign_event_finish(GAsyncResult *res, char **out_signed_event,
                                   GError **error)
{ (void)res; (void)out_signed_event; (void)error; return FALSE; }
NostrSignerProxy *gnostr_signer_proxy_get(GError **error)
{ (void)error; return NULL; }
GnostrSignerService *gnostr_signer_service_get_default(void)
{ return NULL; }
gboolean gnostr_signer_service_is_available(GnostrSignerService *self)
{ (void)self; return FALSE; }

/* ── Negentropy sync stubs ─────────────────────────────────────────── */
void gnostr_neg_sync_kinds_async(const char *relay_url, const int *kinds,
                                  size_t kind_count, GCancellable *cancellable,
                                  GAsyncReadyCallback callback, gpointer user_data)
{ (void)relay_url; (void)kinds; (void)kind_count;
  (void)cancellable; (void)callback; (void)user_data; }
gboolean gnostr_neg_sync_kinds_finish(GAsyncResult *result,
                                       GnostrNegSyncStats *stats_out,
                                       GError **error)
{ (void)result; (void)stats_out; (void)error; return FALSE; }

/* ── Relay publish stub ────────────────────────────────────────────── */
void gnostr_publish_to_relays_async(NostrEvent *event, GPtrArray *relay_urls,
                                     GnostrRelayPublishDoneCallback callback,
                                     gpointer user_data)
{ (void)event; (void)relay_urls; (void)callback; (void)user_data; }

/* ── Relay info stubs ──────────────────────────────────────────────── */
GnostrRelayInfo *gnostr_relay_info_cache_get(const gchar *relay_url)
{ (void)relay_url; return NULL; }
void gnostr_relay_info_free(GnostrRelayInfo *info) { (void)info; }
GnostrRelayValidationResult *gnostr_relay_info_validate_event(
    const GnostrRelayInfo *info, const gchar *content, gssize content_length,
    gint tag_count, gint64 created_at, gssize serialized_length)
{ (void)info; (void)content; (void)content_length;
  (void)tag_count; (void)created_at; (void)serialized_length; return NULL; }
GnostrRelayValidationResult *gnostr_relay_info_validate_for_publishing(
    const GnostrRelayInfo *info)
{ (void)info; return NULL; }
void gnostr_relay_validation_result_free(GnostrRelayValidationResult *result)
{ (void)result; }
gboolean gnostr_relay_validation_result_is_valid(
    const GnostrRelayValidationResult *result)
{ (void)result; return TRUE; }
gchar *gnostr_relay_validation_result_format_errors(
    const GnostrRelayValidationResult *result)
{ (void)result; return g_strdup(""); }
