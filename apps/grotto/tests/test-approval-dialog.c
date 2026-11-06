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
  gchar *identity, *mode;
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
static void mode_selected(GnostrApprovalDialog *dialog, const gchar *mode, gpointer data) {
  (void)dialog; Fixture *f = data; g_free(f->mode); f->mode = g_strdup(mode);
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
  g_signal_connect(f->dialog, "decision-mode", G_CALLBACK(mode_selected), f);
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
  g_free(f->identity); g_free(f->mode);
  g_strfreev(f->selected_ids);
}
static GtkWidget *child(Fixture *f, const char *name) {
  GtkWidget *w = GTK_WIDGET(
      gtk_widget_get_template_child(GTK_WIDGET(f->dialog), GNOSTR_TYPE_APPROVAL_DIALOG, name));
  g_assert_nonnull(w);
  return w;
}
static const gchar *subtitle(Fixture *f, const gchar *name) {
  return adw_action_row_get_subtitle(ADW_ACTION_ROW(child(f, name)));
}
static const gchar *scope_text(Fixture *f) {
  return gtk_label_get_text(GTK_LABEL(child(f, "scope_summary")));
}
static void choose(Fixture *f, const gchar *prefix) {
  AdwComboRow *row = ADW_COMBO_ROW(child(f, "permission_choice"));
  GListModel *model = adw_combo_row_get_model(row);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GtkStringObject) item = g_list_model_get_item(model, i);
    if (g_str_has_prefix(gtk_string_object_get_string(item), prefix)) {
      adw_combo_row_set_selected(row, i);
      return;
    }
  }
  g_error("Missing permission choice: %s", prefix);
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
  S("principal_kind", "executable");
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
  g_variant_builder_add(&b, "{sv}", "routine_trust_available", g_variant_new_boolean(TRUE));
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
      strstr(subtitle(f, "row_identity"), "npub-pinned-by-daemon"));
  g_assert_nonnull(strstr(scope_text(f), "kind 1"));
  g_assert_nonnull(strstr(subtitle(f, "row_reason"), "Send my note"));
  choose(f, "Allow for 24 hours");
  g_assert_nonnull(strstr(scope_text(f), "24 hours"));
  click(f, "btn_approve");
  wait_closed(f);
  g_assert_cmpstr(f->identity, ==, "npub-pinned-by-daemon");
  g_assert_true(f->remember);
  g_assert_cmpuint(f->ttl, ==, 86400);
}
static void until_revoked(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) row = info(NOSTR_KIND_PROFILE_METADATA, "sign_event", 120);
  gnostr_approval_dialog_set_info(f->dialog, row, "Verified client");
  g_assert_cmpuint(adw_combo_row_get_selected(ADW_COMBO_ROW(child(f, "permission_choice"))), ==, 0);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, "_Allow Once");
  g_assert_nonnull(strstr(scope_text(f), "No permission is saved"));
  choose(f, "Always allow");
  click(f, "btn_approve"); wait_closed(f);
  g_assert_true(f->decision); g_assert_true(f->remember); g_assert_cmpuint(f->ttl, ==, 0);
}
static void deny_with_reusable_allow(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) row = info(NOSTR_KIND_PROFILE_METADATA, "sign_event", 120);
  gnostr_approval_dialog_set_info(f->dialog, row, "Verified client");
  choose(f, "Always allow");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(child(f, "btn_deny")), ==, "Deny Once (Alt+D, Escape)");
  click(f, "btn_deny"); wait_closed(f);
  g_assert_false(f->decision); g_assert_false(f->remember); g_assert_cmpuint(f->ttl, ==, 0);
}
static void explicit_mode(Fixture *f, gconstpointer data) {
  const gchar *mode = data;
  g_autoptr(GVariant) row = info(NOSTR_KIND_PROFILE_METADATA, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, row);
  g_variant_dict_insert(&d, "policy_review", "b", g_str_has_suffix(mode, "saved"));
  g_variant_dict_insert(&d, "identity_drift", "b", TRUE);
  GVariantBuilder permissions, permission;
  g_variant_builder_init(&permissions, G_VARIANT_TYPE("aa{sv}"));
  g_variant_builder_init(&permission, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&permission, "{sv}", "operation", g_variant_new_string("event"));
  g_variant_builder_add(&permission, "{sv}", "identity", g_variant_new_string("npub-pinned-by-daemon"));
  g_variant_builder_add(&permission, "{sv}", "decision", g_variant_new_string("policy-review"));
  g_variant_builder_add(&permission, "{sv}", "reason", g_variant_new_string("pending policy review"));
  g_variant_builder_add(&permission, "{sv}", "event-kind", g_variant_new_int32(NOSTR_KIND_PROFILE_METADATA));
  g_variant_builder_add(&permission, "{sv}", "expires-at", g_variant_new_uint64(0));
  g_variant_builder_add_value(&permissions, g_variant_builder_end(&permission));
  g_variant_dict_insert_value(&d, "permission_rows", g_variant_builder_end(&permissions));
  g_autoptr(GVariant) with_review = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, with_review, "Verified client");
  g_assert_true(gtk_widget_get_visible(child(f, "group_saved")));
  if (!strcmp(mode, "block-operation"))
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->dialog), "review.block-operation", NULL));
  else {
    choose(f, !strcmp(mode, "trust-routine") ? "Trust this app" :
        !strcmp(mode, "activate-saved") ? "Activate saved" : "Discard saved");
    if (!strcmp(mode, "trust-routine")) {
      g_assert_nonnull(strstr(scope_text(f), "not only this request or identity"));
      g_assert_nonnull(strstr(scope_text(f), "Blocks remain in effect"));
    }
    click(f, "btn_approve");
  }
  if (strcmp(mode, "discard-saved")) wait_closed(f);
  else {
    g_assert_false(f->closed);
    g_assert_false(gtk_widget_get_sensitive(child(f, "btn_approve")));
    g_assert_false(gtk_widget_has_css_class(child(f, "btn_approve"), "suggested-action"));
    /* A refetched still-pending request resets consent and remains reviewable. */
    gnostr_approval_dialog_set_info(f->dialog, row, "Verified client");
    g_assert_cmpuint(adw_combo_row_get_selected(ADW_COMBO_ROW(child(f, "permission_choice"))), ==, 0);
    g_assert_true(gtk_widget_get_sensitive(child(f, "btn_approve")));
  }
  g_assert_cmpstr(f->mode, ==, mode); g_assert_cmpuint(f->calls, ==, 0);
}
static GtkWidget *button_named(GtkWidget *root, const gchar *label) {
  if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), label)) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = button_named(child, label); if (found) return found;
  }
  return NULL;
}
static void block_app_confirmation(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) row = info(NOSTR_KIND_TEXT_NOTE, "sign_event", 120);
  gnostr_approval_dialog_set_info(f->dialog, row, "Client");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->dialog), "review.block-app", NULL));
  AdwDialog *alert = adw_window_get_visible_dialog(ADW_WINDOW(f->window));
  g_assert_true(ADW_IS_ALERT_DIALOG(alert)); g_assert_null(f->mode); g_assert_cmpuint(f->calls, ==, 0);
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(ADW_ALERT_DIALOG(alert), "block"), ==, ADW_RESPONSE_DESTRUCTIVE);
  GtkWidget *block = button_named(GTK_WIDGET(alert), "Block App"); g_assert_nonnull(block);
  g_signal_emit_by_name(block, "clicked"); wait_closed(f);
  g_assert_cmpstr(f->mode, ==, "block-app"); g_assert_cmpuint(f->calls, ==, 0);
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
  const gchar *operations[] = {"nip04_encrypt", "nip04_decrypt", "nip44_encrypt", "nip44_decrypt"};
  for (guint i = 0; i < G_N_ELEMENTS(operations); i++) {
    g_autoptr(GVariant) row = info(NOSTR_KIND_TEXT_NOTE, operations[i], 120);
    gnostr_approval_dialog_set_info(f->dialog, row, "Client");
    const gchar *preview = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
    g_assert_cmpstr(preview, ==, "Encrypted message. Private message bodies are not shown.");
    g_assert_null(strstr(preview, operations[i]));
    g_assert_null(strstr(preview, "peer-fingerprint"));
    g_assert_null(strstr(preview, "ciphertext"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "label_details"))), "peer-fingerprint"));
  }
}
static void stepper_focus(Fixture *f, gconstpointer data) {
  (void)data;
  gnostr_approval_dialog_set_position(f->dialog, 1, 3);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(child(f, "stepper"))), ==, "1 of 3");
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_previous")));
  g_assert_true(gtk_widget_get_sensitive(child(f, "btn_next")));
  g_assert_true(adw_dialog_get_focus(ADW_DIALOG(f->dialog)) == child(f, "btn_deny"));
  g_assert_null(adw_dialog_get_default_widget(ADW_DIALOG(f->dialog)));
  g_assert_null(gtk_widget_get_ancestor(child(f, "btn_approve"), GTK_TYPE_SCROLLED_WINDOW));
  g_assert_false(gtk_widget_is_ancestor(child(f, "scope_summary"), child(f, "details_expander")));
  g_assert_null(gtk_widget_get_ancestor(child(f, "scope_summary"), GTK_TYPE_SCROLLED_WINDOW));
  g_assert_true(gtk_label_get_wrap(GTK_LABEL(child(f, "scope_summary"))));
  g_assert_cmpint(gtk_label_get_lines(GTK_LABEL(child(f, "scope_summary"))), ==, -1);
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
    g_variant_builder_add(&row, "{sv}", "identity", g_variant_new_string(i ? "npub-second" : "npub-first"));
    GVariantBuilder summary; g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&summary, "{sv}", "peer", g_variant_new_string(i ? "peer-second" : "peer-first"));
    g_variant_builder_add(&row, "{sv}", "event_summary", g_variant_builder_end(&summary));
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
  guint i = 0;
  for (GtkWidget *check = first; check; check = gtk_widget_get_next_sibling(check), i++) {
    g_autofree gchar *label = g_strdup_printf("sign_event · Text note (kind 1)\nIdentity: %s\nPeer: %s",
        i ? "npub-second" : "npub-first", i ? "peer-second" : "peer-first");
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(check), GTK_ACCESSIBLE_PROPERTY_LABEL, label);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(gtk_check_button_get_child(GTK_CHECK_BUTTON(check)))), ==, label);
  }
  g_assert_cmpuint(i, ==, 2);
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(first)));
  gtk_check_button_set_active(GTK_CHECK_BUTTON(first), TRUE);
  click(f, "btn_approve_selected");
  wait_closed(f);
  g_assert_cmpuint(f->selected, ==, 1);
  g_assert_cmpstr(f->selected_ids[0], ==, "first");
  g_assert_null(f->selected_ids[1]);
  g_assert_cmpuint(f->calls, ==, 0);
}
static void set_room_flow_remaining(Fixture *f, guint remaining) {
  GVariantBuilder b, summary;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&summary, "{sv}", "peer", g_variant_new_string(
      "79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"));
  g_variant_builder_add(&b, "{sv}", "operation", g_variant_new_string("nip44_encrypt"));
  g_variant_builder_add(&b, "{sv}", "identity", g_variant_new_string("npub-pinned"));
  g_variant_builder_add(&b, "{sv}", "identity_label", g_variant_new_string("Alice"));
  g_variant_builder_add(&b, "{sv}", "purpose", g_variant_new_string("nip17-room-send"));
  g_variant_builder_add(&b, "{sv}", "flow_id", g_variant_new_string("room-send"));
  g_variant_builder_add(&b, "{sv}", "flow_size", g_variant_new_uint32(4));
  g_variant_builder_add(&b, "{sv}", "flow_remaining_seconds", g_variant_new_uint32(remaining));
  g_variant_builder_add(&b, "{sv}", "state", g_variant_new_string("PENDING"));
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32(120));
  g_variant_builder_add(&b, "{sv}", "rememberable", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&b, "{sv}", "routine_trust_available", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&b, "{sv}", "event_summary", g_variant_builder_end(&summary));
  g_autoptr(GVariant) info = g_variant_ref_sink(g_variant_builder_end(&b));
  gnostr_approval_dialog_set_info(f->dialog, info, "Room client");
  g_assert_false(gtk_widget_get_visible(child(f, "group_flow")));
  if (remaining) {
    choose(f, "This message only");
    g_assert_true(gtk_widget_get_visible(child(f, "group_flow")));
    g_assert_nonnull(strstr(scope_text(f), "4 encrypted copies"));
    g_assert_nonnull(strstr(scope_text(f), "30 seconds left"));
    g_assert_null(strstr(scope_text(f), "600 seconds"));
    g_assert_nonnull(strstr(scope_text(f), "as Alice"));
    g_assert_null(strstr(scope_text(f), "npub"));
  }
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt"))));
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(child(f, "chk_flow_seals"))));
}
static void set_room_flow(Fixture *f) { set_room_flow_remaining(f, 30); }
static void flow_expired(Fixture *f, gconstpointer data) {
  (void)data; set_room_flow_remaining(f, 0); click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpuint(f->flow_calls, ==, 0); g_assert_cmpuint(f->calls, ==, 1);
}
static void custom_trust_unavailable(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) original = info(65534, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "routine_trust_available", "b", FALSE);
  g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, row, "Custom client");
  GListModel *model = adw_combo_row_get_model(ADW_COMBO_ROW(child(f, "permission_choice")));
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 3);
  choose(f, "Always allow");
  g_assert_nonnull(strstr(scope_text(f), "until revoked"));
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
  gnostr_approval_dialog_set_request(f->dialog, "sign_event", "Legacy app", NULL, FALSE, "Public text");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "header_title"))), ==, "Legacy app");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "label_verified"))), ==, "Unverified app");
  g_autoptr(GVariant) row = info(NOSTR_KIND_TEXT_NOTE, "sign_event", 120);
  gnostr_approval_dialog_set_info(f->dialog, row, "Client");
  choose(f, "Always allow");
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
    g_variant_builder_add(&info, "{sv}", "transport", g_variant_new_string("NIP46"));
    g_variant_builder_add(&info, "{sv}", "verified", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&info, "{sv}", "principal_kind", g_variant_new_string("nip46"));
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
    const gchar *caller = gtk_label_get_text(GTK_LABEL(child(f, "label_verified")));
    g_assert_nonnull(strstr(caller, "Remote NIP-46 client"));
    g_assert_null(strstr(caller, "local app"));
    const gchar *preview = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
    g_assert_nonnull(strstr(preview, "Unverified client"));
    g_assert_nonnull(strstr(preview, "sign_event:1"));
    g_assert_null(strstr(preview, "must-not-render"));
    if (complete) g_assert_nonnull(strstr(preview, urls[0]));
  }
}
static void scope_and_reset(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) original = info(NOSTR_KIND_CLIENT_AUTHENTICATION, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "identity_label", "s", "Alice");
  g_autoptr(GVariant) auth = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, auth, "Jank");
  const gchar *choices[] = {"Allow once", "Allow for 24 hours", "Always allow"};
  const gchar *scopes[] = {"No permission is saved", "24 hours", "until revoked"};
  const gchar *commits[] = {"_Allow Once", "_Allow for 24 Hours", "_Always Allow"};
  for (guint i = 0; i < G_N_ELEMENTS(choices); i++) {
    choose(f, choices[i]);
    const gchar *scope = scope_text(f);
    g_assert_nonnull(strstr(scope, "Alice"));
    g_assert_null(strstr(scope, "npub"));
    g_assert_nonnull(strstr(subtitle(f, "row_identity"), "Alice · npub-pinned-by-daemon"));
    g_assert_nonnull(strstr(scope, "wss://nos.lol"));
    g_assert_nonnull(strstr(scope, scopes[i]));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, commits[i]);
  }
  AdwComboRow *choice = ADW_COMBO_ROW(child(f, "permission_choice"));
  g_autoptr(GtkStringObject) selected = g_list_model_get_item(
      adw_combo_row_get_model(choice), adw_combo_row_get_selected(choice));
  g_assert_cmpstr(gtk_string_object_get_string(selected), ==,
      "Always allow this app to sign in to a relay as Alice at wss://nos.lol");
  gnostr_approval_dialog_set_info(f->dialog, original, "Other app");
  g_assert_cmpuint(adw_combo_row_get_selected(ADW_COMBO_ROW(child(f, "permission_choice"))), ==, 0);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, "_Allow Once");
  g_autoptr(GVariant) crypto = info(0, "nip44_decrypt", 120);
  gnostr_approval_dialog_set_info(f->dialog, crypto, "Jank");
  choose(f, "Always allow");
  g_assert_nonnull(strstr(scope_text(f), "all peers"));
  g_assert_nonnull(strstr(scope_text(f), "decrypt"));
}
static void flow_exclusive(Fixture *f, gconstpointer data) {
  (void)data; set_room_flow(f);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt")), TRUE);
  choose(f, "Always allow");
  g_assert_false(gtk_widget_get_visible(child(f, "group_flow")));
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt"))));
  choose(f, "This message only");
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(child(f, "chk_flow_encrypt"))));
  choose(f, "Allow once");
  click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpuint(f->flow_calls, ==, 0); g_assert_false(f->remember);
}
static void expired_and_empty_claim(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) expired = info(NOSTR_KIND_TEXT_NOTE, "sign_event", 0);
  gnostr_approval_dialog_set_info(f->dialog, expired, "Jank");
  choose(f, "Always allow");
  click(f, "btn_approve");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->dialog), "review.block-operation", NULL));
  g_assert_cmpuint(f->calls, ==, 0); g_assert_null(f->mode); g_assert_false(f->closed);
  g_autoptr(GVariant) original = info(NOSTR_KIND_TEXT_NOTE, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "principal", "s", "claimed:");
  g_autoptr(GVariant) claim = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, claim, "Jank");
  g_assert_cmpuint(g_list_model_get_n_items(adw_combo_row_get_model(ADW_COMBO_ROW(child(f, "permission_choice")))), ==, 1);
}
static guint primary_shortcuts(GtkWidget *widget) {
  guint count = 0;
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(widget);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GObject) object = g_list_model_get_item(controllers, i);
    if (!GTK_IS_SHORTCUT_CONTROLLER(object)) continue;
    GListModel *shortcuts = G_LIST_MODEL(object);
    for (guint j = 0; j < g_list_model_get_n_items(shortcuts); j++) {
      g_autoptr(GtkShortcut) shortcut = g_list_model_get_item(shortcuts, j);
      g_autofree gchar *trigger = gtk_shortcut_trigger_to_string(gtk_shortcut_get_trigger(shortcut));
      if (strstr(trigger, "<Control>a") || strstr(trigger, "<Control>d")) count++;
    }
  }
  return count;
}
static void no_primary_consent(Fixture *f, gconstpointer data) {
  (void)data;
  /* Preserve libadwaita's own bindings (including its debug binding), but add
   * no Ctrl+A / Ctrl+D approval callbacks. Select All remains label-owned. */
  AdwDialog *baseline = adw_dialog_new();
  g_object_ref_sink(baseline);
  g_assert_cmpuint(primary_shortcuts(GTK_WIDGET(f->dialog)), ==, primary_shortcuts(GTK_WIDGET(baseline)));
  g_object_unref(baseline);
  GtkLabel *preview = GTK_LABEL(child(f, "content_preview"));
  gnostr_approval_dialog_set_content(f->dialog, "Select this public text");
  gtk_label_select_region(preview, 0, -1);
  gint start, end;
  g_assert_true(gtk_label_get_selection_bounds(preview, &start, &end));
  g_assert_cmpint(end - start, ==, 23);
  g_assert_cmpuint(f->calls, ==, 0);
}
static const gchar *public_npub = "npub10xlxvlhemja6c4dqv22uapctqupfhlxm9h8z3k2e72q4k9hcz7vqpkge6d";
static void caller_labels(Fixture *f, gconstpointer data) {
  (void)data;
  const struct {
    const gchar *transport, *kind, *principal, *via, *app_id, *operation, *expected;
    gboolean verified;
  } cases[] = {
    {"DBUS", "executable", "exe:/usr/bin/jank", "", "", "sign_event", "Verified local app", TRUE},
    {"NIP5F", "flatpak", "flatpak:org.example.Jank", "", "", "sign_event", "Verified local app", TRUE},
    {"DBUS", "website", "origin:https://example.org", "exe:/usr/bin/browser-bridge", "https://example.org", "sign_event", "Browser origin via bridge · https://example.org", TRUE},
    {"NIP46", "nip46", "nip46:remote-key", "", "", "sign_event", "Remote NIP-46 client · verified transport key; app name unverified", TRUE},
    {"NIP46", "nip46", "nip46:remote-key", "", "", "sign_event", "Remote NIP-46 client · unverified", FALSE},
    {"DBUS", "executable", "nip46:remote-key", "", "", "sign_event", "Remote NIP-46 client · verified transport key; app name unverified", TRUE},
    {"DBUS", "nip46", "remote-key", "exe:/usr/bin/bridge", "https://example.org", "sign_event", "Remote NIP-46 client · verified transport key; app name unverified", TRUE},
    {"MANAGEMENT", "executable", "exe:/usr/bin/grotto", "", "", "self_test", "Grotto identity test", TRUE},
    {"DBUS", "claimed", "claimed:Jank", "", "", "sign_event", "Unverified: claims to be “Bogus name”", FALSE},
    {"", "executable", "exe:/usr/bin/jank", "", "", "sign_event", "Verified caller · transport not identified", TRUE},
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
    g_autoptr(GVariant) original = info(NOSTR_KIND_TEXT_NOTE, cases[i].operation, 120);
    GVariantDict d; g_variant_dict_init(&d, original);
    g_variant_dict_insert(&d, "transport", "s", cases[i].transport);
    g_variant_dict_insert(&d, "principal_kind", "s", cases[i].kind);
    g_variant_dict_insert(&d, "principal", "s", cases[i].principal);
    g_variant_dict_insert(&d, "via", "s", cases[i].via);
    g_variant_dict_insert(&d, "app_id", "s", cases[i].app_id);
    g_variant_dict_insert(&d, "verified", "b", cases[i].verified);
    g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_dict_end(&d));
    gnostr_approval_dialog_set_info(f->dialog, row, "Jank");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "label_verified"))), ==, cases[i].expected);
  }
  gnostr_approval_dialog_set_request(f->dialog, "sign_event", "Legacy caller", NULL, TRUE, "Public text");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "label_verified"))), ==, "Verified caller");
}
static void wait_frames(GtkWidget *widget) {
  guint frames = 0, timeout = g_timeout_add_seconds(10, failure_bound, NULL);
  gtk_widget_add_tick_callback(widget, frame, &frames, NULL);
  while (frames < 2) g_main_context_iteration(NULL, TRUE);
  g_source_remove(timeout);
}
static void assert_decision_visible(Fixture *f) {
  const gchar *names[] = {"scope_summary", "btn_deny", "btn_approve"};
  graphene_rect_t scope_bounds, button_bounds;
  for (guint i = 0; i < G_N_ELEMENTS(names); i++) {
    GtkWidget *widget = child(f, names[i]);
    graphene_rect_t bounds;
    g_assert_null(gtk_widget_get_ancestor(widget, GTK_TYPE_SCROLLED_WINDOW));
    g_assert_true(gtk_widget_get_mapped(widget));
    g_assert_true(gtk_widget_compute_bounds(widget, f->window, &bounds));
    g_test_message("%s: bounds %.1f,%.1f %.1fx%.1f; window content %dx%d, dialog %dx%d", names[i],
        bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height,
        gtk_widget_get_width(f->window), gtk_widget_get_height(f->window),
        gtk_widget_get_width(GTK_WIDGET(f->dialog)), gtk_widget_get_height(GTK_WIDGET(f->dialog)));
    g_assert_cmpfloat(bounds.origin.x, >=, 0);
    g_assert_cmpfloat(bounds.origin.y, >=, 0);
    g_assert_cmpfloat(bounds.origin.x + bounds.size.width, <=, gtk_widget_get_width(f->window));
    g_assert_cmpfloat(bounds.origin.y + bounds.size.height, <=, gtk_widget_get_height(f->window));
  }
  g_assert_true(gtk_widget_compute_bounds(child(f, "scope_summary"), f->window, &scope_bounds));
  g_assert_true(gtk_widget_compute_bounds(child(f, "btn_approve"), f->window, &button_bounds));
  g_assert_cmpfloat(scope_bounds.origin.y + scope_bounds.size.height, <=, button_bounds.origin.y);
}
static void scope_viewport(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) original = info(NOSTR_KIND_CLIENT_AUTHENTICATION, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "identity", "s", public_npub);
  g_variant_dict_insert(&d, "identity_label", "s", "Alice");
  g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, row, "Jank");
  gtk_window_set_default_size(GTK_WINDOW(f->window), 360, 700);
  const gchar *modes[] = {"Allow once", "Allow for 24 hours", "Always allow", "Trust this app"};
  GtkWidget *scroller = gtk_widget_get_ancestor(child(f, "permission_choice"), GTK_TYPE_SCROLLED_WINDOW);
  g_assert_nonnull(scroller);
  GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
  for (guint i = 0; i < G_N_ELEMENTS(modes); i++) {
    choose(f, modes[i]);
    wait_frames(f->window);
    g_assert_cmpint(gtk_widget_get_width(f->window), <=, 360);
    for (guint bottom = 0; bottom < 2; bottom++) {
      gtk_adjustment_set_value(adjustment, bottom
          ? gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment)
          : gtk_adjustment_get_lower(adjustment));
      wait_frames(f->window);
      assert_decision_visible(f);
      if (i < 3) {
        g_assert_null(strstr(scope_text(f), public_npub));
        g_assert_nonnull(strstr(scope_text(f), "Alice"));
        g_assert_nonnull(strstr(scope_text(f), "wss://nos.lol"));
      }
    }
  }
}
static void queue_scene(GtkWidget *widget) {
  gtk_widget_queue_draw(widget);
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    queue_scene(child);
}
/* Check the saved scene contains each actual commit button, not just a mapped
 * widget. Compare its interior colour to a fresh same-theme composite. */
