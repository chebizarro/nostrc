/* Groundhog services for the portable NIP-34 issue form (nostrc-8xfib.5). */
#include "gh-issue-adapters.h"
#include "gh-diagnostics.h"
#include "gh-identity.h"
#include "gh-inbox-setup.h"
#include "gh-relay-publish.h"
#include <glib/gi18n.h>

/* gnostr targets this same kind-30617 announcement. */
#define REPO_OWNER "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"
#define REPO_ID "nostrc"

/* ---- Publisher: Grotto signs, GhRelayPublish sends ---- */

#define GH_TYPE_ISSUE_PUBLISHER (gh_issue_publisher_get_type())
G_DECLARE_FINAL_TYPE(GhIssuePublisher, gh_issue_publisher, GH, ISSUE_PUBLISHER, GObject)

struct _GhIssuePublisher {
  GObject parent_instance;
  GhAccountController *accounts;
  guint64 generation;
};

static void publisher_iface_init(GnIssuePublisherInterface *iface);
G_DEFINE_FINAL_TYPE_WITH_CODE(GhIssuePublisher, gh_issue_publisher, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_PUBLISHER, publisher_iface_init))

typedef struct {
  GStrv relays;
  GhRelayPublish *publish;
  gulong cancel_id;
  gboolean done;
} PublishJob;

typedef struct {
  gchar *event_id;
  gchar *message;
} PublishResult;

static void
publish_result_free(PublishResult *result)
{
  g_free(result->event_id);
  g_free(result->message);
  g_free(result);
}

static void
publish_job_free(PublishJob *job)
{
  g_strfreev(job->relays);
  g_clear_pointer(&job->publish, gh_relay_publish_unref);
  g_free(job);
}

/* Task references: one for the pending completion (dropped by whoever
 * returns the task), one held by the cancellable handler (dropped on
 * disconnect), one for the signer callback. */
static void
finish_job(GTask *task)
{
  PublishJob *job = g_task_get_task_data(task);
  job->done = TRUE;
  GCancellable *cancellable = g_task_get_cancellable(task);
  if (cancellable && job->cancel_id)
    g_cancellable_disconnect(cancellable, job->cancel_id);
  job->cancel_id = 0;
}

static void
publish_cancelled(GCancellable *cancellable, gpointer data)
{
  (void)cancellable;
  GTask *task = data;
  PublishJob *job = g_task_get_task_data(task);
  if (job->done)
    return;
  job->done = TRUE;
  /* An EVENT already written cannot be recalled; no callback runs after this. */
  if (job->publish)
    gh_relay_publish_cancel(job->publish);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
  g_object_unref(task);
}

static void
published(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  GTask *task = data;
  PublishJob *job = g_task_get_task_data(task);
  if (job->done)
    return;
  finish_job(task);
  if (!summary->any_accepted) {
    /* Never rebuild and re-sign on retry: that could create a duplicate issue. */
    g_task_return_new_error(task, GN_ISSUE_PUBLISH_ERROR, GN_ISSUE_PUBLISH_ERROR_UNCONFIRMED,
                            _("No relay confirmed publication. The issue may still have reached a relay."));
  } else {
    PublishResult *result = g_new0(PublishResult, 1);
    result->event_id = g_strdup(gh_relay_publish_get_event_id(publish));
    result->message = g_strdup(summary->accepted == summary->total
      ? _("Issue published. It is public on Nostr.")
      : _("Issue published on some relays, but not all. It is public on Nostr."));
    g_task_return_pointer(task, result, (GDestroyNotify)publish_result_free);
  }
  g_object_unref(task);
}

static void
signed_issue(GObject *source, GAsyncResult *res, gpointer data)
{
  (void)source;
  g_autoptr(GTask) signer_ref = data;
  GTask *task = data;
  GhIssuePublisher *self = g_task_get_source_object(task);
  PublishJob *job = g_task_get_task_data(task);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gh_account_controller_sign_finish(res, &error);
  if (job->done)
    return;
  if (!error && !gh_account_controller_is_current(self->accounts, self->generation))
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                _("The account changed. Close this report and open a new one."));
  /* GhSigner checks signature, pubkey and every unsigned field against the
   * requested (reviewed) event. The publisher verifies again and requires
   * relay OKs. */
  if (json && !error)
    job->publish = gh_relay_publish_new(self->generation, json, NULL, published, task, &error);
  if (job->publish) {
    for (guint i = 0; job->relays[i] && !error; i++)
      gh_relay_publish_add_url(job->publish, job->relays[i], &error);
    if (!error && gh_relay_publish_start(job->publish, &error))
      return; /* published() completes the task */
  }
  if (job->done) /* a callback already ran during start */
    return;
  finish_job(task);
  if (job->publish)
    gh_relay_publish_cancel(job->publish);
  if (!error)
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, _("The issue could not be signed."));
  g_task_return_error(task, g_steal_pointer(&error));
  g_object_unref(task);
}

