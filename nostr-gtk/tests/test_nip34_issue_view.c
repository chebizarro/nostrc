/* GnNip34IssueView with mock services (nostrc-8xfib.5): no resolver,
 * uploader or publisher call happens before the user consents. */
#include <adwaita.h>
#include <nostr-gtk-1.0/gn-nip34-issue-view.h>
#include <nostr-gtk-1.0/gn-nip34-issue-fields.h>
#include <nostr-event.h>
#include <glib/gstdio.h>
#include <string.h>
#include "nostrc-test-gdk-frame.h"

#define OWNER "cdee943cbb19c51ab847a66d5d774373aa9f63d287246bb59b0827fa5e637400"
#define PK "2222222222222222222222222222222222222222222222222222222222222222"
#define PK2 "5555555555555555555555555555555555555555555555555555555555555555"
#define MAINT "3333333333333333333333333333333333333333333333333333333333333333"

static void drain(void) { while (g_main_context_iteration(NULL, FALSE)); }

/* ---- Mock services ---- */

typedef struct {
  GObject parent_instance;
  guint publish_calls, upload_calls, resolve_calls;
  gchar *last_json;
  GStrv last_relays;
  gboolean fail_not_sent;
} Mock;
typedef GObjectClass MockClass;

static void mock_publisher_iface_init(GnIssuePublisherInterface *iface);
static void mock_uploader_iface_init(GnIssueUploaderInterface *iface);
static void mock_resolver_iface_init(GnIssueRepoResolverInterface *iface);
G_DEFINE_TYPE_WITH_CODE(Mock, mock, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_PUBLISHER, mock_publisher_iface_init)
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_UPLOADER, mock_uploader_iface_init)
  G_IMPLEMENT_INTERFACE(GN_TYPE_ISSUE_REPO_RESOLVER, mock_resolver_iface_init))

static void
mock_finalize(GObject *object)
{
  Mock *self = (Mock *)object;
  g_free(self->last_json);
  g_strfreev(self->last_relays);
  G_OBJECT_CLASS(mock_parent_class)->finalize(object);
}
static void mock_class_init(MockClass *klass) { klass->finalize = mock_finalize; }
static void mock_init(Mock *self) { (void)self; }

static void
publish_async(GnIssuePublisher *publisher, const gchar *json, const gchar *const *relays,
              GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  Mock *self = (Mock *)publisher;
  self->publish_calls++;
  g_free(self->last_json);
  self->last_json = g_strdup(json);
  g_strfreev(self->last_relays);
  self->last_relays = g_strdupv((gchar **)relays);
  GTask *task = g_task_new(publisher, cancel, callback, data);
  if (self->fail_not_sent)
    g_task_return_new_error(task, GN_ISSUE_PUBLISH_ERROR, GN_ISSUE_PUBLISH_ERROR_NOT_SENT,
                            "Signer not available");
  else
    g_task_return_pointer(task, g_strdup("e1e1"), g_free);
  g_object_unref(task);
}

static gboolean
publish_finish(GnIssuePublisher *publisher, GAsyncResult *result, gchar **event_id,
               gchar **message, GError **error)
{
  (void)publisher;
  (void)message;
  gchar *id = g_task_propagate_pointer(G_TASK(result), error);
  if (event_id)
    *event_id = id;
  else
    g_free(id);
  return id != NULL;
}

static void
mock_publisher_iface_init(GnIssuePublisherInterface *iface)
{
  iface->sign_and_publish_async = publish_async;
  iface->sign_and_publish_finish = publish_finish;
}

static const gchar *describe(GnIssueUploader *u) { (void)u; return "https://blossom.test"; }

static void
upload_async(GnIssueUploader *uploader, GFile *file, const gchar *content_type,
             GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  (void)content_type;
  ((Mock *)uploader)->upload_calls++;
  GTask *task = g_task_new(uploader, cancel, callback, data);
  g_autofree gchar *name = g_file_get_basename(file);
  if (strstr(name, "fail"))
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "upload refused");
  else
    g_task_return_pointer(task, g_strdup_printf("https://blossom.test/%s", name), g_free);
  g_object_unref(task);
}