static void assert_button_painted(Fixture *f, GdkTexture *scene, GskRenderer *renderer, const gchar *name) {
  GtkWidget *button = child(f, name);
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(button);
  int w = gdk_paintable_get_intrinsic_width(paintable), h = gdk_paintable_get_intrinsic_height(paintable);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), w, h);
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  gsize scene_stride = gdk_texture_get_width(scene) * 4, reference_stride = w * 4;
  g_autofree guchar *actual = g_malloc0(scene_stride * gdk_texture_get_height(scene));
  g_autofree guchar *expected = g_malloc0(reference_stride * h);
  gdk_texture_download(scene, actual, scene_stride);
  graphene_rect_t bounds, bar_bounds;
  g_assert_true(gtk_widget_compute_bounds(button, GTK_WIDGET(f->dialog), &bounds));
  g_assert_true(gtk_widget_compute_bounds(gtk_widget_get_parent(button), GTK_WIDGET(f->dialog), &bar_bounds));
  guint x = w / 2, y = h - 8;
  guint sx = (guint)(bounds.origin.x + 0.5f) + x, sy = (guint)(bounds.origin.y + 0.5f) + y;
  guint gap_x = (guint)(bar_bounds.origin.x + bar_bounds.size.width / 2 + 0.5f);
  g_assert_cmpuint(sx, <, gdk_texture_get_width(scene));
  g_assert_cmpuint(gap_x, <, gdk_texture_get_width(scene));
  g_assert_cmpuint(sy, <, gdk_texture_get_height(scene));
  /* Normal buttons use a translucent fill. Composite the reference over the
   * actual empty gap between the two buttons, using the same GSK renderer. */
  g_autoptr(GBytes) pixel = g_bytes_new(actual + sy * scene_stride + gap_x * 4, 4);
  g_autoptr(GdkTexture) background = gdk_memory_texture_new(1, 1, GDK_MEMORY_DEFAULT, pixel, 4);
  g_autoptr(GskRenderNode) fill = gsk_texture_node_new(background, &GRAPHENE_RECT_INIT(0, 0, w, h));
  GskRenderNode *layers[] = {fill, node};
  g_autoptr(GskRenderNode) composite = gsk_container_node_new(layers, G_N_ELEMENTS(layers));
  g_autoptr(GdkTexture) reference = gsk_renderer_render_texture(renderer, composite, &GRAPHENE_RECT_INIT(0, 0, w, h));
  gdk_texture_download(reference, expected, reference_stride);
  guchar *a = actual + sy * scene_stride + sx * 4, *b = expected + y * reference_stride + x * 4;
  g_test_message("%s sample (%u,%u) actual %u,%u,%u,%u reference %u,%u,%u,%u", name, sx, sy,
      a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
  g_assert_cmpmem(a, 4, b, 4);
}
static void snapshots(Fixture *f, gconstpointer data) {
  (void)data;
  const gchar *directory = g_getenv("GROTTO_APPROVAL_SCREENSHOT_DIR");
  g_autoptr(GVariant) request = info(NOSTR_KIND_FOLLOW_LIST, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, request);
  g_variant_dict_insert(&d, "identity_label", "s", "Alice");
  g_variant_dict_insert(&d, "identity", "s", public_npub);
  g_autoptr(GVariant) display = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, display, "Jank");
  choose(f, "Always allow");
  gtk_window_set_decorated(GTK_WINDOW(f->window), FALSE);
  for (guint dark = 0; dark < 2; dark++) {
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
        dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    for (guint narrow = 0; narrow < 2; narrow++) {
      guint width = narrow ? 360 : 920;
      gtk_window_set_default_size(GTK_WINDOW(f->window), width, 640);
      wait_frames(f->window);
      gint minimum;
      gtk_widget_measure(GTK_WIDGET(f->dialog), GTK_ORIENTATION_HORIZONTAL, -1, &minimum, NULL, NULL, NULL);
      g_assert_cmpint(minimum, <=, 360);
      for (guint scrolled = 0; scrolled < 2; scrolled++) {
        GtkWidget *scroller = gtk_widget_get_ancestor(child(f, "permission_choice"), GTK_TYPE_SCROLLED_WINDOW);
        g_assert_nonnull(scroller);
        GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
        gtk_adjustment_set_value(adjustment, scrolled
            ? gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_page_size(adjustment)
            : gtk_adjustment_get_lower(adjustment));
        /* Invalidate the complete scene, not only the scrolled dirty region. */
        queue_scene(f->window);
        wait_frames(f->window);
        assert_decision_visible(f);
        /* Capture the dialog itself: window paintables include CSS shadows
         * and scaling their content size distorts the sample coordinates. */
        GtkWidget *widget = GTK_WIDGET(f->dialog);
        g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(widget);
        int w = gdk_paintable_get_intrinsic_width(paintable), h = gdk_paintable_get_intrinsic_height(paintable);
        GtkSnapshot *snapshot = gtk_snapshot_new();
        gdk_paintable_snapshot(paintable, GDK_SNAPSHOT(snapshot), w, h);
        g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
        g_assert_nonnull(node);
        GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(f->window));
        g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, w, h));
        assert_button_painted(f, texture, renderer, "btn_approve");
        assert_button_painted(f, texture, renderer, "btn_deny");
        if (directory) {
          g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
          g_autofree gchar *path = g_strdup_printf("%s/approval-%u-%s%s.png", directory, width, dark ? "dark" : "light", scrolled ? "-scope" : "");
          g_assert_true(gdk_texture_save_to_png(texture, path));
          g_autofree gchar *bytes = NULL;
          gsize length;
          g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
          g_autofree gchar *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, length);
          g_test_message("saved %s (%dx%d) sha256=%s", path, w, h, hash);
        }
      }
    }
  }
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_DEFAULT);
}
/* G18: an app's declared permission set is one review; the default view is
 * compact (app, one line, identity, decision) and fits 360 px unscrolled. */
