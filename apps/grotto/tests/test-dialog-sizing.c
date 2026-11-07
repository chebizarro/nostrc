/* Executed sizing sweep. The private bus, identities, profile relay, schemas,
 * animation settings and real application lifecycle come from test-ui-layout.c.
 * Add a surface with one row in surfaces[]; openers use production entrypoints.
 * This is geometry coverage, not a second onboarding/footer or policy test. */
#include "ui/signer-window.h"
#include "test-ui-layout-harness.h"
#include "ui/onboarding-assistant.h"
#include "ui/page-settings.h"
#include "ui/page-identity-detail.h"
#include "ui/pairing-flow.h"
#include "ui/approval_dialog.h"
#include "ui/sheets/sheet-qr-display.h"
#include "ui/sheets/sheet-qr-scanner.h"
#include "accounts_store.h"
#include "backup-recovery.h"
#include "settings_manager.h"
#include "i18n.h"
#include <grotto-panels.h>
#include <nostr-kinds.h>
#include <string.h>

#define P_(s) g_dgettext("grotto-panels", s)
static gboolean sizing_deadline(gpointer data) {
  g_error("Dialog sizing barrier: %s", (const char *)data);
  return G_SOURCE_REMOVE;
}
#define SIZING_WAIT(condition) do { \
  guint timer = g_timeout_add_seconds(15, sizing_deadline, #condition); \
  while (!(condition)) g_main_context_iteration(NULL, TRUE); \
  g_source_remove(timer); \
} while (0)
static gboolean sizing_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data) {
  (void)widget; (void)clock; (*(guint *)data)++; return G_SOURCE_CONTINUE;
}
static void sizing_frames(GtkWidget *widget) {
  guint frames = 0, tick = gtk_widget_add_tick_callback(widget, sizing_tick, &frames, NULL);
  SIZING_WAIT(frames >= 4);
  gtk_widget_remove_tick_callback(widget, tick);
}
static void sizing_present_window(GtkWindow *window, int width) {
  gtk_window_set_default_size(window, width, 640); gtk_window_present(window);
  SIZING_WAIT(gtk_widget_get_mapped(GTK_WIDGET(window))); sizing_frames(GTK_WIDGET(window));
  /* X11 client-side shadows subtract decoration extents from default-size.
   * Calibrate once so the tested host allocation really is 920/360 by 640. */
  gtk_window_set_default_size(window, width + width - gtk_widget_get_width(GTK_WIDGET(window)),
    640 + 640 - gtk_widget_get_height(GTK_WIDGET(window)));
  sizing_frames(GTK_WIDGET(window));
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(window)), ==, width);
  g_assert_cmpint(gtk_widget_get_height(GTK_WIDGET(window)), ==, 640);
}
static GtkWidget *sizing_find(GtkWidget *widget, GType type, const char *name) {
  if (g_type_is_a(G_OBJECT_TYPE(widget), type)) {
    const char *label = GTK_IS_BUTTON(widget) ? gtk_button_get_label(GTK_BUTTON(widget)) :
      ADW_IS_PREFERENCES_ROW(widget) ? adw_preferences_row_get_title(ADW_PREFERENCES_ROW(widget)) : gtk_widget_get_name(widget);
    if (!name || !g_strcmp0(label, name)) return widget;
  }
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = sizing_find(child, type, name); if (found) return found;
  }
  return NULL;
}
static AdwDialog *sizing_dialog(SignerWindow *window) {
  return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(window));
}
static GtkWidget *sizing_panel(SignerWindow *window, const char *name) {
  signer_window_show_page(window, name);
  sizing_frames(GTK_WIDGET(window));
  AdwNavigationSplitView *split = ADW_NAVIGATION_SPLIT_VIEW(gtk_widget_get_template_child(GTK_WIDGET(window), TYPE_SIGNER_WINDOW, "split_view"));
  return gtk_stack_get_visible_child(GTK_STACK(adw_navigation_page_get_child(adw_navigation_split_view_get_content(split))));
}
static GtkWidget *sizing_backup(SignerWindow *window) {
  GtkWidget *panel = sizing_panel(window, "identity");
  g_assert_true(GROTTO_IS_IDENTITY_PANEL(panel));
  AdwNavigationView *navigation = grotto_identity_panel_get_navigation(GROTTO_IDENTITY_PANEL(panel));
  if (!GROTTO_IS_IDENTITY_DETAIL(adw_navigation_view_get_visible_page(navigation)))
    signer_window_show_page(window, "backup");
  SIZING_WAIT(GROTTO_IS_IDENTITY_DETAIL(adw_navigation_view_get_visible_page(navigation)));
  return GTK_WIDGET(adw_navigation_view_get_visible_page(navigation));
}
static void sizing_activate_row(GtkWidget *root, const char *name) {
  GtkWidget *row = sizing_find(root, ADW_TYPE_PREFERENCES_ROW, name);
  g_assert_nonnull(row); SIZING_WAIT(gtk_widget_is_sensitive(row));
  g_signal_emit_by_name(row, "activated");
}
static GtkWidget *open_qr(SignerWindow *window, const char *npub) {
  SheetQrDisplay *sheet = sheet_qr_display_new(); sheet_qr_display_set_npub(sheet, npub);
  adw_dialog_present(ADW_DIALOG(sheet), GTK_WIDGET(window)); return GTK_WIDGET(sheet);
}
static GtkWidget *open_scanner(SignerWindow *window, const char *npub) {
  (void)npub; SheetQrScanner *sheet = sheet_qr_scanner_new();
  adw_dialog_present(ADW_DIALOG(sheet), GTK_WIDGET(window)); return GTK_WIDGET(sheet);
}
static GtkWidget *open_scanner_result(SignerWindow *window, const char *npub) {
  GtkWidget *sheet = open_scanner(window, npub);
  GtkWidget *page = GTK_WIDGET(gtk_widget_get_template_child(sheet, TYPE_SHEET_QR_SCANNER, "scanner_page"));
  const GnQrContentType types[] = {GN_QR_TYPE_NPUB, GN_QR_TYPE_UNKNOWN};
  sheet_qr_scanner_set_accepted_types(SHEET_QR_SCANNER(sheet), types);
  g_autofree char *uri = g_strconcat("nostr:", npub, NULL);
  GnQrScanResult result = {.data = uri, .type = GN_QR_TYPE_NPUB};
  g_signal_emit_by_name(gtk_widget_get_first_child(page), "qr-detected", &result);
  GtkStack *stack = GTK_STACK(gtk_widget_get_template_child(sheet, TYPE_SHEET_QR_SCANNER, "stack"));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "result");
  return sheet;
}
static GtkWidget *open_connection(SignerWindow *window, const char *npub) {
  (void)npub; grotto_test_connection_listening(FALSE);
  g_action_group_activate_action(G_ACTION_GROUP(window), "show-connection-link", NULL);
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_connection_ready(SignerWindow *window, const char *npub) {
  GtkWidget *sheet = open_connection(window, npub);
  GtkWidget *warning = GTK_WIDGET(gtk_widget_get_template_child(sheet, TYPE_SHEET_QR_DISPLAY, "warning_box"));
  SIZING_WAIT(gtk_widget_get_mapped(warning));
  sizing_frames(GTK_WIDGET(window));
  grotto_test_connection_listening(TRUE);
  GtkWidget *picture = GTK_WIDGET(gtk_widget_get_template_child(sheet, TYPE_SHEET_QR_DISPLAY, "qr_picture"));
  SIZING_WAIT(gtk_widget_get_mapped(picture));
  return sheet;
}
static GtkWidget *open_connection_unavailable(SignerWindow *window, const char *npub) {
  GtkWidget *sheet = open_connection(window, npub);
  /* Same production sheet, including its empty/loading/error state. No fixture
   * widget or size override: clear the QR through the public data setter. */
  sheet_qr_display_set_data(SHEET_QR_DISPLAY(sheet), NULL, GN_QR_TYPE_BUNKER_URI, _("Connection Link"));
  return sheet;
}
static GtkWidget *open_passphrase(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_backup(window);
  GtkWidget *row = GTK_WIDGET(gtk_widget_get_template_child(detail, GROTTO_TYPE_IDENTITY_DETAIL, "qr_row"));
  SIZING_WAIT(gtk_widget_is_sensitive(row)); g_signal_emit_by_name(row, "activated");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_file_passphrase(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_backup(window);
  GtkWidget *row = GTK_WIDGET(gtk_widget_get_template_child(detail, GROTTO_TYPE_IDENTITY_DETAIL, "save_row"));
  SIZING_WAIT(gtk_widget_is_sensitive(row)); g_signal_emit_by_name(row, "activated");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_backup_shell(SignerWindow *window, const char *npub) {
  /* This shell is the assistant's pre-management route; a SignerWindow instead
   * routes the same public entrypoint to its pinned Identity navigation page. */
  OnboardingAssistant *assistant = onboarding_assistant_new();
  gtk_window_set_transient_for(GTK_WINDOW(assistant), GTK_WINDOW(window));
  sizing_present_window(GTK_WINDOW(assistant), gtk_widget_get_width(GTK_WIDGET(window)));
  page_identity_detail_present_identity(GTK_WIDGET(assistant), npub);
  AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(assistant)); g_assert_nonnull(dialog);
  GtkWidget *detail = sizing_find(GTK_WIDGET(dialog), GROTTO_TYPE_IDENTITY_DETAIL, NULL); g_assert_nonnull(detail);
  GtkWidget *phrase = GTK_WIDGET(gtk_widget_get_template_child(detail, GROTTO_TYPE_IDENTITY_DETAIL, "phrase_row"));
  SIZING_WAIT(gtk_widget_is_sensitive(phrase));
  g_object_set_data_full(G_OBJECT(dialog), "sizing-auxiliary-host", g_object_ref(assistant), g_object_unref);
  return GTK_WIDGET(dialog);
}
static GtkWidget *open_raw_alert(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_backup(window);
  sizing_activate_row(detail, _("Copy Secret Key…"));
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_replace_alert(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_backup(window);
  sizing_activate_row(detail, _("Create Replacement Identity…"));
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_recovery(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_backup(window);
  GtkWidget *row = GTK_WIDGET(gtk_widget_get_template_child(detail, GROTTO_TYPE_IDENTITY_DETAIL, "phrase_row"));
  SIZING_WAIT(gtk_widget_is_sensitive(row)); g_signal_emit_by_name(row, "activated");
  SIZING_WAIT(sizing_dialog(window));
  GtkWidget *words = sizing_find(GTK_WIDGET(sizing_dialog(window)), GTK_TYPE_BOX, "backup-words");
  g_assert_nonnull(words); SIZING_WAIT(gtk_widget_get_first_child(words));
  return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_copy_phrase(SignerWindow *window, const char *npub) {
  GtkWidget *recovery = open_recovery(window, npub);
  GtkWidget *copy = sizing_find(recovery, ADW_TYPE_ACTION_ROW, _("Cop_y Recovery Phrase…"));
  g_assert_nonnull(copy); g_signal_emit_by_name(copy, "activated");
  SIZING_WAIT(GTK_WIDGET(sizing_dialog(window)) != recovery);
  return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_retention(SignerWindow *window, const char *npub) {
  (void)npub; PageSettings *page = g_object_ref_sink(g_object_new(TYPE_PAGE_SETTINGS, NULL));
  page_settings_show_retention(page, GTK_WIDGET(window));
  g_object_set_data_full(G_OBJECT(sizing_dialog(window)), "sizing-settings", page, g_object_unref);
  return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_ask(SignerWindow *window, const char *npub) {
  (void)npub; PageSettings *page = g_object_ref_sink(g_object_new(TYPE_PAGE_SETTINGS, NULL));
  page_settings_show_ask_defaults(page, GTK_WIDGET(window));
  g_object_unref(page); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_add(SignerWindow *window, const char *npub) {
  (void)npub; g_action_group_activate_action(G_ACTION_GROUP(window), "pair-application", NULL);
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_pairing_selector(SignerWindow *window, const char *npub) {
  (void)npub; grotto_pairing_present(GTK_WIDGET(window), NULL);
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_preview(SignerWindow *window, const char *npub) {
  (void)npub; grotto_pairing_present(GTK_WIDGET(window),
    "nostrconnect://79be667ef9dcbbac55a06295ce870b07029bfcdb2dce28d959f2815b16f81798?relay=wss%3A%2F%2Frelay.example&secret=fixture&name=Example%20App");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_invalid_preview(SignerWindow *window, const char *npub) {
  (void)npub; grotto_pairing_present(GTK_WIDGET(window), "nostrconnect://invalid");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *sizing_app_detail(SignerWindow *window) {
  GtkWidget *apps = sizing_panel(window, "apps");
  GtkWidget *row = NULL;
  SIZING_WAIT((row = sizing_find(apps, ADW_TYPE_ACTION_ROW, "Jank")) && gtk_widget_is_sensitive(row));
  g_signal_emit_by_name(row, "activated");
  return GTK_WIDGET(adw_navigation_view_get_visible_page(grotto_apps_panel_get_navigation(GROTTO_APPS_PANEL(apps))));
}
static GtkWidget *sizing_permission(GtkWidget *widget, const char *key, const char *decision) {
  GrottoPanelsItem *item = g_object_get_data(G_OBJECT(widget), key);
  if (item && (!decision || !strcmp(grotto_panels_item_get_metadata(item)->decision, decision))) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = sizing_permission(child, key, decision); if (found) return found;
  }
  return NULL;
}
static GtkWidget *sizing_permission_editor(SignerWindow *window, const char *decision) {
  GtkWidget *detail = sizing_app_detail(window), *row = NULL;
  sizing_activate_row(detail, P_("Permissions"));
  SIZING_WAIT((row = sizing_permission(detail, "permission", decision)) && gtk_widget_is_sensitive(row));
  g_signal_emit_by_name(row, "activated");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_permission(SignerWindow *window, const char *npub) {
  (void)npub; return sizing_permission_editor(window, "allow");
}
static GtkWidget *open_inactive_permission(SignerWindow *window, const char *npub) {
  (void)npub; return sizing_permission_editor(window, "policy-review");
}
static GtkWidget *open_discard(SignerWindow *window, const char *npub) {
  GtkWidget *editor = open_inactive_permission(window, npub);
  GtkWidget *button = sizing_find(editor, GTK_TYPE_BUTTON, P_("Discard Saved Permission…"));
  g_assert_nonnull(button); g_signal_emit_by_name(button, "clicked");
  SIZING_WAIT(GTK_WIDGET(sizing_dialog(window)) != editor); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_remove_permission(SignerWindow *window, const char *npub) {
  GtkWidget *editor = open_permission(window, npub);
  GtkWidget *button = sizing_find(editor, GTK_TYPE_BUTTON, P_("Remove Permission…"));
  g_assert_nonnull(button); g_signal_emit_by_name(button, "clicked");
  SIZING_WAIT(GTK_WIDGET(sizing_dialog(window)) != editor); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_remove_app(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_app_detail(window);
  GtkWidget *button = NULL;
  SIZING_WAIT((button = sizing_find(detail, GTK_TYPE_BUTTON, P_("Remove App…"))) && gtk_widget_is_sensitive(button));
  g_signal_emit_by_name(button, "clicked");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_clear_permissions(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *detail = sizing_app_detail(window), *row = NULL;
  sizing_activate_row(detail, P_("Permissions"));
  SIZING_WAIT((row = sizing_permission(detail, "clear-scope", NULL)) && gtk_widget_is_sensitive(row));
  g_signal_emit_by_name(row, "activated");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_edit(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *panel = sizing_panel(window, "identity");
  GtkWidget *edit = NULL;
  SIZING_WAIT((edit = sizing_find(panel, GTK_TYPE_BUTTON, P_("Edit Profile…"))) && gtk_widget_is_sensitive(edit));
  g_signal_emit_by_name(edit, "clicked");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_remove(SignerWindow *window, const char *npub) {
  GtkWidget *panel = sizing_panel(window, "identity");
  g_assert_true(grotto_panel_host_invoke(GROTTO_PANEL(panel), GROTTO_PANEL_HOST_REMOVE, npub, NULL));
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_clear(SignerWindow *window, const char *npub) {
  (void)npub; GtkWidget *panel = sizing_panel(window, "activity");
  GtkWidget *clear = GTK_WIDGET(gtk_widget_get_template_child(panel, GROTTO_TYPE_ACTIVITY_PANEL, "clear"));
  SIZING_WAIT(gtk_widget_is_sensitive(clear)); g_signal_emit_by_name(clear, "clicked");
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_review(SignerWindow *window, const char *npub) {
  GnostrApprovalDialog *dialog = gnostr_approval_dialog_new();
  gnostr_approval_dialog_set_app_name(dialog, "https://example.test");
  gnostr_approval_dialog_set_identity(dialog, npub);
  gnostr_approval_dialog_set_event_type(dialog, NOSTR_KIND_TEXT_NOTE);
  gnostr_approval_dialog_set_content(dialog, "A public fixture note for sizing the real Review Request surface.");
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window)); return GTK_WIDGET(dialog);
}
static GtkWidget *open_preferences(SignerWindow *window, const char *npub) {
  (void)npub; g_action_group_activate_action(G_ACTION_GROUP(window), "preferences", NULL);
  SIZING_WAIT(sizing_dialog(window)); return GTK_WIDGET(sizing_dialog(window));
}
static GtkWidget *open_assistant(SignerWindow *window, const char *npub) {
  (void)npub; OnboardingAssistant *assistant = onboarding_assistant_new();
  gtk_window_set_transient_for(GTK_WINDOW(assistant), GTK_WINDOW(window));
  sizing_present_window(GTK_WINDOW(assistant), gtk_widget_get_width(GTK_WIDGET(window))); return GTK_WIDGET(assistant);
}

typedef struct {
  const char *name;
  GtkWidget *(*open)(SignerWindow *, const char *);
  const char *affirmative, *cancel;
  int floor;
  gboolean body_actions;
} SizingSurface;
static const SizingSurface surfaces[] = {
  {"qr-display", open_qr, NULL, "_Close", 200},
  {"qr-scanner", open_scanner, "_Import Key", "_Cancel", 200},
  {"qr-scanner-result", open_scanner_result, "_Import Key", "_Cancel", 200},
  {"connection-link", open_connection, NULL, "_Close", 200},
  {"connection-link-ready", open_connection_ready, NULL, "_Close", 200},
  {"connection-link-empty", open_connection_unavailable, NULL, "_Close", 200},
  {"encrypted-backup-passphrase", open_passphrase, "Show _QR Code", "_Cancel", 200},
  {"encrypted-backup-file-passphrase", open_file_passphrase, "_Save Backup…", "_Cancel", 200},
  {"backup-sheet", open_backup_shell, NULL, "_Close", 200},
  {"raw-key-alert", open_raw_alert, "Copy _Key", "_Cancel", 120},
  {"replace-identity-alert", open_replace_alert, "Create _Identity", "_Cancel", 120},
  {"recovery-phrase", open_recovery, "Confirm _Backup", "_Cancel", 200},
  {"copy-recovery-alert", open_copy_phrase, "Cop_y Phrase", "_Cancel", 120},
  {"ask-by-default", open_ask, "_Save", "_Cancel", 200},
  {"activity-retention", open_retention, "_Save", "_Cancel", 200},
  {"add-app", open_add, NULL, "_Cancel", 200},
  {"pairing-selector", open_pairing_selector, NULL, "_Cancel", 200},
  {"paste-link-preview", open_preview, "_Review", "_Cancel", 200},
  {"invalid-link-preview", open_invalid_preview, "_Review", "_Cancel", 200},
  {"edit-profile", open_edit, "Save", "Cancel", 200},
  {"edit-permission", open_permission, "Save Permission", "Cancel", 200, TRUE},
  {"inactive-permission", open_inactive_permission, "Activate Saved Permission", "Cancel", 200, TRUE},
  {"discard-permission-alert", open_discard, "Discard", "Cancel", 120},
  {"remove-permission-alert", open_remove_permission, "Remove Permission", "Cancel", 120},
  {"remove-app-alert", open_remove_app, "Remove App", "Cancel", 120},
  {"clear-permissions-alert", open_clear_permissions, "Clear Permissions", "Cancel", 120},
  {"remove-identity-alert", open_remove, "_Remove", "_Cancel", 120},
  {"clear-activity-alert", open_clear, "Clear Activity", "Cancel", 120},
  {"review-request", open_review, "_Allow Once", "_Deny Once", 200},
  {"preferences", open_preferences, NULL, NULL, 200},
  {"onboarding-assistant", open_assistant, NULL, NULL, 200},
};

/* Measure the actual content, not the scroller's default minimum/natural height
 * (which is precisely the collapsed-sheet bug). Only mapped stack pages count.
 * A deliberate max-content-height remains a ceiling, not a regression. */
static int sizing_natural(GtkWidget *widget) {
  int natural = 0;
  gtk_widget_measure(widget, GTK_ORIENTATION_VERTICAL, MAX(1, gtk_widget_get_width(widget)), NULL, &natural, NULL, NULL);
  if ((GTK_IS_REVEALER(widget) && !gtk_revealer_get_reveal_child(GTK_REVEALER(widget))) ||
      (ADW_IS_BANNER(widget) && !adw_banner_get_revealed(ADW_BANNER(widget)))) return natural;
  if (GTK_IS_SCROLLED_WINDOW(widget)) {
    GtkWidget *child = gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(widget));
    if (child && gtk_widget_get_mapped(child)) {
      int height = sizing_natural(child);
      int ceiling = gtk_scrolled_window_get_max_content_height(GTK_SCROLLED_WINDOW(widget));
      if (ceiling >= 0) height = MIN(height, ceiling);
      return MAX(natural, height);
    }
    return natural;
  }
  int extra = 0;
  gboolean horizontal = GTK_IS_BOX(widget) && gtk_orientable_get_orientation(GTK_ORIENTABLE(widget)) == GTK_ORIENTATION_HORIZONTAL;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    if (!gtk_widget_get_mapped(child)) continue;
    int child_natural = 0;
    gtk_widget_measure(child, GTK_ORIENTATION_VERTICAL, MAX(1, gtk_widget_get_width(child)), NULL, &child_natural, NULL, NULL);
    int correction = MAX(0, sizing_natural(child) - child_natural);
    extra = horizontal ? MAX(extra, correction) : extra + correction;
  }
  return natural + extra;
}
static void sizing_contained(GtkWidget *widget, GtkWidget *root) {
  if (!gtk_widget_get_mapped(widget)) return;
  graphene_rect_t bounds;
  g_assert_true(gtk_widget_compute_bounds(widget, root, &bounds));
  if (bounds.origin.x < -1 || bounds.origin.y < -1 ||
      bounds.origin.x + bounds.size.width > gtk_widget_get_width(root) + 1 ||
      bounds.origin.y + bounds.size.height > gtk_widget_get_height(root) + 1) {
    g_error("%s (css=%s) clipped outside dialog: %.1f,%.1f %.1fx%.1f, content %dx%d",
      G_OBJECT_TYPE_NAME(widget), gtk_widget_get_css_name(widget), bounds.origin.x, bounds.origin.y,
      bounds.size.width, bounds.size.height, gtk_widget_get_width(root), gtk_widget_get_height(root));
  }
  /* Explicit containment exceptions: scroll content belongs to its viewport;
   * popovers have their own bounds; an unrevealed GtkRevealer intentionally
   * clips its still-mapped descendants; likewise an unrevealed AdwBanner.
   * Assert these containers, not intentionally invisible descendants. */
  if (GTK_IS_SCROLLED_WINDOW(widget) || GTK_IS_POPOVER(widget) ||
      (GTK_IS_REVEALER(widget) && !gtk_revealer_get_reveal_child(GTK_REVEALER(widget))) ||
      (ADW_IS_BANNER(widget) && !adw_banner_get_revealed(ADW_BANNER(widget)))) return;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child))
    sizing_contained(child, root);
}
static void sizing_control(GtkWidget *surface, GtkWidget *content, const char *label, gboolean header) {
  if (!label) return;
  GtkWidget *button = sizing_find(surface, GTK_TYPE_BUTTON, label);
  g_assert_nonnull(button); g_assert_true(gtk_widget_get_mapped(button));
  g_assert_cmpint(gtk_widget_get_width(button), >, 0); g_assert_cmpint(gtk_widget_get_height(button), >, 0);
  if (header) g_assert_nonnull(gtk_widget_get_ancestor(button, ADW_TYPE_HEADER_BAR));
  sizing_contained(button, content);
}
static void sizing_assert(GtkWidget *surface, SignerWindow *window, const SizingSurface *test) {
  SIZING_WAIT(gtk_widget_get_mapped(surface));
  sizing_frames(GTK_WIDGET(gtk_widget_get_root(surface)));
  GtkWidget *content = ADW_IS_DIALOG(surface) ? adw_dialog_get_child(ADW_DIALOG(surface)) : gtk_window_get_child(GTK_WINDOW(surface));
  g_assert_nonnull(content); g_assert_true(gtk_widget_get_mapped(content));
  int height = gtk_widget_get_height(content), natural = sizing_natural(content);
  /* Floating dialogs reserve outer shadow/margin space; bottom sheets reserve
   * the top inset. Do not mistake window decorations for available content. */
  /* libadwaita 1.5 reserves 30px above bottom sheets and 30px on each
   * side of floating dialogs at these host sizes (GTK logical pixels). */
  int available = gtk_widget_get_height(GTK_WIDGET(window)) - (gtk_widget_get_width(GTK_WIDGET(window)) > 450 ? 60 : 30);
  int host_available = available;
  if (ADW_IS_DIALOG(surface) && !adw_dialog_get_follows_content_size(ADW_DIALOG(surface))) {
    int ceiling = adw_dialog_get_content_height(ADW_DIALOG(surface));
    if (ceiling > 0) available = MIN(available, ceiling);
  }
  int required = MAX(MIN(test->floor, host_available), MIN(natural, available));
  g_test_message("%s at %dx640: content=%dx%d natural=%d available=%d required=%d", test->name,
    gtk_widget_get_width(GTK_WIDGET(window)), gtk_widget_get_width(content), height, natural, available, required);
  g_assert_cmpint(height + 2, >=, required);
  g_assert_cmpint(height, <=, gtk_widget_get_height(GTK_WIDGET(window)));
  g_assert_cmpint(gtk_widget_get_width(content), <=, gtk_widget_get_width(GTK_WIDGET(window)));
  sizing_contained(content, content);
  gboolean header = ADW_IS_DIALOG(surface) && !ADW_IS_ALERT_DIALOG(surface) && !GNOSTR_IS_APPROVAL_DIALOG(surface) && !test->body_actions;
  if ((ADW_IS_DIALOG(surface) && !ADW_IS_ALERT_DIALOG(surface)) || ADW_IS_WINDOW(surface)) {
    /* Window chrome only: the assistant footer belongs to the sibling task. */
    GtkWidget *bar = sizing_find(content, ADW_TYPE_HEADER_BAR, NULL);
    g_assert_nonnull(bar); g_assert_true(gtk_widget_get_mapped(bar)); sizing_contained(bar, content);
  }
  sizing_control(surface, content, test->affirmative ? _(test->affirmative) : NULL, header);
  sizing_control(surface, content, test->cancel ? _(test->cancel) : NULL, header);
  if (SHEET_IS_QR_DISPLAY(surface)) {
    g_assert_true(adw_dialog_get_follows_content_size(ADW_DIALOG(surface)));
    GtkWidget *scroll = sizing_find(content, GTK_TYPE_SCROLLED_WINDOW, NULL); g_assert_nonnull(scroll);
    g_assert_true(gtk_scrolled_window_get_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll)));
    g_assert_cmpint(gtk_scrolled_window_get_min_content_height(GTK_SCROLLED_WINDOW(scroll)), ==, 200);
    g_assert_cmpint(gtk_scrolled_window_get_max_content_height(GTK_SCROLLED_WINDOW(scroll)), ==, 560);
    /* Bottom sheets have a toolkit-owned 360px minimum; short content must not
     * otherwise expand to fill the host. Long content is capped by the viewport. */
    int sheet_floor = gtk_widget_get_width(GTK_WIDGET(window)) == 360 ? 360 : test->floor;
    g_assert_cmpint(height, <=, MIN(host_available, MAX(natural, sheet_floor)) + 2);
    GtkWidget *copy = GTK_WIDGET(gtk_widget_get_template_child(surface, TYPE_SHEET_QR_DISPLAY, "btn_copy"));
    g_assert_true(gtk_widget_get_mapped(copy)); sizing_contained(copy, content);
  }
  if (ADW_IS_DIALOG(surface) && gtk_widget_get_width(GTK_WIDGET(window)) == 360 && !ADW_IS_ALERT_DIALOG(surface)) {
    /* On narrow hosts the production AUTO presentation must be a bottom sheet. */
    GtkWidget *sheet = gtk_widget_get_ancestor(content, g_type_from_name("AdwBottomSheet"));
    g_assert_nonnull(sheet);
  }
}
static void sizing_activate(GApplication *application, gpointer data) {
  (void)data;
  g_autofree char *npub = NULL;
  g_assert_true(gn_backup_get_npub("0000000000000000000000000000000000000000000000000000000000000001", &npub, NULL));
  AccountsStore *store = accounts_store_get_default();
  g_assert_true(accounts_store_add(store, npub, "Sizing fixture", NULL));
  g_settings_set_string(settings_manager_get_gsettings(settings_manager_get_default()), "default-identity", npub);
  const int widths[] = {920, 360};
  for (guint width = 0; width < G_N_ELEMENTS(widths); width++) {
    SignerWindow *window = signer_window_new(ADW_APPLICATION(application)); g_object_ref(window);
    sizing_present_window(GTK_WINDOW(window), widths[width]);
    for (guint i = 0; i < G_N_ELEMENTS(surfaces); i++) {
      const SizingSurface *test = &surfaces[i];
      g_test_message("Present %s", test->name);
      GtkWidget *surface = test->open(window, npub);
      sizing_assert(surface, window, test);
      if (GTK_IS_WINDOW(surface)) gtk_window_destroy(GTK_WINDOW(surface));
      else {
        g_object_ref(surface);
        GtkWindow *auxiliary = g_object_get_data(G_OBJECT(surface), "sizing-auxiliary-host");
        adw_dialog_force_close(ADW_DIALOG(surface));
        SIZING_WAIT(!gtk_widget_get_mapped(surface) && !gtk_widget_get_parent(surface));
        if (auxiliary) gtk_window_destroy(auxiliary);
        g_object_unref(surface);
        while (sizing_dialog(window)) {
          AdwDialog *dialog = sizing_dialog(window); g_object_ref(dialog); adw_dialog_force_close(dialog);
          SIZING_WAIT(sizing_dialog(window) != dialog); g_object_unref(dialog);
        }
      }
      sizing_frames(GTK_WIDGET(window));
    }
    gtk_window_destroy(GTK_WINDOW(window)); g_object_unref(window);
  }
  g_application_quit(application);
}
static void dialog_sizing(void) {
  g_autoptr(AdwApplication) app = adw_application_new("org.nostr.Grotto.DialogSizingTest", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(sizing_activate), NULL);
  g_assert_cmpint(g_application_run(G_APPLICATION(app), 0, NULL), ==, 0);
}
void grotto_test_add_dialog_sizing(void) {
  g_test_add_func("/signer/dialog-sizing/all-surfaces", dialog_sizing);
}
