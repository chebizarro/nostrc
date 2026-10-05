/* test-approval-dialog.c - regression tests for the approval dialog (nostrc-n98j)
 *
 * Drives the real src/ui/approval_dialog.c (and its real template) against
 * stubs for the accounts store, client sessions and keyboard navigation, to
 * pin the two crashes found on the 2026-10-01 macOS device smoke:
 *
 *  1. The decision callback must fire exactly once. do_finish() used to call
 *     it and then adw_dialog_close() with it still set, so the "closed" class
 *     handler reported a second (reject) decision on the caller's already-
 *     freed context (SIGSEGV in approve_call_done after every Approve).
 *
 *  2. accounts_store_list() entries are owned by the array (its free func).
 *     gnostr_approval_dialog_set_accounts() used to g_free() each entry and
 *     then free the array: a double free on every signing request. The stub
 *     counts entry frees, and the double free itself aborts under ASAN or the
 *     allocator's own double-free check.
 *
 * Every test presents the dialog in a window, so the close path is
 * libadwaita's own: adw_dialog_close() -> ::closed -> the class handler.
 */
#include "nostrc-test-gdk-frame.h"
#include <adwaita.h>
#include <gtk/gtk.h>
#include <string.h>

#include "accounts_store.h"
#include "client_session.h"
#include "keyboard-nav.h"
#include "ui/approval_dialog.h"

/* ---- stubs for approval_dialog.c's collaborators ------------------------ */

/* Opaque to the dialog; it only has to be non-NULL for set_accounts(). */
static int fake_store_storage;
#define FAKE_STORE ((AccountsStore *)&fake_store_storage)

static guint entries_freed;
static guint sessions_created;
static GPtrArray *(*next_list)(void);

static void stub_entry_free(gpointer p) {
  AccountEntry *e = p;
  entries_freed++;
  g_free(e->id);
  g_free(e->label);
  g_free(e);
}

static AccountEntry *stub_entry(const char *id, gboolean watch_only) {
  AccountEntry *e = g_new0(AccountEntry, 1);
  e->id = g_strdup(id);
  e->label = g_strdup("label");
  e->has_secret = !watch_only;
  e->watch_only = watch_only;
  return e;
}

#define NPUB_A "npub1aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define NPUB_B "npub1bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define NPUB_W "npub1wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww"

/* The shape accounts_store_list() really returns: the array owns entries. */
static GPtrArray *three_accounts(void) {
  GPtrArray *arr = g_ptr_array_new_with_free_func(stub_entry_free);
  g_ptr_array_add(arr, stub_entry(NPUB_A, FALSE));
  g_ptr_array_add(arr, stub_entry(NPUB_W, TRUE));
  g_ptr_array_add(arr, stub_entry(NPUB_B, FALSE));
  return arr;
}

GPtrArray *accounts_store_list(AccountsStore *as) {
  (void)as;
  return next_list ? next_list() : g_ptr_array_new_with_free_func(stub_entry_free);
}

GnClientSessionManager *gn_client_session_manager_get_default(void) {
  return NULL;
}

GnClientSession *gn_client_session_manager_create_session(
    GnClientSessionManager *self, const gchar *client_pubkey,
    const gchar *identity, const gchar *app_name, guint permissions,
    gboolean persistent, gint64 ttl_seconds) {
  (void)self; (void)client_pubkey; (void)identity; (void)app_name;
  (void)permissions; (void)persistent; (void)ttl_seconds;
  sessions_created++;
  return NULL;
}

GnClientSession *gn_client_session_manager_get_session(
    GnClientSessionManager *self, const gchar *client_pubkey, const gchar *identity) {
  (void)self; (void)client_pubkey; (void)identity;
  return NULL;
}

gboolean gn_client_session_manager_has_active_session(
    GnClientSessionManager *self, const gchar *client_pubkey, const gchar *identity) {
  (void)self; (void)client_pubkey; (void)identity;
  return FALSE;
}

gboolean gn_client_session_manager_touch_session(
    GnClientSessionManager *self, const gchar *client_pubkey, const gchar *identity) {
  (void)self; (void)client_pubkey; (void)identity;
  return FALSE;
}

gboolean gn_client_session_has_permission(GnClientSession *self,
                                          GnClientSessionPermission perm) {
  (void)self; (void)perm;
  return FALSE;
}

void gn_keyboard_nav_setup_dialog(AdwDialog *dialog, GtkWidget *first_focus,
                                  GtkWidget *default_button) {
  (void)dialog; (void)first_focus; (void)default_button;
}