static GVariant *declared_info(guint remaining, gboolean rememberable, gboolean mixed) {
  static const guint32 kinds[] = {1, 5, 6, 7, 9, 13, 16, 450, 1621, 9000, 9001, 9002, 9005, 9007, 9009,
                                  9021, 9022, 10002, 10009, 10050, 22242, 24242, 27001, 30443};
  GVariantBuilder b, summary, declared;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&declared, G_VARIANT_TYPE("aa{sv}"));
  const gchar *types[] = {"nip04_decrypt", "nip44_encrypt", "nip44_decrypt"};
  for (guint i = 0; i < G_N_ELEMENTS(kinds) + G_N_ELEMENTS(types); i++) {
    GVariantBuilder entry; g_variant_builder_init(&entry, G_VARIANT_TYPE_VARDICT);
    if (i < G_N_ELEMENTS(kinds)) {
      g_variant_builder_add(&entry, "{sv}", "type", g_variant_new_string("sign_event"));
      g_variant_builder_add(&entry, "{sv}", "kind", g_variant_new_uint32(kinds[i]));
    } else g_variant_builder_add(&entry, "{sv}", "type", g_variant_new_string(types[i - G_N_ELEMENTS(kinds)]));
    g_variant_builder_add(&entry, "{sv}", "state", g_variant_new_string(
        mixed && i == 0 ? "granted" : mixed && i == 1 ? "denied" : "requested"));
    g_variant_builder_add_value(&declared, g_variant_builder_end(&entry));
  }