static gchar *
upload_finish(GnIssueUploader *u, GAsyncResult *result, GError **error)
{
  (void)u;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_uploader_iface_init(GnIssueUploaderInterface *iface)
{
  iface->describe_destination = describe;
  iface->upload_async = upload_async;
  iface->upload_finish = upload_finish;
}

static void
resolve_async(GnIssueRepoResolver *resolver, const GnIssueTarget *hint, GCancellable *cancel,
              GAsyncReadyCallback callback, gpointer data)
{
  ((Mock *)resolver)->resolve_calls++;
  GTask *task = g_task_new(resolver, cancel, callback, data);
  const char *maintainers[] = { MAINT, NULL };
  const char *relays[] = { "wss://repo.example", NULL };
  g_task_return_pointer(task, gn_issue_target_new(hint->owner_hex, hint->repo_id, maintainers, relays),
                        (GDestroyNotify)gn_issue_target_free);
  g_object_unref(task);
}

static GnIssueTarget *
resolve_finish(GnIssueRepoResolver *r, GAsyncResult *result, GError **error)
{
  (void)r;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
mock_resolver_iface_init(GnIssueRepoResolverInterface *iface)
{
  iface->resolve_async = resolve_async;
  iface->resolve_finish = resolve_finish;
}

/* ---- Fixture ---- */

typedef struct {
  GtkWindow *window;
  GnNip34IssueView *view;
  Mock *mock;
  gchar *dir;
} Fixture;

static GtkWidget *
find(GtkWidget *root, const char *name)
{
  if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
    return root;
  for (GtkWidget *c = gtk_widget_get_first_child(root); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find(c, name);
    if (found)
      return found;
  }
  return NULL;
}

static GtkWidget *
child(Fixture *f, const char *name)
{
  GtkWidget *w = find(GTK_WIDGET(f->view), name);
  g_assert_nonnull(w);
  return w;
}

static GFile *
temp_file(Fixture *f, const char *name)
{
  g_autofree gchar *path = g_build_filename(f->dir, name, NULL);
  g_assert_true(g_file_set_contents(path, "x", 1, NULL));
  return g_file_new_for_path(path);
}

static void
setup(Fixture *f, gboolean with_uploader, gboolean with_resolver)
{
  f->mock = g_object_new(mock_get_type(), NULL);
  f->dir = g_dir_make_tmp("gn-issue-view-XXXXXX", NULL);
  g_autoptr(GnIssueTarget) target = gn_issue_target_new(OWNER, "nostrc", NULL, NULL);
  f->view = gn_nip34_issue_view_new(target, GN_ISSUE_PUBLISHER(f->mock),
                                    with_resolver ? GN_ISSUE_REPO_RESOLVER(f->mock) : NULL,
                                    with_uploader ? GN_ISSUE_UPLOADER(f->mock) : NULL);
  g_object_ref_sink(f->view);
  gn_nip34_issue_view_set_pubkey(f->view, PK);
  const char *relays[] = { "wss://relay.example", NULL };
  gn_nip34_issue_view_set_relays(f->view, relays);
  f->window = GTK_WINDOW(adw_window_new());
  gtk_window_present(f->window);
  gn_nip34_issue_view_present(f->view, GTK_WIDGET(f->window));
  gtk_editable_set_text(GTK_EDITABLE(child(f, "title_row")), "Crash on start");
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(child(f, "body_view"))),
                           "It crashes.", -1);
  drain();
}

static void
teardown(Fixture *f)
{
  gtk_window_destroy(f->window);
  drain();
  g_object_unref(f->view);
  g_object_unref(f->mock);
  g_autoptr(GDir) dir = g_dir_open(f->dir, 0, NULL);
  const char *name;
  while (dir && (name = g_dir_read_name(dir))) {
    g_autofree gchar *path = g_build_filename(f->dir, name, NULL);
    g_unlink(path);
  }
  g_rmdir(f->dir);
  g_free(f->dir);
}

static gboolean
count_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget;
  (void)clock;
  return ++*(guint *)data < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* An AdwDialog opens its sheet on the second frame after it is mapped, and
 * closing it before then does nothing: the sheet is not open, so it never
 * emits "closing" and the dialog stays in the window.  A user can only answer
 * a dialog that is on screen, so wait for those two frames.  A non-blocking
 * drain() does not wait for the frame clock (libadwaita 1.5 under Xvfb gets no
 * frame during it; macOS happened to). */