void gn_keyboard_nav_setup_focus_chain(GtkWidget **widgets) {
  (void)widgets;
}

/* ---- decision recorder --------------------------------------------------- */

typedef struct {
  guint calls;
  gboolean decision;
  gchar *selected;
} Decision;

static void on_decision(gboolean decision, gboolean remember, const char *selected,
                        guint64 ttl_seconds, gpointer user_data) {
  (void)remember; (void)ttl_seconds;
  Decision *d = user_data;
  d->calls++;
  d->decision = decision;
  g_free(d->selected);
  d->selected = g_strdup(selected);
}

static void reset_stubs(void) {
  entries_freed = 0;
  sessions_created = 0;
  next_list = NULL;
}

/* A presented dialog: libadwaita only emits ::closed (and so runs the
 * dialog's closed class handler) for a dialog that was presented. */
typedef struct {
  GtkWidget *win;
  GnostrApprovalDialog *dlg;
  Decision d;
  gboolean closed;
} Fixture;

static void on_closed(AdwDialog *dialog, gpointer user_data) {
  (void)dialog;
  ((Fixture *)user_data)->closed = TRUE;
}

static gboolean on_timeout(gpointer user_data);

static gboolean count_frame(GtkWidget *w, GdkFrameClock *clock, gpointer user_data) {
  (void)w; (void)clock;
  guint *frames = user_data;
  return ++*frames < 2 ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}

/* Run the main loop until @w is on screen: mapped and two frames drawn (or
 * 10 s pass). A user only clicks a dialog they can see; and libadwaita 1.5
 * drops a close requested before the dialog's first frames (on X11, where
 * mapping is asynchronous, ::closed then never comes). */