#define S(key, value) g_variant_builder_add(&b, "{sv}", key, g_variant_new_string(value))
  S("identity", public_npub); S("identity_label", "Alice"); S("principal", "exe:/usr/bin/groundhog");
  S("transport", "DBUS"); S("principal_kind", "executable"); S("operation", "permissions"); S("kind", "permissions");
  S("state", "PENDING"); S("reason", "Groundhog first contact"); S("reason_source", "caller"); S("claimed_app_id", "org.nostr.Groundhog");
#undef S
  g_variant_builder_add(&b, "{sv}", "remaining_seconds", g_variant_new_uint32(remaining));
  g_variant_builder_add(&b, "{sv}", "verified", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&b, "{sv}", "rememberable", g_variant_new_boolean(rememberable));
  g_variant_builder_add(&b, "{sv}", "routine_trust_available", g_variant_new_boolean(rememberable));
  g_variant_builder_add(&summary, "{sv}", "preview", g_variant_new_string("use this identity for its declared operations"));
  g_variant_builder_add(&summary, "{sv}", "declared_permissions", g_variant_builder_end(&declared));
  g_variant_builder_add(&b, "{sv}", "event_summary", g_variant_builder_end(&summary));
  return g_variant_ref_sink(g_variant_builder_end(&b));
}
static GtkAdjustment *page_adjustment(Fixture *f) {
  GtkWidget *scroller = gtk_widget_get_ancestor(child(f, "permission_choice"), GTK_TYPE_SCROLLED_WINDOW);
  g_assert_nonnull(scroller);
  return gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
}
static void assert_fits_unscrolled(Fixture *f) {
  gtk_window_set_default_size(GTK_WINDOW(f->window), 360, 640);
  wait_frames(f->window);
  g_assert_cmpint(gtk_widget_get_width(f->window), <=, 360);
  GtkAdjustment *adjustment = page_adjustment(f);
  gint minimum = 0, natural = 0;
  gtk_widget_measure(GTK_WIDGET(f->dialog), GTK_ORIENTATION_VERTICAL, 360, &minimum, &natural, NULL, NULL);
  graphene_rect_t footer, title;
  g_assert_true(gtk_widget_compute_bounds(child(f, "btn_approve"), f->window, &footer));
  g_assert_true(gtk_widget_compute_bounds(child(f, "header_title"), f->window, &title));
  g_test_message("page %.0f of %.0f content; sheet natural height %d in a %dx%d window; title y=%.0f approve bottom y=%.0f",
      gtk_adjustment_get_page_size(adjustment), gtk_adjustment_get_upper(adjustment), natural,
      gtk_widget_get_width(f->window), gtk_widget_get_height(f->window), title.origin.y, footer.origin.y + footer.size.height);
  /* The sheet is sized naturally and fits the window; the scroller reports its
   * viewport 2 px shorter than the content it propagates (focus padding).
   * Anything beyond that would be real scrolling. */
  g_assert_cmpint(natural, <=, gtk_widget_get_height(f->window));
  g_assert_cmpfloat(gtk_adjustment_get_upper(adjustment), <=, gtk_adjustment_get_page_size(adjustment) + 2.5);
  g_assert_false(adw_expander_row_get_expanded(ADW_EXPANDER_ROW(child(f, "details_expander"))));
  const gchar *visible[] = {"header_title", "row_action", "row_identity", "btn_deny", "btn_approve"};
  for (guint i = 0; i < G_N_ELEMENTS(visible); i++) g_assert_true(gtk_widget_get_mapped(child(f, visible[i])));
  assert_decision_visible(f);
}
static void declared_set(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) row = declared_info(120, TRUE, FALSE);
  g_assert_true(gnostr_approval_info_can_approve(row));
  gnostr_approval_dialog_set_info(f->dialog, row, "Groundhog");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "header_title"))), ==, "Groundhog wants to use your identity");
  const gchar *action = subtitle(f, "row_action");
  g_assert_nonnull(strstr(action, "NIP-29 groups"));
  g_assert_nonnull(strstr(action, "encrypted messages"));
  g_assert_nonnull(strstr(action, "sign-in to any relay it chooses")); /* any-relay AUTH is disclosed (F4) */
  g_assert_nonnull(strstr(action, "relay lists"));
  g_assert_nonnull(strstr(action, "deletions"));
  g_assert_nonnull(strstr(action, "custom kinds (450, 27001, 30443)"));
  g_assert_null(strstr(action, "9021")); /* kind numbers live under Details */
  const gchar *details = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
  g_assert_nonnull(strstr(details, "Simple group join request (kind 9021)"));
  g_assert_nonnull(strstr(details, "Encrypted message (kind 13)"));
  g_assert_nonnull(strstr(details, "Relay authentication (kind 22242) — any relay this app chooses"));
  g_assert_nonnull(strstr(details, "Decrypt messages (NIP-44)"));
  g_assert_nonnull(strstr(details, "Custom event (kind 27001)"));
  AdwComboRow *choice = ADW_COMBO_ROW(child(f, "permission_choice"));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(choice)));
  g_assert_cmpuint(g_list_model_get_n_items(adw_combo_row_get_model(choice)), ==, 2);
  g_assert_cmpuint(adw_combo_row_get_selected(choice), ==, 0);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, "_Allow");
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_deny"))), ==, "_Deny");
  g_assert_nonnull(strstr(scope_text(f), "Until you revoke"));
  g_assert_nonnull(strstr(subtitle(f, "row_identity"), "Alice"));
  /* No single operation to block in a declared set: the action stays inert. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->dialog), "review.block-operation", NULL));
  g_assert_null(f->mode); g_assert_false(f->closed);
  assert_fits_unscrolled(f);
  /* Mixed standing is reported, never re-asked. */
  g_autoptr(GVariant) mixed = declared_info(120, TRUE, TRUE);
  gnostr_approval_dialog_set_info(f->dialog, mixed, "Groundhog");
  details = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
  g_assert_nonnull(strstr(details, "already allowed"));
  g_assert_nonnull(strstr(details, "blocked"));
  g_assert_nonnull(strstr(subtitle(f, "row_impact"), "1 already allowed, 1 blocked"));
  /* An app that cannot hold reusable consent cannot be granted a set. */
  g_autoptr(GVariant) claimed = declared_info(120, FALSE, FALSE);
  gnostr_approval_dialog_set_info(f->dialog, claimed, "Groundhog");
  g_assert_false(gtk_widget_get_sensitive(child(f, "btn_approve")));
  gnostr_approval_dialog_set_info(f->dialog, row, "Groundhog");
  click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpuint(f->calls, ==, 1); g_assert_true(f->decision); g_assert_true(f->remember); g_assert_cmpuint(f->ttl, ==, 0);
  g_assert_cmpstr(f->identity, ==, public_npub);
}
/* F1: a pairing review names each requested perm's standing; an explicit
 * block is shown as kept, and any-relay AUTH is disclosed. */
