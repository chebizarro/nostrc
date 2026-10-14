#include "nostr-kinds.h"
#include "nostrc-test-gdk-frame.h"
#include "ui/approval_dialog.h"
#include <string.h>

typedef struct {
  GtkWidget *window;
  GnostrApprovalDialog *dialog;
  guint calls, deferred, selected, flow_calls;
  gboolean flow_encrypt, flow_seals;
  gchar **selected_ids;
  gboolean decision, remember, closed;
  guint64 ttl;
  gchar *identity;
} Fixture;
static void decision(gboolean allow, gboolean remember, const char *identity, guint64 ttl,
                     gpointer data) {
  Fixture *f = data;
  f->calls++;
  f->decision = allow;
  f->remember = remember;
  f->ttl = ttl;
  g_free(f->identity);
  f->identity = g_strdup(identity);
}
static void selected(GnostrApprovalDialog *dialog, gchar **ids, gpointer data) {
  (void)dialog;
  Fixture *f = data;
  f->selected++;
  g_strfreev(f->selected_ids);
  f->selected_ids = g_strdupv(ids);
}
static void flow_selected(GnostrApprovalDialog *dialog, gboolean encrypt, gboolean seals, gpointer data) {
  (void)dialog; Fixture *f = data;
  f->flow_calls++; f->flow_encrypt = encrypt; f->flow_seals = seals;
}
static void closed(AdwDialog *dialog, gpointer data) {
  (void)dialog;
  ((Fixture *)data)->closed = TRUE;
}
static void deferred(GnostrApprovalDialog *dialog, gpointer data) {
  (void)dialog;
  ((Fixture *)data)->deferred++;
}
static gboolean failure_bound(gpointer data) {
  (void)data;
  g_error("Approval dialog did not complete");
  return G_SOURCE_REMOVE;
}
static gboolean frame(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget;
  (void)clock;
  return ++*(guint *)data < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static void setup(Fixture *f, gconstpointer data) {
  (void)data;
  f->dialog = gnostr_approval_dialog_new();
  g_object_ref_sink(f->dialog);
  gnostr_approval_dialog_set_identity(f->dialog, "npub-pinned-by-daemon");
  gnostr_approval_dialog_set_callback(f->dialog, decision, f);
  g_signal_connect(f->dialog, "closed", G_CALLBACK(closed), f);
  g_signal_connect(f->dialog, "deferred", G_CALLBACK(deferred), f);
  g_signal_connect(f->dialog, "approve-selected", G_CALLBACK(selected), f);
  g_signal_connect(f->dialog, "approve-flow", G_CALLBACK(flow_selected), f);
  f->window = adw_window_new();
  gtk_window_set_default_size(GTK_WINDOW(f->window), 800, 700);
  gtk_window_present(GTK_WINDOW(f->window));
  adw_dialog_present(ADW_DIALOG(f->dialog), f->window);
  guint timeout = g_timeout_add_seconds(10, failure_bound, NULL), frames = 0;
  while (!gtk_widget_get_mapped(GTK_WIDGET(f->dialog)))
    g_main_context_iteration(NULL, TRUE);
  gtk_widget_add_tick_callback(GTK_WIDGET(f->dialog), frame, &frames, NULL);
  while (frames < 2)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static void teardown(Fixture *f, gconstpointer data) {
  (void)data;
  gnostr_approval_dialog_set_callback(f->dialog, NULL, NULL);
  gtk_window_destroy(GTK_WINDOW(f->window));
  while (g_main_context_iteration(NULL, FALSE)) {
  }
  g_object_unref(f->dialog);
  g_free(f->identity);
  g_strfreev(f->selected_ids);
}
static GtkWidget *child(Fixture *f, const char *name) {
  GtkWidget *w = GTK_WIDGET(
      gtk_widget_get_template_child(GTK_WIDGET(f->dialog), GNOSTR_TYPE_APPROVAL_DIALOG, name));
  g_assert_nonnull(w);
  return w;
}
static void click(Fixture *f, const char *button) {
  g_signal_emit_by_name(child(f, button), "clicked");
}
static void wait_closed(Fixture *f) {
  guint timeout = g_timeout_add_seconds(10, failure_bound, NULL);
  while (!f->closed)
    g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
  while (g_main_context_iteration(NULL, FALSE)) {
  }
}
static GVariant *info(guint32 kind, const char *operation, guint remaining) {
  GVariantBuilder b, summary;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
#define S(key, value) g_variant_builder_add(&b, "{sv}", key, g_variant_new_string(value))
  S("identity", "npub-pinned-by-daemon");
  S("principal", "org.example.Client");
  S("transport", "DBUS");
  S("operation", operation);
  S("state", "PENDING");
  S("reason", "Send my note");
  S("reason_source", "caller");
  S("claimed_app_id", "Bogus name");
#undef S
  g_variant_builder_add(&b, "{sv}", "event_kind", g_variant_new_uint32(kind));
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32(remaining));
  g_variant_builder_add(&b, "{sv}", "verified", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&b, "{sv}", "rememberable", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&summary, "{sv}", "preview",
                        g_variant_new_string("ciphertext must not be displayed"));
  g_variant_builder_add(&summary, "{sv}", "target_event_id",
                        g_variant_new_string("target-event-id"));
  g_variant_builder_add(&summary, "{sv}", "auth_relay", g_variant_new_string("wss://nos.lol"));
  g_variant_builder_add(&summary, "{sv}", "peer", g_variant_new_string("peer-fingerprint"));
  g_variant_builder_add(&summary, "{sv}", "recipient_peer",
                        g_variant_new_string("recipient-fingerprint"));
  g_variant_builder_add(&summary, "{sv}", "reaction_value", g_variant_new_string("+"));
  g_variant_builder_add(&summary, "{sv}", "delete_target_count", g_variant_new_uint32(3));
  g_variant_builder_add(&summary, "{sv}", "tag_count", g_variant_new_uint32(4));
  GVariantBuilder counts;
  g_variant_builder_init(&counts, G_VARIANT_TYPE("a{su}"));
  g_variant_builder_add(&counts, "{su}", "x", 2);
  g_variant_builder_add(&summary, "{sv}", "tag_counts", g_variant_builder_end(&counts));
  g_variant_builder_add(&b, "{sv}", "event_summary", g_variant_builder_end(&summary));
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
static void approve_once(Fixture *f, gconstpointer data) {
  (void)data;
  click(f, "btn_approve");
  click(f, "btn_approve");
  wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 1);
  g_assert_true(f->decision);
  g_assert_cmpstr(f->identity, ==, "npub-pinned-by-daemon");
}
static void deny_once(Fixture *f, gconstpointer data) {
  (void)data;
  click(f, "btn_deny");
  wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 1);
  g_assert_false(f->decision);
}
static void close_denies(Fixture *f, gconstpointer data) {
  (void)data;
  adw_dialog_close(ADW_DIALOG(f->dialog));
  wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 1);
  g_assert_false(f->decision);
  g_assert_false(f->remember);
}
static void not_now(Fixture *f, gconstpointer data) {
  (void)data;
  click(f, "btn_not_now");
  wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 0);
  g_assert_cmpuint(f->deferred, ==, 1);
}
static void disabled_approve(Fixture *f, gconstpointer data) {
  (void)data;
  gtk_widget_set_sensitive(child(f, "btn_approve"), FALSE);
  click(f, "btn_approve");
  g_assert_cmpuint(f->calls, ==, 0);
  g_assert_false(f->closed);
}
static void fixed_identity_remember(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) row = info(NOSTR_KIND_TEXT_NOTE, "sign_event", 120);
  gnostr_approval_dialog_set_info(f->dialog, row, "Verified client");
  g_assert_nonnull(
      strstr(gtk_label_get_text(GTK_LABEL(child(f, "label_identity"))), "npub-pinned-by-daemon"));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "label_scope"))), "kind 1"));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "label_reason"))),
                          "Reason supplied by the application"));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_remember")), TRUE);
  gtk_drop_down_set_selected(GTK_DROP_DOWN(child(f, "ttl_dropdown")), 1);
  click(f, "btn_approve");
  wait_closed(f);
  g_assert_cmpstr(f->identity, ==, "npub-pinned-by-daemon");
  g_assert_true(f->remember);
  g_assert_cmpuint(f->ttl, ==, 3600);
}
static void unicode(Fixture *f, gconstpointer data) {
  (void)data;
  GString *s = g_string_new(NULL);
  for (guint i = 0; i < 400; i++)
    g_string_append(s, "🌲");
  gnostr_approval_dialog_set_content(f->dialog, s->str);
  g_string_free(s, TRUE);
  const char *preview = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
  g_assert_true(g_utf8_validate(preview, -1, NULL));
  g_assert_cmpint(g_utf8_strlen(preview, -1), ==, 281);
  gnostr_approval_dialog_set_content(f->dialog, "bad\xff");
  g_assert_true(
      g_utf8_validate(gtk_label_get_text(GTK_LABEL(child(f, "content_preview"))), -1, NULL));
}
static void completion_cancels(Fixture *f, gconstpointer data) {
  (void)data;
  gnostr_approval_dialog_set_callback(f->dialog, NULL, NULL);
  adw_dialog_force_close(ADW_DIALOG(f->dialog));
  wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 0);
}
static void previews(Fixture *f, gconstpointer data) {
  (void)data;
  const guint32 kinds[] = {NOSTR_KIND_GIFT_WRAP, NOSTR_KIND_DELETION,
                           NOSTR_KIND_CLIENT_AUTHENTICATION, NOSTR_KIND_REACTION, 65535};
  const char *expected[] = {"Encrypted message", "Delete 3 target(s)", "wss://nos.lol",
                            "Reaction: +", "x: 2"};
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
    g_autoptr(GVariant) row = info(kinds[i], "sign_event", 120);
    gnostr_approval_dialog_set_info(f->dialog, row, "Client");
    const char *preview = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
    g_assert_nonnull(strstr(preview, expected[i]));
    if (i == 0) {
      g_assert_null(strstr(preview, "ciphertext"));
      g_assert_nonnull(strstr(preview, "recipient-fingerprint"));
    }
    if (kinds[i] == 65535)
      g_assert_null(strstr(preview, "ciphertext"));
  }
  /* Exact AUTH relay must not be cut at the public-content preview bound. */
  g_autoptr(GVariant) auth = info(NOSTR_KIND_CLIENT_AUTHENTICATION, "sign_event", 120);
  g_autoptr(GVariant) old_summary =
      g_variant_lookup_value(auth, "event_summary", G_VARIANT_TYPE_VARDICT);
  GVariantDict summary, outer;
  g_variant_dict_init(&summary, old_summary);
  g_autofree gchar *path = g_strnfill(300, 'a');
  g_autofree gchar *relay = g_strconcat("wss://nos.lol/", path, "/exact-end", NULL);
  g_variant_dict_insert(&summary, "auth_relay", "s", relay);
  g_variant_dict_init(&outer, auth);
  g_variant_dict_insert_value(&outer, "event_summary", g_variant_dict_end(&summary));
  g_autoptr(GVariant) exact = g_variant_ref_sink(g_variant_dict_end(&outer));
  gnostr_approval_dialog_set_info(f->dialog, exact, "Client");
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "content_preview"))), relay));
  g_autoptr(GVariant) row = info(NOSTR_KIND_TEXT_NOTE, "nip44_conversation_key", 120);
  gnostr_approval_dialog_set_info(f->dialog, row, "Client");
  g_assert_nonnull(
      strstr(gtk_label_get_text(GTK_LABEL(child(f, "subtitle_label"))), "every NIP-44 message"));
  g_assert_nonnull(
      strstr(gtk_label_get_text(GTK_LABEL(child(f, "content_preview"))), "peer-fingerprint"));
}
static void stepper_focus(Fixture *f, gconstpointer data) {
  (void)data;
  gnostr_approval_dialog_set_position(f->dialog, 1, 3);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "label_position"))), ==, "1 of 3");
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_previous")));
  g_assert_true(gtk_widget_get_sensitive(child(f, "btn_next")));
  g_assert_true(adw_dialog_get_focus(ADW_DIALOG(f->dialog)) == child(f, "btn_deny"));
  g_assert_null(adw_dialog_get_default_widget(ADW_DIALOG(f->dialog)));
}
static void batch_selection(Fixture *f, gconstpointer data) {
  (void)data;
  GVariantBuilder rows;
  g_variant_builder_init(&rows, G_VARIANT_TYPE("aa{sv}"));
  for (guint i = 0; i < 2; i++) {
    GVariantBuilder row;
    g_variant_builder_init(&row, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&row, "{sv}", "request_id", g_variant_new_string(i ? "second" : "first"));
    g_variant_builder_add(&row, "{sv}", "operation", g_variant_new_string("sign_event"));
    g_variant_builder_add(&row, "{sv}", "event_kind", g_variant_new_uint32(NOSTR_KIND_TEXT_NOTE));
    g_variant_builder_add_value(&rows, g_variant_builder_end(&row));
  }
  g_autoptr(GVariant) list = g_variant_ref_sink(g_variant_builder_end(&rows));
  gnostr_approval_dialog_set_batch(f->dialog, list);
  g_assert_true(gtk_widget_get_visible(child(f, "batch_expander")));
  g_assert_true(gnostr_approval_dialog_has_batch_request(f->dialog, "first"));
  g_assert_true(gnostr_approval_dialog_has_batch_request(f->dialog, "second"));
  g_assert_false(gnostr_approval_dialog_has_batch_request(f->dialog, "future-unseen"));
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_approve_selected")));
  GtkWidget *first = gtk_widget_get_first_child(child(f, "batch_box"));
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(first)));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(first), TRUE);
  click(f, "btn_approve_selected");
  wait_closed(f);
  g_assert_cmpuint(f->selected, ==, 1);
  g_assert_cmpstr(f->selected_ids[0], ==, "first");
  g_assert_null(f->selected_ids[1]);
  g_assert_cmpuint(f->calls, ==, 0);
}
static void set_room_flow(Fixture *f) {
  GVariantBuilder b, summary;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&summary, "{sv}", "peer", g_variant_new_string(
      "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"));
  g_variant_builder_add(&b, "{sv}", "operation", g_variant_new_string("nip44_encrypt"));
  g_variant_builder_add(&b, "{sv}", "identity", g_variant_new_string("npub-pinned"));
  g_variant_builder_add(&b, "{sv}", "purpose", g_variant_new_string("nip17-room-send"));
  g_variant_builder_add(&b, "{sv}", "flow_id", g_variant_new_string("room-send"));
  g_variant_builder_add(&b, "{sv}", "flow_size", g_variant_new_uint32(4));
  g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string("PENDING"));
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32(120));
  g_variant_builder_add(&b, "{sv}", "rememberable", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&b, "{sv}", "event_summary", g_variant_builder_end(&summary));
  g_autoptr(GVariant) info = g_variant_ref_sink(g_variant_builder_end(&b));
  gnostr_approval_dialog_set_info(f->dialog, info, "Room client");
  g_assert_true(gtk_widget_get_visible(child(f, "flow_box")));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "label_flow"))), "4 encrypted messages"));
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt"))));
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(child(f, "chk_flow_seals"))));
}
static void flow_unchecked(Fixture *f, gconstpointer data) {
  (void)data; set_room_flow(f); click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpuint(f->flow_calls, ==, 0); g_assert_cmpuint(f->calls, ==, 1);
  g_assert_false(f->remember);
}
static void flow_encrypt_only(Fixture *f, gconstpointer data) {
  (void)data; set_room_flow(f);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt")), TRUE);
  click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpuint(f->flow_calls, ==, 1); g_assert_cmpuint(f->calls, ==, 0);
  g_assert_true(f->flow_encrypt); g_assert_false(f->flow_seals);
}
static void flow_seals_only(Fixture *f, gconstpointer data) {
  (void)data; set_room_flow(f);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_flow_seals")), TRUE);
  click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpuint(f->flow_calls, ==, 1); g_assert_cmpuint(f->calls, ==, 0);
  g_assert_false(f->flow_encrypt); g_assert_true(f->flow_seals);
}
static void flow_deny(Fixture *f, gconstpointer data) {
  (void)data; set_room_flow(f);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt")), TRUE);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_flow_seals")), TRUE);
  click(f, "btn_deny"); wait_closed(f);
  g_assert_cmpuint(f->flow_calls, ==, 0); g_assert_cmpuint(f->calls, ==, 1);
  g_assert_false(f->decision);
}
static void legacy_no_session(Fixture *f, gconstpointer data) {
  (void)data;
  /* The implementation links without client_session.c: reintroducing a GUI
   * authorization/session call makes this production-dialog target fail link. */
  gnostr_approval_dialog_set_client_pubkey(f->dialog, "client-key");
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_remember")), TRUE);
  click(f, "btn_approve");
  wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 1);
  g_assert_true(f->remember);
  g_assert_cmpstr(f->identity, ==, "npub-pinned-by-daemon");
}
static void pairing_preview(Fixture *f, gconstpointer data) {
  (void)data;
  for (guint complete = 0; complete < 2; complete++) {
    GVariantBuilder info, summary; g_variant_builder_init(&info, G_VARIANT_TYPE_VARDICT); g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&info, "{sv}", "operation", g_variant_new_string("connect"));
    g_variant_builder_add(&info, "{sv}", "principal", g_variant_new_string("nip46:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"));
    g_variant_builder_add(&info, "{sv}", "identity", g_variant_new_string("npub-pinned-by-daemon"));
    g_variant_builder_add(&info, "{sv}", "claimed_app_id", g_variant_new_string("Unverified client"));
    const gchar *urls[] = {"wss://relay.example.test/", NULL};
    const gchar *hints[] = {"sign_event:1", NULL};
    if (complete) g_variant_builder_add(&summary, "{sv}", "relays", g_variant_new_strv(urls, -1));
    g_variant_builder_add(&summary, "{sv}", "requested_permissions_hint", g_variant_new_strv(hints, -1));
    g_variant_builder_add(&summary, "{sv}", "secret", g_variant_new_string("must-not-render"));
    g_variant_builder_add(&info, "{sv}", "event_summary", g_variant_builder_end(&summary));
    g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_builder_end(&info));
    g_assert_cmpint(gnostr_approval_info_can_approve(row), ==, complete);
    gnostr_approval_dialog_set_info(f->dialog, row, "Remote application");
    const gchar *preview = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
    g_assert_nonnull(strstr(preview, "Unverified client"));
    g_assert_nonnull(strstr(preview, "sign_event:1"));
    g_assert_null(strstr(preview, "must-not-render"));
    if (complete) g_assert_nonnull(strstr(preview, urls[0]));
  }
}
int main(int argc, char **argv) {
  if (g_getenv("GNOSTR_SKIP_UI_TESTS")) {
    g_print("1..0 # SKIP UI disabled\n");
    return 0;
  }
#ifndef __APPLE__
  if (!g_getenv("DISPLAY") && !g_getenv("WAYLAND_DISPLAY")) {
    g_print("1..0 # SKIP display required\n");
    return 0;
  }
#endif
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  gtk_init();
  adw_init();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
#define ADD(name, fn)                                                                              \
  g_test_add("/signer/approval-dialog/" name, Fixture, NULL, setup, fn, teardown)
  ADD("approve-once", approve_once);
  ADD("deny-once", deny_once);
  ADD("close-denies", close_denies);
  ADD("not-now", not_now);
  ADD("disabled-approve", disabled_approve);
  ADD("fixed-identity-remember", fixed_identity_remember);
  ADD("unicode", unicode);
  ADD("completion-cancels", completion_cancels);
  ADD("previews", previews);
  ADD("stepper-focus", stepper_focus);
  ADD("batch-selection", batch_selection);
  ADD("flow-unchecked", flow_unchecked);
  ADD("flow-encrypt-only", flow_encrypt_only);
  ADD("flow-seals-only", flow_seals_only);
  ADD("flow-deny", flow_deny);
  ADD("legacy-no-session", legacy_no_session);
  ADD("pairing-preview", pairing_preview);
#undef ADD
  return g_test_run();
}