static void wait_shown(GtkWidget *w) {
  gboolean timed_out = FALSE;
  guint frames = 0;
  guint id = g_timeout_add_seconds(10, on_timeout, &timed_out);
  while (!gtk_widget_get_mapped(w) && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  g_assert_true(gtk_widget_get_mapped(w));
  gtk_widget_add_tick_callback(w, count_frame, &frames, NULL);
  while (frames < 2 && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!timed_out)
    g_source_remove(id);
  g_assert_cmpuint(frames, >=, 2);
}

static void fixture_setup(Fixture *f, gconstpointer data) {
  GPtrArray *(*list)(void) = (GPtrArray *(*)(void))data;
  reset_stubs();
  next_list = list;
  memset(&f->d, 0, sizeof f->d);
  f->closed = FALSE;
  f->dlg = gnostr_approval_dialog_new();
  g_object_ref_sink(f->dlg);
  gnostr_approval_dialog_set_callback(f->dlg, on_decision, &f->d);
  g_signal_connect(f->dlg, "closed", G_CALLBACK(on_closed), f);
  /* An AdwWindow hosts the dialog in-window; a plain GtkWindow would make
   * it a separate toplevel, whose close never completes on WM-less Xvfb. */
  f->win = adw_window_new();
  /* Room for the floating dialog the app shows (not a bottom sheet). */
  gtk_window_set_default_size(GTK_WINDOW(f->win), 800, 600);
  gtk_window_present(GTK_WINDOW(f->win));
  adw_dialog_present(ADW_DIALOG(f->dlg), f->win);
  wait_shown(GTK_WIDGET(f->dlg));
}

static void fixture_teardown(Fixture *f, gconstpointer data) {
  (void)data;
  gtk_window_destroy(GTK_WINDOW(f->win));
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_object_unref(f->dlg);
  g_free(f->d.selected);
}

static gboolean on_timeout(gpointer user_data) {
  *(gboolean *)user_data = TRUE;
  return G_SOURCE_REMOVE;
}

/* Run the main loop until the dialog has closed (or 10 s pass), then a
 * little longer so a late second decision would be seen. */
static void wait_closed(Fixture *f) {
  gboolean timed_out = FALSE;
  guint id = g_timeout_add_seconds(10, on_timeout, &timed_out);
  while (!f->closed && !timed_out)
    g_main_context_iteration(NULL, TRUE);
  if (!timed_out)
    g_source_remove(id);
  while (g_main_context_iteration(NULL, FALSE))
    ;
  g_assert_true(f->closed);
}

static GtkWidget *child(GnostrApprovalDialog *dlg, const char *name) {
  GObject *o = gtk_widget_get_template_child(GTK_WIDGET(dlg),
                                             GNOSTR_TYPE_APPROVAL_DIALOG, name);
  g_assert_nonnull(o);
  return GTK_WIDGET(o);
}

static void click(GnostrApprovalDialog *dlg, const char *button) {
  g_signal_emit_by_name(child(dlg, button), "clicked");
}

/* ---- tests --------------------------------------------------------------- */

/* Approve -> callback(TRUE) -> adw_dialog_close() -> ::closed: the closed
 * handler must not report a second (reject) decision. */
static void test_approve_fires_once(Fixture *f, gconstpointer data) {
  (void)data;
  click(f->dlg, "btn_approve");
  g_assert_cmpuint(f->d.calls, ==, 1);
  wait_closed(f);
  g_assert_cmpuint(f->d.calls, ==, 1);
  g_assert_true(f->d.decision);
}

static void test_deny_fires_once(Fixture *f, gconstpointer data) {
  (void)data;
  click(f->dlg, "btn_deny");
  wait_closed(f);
  g_assert_cmpuint(f->d.calls, ==, 1);
  g_assert_false(f->d.decision);
}

/* Closed without a decision (Escape, parent gone): one denial. */
static void test_close_without_decision_denies_once(Fixture *f, gconstpointer data) {
  (void)data;
  adw_dialog_close(ADW_DIALOG(f->dlg));
  wait_closed(f);
  g_assert_cmpuint(f->d.calls, ==, 1);
  g_assert_false(f->d.decision);
}

/* Approve twice (button and Ctrl+A before the close completes): one decision
 * and one remembered client session, not two. */
static void test_second_approve_is_noop(Fixture *f, gconstpointer data) {
  (void)data;
  gnostr_approval_dialog_set_client_pubkey(f->dlg, "ab12");
  gnostr_approval_dialog_set_accounts(f->dlg, FAKE_STORE, NULL);
  gtk_check_button_set_active(GTK_CHECK_BUTTON(child(f->dlg, "chk_remember")), TRUE);

  click(f->dlg, "btn_approve");
  click(f->dlg, "btn_approve");
  wait_closed(f);

  g_assert_cmpuint(f->d.calls, ==, 1);
  g_assert_true(f->d.decision);
  g_assert_cmpstr(f->d.selected, ==, NPUB_A);
  g_assert_cmpuint(sessions_created, ==, 1);
}

/* The array owns its entries: each is freed exactly once, by the array (a
 * manual free as well is a double free). The identity model keeps its own
 * copies, so the selection survives the array. */
static void test_set_accounts_leaves_entries_to_array(Fixture *f, gconstpointer data) {
  (void)data;
  gnostr_approval_dialog_set_accounts(f->dlg, FAKE_STORE, NPUB_B);
  g_assert_cmpuint(entries_freed, ==, 3);

  /* Again, as each signing request does. */
  gnostr_approval_dialog_set_accounts(f->dlg, FAKE_STORE, NPUB_B);
  g_assert_cmpuint(entries_freed, ==, 6);

  /* The watch-only account is not offered; the requested one is selected. */
  GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(child(f->dlg, "identity_dropdown")));
  g_assert_nonnull(model);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 2);

  click(f->dlg, "btn_approve");
  wait_closed(f);
  g_assert_cmpuint(f->d.calls, ==, 1);
  g_assert_cmpstr(f->d.selected, ==, NPUB_B);
}

int main(int argc, char *argv[]) {
  /* Same skip contract as test-ui.c: no window-system connection, no GTK. */
  if (g_getenv("GNOSTR_SKIP_UI_TESTS") && *g_getenv("GNOSTR_SKIP_UI_TESTS")) {
    g_print("TAP version 14\n1..0 # SKIP UI tests disabled via GNOSTR_SKIP_UI_TESTS\n");
    return 0;
  }
#ifndef __APPLE__
  if (!g_getenv("DISPLAY") && !g_getenv("WAYLAND_DISPLAY")) {
    g_print("TAP version 14\n1..0 # SKIP needs a display (DISPLAY/WAYLAND_DISPLAY not set)\n");
    return 0;
  }
#endif

  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  gtk_init();
  adw_init();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);

#define ADD(path, fn, list) \
  g_test_add("/signer/approval-dialog/" path, Fixture, (gconstpointer)(list), \
             fixture_setup, fn, fixture_teardown)
  ADD("approve-fires-once", test_approve_fires_once, NULL);
  ADD("deny-fires-once", test_deny_fires_once, NULL);
  ADD("close-without-decision-denies-once", test_close_without_decision_denies_once, NULL);
  ADD("second-approve-is-noop", test_second_approve_is_noop, three_accounts);
  ADD("set-accounts-leaves-entries-to-array", test_set_accounts_leaves_entries_to_array,
      three_accounts);
#undef ADD
  return g_test_run();
}