static void
wait_presented(AdwDialog *dialog)
{
  while (!gtk_widget_get_mapped(GTK_WIDGET(dialog)))
    g_main_context_iteration(NULL, TRUE);
  guint ticks = 0;
  gtk_widget_add_tick_callback(GTK_WIDGET(dialog), count_tick, &ticks, NULL);
  while (ticks < 2)
    g_main_context_iteration(NULL, TRUE);
}

static AdwAlertDialog *
alert(Fixture *f)
{
  AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(f->window));
  if (!ADW_IS_ALERT_DIALOG(dialog))
    g_error("no consent prompt; status: %s",
            gtk_label_get_text(GTK_LABEL(find(GTK_WIDGET(f->view), "status"))));
  wait_presented(dialog);
  return ADW_ALERT_DIALOG(dialog);
}

static const char *
preview(AdwAlertDialog *a)
{
  GtkWidget *scroll = adw_alert_dialog_get_extra_child(a);
  GtkWidget *viewport = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroll));
  GtkWidget *label = GTK_IS_VIEWPORT(viewport) ? gtk_viewport_get_child(GTK_VIEWPORT(viewport)) : viewport;
  return gtk_label_get_text(GTK_LABEL(label));
}

static void
respond(AdwAlertDialog *a, const char *response)
{
  g_signal_emit_by_name(a, "response", response);
  adw_dialog_force_close(ADW_DIALOG(a));
  drain();
}

static void
click(Fixture *f, const char *name)
{
  g_signal_emit_by_name(child(f, name), "clicked");
  drain();
}

static const char *
status(Fixture *f)
{
  return gtk_label_get_text(GTK_LABEL(child(f, "status")));
}

/* ---- Tests ---- */

static void
test_no_calls_before_consent(void)
{
  Fixture f = {0};
  setup(&f, TRUE, TRUE);
  g_autoptr(GFile) file = temp_file(&f, "shot.png");
  g_assert_true(gn_nip34_issue_view_add_attachment(f.view, file));
  click(&f, "review_button");
  AdwAlertDialog *a = alert(&f);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(a), "https://blossom.test"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(a), "shot.png"));
  g_assert_cmpuint(f.mock->resolve_calls + f.mock->upload_calls + f.mock->publish_calls, ==, 0);
  respond(a, "cancel");
  g_assert_cmpuint(f.mock->resolve_calls + f.mock->upload_calls + f.mock->publish_calls, ==, 0);

  /* Consent to upload, then the final event is reviewed again before signing. */
  click(&f, "review_button");
  respond(alert(&f), "upload");
  g_assert_cmpuint(f.mock->upload_calls, ==, 1);
  g_assert_cmpuint(f.mock->publish_calls, ==, 0);
  a = alert(&f);
  g_assert_nonnull(strstr(preview(a), "## Attachments\n\n- https://blossom.test/shot.png"));
  respond(a, "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 1);
  g_assert_cmpuint(f.mock->upload_calls, ==, 1);
  g_assert_nonnull(strstr(f.mock->last_json, "https://blossom.test/shot.png"));
  g_assert_cmpuint(f.mock->resolve_calls, ==, 0); /* never on its own */
  g_assert_cmpstr(status(&f), ==, "Issue published. It is public on Nostr.");
  teardown(&f);
}

static void
test_edit_after_review(void)
{
  Fixture f = {0};
  setup(&f, FALSE, FALSE);
  click(&f, "review_button");
  AdwAlertDialog *a = alert(&f);
  g_assert_nonnull(strstr(preview(a), "Crash on start"));
  g_assert_nonnull(strstr(preview(a), "wss://relay.example"));
  g_assert_nonnull(strstr(preview(a), PK));
  gtk_editable_set_text(GTK_EDITABLE(child(&f, "title_row")), "Edited");
  respond(a, "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 0);
  g_assert_nonnull(strstr(status(&f), "changed after review"));
  click(&f, "review_button");
  respond(alert(&f), "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 1);
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, f.mock->last_json, NULL), ==, 1);
  g_assert_cmpstr(nostr_event_get_pubkey(event), ==, PK);
  nostr_event_free(event);
  teardown(&f);
}

