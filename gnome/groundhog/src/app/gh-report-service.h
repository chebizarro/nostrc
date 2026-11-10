#ifndef GH_REPORT_SERVICE_H
#define GH_REPORT_SERVICE_H
#include "gh-social-lists.h"
#include "gh-store-reports.h"
#include <nostr/nip56/nip56.h>
G_BEGIN_DECLS
#define GH_TYPE_REPORT_SERVICE (gh_report_service_get_type())
G_DECLARE_FINAL_TYPE(GhReportService, gh_report_service, GH, REPORT_SERVICE, GObject)
/* No private-origin evidence parameter exists. A public event may only be
 * supplied after the caller verifies the NIP-29 visibility and signature. */
gchar *gh_report_build_account(const gchar *account, const gchar *target,
    NostrNip56Category category, const gchar *explanation, gint64 now, GError **error);
gchar *gh_report_build_public_message(const gchar *account, const gchar *public_event_json,
    NostrNip56Category category, const gchar *explanation, gint64 now, GError **error);
gboolean gh_report_matches_signed(const gchar *approved, const gchar *signed_json, GError **error);
/* Main context only. changed() signals delivery updates, including cancellation.
 * Signer seam follows the controller async/finish contract; production passes
 * NULL and uses network.accounts. Test signers still return verified signatures. */
typedef struct {
  void (*sign_async)(gpointer data, const gchar *json, GCancellable *cancel,
                     GAsyncReadyCallback callback, gpointer user_data);
  gchar *(*sign_finish)(GAsyncResult *result, GError **error);
  gpointer data;
} GhReportSigner;
GhReportService *gh_report_service_new(GhSocialLists *lists, GhAccountStore *account_store,
    const GhListAdapterNetwork *network, const GhReportSigner *signer);
/* Explicit seam for a session store; close before releasing the borrowed store. */
void gh_report_service_bind(GhReportService *self, GhStore *store, guint64 generation);
void gh_report_service_close(GhReportService *self);
GStrv gh_report_service_dup_targets(GhReportService *self);
/* Approval is persisted before requesting any signature. Also-hide is its own
 * local transaction before publication and survives publication failure. */
gint64 gh_report_service_submit(GhReportService *self, const gchar *approved_json,
    const gchar *const *approved_targets, gboolean also_hide, GError **error);
gboolean gh_report_service_retry(GhReportService *self, gint64 id, const gchar *url, GError **error);
void gh_report_service_cancel(GhReportService *self);
GPtrArray *gh_report_service_list(GhReportService *self, GError **error);
const gchar *gh_report_service_get_error(GhReportService *self);
gboolean gh_report_service_get_busy(GhReportService *self);
G_END_DECLS
#endif