static void
sign_and_publish_async(GnIssuePublisher *publisher, const gchar *unsigned_json,
                       const gchar *const *relays, GCancellable *cancellable,
                       GAsyncReadyCallback callback, gpointer data)
{
  GhIssuePublisher *self = GH_ISSUE_PUBLISHER(publisher);
  GTask *task = g_task_new(self, cancellable, callback, data);
  g_task_set_check_cancellable(task, FALSE);
  PublishJob *job = g_new0(PublishJob, 1);
  job->relays = g_strdupv((gchar **)relays);
  g_task_set_task_data(task, job, (GDestroyNotify)publish_job_free);
  if (!gh_account_controller_is_current(self->accounts, self->generation)) {
    job->done = TRUE;
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            _("The account changed. Close this report and open a new one."));
    g_object_unref(task);
    return;
  }
  if (cancellable) {
    job->cancel_id = g_cancellable_connect(cancellable, G_CALLBACK(publish_cancelled),
                                           g_object_ref(task), g_object_unref);
    if (job->done) /* already cancelled: the handler returned the task */
      return;
  }
  gh_account_controller_sign_with_cancellable_async(self->accounts, unsigned_json, cancellable,
                                                     signed_issue, g_object_ref(task));
}

static gboolean
sign_and_publish_finish(GnIssuePublisher *publisher, GAsyncResult *res, gchar **event_id,
                        gchar **message, GError **error)
{
  (void)publisher;
  PublishResult *result = g_task_propagate_pointer(G_TASK(res), error);
  if (!result)
    return FALSE;
  if (event_id)
    *event_id = g_steal_pointer(&result->event_id);
  if (message)
    *message = g_steal_pointer(&result->message);
  publish_result_free(result);
  return TRUE;
}

static gchar *
normalize_relay(GnIssuePublisher *publisher, const gchar *url, GError **error)
{
  (void)publisher;
  gchar *normalized = gh_inbox_setup_normalize_url(url, NULL);
  if (!normalized)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        _("Enter up to 16 secure WebSocket relay URLs, separated by spaces."));
  return normalized;
}

static void
publisher_iface_init(GnIssuePublisherInterface *iface)
{
  iface->sign_and_publish_async = sign_and_publish_async;
  iface->sign_and_publish_finish = sign_and_publish_finish;
  iface->normalize_relay = normalize_relay;
}

static void
gh_issue_publisher_dispose(GObject *object)
{
  g_clear_object(&GH_ISSUE_PUBLISHER(object)->accounts);
  G_OBJECT_CLASS(gh_issue_publisher_parent_class)->dispose(object);
}

static void
gh_issue_publisher_class_init(GhIssuePublisherClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_issue_publisher_dispose;
}

static void gh_issue_publisher_init(GhIssuePublisher *self) { (void)self; }

GnIssuePublisher *
gh_issue_publisher_new(GhAccountController *accounts)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  GhIssuePublisher *self = g_object_new(GH_TYPE_ISSUE_PUBLISHER, NULL);
  self->accounts = g_object_ref(accounts);
  self->generation = gh_account_controller_get_generation(accounts);
  return GN_ISSUE_PUBLISHER(self);
}

/* ---- Factory ---- */

static gchar *
diagnostics_snapshot(gpointer data)
{
  (void)data;
  GhDiagnostics *diagnostics = gh_diagnostics_get_default();
  return diagnostics ? gh_diagnostics_snapshot(diagnostics) : NULL;
}

static void
account_changed(GhAccountController *accounts, GnNip34IssueView *view)
{
  guint64 generation = GPOINTER_TO_SIZE(g_object_get_data(G_OBJECT(view), "gh-issue-generation"));
  if (!gh_account_controller_is_current(accounts, generation))
    gn_nip34_issue_view_invalidate(view, _("The account changed. Close this report and open a new one."));
}

GnNip34IssueView *
gh_issue_dialog_new(GhAccountController *accounts, GSettings *settings)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_autoptr(GnIssueTarget) target = gn_issue_target_new(REPO_OWNER, REPO_ID, NULL, NULL);
  g_autoptr(GnIssuePublisher) publisher = gh_issue_publisher_new(accounts);
  /* No resolver and no uploader: no lookup, no files (privacy charter). */
  GnNip34IssueView *view = gn_nip34_issue_view_new(target, publisher, NULL, NULL);
  gn_nip34_issue_view_set_intro(view,
    _("File a public NIP-34 issue for nostrc. Describe the problem without including private messages or keys. Nothing is sent until you review and confirm. Groundhog does not upload files; add web links instead."));
  const gchar *labels[] = { "bug", "groundhog", NULL };
  gn_nip34_issue_view_set_required_labels(view, labels);
  if (settings) {
    g_auto(GStrv) relays = g_settings_get_strv(settings, "discovery-relays");
    gn_nip34_issue_view_set_relays(view, (const gchar *const *)relays);
  }
  if (gh_diagnostics_get_default())
    gn_nip34_issue_view_set_diagnostics(view, "Local diagnostics", _("Include Local Diagnostics"),
      _("Off for every new report. When checked, the exact text below is added to the public issue: daily counts of typed app events (component, event and result) from this device. It never contains messages, keys, contacts, identifiers, relay or website addresses, or file paths."),
      GN_ISSUE_DIAGNOSTICS_NONE, diagnostics_snapshot, NULL, NULL);
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(accounts));
  gn_nip34_issue_view_set_pubkey(view, pubkey);
  g_object_set_data(G_OBJECT(view), "gh-issue-generation",
                    GSIZE_TO_POINTER((gsize)gh_account_controller_get_generation(accounts)));
  g_signal_connect_object(accounts, "changed", G_CALLBACK(account_changed), view, 0);
  return view;
}