static void
test_pubkey_change(void)
{
  Fixture f = {0};
  setup(&f, FALSE, FALSE);
  click(&f, "review_button");
  AdwAlertDialog *a = alert(&f);
  gn_nip34_issue_view_set_pubkey(f.view, PK2);
  respond(a, "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 0);
  /* invalidate() locks the report (account changed). */
  gn_nip34_issue_view_invalidate(f.view, "The account changed.");
  click(&f, "review_button");
  g_assert_false(ADW_IS_ALERT_DIALOG(adw_window_get_visible_dialog(ADW_WINDOW(f.window))));
  g_assert_cmpuint(f.mock->publish_calls, ==, 0);
  g_assert_cmpstr(status(&f), ==, "The account changed.");
  teardown(&f);
}

static void
test_partial_upload_failure(void)
{
  Fixture f = {0};
  setup(&f, TRUE, FALSE);
  g_autoptr(GFile) ok = temp_file(&f, "ok.txt");
  g_autoptr(GFile) bad = temp_file(&f, "fail.txt");
  g_assert_true(gn_nip34_issue_view_add_attachment(f.view, ok));
  g_assert_true(gn_nip34_issue_view_add_attachment(f.view, bad));
  click(&f, "review_button");
  respond(alert(&f), "upload");
  g_assert_cmpuint(f.mock->upload_calls, ==, 2);
  AdwAlertDialog *a = alert(&f);
  g_assert_nonnull(strstr(preview(a), "https://blossom.test/ok.txt"));
  g_assert_null(strstr(preview(a), "fail.txt"));
  respond(a, "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 1);
  teardown(&f);
}

static void
test_send_without_logs(void)
{
  Fixture f = {0};
  setup(&f, TRUE, FALSE);
  g_autoptr(GFile) crash = temp_file(&f, "gnostr-fail.ips");
  g_autoptr(GFile) good = temp_file(&f, "gnostr-good.ips");
  g_autoptr(GListStore) logs = g_list_store_new(G_TYPE_FILE);
  g_list_store_append(logs, good);
  g_list_store_append(logs, crash);
  gn_nip34_issue_view_set_crash_logs(f.view, G_LIST_MODEL(logs));
  /* Nothing is selected by default. */
  GtkWidget *list = child(&f, "crash_list");
  guint checked = 0;
  for (GtkWidget *row = gtk_widget_get_first_child(list); row; row = gtk_widget_get_next_sibling(row)) {
    GtkCheckButton *check = g_object_get_data(G_OBJECT(row), "gn-crash-check");
    g_assert_false(gtk_check_button_get_active(check));
    gtk_check_button_set_active(check, TRUE);
    checked++;
  }
  g_assert_cmpuint(checked, ==, 2);
  click(&f, "review_button");
  respond(alert(&f), "upload");
  g_assert_cmpuint(f.mock->upload_calls, ==, 2);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(&f, "review_button"))), ==, "Send Without Logs");
  g_assert_cmpuint(f.mock->publish_calls, ==, 0);
  click(&f, "review_button");
  AdwAlertDialog *a = alert(&f);
  g_assert_null(strstr(preview(a), "## Crash logs"));
  respond(a, "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 1);
  g_assert_null(strstr(f.mock->last_json, "Crash logs"));
  teardown(&f);
}

static gchar *diag_func(gpointer data) { (void)data; return g_strdup("App: test 1.0"); }

static void
test_diagnostics(void)
{
  Fixture f = {0};
  setup(&f, FALSE, FALSE);
  g_assert_false(gtk_widget_get_visible(gtk_widget_get_parent(child(&f, "diagnostics_check"))));
  gn_nip34_issue_view_set_diagnostics(f.view, "System info", "Include System Info", NULL,
                                      GN_ISSUE_DIAGNOSTICS_NONE, diag_func, NULL, NULL);
  GtkCheckButton *check = GTK_CHECK_BUTTON(child(&f, "diagnostics_check"));
  g_assert_true(gtk_widget_get_visible(gtk_widget_get_parent(GTK_WIDGET(check))));
  g_assert_false(gtk_check_button_get_active(check)); /* off unless the host asks */
  gtk_check_button_set_active(check, TRUE);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(&f, "diagnostics_preview"))), ==, "App: test 1.0");
  click(&f, "review_button");
  AdwAlertDialog *a = alert(&f);
  g_assert_nonnull(strstr(preview(a), "## System info\n\n```text\nApp: test 1.0\n```"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(a), "System info"));
  respond(a, "cancel");
  teardown(&f);
}

