#include "qr-scanner.h"
#include "pairing_uri.h"
#include <gst/gst.h>
#include <glib/gstdio.h>
#include "nostrc-test-gdk-frame.h"

static const gchar *fixture_uri = "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example.test%2F&secret=fixture-only&name=Fixture&perms=sign_event%3A1";
static gboolean finished, decoded;
static gchar *last_error;
static GThread *owner;
static void detected(GnQrScanner *s, GnQrScanResult *result, gpointer unused) {
  (void)unused; g_assert_true(g_thread_self() == owner);
  g_assert_cmpstr(result->data, ==, fixture_uri);
  NostrNip46ConnectURI uri; g_autoptr(GError) error = NULL;
  g_assert_true(grotto_connect_parse(result->data, &uri, &error));
  g_autoptr(GVariant) preview = grotto_connect_preview(&uri);
  g_assert_null(g_variant_lookup_value(preview, "secret", NULL));
  g_assert_cmpuint(uri.n_relays, ==, 1); grotto_connect_clear(&uri);
  decoded = finished = TRUE; gn_qr_scanner_stop(s);
}
static void scan_error(GnQrScanner *s, GError *error, gpointer unused) {
  (void)s; (void)unused; g_assert_nonnull(error); g_free(last_error); last_error = g_strdup(error->message); finished = TRUE;
}
static gboolean deadline(gpointer data) { (void)data; g_error("Scanner fixture did not produce a terminal signal"); return G_SOURCE_REMOVE; }
static void wait_signal(void) {
  guint bound = g_timeout_add_seconds(10, deadline, NULL);
  while (!finished) g_main_context_iteration(NULL, TRUE);
  g_source_remove(bound);
}
static void frame_pipeline(void) {
  g_autoptr(GError) error = NULL; g_autoptr(GdkPixbuf) qr = gn_qr_generate_pixbuf(fixture_uri, 600, &error);
  g_assert_no_error(error); g_assert_nonnull(qr);
  g_autofree gchar *dir = g_dir_make_tmp("grotto-qr-XXXXXX", &error); g_assert_no_error(error);
  g_autofree gchar *path = g_build_filename(dir, "fixture.png", NULL);
  g_assert_true(gdk_pixbuf_save(qr, path, "png", &error, NULL)); g_assert_no_error(error);
  g_autofree gchar *pipeline = g_strdup_printf("filesrc location=\"%s\" ! pngdec ! videoconvert ! video/x-raw,format=RGB ! appsink name=scan_sink emit-signals=true max-buffers=1 drop=true sync=false", path);
  GnQrScanner *s = g_object_ref_sink(gn_qr_scanner_new());
  g_signal_connect(s, "qr-detected", G_CALLBACK(detected), NULL);
  finished = decoded = FALSE;
  g_assert_true(gn_qr_scanner_start_test_pipeline(s, pipeline, &error)); g_assert_no_error(error);
  wait_signal(); g_assert_true(decoded);
  g_object_unref(s); while (g_main_context_iteration(NULL, FALSE));
  g_remove(path); g_rmdir(dir);
}
static void unavailable_pipeline(void) {
  GnQrScanner *s = g_object_ref_sink(gn_qr_scanner_new());
  g_signal_connect(s, "error", G_CALLBACK(scan_error), NULL);
  finished = FALSE; g_autoptr(GError) error = NULL;
  g_assert_true(gn_qr_scanner_start_test_pipeline(s,
      "filesrc location=/nonexistent/grotto-qr-fixture ! pngdec ! videoconvert ! video/x-raw,format=RGB ! appsink name=scan_sink emit-signals=true", &error));
  wait_signal(); g_assert_false(gn_qr_scanner_is_active(s));
  g_object_unref(s); while (g_main_context_iteration(NULL, FALSE));
}
static void denied_busy(void) {
  const gchar *messages[] = {"Permission denied", "Device or resource busy"};
  const gchar *expected[] = {"denied", "busy"};
  for (guint i = 0; i < 2; i++) {
    GnQrScanner *s = g_object_ref_sink(gn_qr_scanner_new()); finished = FALSE;
    g_signal_connect(s, "error", G_CALLBACK(scan_error), NULL);
    g_autoptr(GError) error = NULL;
    g_assert_true(gn_qr_scanner_start_test_pipeline(s, "videotestsrc is-live=true ! videoconvert ! video/x-raw,format=RGB ! appsink name=scan_sink emit-signals=true max-buffers=1 drop=true", &error));
    g_assert_no_error(error);
    error = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_OPEN_READ, messages[i]);
    gn_qr_scanner_test_post_error(s, error); wait_signal();
    g_assert_nonnull(strstr(last_error, expected[i])); g_assert_nonnull(strstr(last_error, "Paste a link"));
    g_assert_false(gn_qr_scanner_is_active(s)); g_object_unref(s);
    while (g_main_context_iteration(NULL, FALSE));
  }
  g_clear_pointer(&last_error, g_free);
}
static void uri_validation(void) {
  const gchar *bad[] = {
    "nostrconnect://bad?relay=wss://relay.example.test&secret=x",
    "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=ws://remote.example.test&secret=x",
    "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss://hidden.onion&secret=x", NULL};
  for (guint i = 0; bad[i]; i++) {
    NostrNip46ConnectURI uri; g_autoptr(GError) error = NULL;
    g_assert_false(grotto_connect_parse(bad[i], &uri, &error)); g_assert_nonnull(error);
  }
}
int main(int argc, char **argv) {
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  uri_validation();
  if (g_getenv("GNOSTR_SKIP_UI_TESTS") || !gtk_init_check()) return 77;
  gst_init(NULL, NULL); owner = g_thread_self();
  g_test_add_func("/scanner/filesrc-decodes-connect-preview", frame_pipeline);
  g_test_add_func("/scanner/no-device-paste-fallback", unavailable_pipeline);
  g_test_add_func("/scanner/denied-busy-paste-fallback", denied_busy);
  gint result = g_test_run(); g_free(last_error); return result;
}