static void pairing_declared(Fixture *f, gconstpointer data) {
  (void)data;
  GVariantBuilder info, summary, declared;
  g_variant_builder_init(&info, G_VARIANT_TYPE_VARDICT); g_variant_builder_init(&summary, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_init(&declared, G_VARIANT_TYPE("aa{sv}"));
  g_variant_builder_add(&info, "{sv}", "operation", g_variant_new_string("connect"));
  g_variant_builder_add(&info, "{sv}", "transport", g_variant_new_string("NIP46"));
  g_variant_builder_add(&info, "{sv}", "verified", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&info, "{sv}", "principal_kind", g_variant_new_string("nip46"));
  g_variant_builder_add(&info, "{sv}", "principal", g_variant_new_string("nip46:79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798"));
  g_variant_builder_add(&info, "{sv}", "identity", g_variant_new_string(public_npub));
  g_variant_builder_add(&info, "{sv}", "claimed_app_id", g_variant_new_string("Remote client"));
  g_variant_builder_add(&info, "{sv}", "state", g_variant_new_string("PENDING"));
  g_variant_builder_add(&info, "{sv}", "remaining_seconds", g_variant_new_uint32(120));
  const gchar *urls[] = {"wss://relay.example.test/", NULL};
  const gchar *hints[] = {"sign_event:1", "sign_event:13", "sign_event:22242", NULL};
  const struct { const gchar *type; guint32 kind; const gchar *state; } entries[] = {
    {"sign_event", 1, "requested"}, {"sign_event", 13, "denied"}, {"sign_event", 22242, "requested"}};
  for (guint i = 0; i < G_N_ELEMENTS(entries); i++) {
    GVariantBuilder entry; g_variant_builder_init(&entry, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&entry, "{sv}", "type", g_variant_new_string(entries[i].type));
    g_variant_builder_add(&entry, "{sv}", "kind", g_variant_new_uint32(entries[i].kind));
    g_variant_builder_add(&entry, "{sv}", "state", g_variant_new_string(entries[i].state));
    if (entries[i].kind == 22242) g_variant_builder_add(&entry, "{sv}", "relay", g_variant_new_string("*"));
    g_variant_builder_add_value(&declared, g_variant_builder_end(&entry));
  }
  g_variant_builder_add(&summary, "{sv}", "relays", g_variant_new_strv(urls, -1));
  g_variant_builder_add(&summary, "{sv}", "requested_permissions_hint", g_variant_new_strv(hints, -1));
  g_variant_builder_add(&summary, "{sv}", "granted_on_pairing", g_variant_new_boolean(TRUE));
  g_variant_builder_add(&summary, "{sv}", "declared_permissions", g_variant_builder_end(&declared));
  g_variant_builder_add(&info, "{sv}", "event_summary", g_variant_builder_end(&summary));
  g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_builder_end(&info));
  g_assert_true(gnostr_approval_info_can_approve(row));
  gnostr_approval_dialog_set_info(f->dialog, row, "Remote application");
  const gchar *preview = gtk_label_get_text(GTK_LABEL(child(f, "content_preview")));
  g_assert_nonnull(strstr(preview, "granted on pairing"));
  g_assert_nonnull(strstr(preview, "Encrypted message (kind 13) — blocked; stays blocked"));
  g_assert_nonnull(strstr(preview, "Relay authentication (kind 22242) — any relay this app chooses"));
  g_assert_nonnull(strstr(preview, "Text note (kind 1)"));
  g_assert_nonnull(strstr(subtitle(f, "row_impact"), "blocked ones stay blocked"));
  g_assert_nonnull(strstr(scope_text(f), "grants the permissions it requested"));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, "Pair _Application");
}
static void declared_set_trust(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) row = declared_info(120, TRUE, FALSE);
  gnostr_approval_dialog_set_info(f->dialog, row, "Groundhog");
  choose(f, "Trust this app");
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, "Trust _App");
  g_assert_nonnull(strstr(scope_text(f), "including custom kinds"));
  click(f, "btn_approve"); wait_closed(f);
  g_assert_cmpstr(f->mode, ==, "trust-routine"); g_assert_cmpuint(f->calls, ==, 0);
}
static void compact_default(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) original = info(NOSTR_KIND_FOLLOW_LIST, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "identity", "s", public_npub);
  g_variant_dict_insert(&d, "identity_label", "s", "Alice");
  g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, row, "Jank");
  /* Everything secondary sits under the single Details expander. */
  const gchar *secondary[] = {"row_impact", "row_reason", "row_expiry", "content_preview", "label_details", "label_timestamp"};
  GtkWidget *expander = child(f, "details_expander");
  for (guint i = 0; i < G_N_ELEMENTS(secondary); i++)
    g_assert_true(gtk_widget_is_ancestor(child(f, secondary[i]), expander));
  g_assert_false(gtk_widget_is_ancestor(child(f, "row_action"), expander));
  g_assert_false(gtk_widget_is_ancestor(child(f, "row_identity"), expander));
  g_assert_false(gtk_widget_is_ancestor(child(f, "permission_choice"), expander));
  assert_fits_unscrolled(f);
  /* Details still carry the full record when opened. */
  adw_expander_row_set_expanded(ADW_EXPANDER_ROW(expander), TRUE);
  wait_frames(f->window);
  g_assert_nonnull(strstr(subtitle(f, "row_reason"), "Send my note"));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(child(f, "label_details"))), "org.example.Client"));
  assert_decision_visible(f);
  /* A fresh snapshot collapses Details again. */
  gnostr_approval_dialog_set_info(f->dialog, row, "Jank");
  g_assert_false(adw_expander_row_get_expanded(ADW_EXPANDER_ROW(expander)));
  /* A single-choice request shows no permission dropdown at all. */
  GVariantDict e; g_variant_dict_init(&e, row);
  g_variant_dict_insert(&e, "rememberable", "b", FALSE);
  g_autoptr(GVariant) once_only = g_variant_ref_sink(g_variant_dict_end(&e));
  gnostr_approval_dialog_set_info(f->dialog, once_only, "Jank");
  g_assert_false(gtk_widget_get_visible(child(f, "permission_choice")));
  assert_fits_unscrolled(f);
}
static void compact_auth_layout(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) original = info(NOSTR_KIND_CLIENT_AUTHENTICATION, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "identity", "s", public_npub);
  g_variant_dict_insert(&d, "identity_label", "s", "UTM");
  g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, row, "Example App");
  const guint widths[] = {920, 360};
  const gchar *modes[] = {"Allow once", "Allow for 24 hours", "Always allow"};
  const gchar *rows[] = {"row_action", "row_identity", "row_relay", "permission_choice", "details_expander"};
  GtkWidget *scroller = gtk_widget_get_ancestor(child(f, "details_expander"), GTK_TYPE_SCROLLED_WINDOW);
  g_assert_nonnull(scroller);
  for (guint dark = 0; dark < 2; dark++) {
    adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
        dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);
    for (guint w = 0; w < G_N_ELEMENTS(widths); w++) {
      gtk_window_set_default_size(GTK_WINDOW(f->window), widths[w], 640);
      for (guint m = 0; m < G_N_ELEMENTS(modes); m++) {
        choose(f, modes[m]);
        gtk_adjustment_set_value(page_adjustment(f), 0);
        wait_frames(f->window);
        g_assert_cmpint(gtk_widget_get_allocated_width(f->window), ==, widths[w]);
        g_assert_cmpint(gtk_widget_get_allocated_height(f->window), ==, 640);
        g_assert_cmpint(adw_dialog_get_content_height(ADW_DIALOG(f->dialog)), ==, 560);
        g_assert_false(adw_expander_row_get_expanded(ADW_EXPANDER_ROW(child(f, "details_expander"))));
        graphene_rect_t footer;
        g_assert_true(gtk_widget_compute_bounds(child(f, "scope_summary"), GTK_WIDGET(f->dialog), &footer));
        for (guint r = 0; r < G_N_ELEMENTS(rows); r++) {
          GtkWidget *widget = child(f, rows[r]);
          graphene_rect_t viewport_bounds, dialog_bounds;
          g_assert_true(gtk_widget_get_mapped(widget));
          g_assert_true(gtk_widget_compute_bounds(widget, scroller, &viewport_bounds));
          g_assert_true(gtk_widget_compute_bounds(widget, GTK_WIDGET(f->dialog), &dialog_bounds));
          g_test_message("%ux640 %s %s: bottom %.0f, viewport %d", widths[w], modes[m], rows[r],
              viewport_bounds.origin.y + viewport_bounds.size.height, gtk_widget_get_height(scroller));
          g_assert_cmpfloat(viewport_bounds.origin.y, >=, 0);
          g_assert_cmpfloat(viewport_bounds.origin.y + viewport_bounds.size.height, <=, gtk_widget_get_height(scroller));
          g_assert_cmpfloat(dialog_bounds.origin.y + dialog_bounds.size.height, <=, footer.origin.y);
          g_assert_cmpfloat(dialog_bounds.origin.y + dialog_bounds.size.height, <=, gtk_widget_get_height(GTK_WIDGET(f->dialog)));
        }
        assert_decision_visible(f);
      }
    }
  }
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_DEFAULT);
}
static void extend_declared(Fixture *f, gconstpointer data) {
  (void)data;
  g_autoptr(GVariant) original = info(NOSTR_KIND_COMMENT, "sign_event", 120);
  GVariantDict d; g_variant_dict_init(&d, original);
  g_variant_dict_insert(&d, "declared_set", "b", TRUE);
  g_autoptr(GVariant) row = g_variant_ref_sink(g_variant_dict_end(&d));
  gnostr_approval_dialog_set_info(f->dialog, row, "Groundhog");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(child(f, "header_title"))), ==, "Groundhog wants an additional permission");
  AdwComboRow *choice = ADW_COMBO_ROW(child(f, "permission_choice"));
  g_autoptr(GtkStringObject) selected = g_list_model_get_item(adw_combo_row_get_model(choice), adw_combo_row_get_selected(choice));
  g_assert_true(g_str_has_prefix(gtk_string_object_get_string(selected), "Always allow"));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(child(f, "btn_approve"))), ==, "_Always Allow");
  g_assert_nonnull(strstr(scope_text(f), "until revoked"));
  /* Allow once stays available for a one-off. */
  choose(f, "Allow once");
  click(f, "btn_approve"); wait_closed(f);
  g_assert_true(f->decision); g_assert_false(f->remember);
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
  ADD("until-revoked-default", until_revoked);
  ADD("deny-with-reusable-allow", deny_with_reusable_allow);
  ADD("block-app-confirmation", block_app_confirmation);
  const gchar *modes[] = {"trust-routine", "block-operation", "activate-saved", "discard-saved"};
  for (guint i = 0; i < G_N_ELEMENTS(modes); i++) {
    g_autofree gchar *path = g_strconcat("/signer/approval-dialog/", modes[i], NULL);
    g_test_add(path, Fixture, modes[i], setup, explicit_mode, teardown);
  }
  ADD("unicode", unicode);
  ADD("completion-cancels", completion_cancels);
  ADD("previews", previews);
  ADD("stepper-focus", stepper_focus);
  ADD("batch-selection", batch_selection);
  ADD("flow-expired", flow_expired);
  ADD("custom-trust-unavailable", custom_trust_unavailable);
  ADD("flow-unchecked", flow_unchecked);
  ADD("flow-encrypt-only", flow_encrypt_only);
  ADD("flow-seals-only", flow_seals_only);
  ADD("flow-deny", flow_deny);
  ADD("legacy-no-session", legacy_no_session);
  ADD("pairing-preview", pairing_preview);
  ADD("scope-and-reset", scope_and_reset);
  ADD("flow-exclusive", flow_exclusive);
  ADD("expired-and-empty-claim", expired_and_empty_claim);
  ADD("no-primary-consent", no_primary_consent);
  ADD("caller-labels", caller_labels);
  ADD("scope-viewport", scope_viewport);
  ADD("snapshots", snapshots);
  ADD("declared-set", declared_set);
  ADD("declared-set-trust", declared_set_trust);
  ADD("pairing-declared", pairing_declared);
  ADD("compact-default", compact_default);
  ADD("compact-auth-layout", compact_auth_layout);
  ADD("extend-declared", extend_declared);
#undef ADD
  return g_test_run();
}