static void
test_publish_not_sent_allows_retry(void)
{
  Fixture f = {0};
  setup(&f, FALSE, FALSE);
  f.mock->fail_not_sent = TRUE;
  click(&f, "review_button");
  respond(alert(&f), "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 1);
  g_assert_true(gtk_widget_get_sensitive(child(&f, "review_button")));
  f.mock->fail_not_sent = FALSE;
  click(&f, "review_button");
  respond(alert(&f), "publish");
  g_assert_cmpuint(f.mock->publish_calls, ==, 2);
  g_assert_false(gtk_widget_get_sensitive(child(&f, "review_button")));
  teardown(&f);
}

static void
test_lookup_repository(void)
{
  Fixture f = {0};
  setup(&f, FALSE, TRUE);
  g_assert_cmpuint(f.mock->resolve_calls, ==, 0);
  g_assert_true(gtk_widget_get_visible(child(&f, "lookup_button")));
  click(&f, "lookup_button");
  g_assert_cmpuint(f.mock->resolve_calls, ==, 1);
  g_assert_cmpstr(gn_nip34_issue_view_get_target(f.view)->relays[0], ==, "wss://repo.example");
  click(&f, "review_button");
  respond(alert(&f), "publish");
  g_assert_cmpstr(f.mock->last_relays[0], ==, "wss://repo.example");
  g_assert_cmpstr(f.mock->last_relays[1], ==, "wss://relay.example");
  g_assert_nonnull(strstr(f.mock->last_json, MAINT));
  teardown(&f);
}

static void
assert_labelled(GtkWidget *w)
{
  if ((GTK_IS_TEXT_VIEW(w) || GTK_IS_ENTRY(w)) &&
      !gtk_test_accessible_has_property(GTK_ACCESSIBLE(w), GTK_ACCESSIBLE_PROPERTY_LABEL))
    g_error("%s %s has no accessible label", G_OBJECT_TYPE_NAME(w), gtk_widget_get_name(w));
  if (ADW_IS_ENTRY_ROW(w))
    g_assert_nonnull(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(w)));
  for (GtkWidget *c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c))
    assert_labelled(c);
}

static void
test_a11y(void)
{
  Fixture f = {0};
  setup(&f, TRUE, TRUE);
  gn_nip34_issue_view_set_diagnostics(f.view, "System info", "Include", NULL,
                                      GN_ISSUE_DIAGNOSTICS_EDITABLE | GN_ISSUE_DIAGNOSTICS_DEFAULT_ON,
                                      diag_func, NULL, NULL);
  assert_labelled(GTK_WIDGET(f.view));
  teardown(&f);
}

int
main(int argc, char **argv)
{
  gtk_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  adw_init();
  /* No cursor blink: wait_presented() runs the main loop blocking, and with
   * one Xvfb shared by parallel tests GTK 4.14's blink timeout can fire while
   * another test's window holds X focus, which it reports as a warning
   * ("GtkText - did not receive a focus-out event"). */
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE,
               "gtk-cursor-blink", FALSE, NULL);
  g_test_add_func("/nostr-gtk/nip34-issue-view/no-calls-before-consent", test_no_calls_before_consent);
  g_test_add_func("/nostr-gtk/nip34-issue-view/edit-after-review", test_edit_after_review);
  g_test_add_func("/nostr-gtk/nip34-issue-view/pubkey-change", test_pubkey_change);
  g_test_add_func("/nostr-gtk/nip34-issue-view/partial-upload-failure", test_partial_upload_failure);
  g_test_add_func("/nostr-gtk/nip34-issue-view/send-without-logs", test_send_without_logs);
  g_test_add_func("/nostr-gtk/nip34-issue-view/diagnostics", test_diagnostics);
  g_test_add_func("/nostr-gtk/nip34-issue-view/not-sent-retry", test_publish_not_sent_allows_retry);
  g_test_add_func("/nostr-gtk/nip34-issue-view/lookup", test_lookup_repository);
  g_test_add_func("/nostr-gtk/nip34-issue-view/a11y", test_a11y);
  return g_test_run();
}
