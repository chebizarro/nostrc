#ifndef NIP55L_SIGNER_APPROVAL_SERVICE_H
#define NIP55L_SIGNER_APPROVAL_SERVICE_H
#include "signer_gate.h"
#include "signer_permissions.h"
G_BEGIN_DECLS

typedef struct _SignerApprovalService SignerApprovalService;
typedef enum {
  SIGNER_REQUEST_PENDING, SIGNER_REQUEST_EXECUTING, SIGNER_REQUEST_SUCCEEDED,
  SIGNER_REQUEST_DENIED, SIGNER_REQUEST_EXPIRED, SIGNER_REQUEST_CANCELLED,
  SIGNER_REQUEST_FAILED
} SignerRequestState;
/* Borrowed snapshot; includes the legacy fields and additive metadata, never
 * secret arguments. PENDING announces a new prompt, terminal states announce
 * completion. No EXECUTING notification is sent. Reentrant callbacks allowed. */
typedef void (*SignerApprovalNotifyFn)(gpointer data, GVariant *snapshot);
typedef void (*SignerApprovalDecisionFn)(gpointer data, gboolean ok,
                                         const gchar *error, const gchar *message);
SignerApprovalService *signer_approval_service_new(GMainContext *context);
SignerApprovalService *signer_approval_service_ref(SignerApprovalService *service);
void signer_approval_service_unref(SignerApprovalService *service);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignerApprovalService, signer_approval_service_unref)
/* All API calls except ref/unref on the owning main-context thread. */
guint signer_approval_service_subscribe(SignerApprovalService *service,
                                       SignerApprovalNotifyFn fn, gpointer data);
void signer_approval_service_unsubscribe(SignerApprovalService *service, guint id);
/* Private daemon observer for snapshot revisions: pending joins/detaches,
 * execution transitions, pause and permission changes as well as lifecycle
 * notifications. Observational only: must not mutate/re-enter the service.
 * Does not duplicate legacy ApprovalRequested or completion notifications. */
void signer_approval_service_set_changed(SignerApprovalService *service,
                                         void (*changed)(gpointer), gpointer data);
SignerGateHandle *signer_approval_service_submit(SignerApprovalService *service,
    const SignerCaller *who, const gchar *conn_key, SignerGateOp op,
    const gchar *a, const gchar *b, const gchar *selector,
    const SignerGateContext *context, SignerGateReplyFn reply, gpointer data);
void signer_approval_service_decide(SignerApprovalService *service, const gchar *id,
    gboolean allow, gboolean remember, guint64 ttl, SignerApprovalDecisionFn fn, gpointer data);
GVariant *signer_approval_service_info(SignerApprovalService *service, const gchar *id);
GVariant *signer_approval_service_list_pending(SignerApprovalService *service);
GVariant *signer_approval_service_list_grants(SignerApprovalService *service);
gboolean signer_approval_service_revoke_grant(SignerApprovalService *service,
    const gchar *kind, const gchar *principal, const gchar *identity, gboolean *removed);
guint signer_approval_service_pending_count(SignerApprovalService *service);
/* Owner-context deletion fence. Includes unresolved, queued and executing calls. */
gboolean signer_approval_service_identity_busy(SignerApprovalService *service, const gchar *npub);
void signer_approval_service_prune_connections(SignerApprovalService *service,
    gboolean (*alive)(const gchar *connection, gpointer data), gpointer data);
void signer_approval_service_connection_closed(SignerApprovalService *service, const gchar *key);
/* Cancels only the approver's self-test continuation for an exact identity. */
void signer_approval_service_cancel_self_test(SignerApprovalService *service,
    const gchar *principal, const gchar *identity);
void signer_approval_service_cancel_principal(SignerApprovalService *service, const gchar *principal);
void signer_approval_service_fail_request(SignerApprovalService *service, const gchar *id,
                                         const gchar *error, const gchar *message);
/* Hook for G04 SetPaused; session-only. Pause increments generation, cancels
 * queued work and suppresses in-flight results, even across pause/resume. */
/* Startup headless policy: scoped allows continue; ASK returns NO_APPROVER
 * without announcing/summoning a GUI. Does not create or expand grants. */
void signer_approval_service_set_no_approval_ui(SignerApprovalService *, gboolean);
void signer_approval_service_set_paused(SignerApprovalService *service, gboolean paused);
gboolean signer_approval_service_is_paused(SignerApprovalService *service);
guint64 signer_approval_service_generation(SignerApprovalService *service);
/* Borrowed evaluator; G04 management mutates it on the service owner thread.
 * Changes automatically invalidate denied queued work and advance admission revision. */
SignerPermissions *signer_approval_service_permissions(SignerApprovalService *service);
void signer_approval_service_shutdown(SignerApprovalService *service);
/* Borrowed exported instance, NULL before export/after unexport. */
SignerApprovalService *signer_get_approval_service(void);
/* Owner-context coarse inventory signal after a successful custody mutation. */
void signer_notify_identities_changed(void);
#ifdef NIP55L_TEST_TRUST_ENV
/* Test-only external-I/O seam: blocks a worker after backend return, before
 * owner delivery. Never built into distribution builds (BUILD_TESTING=OFF). */
void signer_approval_service_test_after_backend(SignerApprovalService *service,
                                               void (*hook)(gpointer), gpointer data);
#endif
G_END_DECLS
#endif
