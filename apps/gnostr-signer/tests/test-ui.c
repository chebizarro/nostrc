/* test-ui.c - UI tests for gnostr-signer GTK application
 *
 * Tests UI components including window creation, page navigation,
 * dialog presentation/dismissal, form validation, and button states.
 *
 * Uses GLib testing framework with GTK test utilities.
 * Mocks D-Bus and network dependencies for isolated testing.
 */

#include <gtk/gtk.h>
#include <adwaita.h>
#include <glib.h>
#include <string.h>

/* ===========================================================================
 * Mock Application and Window Types for Testing
 *
 * We create lightweight mocks of the actual application types to avoid
 * needing full D-Bus connections and other heavy dependencies.
 * =========================================================================== */

/* Forward declarations for page types we reference */
#define TYPE_PAGE_PERMISSIONS (page_permissions_get_type())
#define TYPE_PAGE_APPLICATIONS (page_applications_get_type())
#define TYPE_PAGE_SETTINGS (page_settings_get_type())
#define GN_TYPE_PAGE_SESSIONS (gn_page_sessions_get_type())

/* Declare the actual GTypes for the pages */
GType page_permissions_get_type(void);
GType page_applications_get_type(void);
GType page_settings_get_type(void);
GType gn_page_sessions_get_type(void);

/* Mock signer window structure */
struct _MockSignerWindow {
  AdwApplicationWindow parent_instance;
  AdwViewStack *stack;
  GtkListBox *sidebar;
  GtkWidget *page_permissions;
  GtkWidget *page_applications;
  GtkWidget *page_sessions;
  GtkWidget *page_settings;
  GSettings *settings;
};

G_DECLARE_FINAL_TYPE(MockSignerWindow, mock_signer_window, MOCK, SIGNER_WINDOW, AdwApplicationWindow)
G_DEFINE_TYPE(MockSignerWindow, mock_signer_window, ADW_TYPE_APPLICATION_WINDOW)

static void mock_signer_window_class_init(MockSignerWindowClass *klass) {
  (void)klass;
}

static void mock_signer_window_init(MockSignerWindow *self) {
  /* Create main content */
  self->stack = ADW_VIEW_STACK(adw_view_stack_new());
  self->sidebar = GTK_LIST_BOX(gtk_list_box_new());
  gtk_list_box_set_selection_mode(self->sidebar, GTK_SELECTION_BROWSE);

  /* Create mock pages as simple boxes */
  self->page_permissions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->page_permissions, "permissions");
  self->page_applications = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->page_applications, "applications");
  self->page_sessions = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->page_sessions, "sessions");
  self->page_settings = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_name(self->page_settings, "settings");

  /* Add pages to stack */
  adw_view_stack_add_named(self->stack, self->page_permissions, "permissions");
  adw_view_stack_add_named(self->stack, self->page_applications, "applications");
  adw_view_stack_add_named(self->stack, self->page_sessions, "sessions");
  adw_view_stack_add_named(self->stack, self->page_settings, "settings");

  /* Create sidebar rows */
  const char *pages[] = { "Permissions & Connection", "Applications", "Active Sessions", "Settings" };
  for (int i = 0; i < 4; i++) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(pages[i]);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_list_box_append(self->sidebar, row);
  }

  /* Create main layout */
  GtkWidget *split_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  GtkWidget *sidebar_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sidebar_scroll), GTK_WIDGET(self->sidebar));
  gtk_widget_set_size_request(sidebar_scroll, 250, -1);
  gtk_box_append(GTK_BOX(split_box), sidebar_scroll);
  gtk_box_append(GTK_BOX(split_box), GTK_WIDGET(self->stack));
  gtk_widget_set_hexpand(GTK_WIDGET(self->stack), TRUE);

  adw_application_window_set_content(ADW_APPLICATION_WINDOW(self), split_box);
  self->settings = NULL;
}

static MockSignerWindow *mock_signer_window_new(AdwApplication *app) {
  return g_object_new(mock_signer_window_get_type(), "application", app, NULL);
}

/* ===========================================================================
 * Mock Dialog Types
 * =========================================================================== */

/* Mock approval dialog */
struct _MockApprovalDialog {
  AdwDialog parent_instance;
  GtkWidget *approve_btn;
  GtkWidget *deny_btn;
  GtkWidget *remember_check;
  gboolean decision_made;
  gboolean approved;
  gboolean remember;
};

G_DECLARE_FINAL_TYPE(MockApprovalDialog, mock_approval_dialog, MOCK, APPROVAL_DIALOG, AdwDialog)
G_DEFINE_TYPE(MockApprovalDialog, mock_approval_dialog, ADW_TYPE_DIALOG)

static void mock_approval_dialog_class_init(MockApprovalDialogClass *klass) {
  (void)klass;
}

static void mock_approval_dialog_init(MockApprovalDialog *self) {
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);

  GtkWidget *label = gtk_label_new("Test application requests signing permission");
  gtk_box_append(GTK_BOX(content), label);

  self->remember_check = gtk_check_button_new_with_label("Remember this decision");
  gtk_box_append(GTK_BOX(content), self->remember_check);

  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);

  self->deny_btn = gtk_button_new_with_label("Deny");
  self->approve_btn = gtk_button_new_with_label("Approve");
  gtk_widget_add_css_class(self->approve_btn, "suggested-action");

  gtk_box_append(GTK_BOX(btn_box), self->deny_btn);
  gtk_box_append(GTK_BOX(btn_box), self->approve_btn);
  gtk_box_append(GTK_BOX(content), btn_box);

  adw_dialog_set_title(ADW_DIALOG(self), "Approval Request");
  adw_dialog_set_content_width(ADW_DIALOG(self), 400);
  adw_dialog_set_content_height(ADW_DIALOG(self), 200);
  adw_dialog_set_child(ADW_DIALOG(self), content);

  self->decision_made = FALSE;
  self->approved = FALSE;
  self->remember = FALSE;
}

static MockApprovalDialog *mock_approval_dialog_new(void) {
  /* Sink the floating ref so g_object_unref() works correctly in tests */
  return g_object_ref_sink(g_object_new(mock_approval_dialog_get_type(), NULL));
}

/* ===========================================================================
 * Mock Create Profile Dialog with Password Validation
 * =========================================================================== */

struct _MockCreateProfileDialog {
  AdwDialog parent_instance;
  GtkWidget *entry_display_name;
  GtkWidget *entry_passphrase;
  GtkWidget *entry_confirm;
  GtkWidget *btn_create;
  GtkWidget *btn_cancel;
  GtkWidget *match_label;
  gboolean passwords_match;
  gboolean passphrase_valid;
};

G_DECLARE_FINAL_TYPE(MockCreateProfileDialog, mock_create_profile_dialog, MOCK, CREATE_PROFILE_DIALOG, AdwDialog)
G_DEFINE_TYPE(MockCreateProfileDialog, mock_create_profile_dialog, ADW_TYPE_DIALOG)

static void update_create_button_sensitivity(MockCreateProfileDialog *self);

static void on_passphrase_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  MockCreateProfileDialog *self = MOCK_CREATE_PROFILE_DIALOG(user_data);
  const char *pass = gtk_editable_get_text(GTK_EDITABLE(self->entry_passphrase));
  const char *confirm = gtk_editable_get_text(GTK_EDITABLE(self->entry_confirm));

  /* Check minimum length (8 characters) */
  self->passphrase_valid = pass && strlen(pass) >= 8;

  /* Check if passwords match */
  self->passwords_match = pass && confirm && g_strcmp0(pass, confirm) == 0;

  /* Update match indicator */
  if (self->passwords_match && strlen(pass) > 0) {
    gtk_label_set_text(GTK_LABEL(self->match_label), "Passphrases match");
    gtk_widget_add_css_class(self->match_label, "success");
    gtk_widget_remove_css_class(self->match_label, "error");
  } else if (strlen(confirm) > 0) {
    gtk_label_set_text(GTK_LABEL(self->match_label), "Passphrases do not match");
    gtk_widget_add_css_class(self->match_label, "error");
    gtk_widget_remove_css_class(self->match_label, "success");
  } else {
    gtk_label_set_text(GTK_LABEL(self->match_label), "");
    gtk_widget_remove_css_class(self->match_label, "success");
    gtk_widget_remove_css_class(self->match_label, "error");
  }

  update_create_button_sensitivity(self);
}

static void update_create_button_sensitivity(MockCreateProfileDialog *self) {
  const char *name = gtk_editable_get_text(GTK_EDITABLE(self->entry_display_name));
  gboolean name_valid = name && strlen(name) > 0;

  gboolean can_create = name_valid && self->passphrase_valid && self->passwords_match;
  gtk_widget_set_sensitive(self->btn_create, can_create);
}

static void on_display_name_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  update_create_button_sensitivity(MOCK_CREATE_PROFILE_DIALOG(user_data));
}

static void mock_create_profile_dialog_class_init(MockCreateProfileDialogClass *klass) {
  (void)klass;
}

static void mock_create_profile_dialog_init(MockCreateProfileDialog *self) {
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);

  /* Display name entry */
  GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *name_label = gtk_label_new("Display Name");
  gtk_label_set_xalign(GTK_LABEL(name_label), 0);
  self->entry_display_name = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_display_name), "Enter your display name");
  g_signal_connect(self->entry_display_name, "changed", G_CALLBACK(on_display_name_changed), self);
  gtk_box_append(GTK_BOX(name_box), name_label);
  gtk_box_append(GTK_BOX(name_box), self->entry_display_name);
  gtk_box_append(GTK_BOX(content), name_box);

  /* Passphrase entry */
  GtkWidget *pass_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *pass_label = gtk_label_new("Passphrase (minimum 8 characters)");
  gtk_label_set_xalign(GTK_LABEL(pass_label), 0);
  self->entry_passphrase = gtk_password_entry_new();
  gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->entry_passphrase), TRUE);
  g_signal_connect(self->entry_passphrase, "changed", G_CALLBACK(on_passphrase_changed), self);
  gtk_box_append(GTK_BOX(pass_box), pass_label);
  gtk_box_append(GTK_BOX(pass_box), self->entry_passphrase);
  gtk_box_append(GTK_BOX(content), pass_box);

  /* Confirm passphrase entry */
  GtkWidget *confirm_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *confirm_label = gtk_label_new("Confirm Passphrase");
  gtk_label_set_xalign(GTK_LABEL(confirm_label), 0);
  self->entry_confirm = gtk_password_entry_new();
  gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->entry_confirm), TRUE);
  g_signal_connect(self->entry_confirm, "changed", G_CALLBACK(on_passphrase_changed), self);
  gtk_box_append(GTK_BOX(confirm_box), confirm_label);
  gtk_box_append(GTK_BOX(confirm_box), self->entry_confirm);
  gtk_box_append(GTK_BOX(content), confirm_box);

  /* Match indicator */
  self->match_label = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(self->match_label), 1);
  gtk_box_append(GTK_BOX(content), self->match_label);

  /* Action buttons */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 12);

  self->btn_cancel = gtk_button_new_with_label("Cancel");
  self->btn_create = gtk_button_new_with_label("Create");
  gtk_widget_add_css_class(self->btn_create, "suggested-action");
  gtk_widget_set_sensitive(self->btn_create, FALSE);

  gtk_box_append(GTK_BOX(btn_box), self->btn_cancel);
  gtk_box_append(GTK_BOX(btn_box), self->btn_create);
  gtk_box_append(GTK_BOX(content), btn_box);

  adw_dialog_set_title(ADW_DIALOG(self), "Create Profile");
  adw_dialog_set_content_width(ADW_DIALOG(self), 480);
  adw_dialog_set_content_height(ADW_DIALOG(self), 400);
  adw_dialog_set_child(ADW_DIALOG(self), content);

  self->passwords_match = FALSE;
  self->passphrase_valid = FALSE;
}

static MockCreateProfileDialog *mock_create_profile_dialog_new(void) {
  return g_object_ref_sink(g_object_new(mock_create_profile_dialog_get_type(), NULL));
}

/* ===========================================================================
 * Test Application Fixture
 * =========================================================================== */

typedef struct {
  AdwApplication *app;
  MockSignerWindow *window;
  GMainContext *context;
} TestUIFixture;

static void test_ui_fixture_setup(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Initialize GTK for testing (headless mode if available) */
  fixture->context = g_main_context_new();
  g_main_context_push_thread_default(fixture->context);

  /* Create application */
  fixture->app = adw_application_new("org.gnostr.Signer.Test", G_APPLICATION_NON_UNIQUE);
  g_application_register(G_APPLICATION(fixture->app), NULL, NULL);

  /* Create window */
  fixture->window = mock_signer_window_new(fixture->app);
  gtk_window_set_default_size(GTK_WINDOW(fixture->window), 920, 640);
}

static void test_ui_fixture_teardown(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  if (fixture->window) {
    gtk_window_destroy(GTK_WINDOW(fixture->window));
    fixture->window = NULL;
  }

  if (fixture->app) {
    g_object_unref(fixture->app);
    fixture->app = NULL;
  }

  if (fixture->context) {
    g_main_context_pop_thread_default(fixture->context);
    g_main_context_unref(fixture->context);
    fixture->context = NULL;
  }
}

/* Process pending GTK events */
static void process_pending_events(void) {
  while (g_main_context_iteration(NULL, FALSE)) {
    /* Process events */
  }
}

/* ===========================================================================
 * Test Cases: Window Creation and Destruction
 * =========================================================================== */

static void test_window_creation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  g_assert_nonnull(fixture->window);
  g_assert_true(GTK_IS_WINDOW(fixture->window));
  g_assert_true(ADW_IS_APPLICATION_WINDOW(fixture->window));

  /* Verify window has proper size */
  int width, height;
  gtk_window_get_default_size(GTK_WINDOW(fixture->window), &width, &height);
  g_assert_cmpint(width, ==, 920);
  g_assert_cmpint(height, ==, 640);
}

static void test_window_destruction(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Create a new window for destruction test */
  MockSignerWindow *win = mock_signer_window_new(fixture->app);
  g_assert_nonnull(win);
  g_assert_true(GTK_IS_WINDOW(win));

  /* Add weak reference to detect destruction */
  gpointer weak_ptr = win;
  g_object_add_weak_pointer(G_OBJECT(win), &weak_ptr);

  /* Destroy window */
  gtk_window_destroy(GTK_WINDOW(win));
  process_pending_events();

  /* Weak pointer should be NULL after destruction */
  g_assert_null(weak_ptr);
}

static void test_window_components(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Verify all essential components exist */
  g_assert_nonnull(fixture->window->stack);
  g_assert_nonnull(fixture->window->sidebar);
  g_assert_nonnull(fixture->window->page_permissions);
  g_assert_nonnull(fixture->window->page_applications);
  g_assert_nonnull(fixture->window->page_sessions);
  g_assert_nonnull(fixture->window->page_settings);

  /* Verify component types */
  g_assert_true(ADW_IS_VIEW_STACK(fixture->window->stack));
  g_assert_true(GTK_IS_LIST_BOX(fixture->window->sidebar));
}

/* ===========================================================================
 * Test Cases: Page Navigation
 * =========================================================================== */

static void test_page_navigation_initial_state(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Verify stack has correct number of pages */
  GtkSelectionModel *pages = adw_view_stack_get_pages(fixture->window->stack);
  guint n_items = g_list_model_get_n_items(G_LIST_MODEL(pages));
  g_assert_cmpuint(n_items, ==, 4);
  g_object_unref(pages);
}

static void test_page_navigation_by_name(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Navigate to each page by name */
  const char *page_names[] = { "permissions", "applications", "sessions", "settings" };

  for (int i = 0; i < 4; i++) {
    adw_view_stack_set_visible_child_name(fixture->window->stack, page_names[i]);
    process_pending_events();

    const char *current = adw_view_stack_get_visible_child_name(fixture->window->stack);
    g_assert_cmpstr(current, ==, page_names[i]);
  }
}

static void test_page_navigation_cycle(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Navigate through all pages in sequence */
  const char *pages[] = { "permissions", "applications", "sessions", "settings" };

  for (int cycle = 0; cycle < 2; cycle++) {
    for (int i = 0; i < 4; i++) {
      adw_view_stack_set_visible_child_name(fixture->window->stack, pages[i]);
      process_pending_events();

      const char *visible = adw_view_stack_get_visible_child_name(fixture->window->stack);
      g_assert_cmpstr(visible, ==, pages[i]);
    }
  }
}

static void test_sidebar_row_selection(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Verify sidebar has correct number of rows */
  int row_count = 0;
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(fixture->window->sidebar, 0);
  while (row != NULL) {
    row_count++;
    row = gtk_list_box_get_row_at_index(fixture->window->sidebar, row_count);
  }
  g_assert_cmpint(row_count, ==, 4);

  /* Select each row */
  for (int i = 0; i < 4; i++) {
    row = gtk_list_box_get_row_at_index(fixture->window->sidebar, i);
    g_assert_nonnull(row);
    gtk_list_box_select_row(fixture->window->sidebar, row);
    process_pending_events();

    /* Verify selection */
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(fixture->window->sidebar);
    g_assert_true(selected == row);
  }
}

/* ===========================================================================
 * Test Cases: Dialog Presentation and Dismissal
 * =========================================================================== */

static void test_approval_dialog_creation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockApprovalDialog *dialog = mock_approval_dialog_new();
  g_assert_nonnull(dialog);
  g_assert_true(ADW_IS_DIALOG(dialog));

  /* Verify dialog components */
  g_assert_nonnull(dialog->approve_btn);
  g_assert_nonnull(dialog->deny_btn);
  g_assert_nonnull(dialog->remember_check);

  /* Verify initial state */
  g_assert_false(dialog->decision_made);
  g_assert_false(dialog->approved);
  g_assert_false(dialog->remember);

  g_object_unref(dialog);
}

static void test_approval_dialog_buttons(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockApprovalDialog *dialog = mock_approval_dialog_new();

  /* Verify approve button has suggested-action style */
  g_assert_true(gtk_widget_has_css_class(dialog->approve_btn, "suggested-action"));

  /* Both buttons should be sensitive */
  g_assert_true(gtk_widget_get_sensitive(dialog->approve_btn));
  g_assert_true(gtk_widget_get_sensitive(dialog->deny_btn));

  g_object_unref(dialog);
}

static void test_create_profile_dialog_creation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();
  g_assert_nonnull(dialog);
  g_assert_true(ADW_IS_DIALOG(dialog));

  /* Verify dialog components */
  g_assert_nonnull(dialog->entry_display_name);
  g_assert_nonnull(dialog->entry_passphrase);
  g_assert_nonnull(dialog->entry_confirm);
  g_assert_nonnull(dialog->btn_create);
  g_assert_nonnull(dialog->btn_cancel);
  g_assert_nonnull(dialog->match_label);

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Form Validation (Password Fields)
 * =========================================================================== */

static void test_password_validation_empty(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Initially, create button should be disabled */
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_create));
  g_assert_false(dialog->passwords_match);
  g_assert_false(dialog->passphrase_valid);

  g_object_unref(dialog);
}

static void test_password_validation_minimum_length(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter display name */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test User");
  process_pending_events();

  /* Enter password shorter than minimum (8 chars) */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "short");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "short");
  process_pending_events();

  /* Should not be valid due to short password */
  g_assert_false(dialog->passphrase_valid);
  g_assert_true(dialog->passwords_match);  /* They match, just too short */
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_create));

  g_object_unref(dialog);
}

static void test_password_validation_mismatch(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter display name */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test User");

  /* Enter different passwords */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "password123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "different456");
  process_pending_events();

  /* Should show mismatch */
  g_assert_true(dialog->passphrase_valid);  /* Length is OK */
  g_assert_false(dialog->passwords_match);
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_create));

  /* Check error message */
  const char *match_text = gtk_label_get_text(GTK_LABEL(dialog->match_label));
  g_assert_cmpstr(match_text, ==, "Passphrases do not match");

  g_object_unref(dialog);
}

static void test_password_validation_match(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter all valid data */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test User");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "validpassword123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "validpassword123");
  process_pending_events();

  /* Should be valid */
  g_assert_true(dialog->passphrase_valid);
  g_assert_true(dialog->passwords_match);
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_create));

  /* Check success message */
  const char *match_text = gtk_label_get_text(GTK_LABEL(dialog->match_label));
  g_assert_cmpstr(match_text, ==, "Passphrases match");

  g_object_unref(dialog);
}

static void test_password_validation_clear_confirm(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter valid data first */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test User");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "validpassword123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "validpassword123");
  process_pending_events();
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_create));

  /* Clear confirm password */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "");
  process_pending_events();

  /* Create button should be disabled */
  g_assert_false(dialog->passwords_match);
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_create));

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Button States (Enabled/Disabled)
 * =========================================================================== */

static void test_create_button_requires_display_name(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter only passwords, no display name */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "validpassword123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "validpassword123");
  process_pending_events();

  /* Create should be disabled without display name */
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_create));

  /* Now add display name */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test User");
  process_pending_events();

  /* Now it should be enabled */
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_create));

  g_object_unref(dialog);
}

static void test_cancel_button_always_enabled(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Cancel should always be enabled regardless of form state */
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_cancel));

  /* Enter partial data */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test");
  process_pending_events();
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_cancel));

  /* Enter all valid data */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "validpassword123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "validpassword123");
  process_pending_events();
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_cancel));

  g_object_unref(dialog);
}

static void test_approval_buttons_state(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockApprovalDialog *dialog = mock_approval_dialog_new();

  /* Both buttons should be enabled initially */
  g_assert_true(gtk_widget_get_sensitive(dialog->approve_btn));
  g_assert_true(gtk_widget_get_sensitive(dialog->deny_btn));

  /* Check button should also be interactive */
  g_assert_true(gtk_widget_get_sensitive(dialog->remember_check));

  g_object_unref(dialog);
}

static void test_remember_checkbox_toggle(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockApprovalDialog *dialog = mock_approval_dialog_new();

  /* Initially unchecked */
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->remember_check)));

  /* Toggle on */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->remember_check), TRUE);
  process_pending_events();
  g_assert_true(gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->remember_check)));

  /* Toggle off */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->remember_check), FALSE);
  process_pending_events();
  g_assert_false(gtk_check_button_get_active(GTK_CHECK_BUTTON(dialog->remember_check)));

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Widget CSS Classes
 * =========================================================================== */

static void test_suggested_action_button_class(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Create button should have suggested-action class */
  g_assert_true(gtk_widget_has_css_class(dialog->btn_create, "suggested-action"));

  /* Cancel button should not have suggested-action */
  g_assert_false(gtk_widget_has_css_class(dialog->btn_cancel, "suggested-action"));

  g_object_unref(dialog);
}

static void test_password_match_success_class(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter matching passwords */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_display_name), "Test User");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "validpassword123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "validpassword123");
  process_pending_events();

  /* Match label should have success class */
  g_assert_true(gtk_widget_has_css_class(dialog->match_label, "success"));
  g_assert_false(gtk_widget_has_css_class(dialog->match_label, "error"));

  g_object_unref(dialog);
}

static void test_password_match_error_class(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  MockCreateProfileDialog *dialog = mock_create_profile_dialog_new();

  /* Enter mismatching passwords */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_passphrase), "password123");
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_confirm), "different456");
  process_pending_events();

  /* Match label should have error class */
  g_assert_true(gtk_widget_has_css_class(dialog->match_label, "error"));
  g_assert_false(gtk_widget_has_css_class(dialog->match_label, "success"));

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Npub Validation
 * =========================================================================== */

/* Helper function to validate npub format */
static gboolean is_valid_npub(const char *npub) {
  if (!npub) return FALSE;
  if (!g_str_has_prefix(npub, "npub1")) return FALSE;
  if (strlen(npub) != 63) return FALSE;  /* npub1 + 58 bech32 chars */

  /* Check that remaining characters are valid bech32 */
  const char *bech32_chars = "023456789acdefghjklmnpqrstuvwxyz";
  for (size_t i = 5; i < strlen(npub); i++) {
    if (strchr(bech32_chars, npub[i]) == NULL) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Helper function to validate nsec format */
static gboolean is_valid_nsec(const char *nsec) {
  if (!nsec) return FALSE;
  if (!g_str_has_prefix(nsec, "nsec1")) return FALSE;
  if (strlen(nsec) != 63) return FALSE;

  const char *bech32_chars = "023456789acdefghjklmnpqrstuvwxyz";
  for (size_t i = 5; i < strlen(nsec); i++) {
    if (strchr(bech32_chars, nsec[i]) == NULL) {
      return FALSE;
    }
  }
  return TRUE;
}

/* Helper function to validate 64-character hex string */
static gboolean is_hex64(const char *s) {
  if (!s) return FALSE;
  if (strlen(s) != 64) return FALSE;
  for (size_t i = 0; i < 64; i++) {
    char c = s[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return FALSE;
    }
  }
  return TRUE;
}

static void test_npub_validation_valid(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Valid npub examples */
  g_assert_true(is_valid_npub("npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_true(is_valid_npub("npub1xtscya34g58tk0z605fvr788k263gsu6cy9x0mhnm87echrgufzsevkk5s"));
}

static void test_npub_validation_invalid_prefix(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Invalid prefix */
  g_assert_false(is_valid_npub("nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_false(is_valid_npub("xpub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_false(is_valid_npub("Npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
}

static void test_npub_validation_invalid_length(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Too short */
  g_assert_false(is_valid_npub("npub1"));
  g_assert_false(is_valid_npub("npub1abc"));

  /* Too long */
  g_assert_false(is_valid_npub("npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq"));
}

static void test_npub_validation_invalid_chars(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Invalid bech32 characters (b, i, o, 1 after prefix) */
  g_assert_false(is_valid_npub("npub1bqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_false(is_valid_npub("npub1iqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_false(is_valid_npub("npub1oqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_false(is_valid_npub("npub11qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
}

static void test_npub_validation_null_empty(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  g_assert_false(is_valid_npub(NULL));
  g_assert_false(is_valid_npub(""));
}

static void test_nsec_validation_valid(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  g_assert_true(is_valid_nsec("nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
}

static void test_nsec_validation_invalid(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  g_assert_false(is_valid_nsec("npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj"));
  g_assert_false(is_valid_nsec(NULL));
  g_assert_false(is_valid_nsec(""));
}

static void test_hex64_validation_valid(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  g_assert_true(is_hex64("0000000000000000000000000000000000000000000000000000000000000000"));
  g_assert_true(is_hex64("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
  g_assert_true(is_hex64("abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789"));
  g_assert_true(is_hex64("ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789"));
}

static void test_hex64_validation_invalid(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Too short */
  g_assert_false(is_hex64("abcdef"));

  /* Too long */
  g_assert_false(is_hex64("00000000000000000000000000000000000000000000000000000000000000000"));

  /* Invalid characters */
  g_assert_false(is_hex64("ghijklmnopqrstuvwxyzabcdef0123456789abcdef0123456789abcdef01234"));

  /* NULL and empty */
  g_assert_false(is_hex64(NULL));
  g_assert_false(is_hex64(""));
}

/* ===========================================================================
 * Test Cases: Mock Import Key Dialog
 * =========================================================================== */

struct _MockImportKeyDialog {
  AdwDialog parent_instance;
  GtkWidget *entry_secret;
  GtkWidget *entry_label;
  GtkWidget *btn_ok;
  GtkWidget *btn_cancel;
  gboolean key_valid;
};

G_DECLARE_FINAL_TYPE(MockImportKeyDialog, mock_import_key_dialog, MOCK, IMPORT_KEY_DIALOG, AdwDialog)
G_DEFINE_TYPE(MockImportKeyDialog, mock_import_key_dialog, ADW_TYPE_DIALOG)

static void validate_key_input(MockImportKeyDialog *self);

static void on_secret_changed(GtkEditable *editable, gpointer user_data) {
  (void)editable;
  MockImportKeyDialog *self = MOCK_IMPORT_KEY_DIALOG(user_data);
  validate_key_input(self);
}

static void validate_key_input(MockImportKeyDialog *self) {
  const char *text = gtk_editable_get_text(GTK_EDITABLE(self->entry_secret));

  /* Validate: nsec1..., ncrypt..., or 64-hex */
  self->key_valid = FALSE;
  if (text && *text) {
    if (g_str_has_prefix(text, "nsec1")) {
      self->key_valid = is_valid_nsec(text);
    } else if (g_str_has_prefix(text, "ncrypt")) {
      /* ncrypt keys have variable length, just check prefix and minimum length */
      self->key_valid = strlen(text) > 10;
    } else {
      self->key_valid = is_hex64(text);
    }
  }

  gtk_widget_set_sensitive(self->btn_ok, self->key_valid);
}

static void mock_import_key_dialog_class_init(MockImportKeyDialogClass *klass) {
  (void)klass;
}

static void mock_import_key_dialog_init(MockImportKeyDialog *self) {
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);

  /* Secret key entry */
  GtkWidget *secret_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *secret_label = gtk_label_new("Private Key (nsec, hex, or ncrypt)");
  gtk_label_set_xalign(GTK_LABEL(secret_label), 0);
  self->entry_secret = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_secret), "nsec1... or 64-hex or ncrypt...");
  g_signal_connect(self->entry_secret, "changed", G_CALLBACK(on_secret_changed), self);
  gtk_box_append(GTK_BOX(secret_box), secret_label);
  gtk_box_append(GTK_BOX(secret_box), self->entry_secret);
  gtk_box_append(GTK_BOX(content), secret_box);

  /* Label entry */
  GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  GtkWidget *label_label = gtk_label_new("Label (optional)");
  gtk_label_set_xalign(GTK_LABEL(label_label), 0);
  self->entry_label = gtk_entry_new();
  gtk_entry_set_placeholder_text(GTK_ENTRY(self->entry_label), "My Key");
  gtk_box_append(GTK_BOX(label_box), label_label);
  gtk_box_append(GTK_BOX(label_box), self->entry_label);
  gtk_box_append(GTK_BOX(content), label_box);

  /* Buttons */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
  gtk_widget_set_margin_top(btn_box, 12);

  self->btn_cancel = gtk_button_new_with_label("Cancel");
  self->btn_ok = gtk_button_new_with_label("Import");
  gtk_widget_add_css_class(self->btn_ok, "suggested-action");
  gtk_widget_set_sensitive(self->btn_ok, FALSE);

  gtk_box_append(GTK_BOX(btn_box), self->btn_cancel);
  gtk_box_append(GTK_BOX(btn_box), self->btn_ok);
  gtk_box_append(GTK_BOX(content), btn_box);

  adw_dialog_set_title(ADW_DIALOG(self), "Import Key");
  adw_dialog_set_content_width(ADW_DIALOG(self), 480);
  adw_dialog_set_child(ADW_DIALOG(self), content);

  self->key_valid = FALSE;
}

static MockImportKeyDialog *mock_import_key_dialog_new(void) {
  return g_object_ref_sink(g_object_new(mock_import_key_dialog_get_type(), NULL));
}

static void test_import_key_dialog_creation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockImportKeyDialog *dialog = mock_import_key_dialog_new();
  g_assert_nonnull(dialog);
  g_assert_true(ADW_IS_DIALOG(dialog));

  g_assert_nonnull(dialog->entry_secret);
  g_assert_nonnull(dialog->entry_label);
  g_assert_nonnull(dialog->btn_ok);
  g_assert_nonnull(dialog->btn_cancel);

  /* Import button should be disabled initially */
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_ok));

  g_object_unref(dialog);
}

static void test_import_key_dialog_nsec_validation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockImportKeyDialog *dialog = mock_import_key_dialog_new();

  /* Valid nsec */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_secret),
                        "nsec1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj");
  process_pending_events();
  g_assert_true(dialog->key_valid);
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_ok));

  /* Invalid nsec (wrong prefix) */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_secret), "npub1abc");
  process_pending_events();
  g_assert_false(dialog->key_valid);
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_ok));

  g_object_unref(dialog);
}

static void test_import_key_dialog_hex_validation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockImportKeyDialog *dialog = mock_import_key_dialog_new();

  /* Valid 64-hex */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_secret),
                        "abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789");
  process_pending_events();
  g_assert_true(dialog->key_valid);
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_ok));

  /* Invalid hex (too short) */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_secret), "abcdef");
  process_pending_events();
  g_assert_false(dialog->key_valid);
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_ok));

  g_object_unref(dialog);
}

static void test_import_key_dialog_ncrypt_validation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockImportKeyDialog *dialog = mock_import_key_dialog_new();

  /* Valid ncrypt (variable length but minimum prefix check) */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_secret), "ncrypt1abcdefghijklmnop");
  process_pending_events();
  g_assert_true(dialog->key_valid);
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_ok));

  /* Too short ncrypt */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_secret), "ncrypt");
  process_pending_events();
  g_assert_false(dialog->key_valid);
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_ok));

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Mock Lock Screen
 * =========================================================================== */

typedef enum {
  MOCK_LOCK_REASON_STARTUP,
  MOCK_LOCK_REASON_MANUAL,
  MOCK_LOCK_REASON_TIMEOUT
} MockLockReason;

struct _MockLockScreen {
  GtkBox parent_instance;
  GtkWidget *entry_password;
  GtkWidget *btn_unlock;
  GtkWidget *lbl_error;
  GtkWidget *lbl_lock_reason;
  MockLockReason lock_reason;
  gboolean busy;
  gboolean password_configured;
};

G_DECLARE_FINAL_TYPE(MockLockScreen, mock_lock_screen, MOCK, LOCK_SCREEN, GtkBox)
G_DEFINE_TYPE(MockLockScreen, mock_lock_screen, GTK_TYPE_BOX)

static void mock_lock_screen_class_init(MockLockScreenClass *klass) {
  (void)klass;
}

static void mock_lock_screen_init(MockLockScreen *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  gtk_widget_set_valign(GTK_WIDGET(self), GTK_ALIGN_CENTER);
  gtk_widget_set_halign(GTK_WIDGET(self), GTK_ALIGN_CENTER);
  gtk_box_set_spacing(GTK_BOX(self), 12);

  /* Lock icon */
  GtkWidget *icon = gtk_image_new_from_icon_name("system-lock-screen-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(icon), 64);
  gtk_box_append(GTK_BOX(self), icon);

  /* Title */
  GtkWidget *title = gtk_label_new("Session Locked");
  gtk_widget_add_css_class(title, "title-1");
  gtk_box_append(GTK_BOX(self), title);

  /* Lock reason */
  self->lbl_lock_reason = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_lock_reason, "dim-label");
  gtk_box_append(GTK_BOX(self), self->lbl_lock_reason);

  /* Password entry */
  self->entry_password = gtk_password_entry_new();
  gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->entry_password), TRUE);
  gtk_widget_set_size_request(self->entry_password, 250, -1);
  gtk_box_append(GTK_BOX(self), self->entry_password);

  /* Error label */
  self->lbl_error = gtk_label_new("");
  gtk_widget_add_css_class(self->lbl_error, "error");
  gtk_widget_set_visible(self->lbl_error, FALSE);
  gtk_box_append(GTK_BOX(self), self->lbl_error);

  /* Unlock button */
  self->btn_unlock = gtk_button_new_with_label("_Unlock");
  gtk_button_set_use_underline(GTK_BUTTON(self->btn_unlock), TRUE);
  gtk_widget_add_css_class(self->btn_unlock, "suggested-action");
  gtk_box_append(GTK_BOX(self), self->btn_unlock);

  self->lock_reason = MOCK_LOCK_REASON_STARTUP;
  self->busy = FALSE;
  self->password_configured = TRUE;
}

static MockLockScreen *mock_lock_screen_new(void) {
  return g_object_ref_sink(g_object_new(mock_lock_screen_get_type(), NULL));
}

static void mock_lock_screen_set_busy(MockLockScreen *self, gboolean busy) {
  self->busy = busy;
  gtk_widget_set_sensitive(self->entry_password, !busy);
  gtk_widget_set_sensitive(self->btn_unlock, !busy);
  if (busy) {
    gtk_button_set_label(GTK_BUTTON(self->btn_unlock), "Unlocking...");
  } else {
    gtk_button_set_label(GTK_BUTTON(self->btn_unlock), "_Unlock");
  }
}

static void mock_lock_screen_show_error(MockLockScreen *self, const char *message) {
  if (message && *message) {
    gtk_label_set_text(GTK_LABEL(self->lbl_error), message);
    gtk_widget_set_visible(self->lbl_error, TRUE);
  } else {
    gtk_widget_set_visible(self->lbl_error, FALSE);
  }
}

static void mock_lock_screen_set_lock_reason(MockLockScreen *self, MockLockReason reason) {
  self->lock_reason = reason;
  const char *text = "Session locked";
  switch (reason) {
    case MOCK_LOCK_REASON_MANUAL: text = "Manually locked"; break;
    case MOCK_LOCK_REASON_TIMEOUT: text = "Locked due to inactivity"; break;
    case MOCK_LOCK_REASON_STARTUP: text = "Session started locked"; break;
  }
  gtk_label_set_text(GTK_LABEL(self->lbl_lock_reason), text);
}

static void test_lock_screen_creation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockLockScreen *lock = mock_lock_screen_new();
  g_assert_nonnull(lock);
  g_assert_true(GTK_IS_BOX(lock));

  g_assert_nonnull(lock->entry_password);
  g_assert_nonnull(lock->btn_unlock);
  g_assert_nonnull(lock->lbl_error);
  g_assert_nonnull(lock->lbl_lock_reason);

  /* Error label should be hidden initially */
  g_assert_false(gtk_widget_get_visible(lock->lbl_error));

  g_object_unref(lock);
}

static void test_lock_screen_busy_state(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockLockScreen *lock = mock_lock_screen_new();

  /* Initially not busy */
  g_assert_false(lock->busy);
  g_assert_true(gtk_widget_get_sensitive(lock->entry_password));
  g_assert_true(gtk_widget_get_sensitive(lock->btn_unlock));

  /* Set busy */
  mock_lock_screen_set_busy(lock, TRUE);
  g_assert_true(lock->busy);
  g_assert_false(gtk_widget_get_sensitive(lock->entry_password));
  g_assert_false(gtk_widget_get_sensitive(lock->btn_unlock));

  /* Clear busy */
  mock_lock_screen_set_busy(lock, FALSE);
  g_assert_false(lock->busy);
  g_assert_true(gtk_widget_get_sensitive(lock->entry_password));
  g_assert_true(gtk_widget_get_sensitive(lock->btn_unlock));

  g_object_unref(lock);
}

static void test_lock_screen_error_display(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockLockScreen *lock = mock_lock_screen_new();

  /* Show error */
  mock_lock_screen_show_error(lock, "Invalid password");
  g_assert_true(gtk_widget_get_visible(lock->lbl_error));
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(lock->lbl_error)), ==, "Invalid password");

  /* Clear error */
  mock_lock_screen_show_error(lock, NULL);
  g_assert_false(gtk_widget_get_visible(lock->lbl_error));

  g_object_unref(lock);
}

static void test_lock_screen_lock_reasons(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockLockScreen *lock = mock_lock_screen_new();

  mock_lock_screen_set_lock_reason(lock, MOCK_LOCK_REASON_MANUAL);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(lock->lbl_lock_reason)), ==, "Manually locked");

  mock_lock_screen_set_lock_reason(lock, MOCK_LOCK_REASON_TIMEOUT);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(lock->lbl_lock_reason)), ==, "Locked due to inactivity");

  mock_lock_screen_set_lock_reason(lock, MOCK_LOCK_REASON_STARTUP);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(lock->lbl_lock_reason)), ==, "Session started locked");

  g_object_unref(lock);
}

/* ===========================================================================
 * Test Cases: Keyboard Shortcuts
 * =========================================================================== */

/* Mock action tracker for keyboard shortcut tests */
typedef struct {
  gboolean new_profile_triggered;
  gboolean import_profile_triggered;
  gboolean export_triggered;
  gboolean lock_triggered;
  gboolean preferences_triggered;
  gboolean quit_triggered;
  gboolean about_triggered;
} MockActionTracker;

static MockActionTracker action_tracker = { FALSE };

static void reset_action_tracker(void) {
  memset(&action_tracker, 0, sizeof(MockActionTracker));
}

/* Test action callbacks */
static void test_action_new_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.new_profile_triggered = TRUE;
}

static void test_action_import_profile(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.import_profile_triggered = TRUE;
}

static void test_action_export(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.export_triggered = TRUE;
}

static void test_action_lock(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.lock_triggered = TRUE;
}

static void test_action_preferences(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.preferences_triggered = TRUE;
}

static void test_action_quit(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.quit_triggered = TRUE;
}

static void test_action_about(GSimpleAction *action, GVariant *param, gpointer user_data) {
  (void)action; (void)param; (void)user_data;
  action_tracker.about_triggered = TRUE;
}

static void setup_test_actions(GtkWindow *window) {
  static const GActionEntry entries[] = {
    { "new-profile", test_action_new_profile, NULL, NULL, NULL },
    { "import-profile", test_action_import_profile, NULL, NULL, NULL },
    { "export", test_action_export, NULL, NULL, NULL },
    { "lock", test_action_lock, NULL, NULL, NULL },
    { "preferences", test_action_preferences, NULL, NULL, NULL },
    { "quit", test_action_quit, NULL, NULL, NULL },
    { "about", test_action_about, NULL, NULL, NULL },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(window), entries, G_N_ELEMENTS(entries), window);
}

static void test_keyboard_shortcuts_action_registration(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Setup actions on the window */
  setup_test_actions(GTK_WINDOW(fixture->window));
  reset_action_tracker();

  /* Verify actions are registered */
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "new-profile");
  g_assert_nonnull(action);

  action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "import-profile");
  g_assert_nonnull(action);

  action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "export");
  g_assert_nonnull(action);

  action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "lock");
  g_assert_nonnull(action);

  action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "preferences");
  g_assert_nonnull(action);

  action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "quit");
  g_assert_nonnull(action);

  action = g_action_map_lookup_action(G_ACTION_MAP(fixture->window), "about");
  g_assert_nonnull(action);
}

static void test_keyboard_shortcuts_action_activation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  setup_test_actions(GTK_WINDOW(fixture->window));
  reset_action_tracker();

  /* Test activating actions directly (simulating keyboard shortcuts) */
  g_action_group_activate_action(G_ACTION_GROUP(fixture->window), "new-profile", NULL);
  process_pending_events();
  g_assert_true(action_tracker.new_profile_triggered);

  reset_action_tracker();
  g_action_group_activate_action(G_ACTION_GROUP(fixture->window), "import-profile", NULL);
  process_pending_events();
  g_assert_true(action_tracker.import_profile_triggered);

  reset_action_tracker();
  g_action_group_activate_action(G_ACTION_GROUP(fixture->window), "export", NULL);
  process_pending_events();
  g_assert_true(action_tracker.export_triggered);

  reset_action_tracker();
  g_action_group_activate_action(G_ACTION_GROUP(fixture->window), "lock", NULL);
  process_pending_events();
  g_assert_true(action_tracker.lock_triggered);

  reset_action_tracker();
  g_action_group_activate_action(G_ACTION_GROUP(fixture->window), "preferences", NULL);
  process_pending_events();
  g_assert_true(action_tracker.preferences_triggered);

  reset_action_tracker();
  g_action_group_activate_action(G_ACTION_GROUP(fixture->window), "about", NULL);
  process_pending_events();
  g_assert_true(action_tracker.about_triggered);
}

/* ===========================================================================
 * Test Cases: Window Layout and Sizing
 * =========================================================================== */

static void test_window_minimum_size(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Set a small size and verify window accepts it */
  gtk_window_set_default_size(GTK_WINDOW(fixture->window), 400, 300);
  process_pending_events();

  int width, height;
  gtk_window_get_default_size(GTK_WINDOW(fixture->window), &width, &height);

  /* Window should accept reasonable minimum sizes */
  g_assert_cmpint(width, >=, 100);
  g_assert_cmpint(height, >=, 100);
}

static void test_window_default_size(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  int width, height;
  gtk_window_get_default_size(GTK_WINDOW(fixture->window), &width, &height);

  /* Default size should be 920x640 as set in fixture */
  g_assert_cmpint(width, ==, 920);
  g_assert_cmpint(height, ==, 640);
}

static void test_sidebar_width(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Find the scrolled window containing sidebar */
  GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(fixture->window->sidebar));
  g_assert_nonnull(parent);

  int min_width, nat_width;
  gtk_widget_measure(parent, GTK_ORIENTATION_HORIZONTAL, -1, &min_width, &nat_width, NULL, NULL);

  /* Sidebar should have reasonable width */
  g_assert_cmpint(min_width, >=, 100);
}

static void test_stack_expands_horizontally(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Stack should be set to expand horizontally */
  g_assert_true(gtk_widget_get_hexpand(GTK_WIDGET(fixture->window->stack)));
}

/* ===========================================================================
 * Test Cases: Approval Dialog Event Types
 * =========================================================================== */

/* Extended approval dialog with event type support */
struct _MockApprovalDialogExt {
  AdwDialog parent_instance;
  GtkWidget *approve_btn;
  GtkWidget *deny_btn;
  GtkWidget *remember_check;
  GtkWidget *event_type_label;
  GtkWidget *event_icon;
  GtkWidget *ttl_dropdown;
  gboolean decision_made;
  gboolean approved;
  gboolean remember;
  int event_kind;
  guint64 ttl_seconds;
};

G_DECLARE_FINAL_TYPE(MockApprovalDialogExt, mock_approval_dialog_ext, MOCK, APPROVAL_DIALOG_EXT, AdwDialog)
G_DEFINE_TYPE(MockApprovalDialogExt, mock_approval_dialog_ext, ADW_TYPE_DIALOG)

static const char *mock_get_event_type_name(int kind) {
  switch (kind) {
    case 0: return "Metadata";
    case 1: return "Short Text Note";
    case 3: return "Contacts";
    case 4: return "Encrypted Direct Message";
    case 6: return "Repost";
    case 7: return "Reaction";
    case 9734: return "Zap Request";
    case 9735: return "Zap";
    case 22242: return "Client Authentication";
    case 24133: return "Nostr Connect";
    case 30023: return "Long-form Content";
    default: return "Unknown Event";
  }
}

static void mock_approval_dialog_ext_set_event_type(MockApprovalDialogExt *self, int kind) {
  self->event_kind = kind;
  const char *name = mock_get_event_type_name(kind);
  gchar *label = g_strdup_printf("%s (kind %d)", name, kind);
  gtk_label_set_text(GTK_LABEL(self->event_type_label), label);
  g_free(label);
}

static void on_remember_toggled_ext(GtkCheckButton *btn, gpointer user_data) {
  MockApprovalDialogExt *self = MOCK_APPROVAL_DIALOG_EXT(user_data);
  gboolean active = gtk_check_button_get_active(btn);
  gtk_widget_set_sensitive(self->ttl_dropdown, active);
}

static void mock_approval_dialog_ext_class_init(MockApprovalDialogExtClass *klass) {
  (void)klass;
}

static void mock_approval_dialog_ext_init(MockApprovalDialogExt *self) {
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 12);
  gtk_widget_set_margin_end(content, 12);
  gtk_widget_set_margin_top(content, 12);
  gtk_widget_set_margin_bottom(content, 12);

  /* Event type display */
  GtkWidget *type_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  self->event_icon = gtk_image_new_from_icon_name("mail-unread-symbolic");
  self->event_type_label = gtk_label_new("Unknown Event");
  gtk_box_append(GTK_BOX(type_box), self->event_icon);
  gtk_box_append(GTK_BOX(type_box), self->event_type_label);
  gtk_box_append(GTK_BOX(content), type_box);

  /* Remember checkbox */
  self->remember_check = gtk_check_button_new_with_label("Remember this decision");
  g_signal_connect(self->remember_check, "toggled", G_CALLBACK(on_remember_toggled_ext), self);
  gtk_box_append(GTK_BOX(content), self->remember_check);

  /* TTL dropdown */
  GtkStringList *ttl_model = gtk_string_list_new(NULL);
  gtk_string_list_append(ttl_model, "10 minutes");
  gtk_string_list_append(ttl_model, "1 hour");
  gtk_string_list_append(ttl_model, "24 hours");
  gtk_string_list_append(ttl_model, "Forever");
  self->ttl_dropdown = gtk_drop_down_new(G_LIST_MODEL(ttl_model), NULL);
  gtk_widget_set_sensitive(self->ttl_dropdown, FALSE);
  gtk_box_append(GTK_BOX(content), self->ttl_dropdown);

  /* Buttons */
  GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(btn_box, GTK_ALIGN_END);

  self->deny_btn = gtk_button_new_with_label("Deny");
  gtk_widget_add_css_class(self->deny_btn, "destructive-action");
  self->approve_btn = gtk_button_new_with_label("Approve");
  gtk_widget_add_css_class(self->approve_btn, "suggested-action");

  gtk_box_append(GTK_BOX(btn_box), self->deny_btn);
  gtk_box_append(GTK_BOX(btn_box), self->approve_btn);
  gtk_box_append(GTK_BOX(content), btn_box);

  adw_dialog_set_title(ADW_DIALOG(self), "Signing Request");
  adw_dialog_set_child(ADW_DIALOG(self), content);

  self->event_kind = 0;
  self->decision_made = FALSE;
  self->approved = FALSE;
  self->remember = FALSE;
  self->ttl_seconds = 0;
}

static MockApprovalDialogExt *mock_approval_dialog_ext_new(void) {
  return g_object_ref_sink(g_object_new(mock_approval_dialog_ext_get_type(), NULL));
}

static void test_approval_dialog_event_types(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockApprovalDialogExt *dialog = mock_approval_dialog_ext_new();

  /* Test various event kinds */
  mock_approval_dialog_ext_set_event_type(dialog, 1);
  const char *text = gtk_label_get_text(GTK_LABEL(dialog->event_type_label));
  g_assert_true(g_str_has_prefix(text, "Short Text Note"));

  mock_approval_dialog_ext_set_event_type(dialog, 4);
  text = gtk_label_get_text(GTK_LABEL(dialog->event_type_label));
  g_assert_true(g_str_has_prefix(text, "Encrypted Direct Message"));

  mock_approval_dialog_ext_set_event_type(dialog, 9735);
  text = gtk_label_get_text(GTK_LABEL(dialog->event_type_label));
  g_assert_true(g_str_has_prefix(text, "Zap"));

  mock_approval_dialog_ext_set_event_type(dialog, 99999);
  text = gtk_label_get_text(GTK_LABEL(dialog->event_type_label));
  g_assert_true(g_str_has_prefix(text, "Unknown Event"));

  g_object_unref(dialog);
}

static void test_approval_dialog_ttl_dropdown(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockApprovalDialogExt *dialog = mock_approval_dialog_ext_new();

  /* TTL dropdown should be disabled initially */
  g_assert_false(gtk_widget_get_sensitive(dialog->ttl_dropdown));

  /* Enable remember */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->remember_check), TRUE);
  process_pending_events();

  /* Now TTL dropdown should be enabled */
  g_assert_true(gtk_widget_get_sensitive(dialog->ttl_dropdown));

  /* Disable remember */
  gtk_check_button_set_active(GTK_CHECK_BUTTON(dialog->remember_check), FALSE);
  process_pending_events();

  /* TTL dropdown should be disabled again */
  g_assert_false(gtk_widget_get_sensitive(dialog->ttl_dropdown));

  g_object_unref(dialog);
}

static void test_approval_dialog_button_styles(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockApprovalDialogExt *dialog = mock_approval_dialog_ext_new();

  /* Approve should have suggested-action */
  g_assert_true(gtk_widget_has_css_class(dialog->approve_btn, "suggested-action"));

  /* Deny should have destructive-action */
  g_assert_true(gtk_widget_has_css_class(dialog->deny_btn, "destructive-action"));

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Mock D-Bus Service for UI Tests
 * =========================================================================== */

/* Simple mock for D-Bus connection state */
typedef struct {
  gboolean connected;
  gchar *stored_npub;
  gchar *error_message;
} MockDBusState;

static MockDBusState mock_dbus = { FALSE, NULL, NULL };

static void mock_dbus_init(void) {
  mock_dbus.connected = TRUE;
  mock_dbus.stored_npub = g_strdup("npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj");
  mock_dbus.error_message = NULL;
}

static void mock_dbus_cleanup(void) {
  mock_dbus.connected = FALSE;
  g_free(mock_dbus.stored_npub);
  mock_dbus.stored_npub = NULL;
  g_free(mock_dbus.error_message);
  mock_dbus.error_message = NULL;
}

static gboolean mock_dbus_get_public_key(gchar **npub_out) {
  if (!mock_dbus.connected) return FALSE;
  *npub_out = g_strdup(mock_dbus.stored_npub);
  return TRUE;
}

static gboolean mock_dbus_sign_event(const gchar *event_json, gchar **signature_out) {
  if (!mock_dbus.connected) return FALSE;
  if (!event_json || !*event_json) return FALSE;

  /* Generate mock signature (128 hex chars) */
  *signature_out = g_strnfill(128, 'a');
  return TRUE;
}

static void test_mock_dbus_connection(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  mock_dbus_init();
  g_assert_true(mock_dbus.connected);

  gchar *npub = NULL;
  g_assert_true(mock_dbus_get_public_key(&npub));
  g_assert_nonnull(npub);
  g_assert_true(g_str_has_prefix(npub, "npub1"));
  g_free(npub);

  mock_dbus_cleanup();
  g_assert_false(mock_dbus.connected);
}

static void test_mock_dbus_sign_event(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  mock_dbus_init();

  gchar *sig = NULL;
  g_assert_true(mock_dbus_sign_event("{\"content\":\"test\"}", &sig));
  g_assert_nonnull(sig);
  g_assert_cmpuint(strlen(sig), ==, 128);
  g_free(sig);

  /* Empty event should fail */
  g_assert_false(mock_dbus_sign_event("", &sig));

  mock_dbus_cleanup();
}

static void test_mock_dbus_disconnected(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Don't call init - simulate disconnected state */
  mock_dbus.connected = FALSE;

  gchar *npub = NULL;
  g_assert_false(mock_dbus_get_public_key(&npub));

  gchar *sig = NULL;
  g_assert_false(mock_dbus_sign_event("{}", &sig));
}

/* ===========================================================================
 * Test Cases: High Contrast Theme Support
 * =========================================================================== */

static void test_high_contrast_css_class_applied(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Verify that high-contrast class can be added to window */
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "high-contrast");
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "high-contrast"));

  /* Verify it can be removed */
  gtk_widget_remove_css_class(GTK_WIDGET(fixture->window), "high-contrast");
  g_assert_false(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "high-contrast"));
}

static void test_high_contrast_variant_inverted(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Test inverted variant (white on black) */
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "high-contrast");
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "inverted");

  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "high-contrast"));
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "inverted"));

  gtk_widget_remove_css_class(GTK_WIDGET(fixture->window), "inverted");
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "high-contrast"));
  g_assert_false(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "inverted"));
}

static void test_high_contrast_variant_yellow_on_black(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Test yellow-on-black variant */
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "high-contrast");
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "yellow-on-black");

  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "high-contrast"));
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(fixture->window), "yellow-on-black"));
}

static void test_high_contrast_focus_indicators(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Create a button to test focus indicator visibility in high contrast */
  GtkWidget *btn = gtk_button_new_with_label("Test Button");
  gtk_window_set_child(GTK_WINDOW(fixture->window), btn);

  /* In high contrast, focus indicators should be visible (3px dotted rings) */
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "high-contrast");

  /* Verify button is accessible and can grab focus */
  g_assert_true(gtk_widget_get_can_focus(btn));
  g_assert_true(gtk_widget_get_focusable(btn));
}

static void test_high_contrast_button_styles(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *normal_btn = gtk_button_new_with_label("Normal");
  GtkWidget *suggested_btn = gtk_button_new_with_label("Suggested");
  GtkWidget *destructive_btn = gtk_button_new_with_label("Destructive");

  gtk_widget_add_css_class(suggested_btn, "suggested-action");
  gtk_widget_add_css_class(destructive_btn, "destructive-action");

  gtk_box_append(GTK_BOX(box), normal_btn);
  gtk_box_append(GTK_BOX(box), suggested_btn);
  gtk_box_append(GTK_BOX(box), destructive_btn);
  gtk_window_set_child(GTK_WINDOW(fixture->window), box);

  /* Add high contrast class to window */
  gtk_widget_add_css_class(GTK_WIDGET(fixture->window), "high-contrast");

  /* Verify all buttons still have their semantic classes */
  g_assert_true(gtk_widget_has_css_class(suggested_btn, "suggested-action"));
  g_assert_true(gtk_widget_has_css_class(destructive_btn, "destructive-action"));
}

static void test_high_contrast_adw_style_manager(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  /* Test AdwStyleManager high-contrast property access */
  AdwStyleManager *style_manager = adw_style_manager_get_default();
  g_assert_nonnull(style_manager);

  /* Get current high contrast state (may be false in test environment) */
  gboolean system_hc = adw_style_manager_get_high_contrast(style_manager);
  /* This is a read-only property reflecting system state, so we just verify it's accessible */
  g_assert_true(system_hc == TRUE || system_hc == FALSE);
}

static void test_high_contrast_color_scheme_integration(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  AdwStyleManager *style_manager = adw_style_manager_get_default();
  g_assert_nonnull(style_manager);

  /* Save original scheme */
  AdwColorScheme original = adw_style_manager_get_color_scheme(style_manager);

  /* Test that we can set force-light (used with high contrast black-on-white) */
  adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
  g_assert_cmpint(adw_style_manager_get_color_scheme(style_manager), ==, ADW_COLOR_SCHEME_FORCE_LIGHT);

  /* Restore original */
  adw_style_manager_set_color_scheme(style_manager, original);
}

/* ===========================================================================
 * Test Cases: Mock Backup Dialog
 * =========================================================================== */

struct _MockBackupDialog {
  AdwDialog parent_instance;
  GtkWidget *tab_switcher;
  GtkWidget *backup_tab;
  GtkWidget *recovery_tab;
  GtkWidget *entry_npub;
  GtkWidget *entry_password;
  GtkWidget *btn_export;
  GtkWidget *btn_copy;
  GtkWidget *btn_import;
  GtkWidget *qr_view;
  GtkWidget *export_format_dropdown;
  GtkWidget *mnemonic_view;
  gboolean export_ready;
  gboolean password_valid;
  gchar *current_npub;
};

G_DECLARE_FINAL_TYPE(MockBackupDialog, mock_backup_dialog, MOCK, BACKUP_DIALOG, AdwDialog)
G_DEFINE_TYPE(MockBackupDialog, mock_backup_dialog, ADW_TYPE_DIALOG)

static void mock_backup_dialog_class_init(MockBackupDialogClass *klass) {
  (void)klass;
}

static void on_backup_password_changed(GtkEditable *editable, gpointer user_data) {
  MockBackupDialog *self = MOCK_BACKUP_DIALOG(user_data);
  const char *password = gtk_editable_get_text(editable);
  /* Password must be at least 8 characters for NIP-49 export */
  self->password_valid = password && strlen(password) >= 8;
  gtk_widget_set_sensitive(self->btn_export, self->password_valid && self->current_npub != NULL);
}

static void mock_backup_dialog_init(MockBackupDialog *self) {
  GtkWidget *content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(content, 16);
  gtk_widget_set_margin_end(content, 16);
  gtk_widget_set_margin_top(content, 16);
  gtk_widget_set_margin_bottom(content, 16);

  /* Tab switcher for backup/recovery */
  self->tab_switcher = gtk_stack_new();
  gtk_stack_set_transition_type(GTK_STACK(self->tab_switcher), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);

  /* Backup tab content */
  self->backup_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *backup_label = gtk_label_new("Export your key backup");
  gtk_box_append(GTK_BOX(self->backup_tab), backup_label);

  /* Export format dropdown */
  GtkStringList *format_model = gtk_string_list_new(NULL);
  gtk_string_list_append(format_model, "NIP-49 Encrypted (ncryptsec)");
  gtk_string_list_append(format_model, "Mnemonic Words (BIP-39)");
  gtk_string_list_append(format_model, "Raw nsec (Unencrypted - Dangerous!)");
  self->export_format_dropdown = gtk_drop_down_new(G_LIST_MODEL(format_model), NULL);
  gtk_box_append(GTK_BOX(self->backup_tab), self->export_format_dropdown);

  /* Password entry for encrypted export */
  self->entry_password = gtk_password_entry_new();
  gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(self->entry_password), TRUE);
  g_signal_connect(self->entry_password, "changed", G_CALLBACK(on_backup_password_changed), self);
  gtk_box_append(GTK_BOX(self->backup_tab), self->entry_password);

  /* QR code display area */
  self->qr_view = gtk_image_new_from_icon_name("qr-code-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(self->qr_view), 200);
  gtk_box_append(GTK_BOX(self->backup_tab), self->qr_view);

  /* Export and copy buttons */
  GtkWidget *export_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_widget_set_halign(export_btns, GTK_ALIGN_END);
  self->btn_export = gtk_button_new_with_label("Export to File");
  gtk_widget_add_css_class(self->btn_export, "suggested-action");
  gtk_widget_set_sensitive(self->btn_export, FALSE);
  self->btn_copy = gtk_button_new_with_label("Copy to Clipboard");
  gtk_box_append(GTK_BOX(export_btns), self->btn_copy);
  gtk_box_append(GTK_BOX(export_btns), self->btn_export);
  gtk_box_append(GTK_BOX(self->backup_tab), export_btns);

  gtk_stack_add_titled(GTK_STACK(self->tab_switcher), self->backup_tab, "backup", "Backup");

  /* Recovery tab content */
  self->recovery_tab = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  GtkWidget *recovery_label = gtk_label_new("Import from backup");
  gtk_box_append(GTK_BOX(self->recovery_tab), recovery_label);

  /* Mnemonic display/input area */
  self->mnemonic_view = gtk_text_view_new();
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(self->mnemonic_view), GTK_WRAP_WORD);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(self->mnemonic_view), TRUE);
  GtkWidget *mnemonic_scroll = gtk_scrolled_window_new();
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(mnemonic_scroll), self->mnemonic_view);
  gtk_widget_set_size_request(mnemonic_scroll, -1, 100);
  gtk_box_append(GTK_BOX(self->recovery_tab), mnemonic_scroll);

  /* Import button */
  self->btn_import = gtk_button_new_with_label("Import Key");
  gtk_widget_add_css_class(self->btn_import, "suggested-action");
  gtk_widget_set_halign(self->btn_import, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(self->recovery_tab), self->btn_import);

  gtk_stack_add_titled(GTK_STACK(self->tab_switcher), self->recovery_tab, "recovery", "Recovery");

  /* Stack switcher for tabs */
  GtkWidget *switcher = gtk_stack_switcher_new();
  gtk_stack_switcher_set_stack(GTK_STACK_SWITCHER(switcher), GTK_STACK(self->tab_switcher));
  gtk_widget_set_halign(switcher, GTK_ALIGN_CENTER);
  gtk_box_append(GTK_BOX(content), switcher);
  gtk_box_append(GTK_BOX(content), self->tab_switcher);

  adw_dialog_set_title(ADW_DIALOG(self), "Backup & Recovery");
  adw_dialog_set_content_width(ADW_DIALOG(self), 500);
  adw_dialog_set_content_height(ADW_DIALOG(self), 450);
  adw_dialog_set_child(ADW_DIALOG(self), content);

  self->export_ready = FALSE;
  self->password_valid = FALSE;
  self->current_npub = NULL;
}

static MockBackupDialog *mock_backup_dialog_new(void) {
  return g_object_ref_sink(g_object_new(mock_backup_dialog_get_type(), NULL));
}

static void mock_backup_dialog_set_account(MockBackupDialog *self, const gchar *npub) {
  g_free(self->current_npub);
  self->current_npub = g_strdup(npub);
  /* Update export button sensitivity */
  gtk_widget_set_sensitive(self->btn_export, self->password_valid && self->current_npub != NULL);
}

static void mock_backup_dialog_show_backup_tab(MockBackupDialog *self) {
  gtk_stack_set_visible_child_name(GTK_STACK(self->tab_switcher), "backup");
}

static void mock_backup_dialog_show_recovery_tab(MockBackupDialog *self) {
  gtk_stack_set_visible_child_name(GTK_STACK(self->tab_switcher), "recovery");
}

static void test_backup_dialog_creation(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockBackupDialog *dialog = mock_backup_dialog_new();
  g_assert_nonnull(dialog);
  g_assert_true(ADW_IS_DIALOG(dialog));

  /* Verify dialog components exist */
  g_assert_nonnull(dialog->tab_switcher);
  g_assert_nonnull(dialog->backup_tab);
  g_assert_nonnull(dialog->recovery_tab);
  g_assert_nonnull(dialog->entry_password);
  g_assert_nonnull(dialog->btn_export);
  g_assert_nonnull(dialog->btn_copy);
  g_assert_nonnull(dialog->btn_import);
  g_assert_nonnull(dialog->qr_view);
  g_assert_nonnull(dialog->export_format_dropdown);
  g_assert_nonnull(dialog->mnemonic_view);

  g_object_unref(dialog);
}

static void test_backup_dialog_tab_switching(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockBackupDialog *dialog = mock_backup_dialog_new();

  /* Default should be backup tab */
  mock_backup_dialog_show_backup_tab(dialog);
  process_pending_events();
  g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(dialog->tab_switcher)), ==, "backup");

  /* Switch to recovery tab */
  mock_backup_dialog_show_recovery_tab(dialog);
  process_pending_events();
  g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(dialog->tab_switcher)), ==, "recovery");

  /* Switch back to backup tab */
  mock_backup_dialog_show_backup_tab(dialog);
  process_pending_events();
  g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(dialog->tab_switcher)), ==, "backup");

  g_object_unref(dialog);
}

static void test_backup_dialog_export_button_state(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockBackupDialog *dialog = mock_backup_dialog_new();

  /* Export button should be disabled initially */
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_export));

  /* Set account but no password */
  mock_backup_dialog_set_account(dialog, "npub1qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq5gj7aj");
  process_pending_events();
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_export));

  /* Enter short password (less than 8 chars) */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_password), "short");
  process_pending_events();
  g_assert_false(gtk_widget_get_sensitive(dialog->btn_export));

  /* Enter valid password (8+ chars) */
  gtk_editable_set_text(GTK_EDITABLE(dialog->entry_password), "validpassword123");
  process_pending_events();
  g_assert_true(gtk_widget_get_sensitive(dialog->btn_export));

  g_object_unref(dialog);
}

static void test_backup_dialog_export_format_options(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockBackupDialog *dialog = mock_backup_dialog_new();

  /* Verify dropdown has expected number of items */
  GListModel *model = gtk_drop_down_get_model(GTK_DROP_DOWN(dialog->export_format_dropdown));
  g_assert_nonnull(model);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, 3);

  /* Test selecting different formats */
  gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->export_format_dropdown), 0);
  g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->export_format_dropdown)), ==, 0);

  gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->export_format_dropdown), 1);
  g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->export_format_dropdown)), ==, 1);

  gtk_drop_down_set_selected(GTK_DROP_DOWN(dialog->export_format_dropdown), 2);
  g_assert_cmpuint(gtk_drop_down_get_selected(GTK_DROP_DOWN(dialog->export_format_dropdown)), ==, 2);

  g_object_unref(dialog);
}

static void test_backup_dialog_mnemonic_input(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockBackupDialog *dialog = mock_backup_dialog_new();

  /* Switch to recovery tab */
  mock_backup_dialog_show_recovery_tab(dialog);
  process_pending_events();

  /* Verify mnemonic view is editable */
  g_assert_true(gtk_text_view_get_editable(GTK_TEXT_VIEW(dialog->mnemonic_view)));

  /* Enter mnemonic text */
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(dialog->mnemonic_view));
  gtk_text_buffer_set_text(buffer, "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", -1);
  process_pending_events();

  /* Verify text was entered */
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  gchar *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_nonnull(text);
  g_assert_true(g_str_has_prefix(text, "abandon"));
  g_free(text);

  g_object_unref(dialog);
}

/* ===========================================================================
 * Test Cases: Sidebar-to-Page Synchronization
 * =========================================================================== */

/* Extended fixture with sidebar-page sync callback */
static gchar *g_last_selected_page = NULL;

static void on_sidebar_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
  MockSignerWindow *window = MOCK_SIGNER_WINDOW(user_data);
  int index = gtk_list_box_row_get_index(row);
  const char *pages[] = { "permissions", "applications", "sessions", "settings" };

  if (index >= 0 && index < 4) {
    adw_view_stack_set_visible_child_name(window->stack, pages[index]);
    g_free(g_last_selected_page);
    g_last_selected_page = g_strdup(pages[index]);
  }
  (void)box;
}

static void test_sidebar_page_sync_on_row_click(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Connect sidebar row activation to page change */
  g_signal_connect(fixture->window->sidebar, "row-activated",
                   G_CALLBACK(on_sidebar_row_activated), fixture->window);

  /* Click each sidebar row and verify page changes */
  const char *expected_pages[] = { "permissions", "applications", "sessions", "settings" };

  for (int i = 0; i < 4; i++) {
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(fixture->window->sidebar, i);
    g_assert_nonnull(row);

    /* Emit row-activated signal (simulating click) */
    g_signal_emit_by_name(fixture->window->sidebar, "row-activated", row);
    process_pending_events();

    /* Verify stack changed to expected page */
    const char *visible = adw_view_stack_get_visible_child_name(fixture->window->stack);
    g_assert_cmpstr(visible, ==, expected_pages[i]);
  }

  g_free(g_last_selected_page);
  g_last_selected_page = NULL;
}

static void test_sidebar_page_sync_bidirectional(TestUIFixture *fixture, gconstpointer user_data) {
  (void)user_data;

  /* Connect sidebar to stack */
  g_signal_connect(fixture->window->sidebar, "row-activated",
                   G_CALLBACK(on_sidebar_row_activated), fixture->window);

  /* Change page programmatically and verify */
  adw_view_stack_set_visible_child_name(fixture->window->stack, "settings");
  process_pending_events();
  g_assert_cmpstr(adw_view_stack_get_visible_child_name(fixture->window->stack), ==, "settings");

  /* Now click sidebar to change to different page */
  GtkListBoxRow *row = gtk_list_box_get_row_at_index(fixture->window->sidebar, 0); /* permissions */
  g_signal_emit_by_name(fixture->window->sidebar, "row-activated", row);
  process_pending_events();
  g_assert_cmpstr(adw_view_stack_get_visible_child_name(fixture->window->stack), ==, "permissions");
}

/* ===========================================================================
 * Test Cases: Button States Based on Authentication
 * =========================================================================== */

typedef enum {
  MOCK_AUTH_STATE_LOCKED,
  MOCK_AUTH_STATE_UNLOCKED,
  MOCK_AUTH_STATE_NO_PROFILE
} MockAuthState;

struct _MockAuthAwareToolbar {
  GtkBox parent_instance;
  GtkWidget *btn_sign;
  GtkWidget *btn_new_identity;
  GtkWidget *btn_import;
  GtkWidget *btn_export;
  GtkWidget *btn_lock;
  GtkWidget *btn_settings;
  MockAuthState auth_state;
};

G_DECLARE_FINAL_TYPE(MockAuthAwareToolbar, mock_auth_aware_toolbar, MOCK, AUTH_AWARE_TOOLBAR, GtkBox)
G_DEFINE_TYPE(MockAuthAwareToolbar, mock_auth_aware_toolbar, GTK_TYPE_BOX)

static void mock_auth_aware_toolbar_class_init(MockAuthAwareToolbarClass *klass) {
  (void)klass;
}

static void mock_auth_aware_toolbar_init(MockAuthAwareToolbar *self) {
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_HORIZONTAL);
  gtk_box_set_spacing(GTK_BOX(self), 8);

  self->btn_sign = gtk_button_new_with_label("Sign Event");
  self->btn_new_identity = gtk_button_new_with_label("New Identity");
  self->btn_import = gtk_button_new_with_label("Import");
  self->btn_export = gtk_button_new_with_label("Export");
  self->btn_lock = gtk_button_new_with_label("Lock");
  self->btn_settings = gtk_button_new_with_label("Settings");

  gtk_box_append(GTK_BOX(self), self->btn_sign);
  gtk_box_append(GTK_BOX(self), self->btn_new_identity);
  gtk_box_append(GTK_BOX(self), self->btn_import);
  gtk_box_append(GTK_BOX(self), self->btn_export);
  gtk_box_append(GTK_BOX(self), self->btn_lock);
  gtk_box_append(GTK_BOX(self), self->btn_settings);

  self->auth_state = MOCK_AUTH_STATE_LOCKED;
}

static MockAuthAwareToolbar *mock_auth_aware_toolbar_new(void) {
  return g_object_ref_sink(g_object_new(mock_auth_aware_toolbar_get_type(), NULL));
}

static void mock_auth_aware_toolbar_set_auth_state(MockAuthAwareToolbar *self, MockAuthState state) {
  self->auth_state = state;

  switch (state) {
    case MOCK_AUTH_STATE_LOCKED:
      /* When locked: only unlock-related actions available */
      gtk_widget_set_sensitive(self->btn_sign, FALSE);
      gtk_widget_set_sensitive(self->btn_new_identity, FALSE);
      gtk_widget_set_sensitive(self->btn_import, FALSE);
      gtk_widget_set_sensitive(self->btn_export, FALSE);
      gtk_widget_set_sensitive(self->btn_lock, FALSE);
      gtk_widget_set_sensitive(self->btn_settings, FALSE);
      break;

    case MOCK_AUTH_STATE_UNLOCKED:
      /* When unlocked: full access */
      gtk_widget_set_sensitive(self->btn_sign, TRUE);
      gtk_widget_set_sensitive(self->btn_new_identity, TRUE);
      gtk_widget_set_sensitive(self->btn_import, TRUE);
      gtk_widget_set_sensitive(self->btn_export, TRUE);
      gtk_widget_set_sensitive(self->btn_lock, TRUE);
      gtk_widget_set_sensitive(self->btn_settings, TRUE);
      break;

    case MOCK_AUTH_STATE_NO_PROFILE:
      /* No profile: can create/import, but not sign/export */
      gtk_widget_set_sensitive(self->btn_sign, FALSE);
      gtk_widget_set_sensitive(self->btn_new_identity, TRUE);
      gtk_widget_set_sensitive(self->btn_import, TRUE);
      gtk_widget_set_sensitive(self->btn_export, FALSE);
      gtk_widget_set_sensitive(self->btn_lock, FALSE);
      gtk_widget_set_sensitive(self->btn_settings, TRUE);
      break;
  }
}

static void test_auth_state_locked_button_states(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockAuthAwareToolbar *toolbar = mock_auth_aware_toolbar_new();

  mock_auth_aware_toolbar_set_auth_state(toolbar, MOCK_AUTH_STATE_LOCKED);
  process_pending_events();

  /* All buttons should be disabled when locked */
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_sign));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_new_identity));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_import));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_export));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_lock));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_settings));

  g_object_unref(toolbar);
}

static void test_auth_state_unlocked_button_states(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockAuthAwareToolbar *toolbar = mock_auth_aware_toolbar_new();

  mock_auth_aware_toolbar_set_auth_state(toolbar, MOCK_AUTH_STATE_UNLOCKED);
  process_pending_events();

  /* All buttons should be enabled when unlocked */
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_sign));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_new_identity));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_import));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_export));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_lock));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_settings));

  g_object_unref(toolbar);
}

static void test_auth_state_no_profile_button_states(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockAuthAwareToolbar *toolbar = mock_auth_aware_toolbar_new();

  mock_auth_aware_toolbar_set_auth_state(toolbar, MOCK_AUTH_STATE_NO_PROFILE);
  process_pending_events();

  /* Sign and export should be disabled without a profile */
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_sign));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_export));
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_lock));

  /* Create and import should be enabled */
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_new_identity));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_import));
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_settings));

  g_object_unref(toolbar);
}

static void test_auth_state_transition(TestUIFixture *fixture, gconstpointer user_data) {
  (void)fixture; (void)user_data;

  MockAuthAwareToolbar *toolbar = mock_auth_aware_toolbar_new();

  /* Start locked */
  mock_auth_aware_toolbar_set_auth_state(toolbar, MOCK_AUTH_STATE_LOCKED);
  process_pending_events();
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_sign));

  /* Unlock */
  mock_auth_aware_toolbar_set_auth_state(toolbar, MOCK_AUTH_STATE_UNLOCKED);
  process_pending_events();
  g_assert_true(gtk_widget_get_sensitive(toolbar->btn_sign));

  /* Lock again */
  mock_auth_aware_toolbar_set_auth_state(toolbar, MOCK_AUTH_STATE_LOCKED);
  process_pending_events();
  g_assert_false(gtk_widget_get_sensitive(toolbar->btn_sign));

  g_object_unref(toolbar);
}

/* ===========================================================================
 * Test Main
 * =========================================================================== */

int main(int argc, char *argv[]) {
  /* Skip UI tests on headless systems where GTK4 would hang
   * macOS Quartz backend requires a window server connection */

  /* Allow explicit skip via environment variable */
  const char *skip_env = g_getenv("GNOSTR_SKIP_UI_TESTS");
  if (skip_env && *skip_env) {
    g_print("TAP version 14\n1..0 # SKIP UI tests disabled via GNOSTR_SKIP_UI_TESTS\n");
    return 0;
  }

#ifdef __APPLE__
  /* On macOS, dialog tests hang without a window server connection.
   * Check for headless indicators:
   * - SSH session without X11 forwarding
   * - CI environment (GITHUB_ACTIONS, CI, etc.)
   * - tmux/screen session without TERM_PROGRAM indicating iTerm/Terminal.app */
  const char *ssh_conn = g_getenv("SSH_CONNECTION");
  const char *ci_env = g_getenv("CI");
  const char *github_actions = g_getenv("GITHUB_ACTIONS");
  const char *term_program = g_getenv("TERM_PROGRAM");

  if (ssh_conn && !g_getenv("DISPLAY")) {
    g_print("TAP version 14\n1..0 # SKIP UI tests require display (SSH without X11 forwarding)\n");
    return 0;
  }
  if (ci_env || github_actions) {
    g_print("TAP version 14\n1..0 # SKIP UI tests require display (CI environment)\n");
    return 0;
  }
  /* tmux/screen without a terminal program suggests headless */
  const char *tmux = g_getenv("TMUX");
  if (tmux && !term_program) {
    g_print("TAP version 14\n1..0 # SKIP UI tests require display (tmux without GUI terminal)\n");
    return 0;
  }
#else
  /* On Linux/other, check DISPLAY or WAYLAND_DISPLAY */
  if (!g_getenv("DISPLAY") && !g_getenv("WAYLAND_DISPLAY")) {
    g_print("TAP version 14\n1..0 # SKIP UI tests require display (DISPLAY/WAYLAND_DISPLAY not set)\n");
    return 0;
  }
#endif

  /* Initialize GTK test framework */
  gtk_test_init(&argc, &argv, NULL);

  /* Window creation and destruction tests */
  g_test_add("/ui/window/creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_creation, test_ui_fixture_teardown);
  g_test_add("/ui/window/destruction", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_destruction, test_ui_fixture_teardown);
  g_test_add("/ui/window/components", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_components, test_ui_fixture_teardown);

  /* Window layout tests */
  g_test_add("/ui/layout/minimum-size", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_minimum_size, test_ui_fixture_teardown);
  g_test_add("/ui/layout/default-size", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_default_size, test_ui_fixture_teardown);
  g_test_add("/ui/layout/sidebar-width", TestUIFixture, NULL,
             test_ui_fixture_setup, test_sidebar_width, test_ui_fixture_teardown);
  g_test_add("/ui/layout/stack-expands", TestUIFixture, NULL,
             test_ui_fixture_setup, test_stack_expands_horizontally, test_ui_fixture_teardown);

  /* Page navigation tests */
  g_test_add("/ui/navigation/initial-state", TestUIFixture, NULL,
             test_ui_fixture_setup, test_page_navigation_initial_state, test_ui_fixture_teardown);
  g_test_add("/ui/navigation/by-name", TestUIFixture, NULL,
             test_ui_fixture_setup, test_page_navigation_by_name, test_ui_fixture_teardown);
  g_test_add("/ui/navigation/cycle", TestUIFixture, NULL,
             test_ui_fixture_setup, test_page_navigation_cycle, test_ui_fixture_teardown);
  g_test_add("/ui/navigation/sidebar-selection", TestUIFixture, NULL,
             test_ui_fixture_setup, test_sidebar_row_selection, test_ui_fixture_teardown);

  /* Dialog presentation tests */
  g_test_add("/ui/dialog/approval-creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_approval_dialog_creation, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/approval-buttons", TestUIFixture, NULL,
             test_ui_fixture_setup, test_approval_dialog_buttons, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/approval-event-types", TestUIFixture, NULL,
             test_ui_fixture_setup, test_approval_dialog_event_types, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/approval-ttl-dropdown", TestUIFixture, NULL,
             test_ui_fixture_setup, test_approval_dialog_ttl_dropdown, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/approval-button-styles", TestUIFixture, NULL,
             test_ui_fixture_setup, test_approval_dialog_button_styles, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/create-profile-creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_create_profile_dialog_creation, test_ui_fixture_teardown);

  /* Import key dialog tests */
  g_test_add("/ui/dialog/import-key-creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_import_key_dialog_creation, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/import-key-nsec-validation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_import_key_dialog_nsec_validation, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/import-key-hex-validation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_import_key_dialog_hex_validation, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/import-key-ncrypt-validation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_import_key_dialog_ncrypt_validation, test_ui_fixture_teardown);

  /* Lock screen tests */
  g_test_add("/ui/lock-screen/creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_lock_screen_creation, test_ui_fixture_teardown);
  g_test_add("/ui/lock-screen/busy-state", TestUIFixture, NULL,
             test_ui_fixture_setup, test_lock_screen_busy_state, test_ui_fixture_teardown);
  g_test_add("/ui/lock-screen/error-display", TestUIFixture, NULL,
             test_ui_fixture_setup, test_lock_screen_error_display, test_ui_fixture_teardown);
  g_test_add("/ui/lock-screen/lock-reasons", TestUIFixture, NULL,
             test_ui_fixture_setup, test_lock_screen_lock_reasons, test_ui_fixture_teardown);

  /* Password validation tests */
  g_test_add("/ui/validation/password-empty", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_validation_empty, test_ui_fixture_teardown);
  g_test_add("/ui/validation/password-minimum-length", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_validation_minimum_length, test_ui_fixture_teardown);
  g_test_add("/ui/validation/password-mismatch", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_validation_mismatch, test_ui_fixture_teardown);
  g_test_add("/ui/validation/password-match", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_validation_match, test_ui_fixture_teardown);
  g_test_add("/ui/validation/password-clear-confirm", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_validation_clear_confirm, test_ui_fixture_teardown);

  /* Npub validation tests */
  g_test_add("/ui/validation/npub-valid", TestUIFixture, NULL,
             test_ui_fixture_setup, test_npub_validation_valid, test_ui_fixture_teardown);
  g_test_add("/ui/validation/npub-invalid-prefix", TestUIFixture, NULL,
             test_ui_fixture_setup, test_npub_validation_invalid_prefix, test_ui_fixture_teardown);
  g_test_add("/ui/validation/npub-invalid-length", TestUIFixture, NULL,
             test_ui_fixture_setup, test_npub_validation_invalid_length, test_ui_fixture_teardown);
  g_test_add("/ui/validation/npub-invalid-chars", TestUIFixture, NULL,
             test_ui_fixture_setup, test_npub_validation_invalid_chars, test_ui_fixture_teardown);
  g_test_add("/ui/validation/npub-null-empty", TestUIFixture, NULL,
             test_ui_fixture_setup, test_npub_validation_null_empty, test_ui_fixture_teardown);
  g_test_add("/ui/validation/nsec-valid", TestUIFixture, NULL,
             test_ui_fixture_setup, test_nsec_validation_valid, test_ui_fixture_teardown);
  g_test_add("/ui/validation/nsec-invalid", TestUIFixture, NULL,
             test_ui_fixture_setup, test_nsec_validation_invalid, test_ui_fixture_teardown);
  g_test_add("/ui/validation/hex64-valid", TestUIFixture, NULL,
             test_ui_fixture_setup, test_hex64_validation_valid, test_ui_fixture_teardown);
  g_test_add("/ui/validation/hex64-invalid", TestUIFixture, NULL,
             test_ui_fixture_setup, test_hex64_validation_invalid, test_ui_fixture_teardown);

  /* Button state tests */
  g_test_add("/ui/button/create-requires-name", TestUIFixture, NULL,
             test_ui_fixture_setup, test_create_button_requires_display_name, test_ui_fixture_teardown);
  g_test_add("/ui/button/cancel-always-enabled", TestUIFixture, NULL,
             test_ui_fixture_setup, test_cancel_button_always_enabled, test_ui_fixture_teardown);
  g_test_add("/ui/button/approval-buttons-state", TestUIFixture, NULL,
             test_ui_fixture_setup, test_approval_buttons_state, test_ui_fixture_teardown);
  g_test_add("/ui/button/remember-checkbox-toggle", TestUIFixture, NULL,
             test_ui_fixture_setup, test_remember_checkbox_toggle, test_ui_fixture_teardown);

  /* CSS class tests */
  g_test_add("/ui/css/suggested-action-class", TestUIFixture, NULL,
             test_ui_fixture_setup, test_suggested_action_button_class, test_ui_fixture_teardown);
  g_test_add("/ui/css/password-match-success", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_match_success_class, test_ui_fixture_teardown);
  g_test_add("/ui/css/password-match-error", TestUIFixture, NULL,
             test_ui_fixture_setup, test_password_match_error_class, test_ui_fixture_teardown);

  /* Keyboard shortcut tests */
  g_test_add("/ui/shortcuts/action-registration", TestUIFixture, NULL,
             test_ui_fixture_setup, test_keyboard_shortcuts_action_registration, test_ui_fixture_teardown);
  g_test_add("/ui/shortcuts/action-activation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_keyboard_shortcuts_action_activation, test_ui_fixture_teardown);

  /* Mock D-Bus tests */
  g_test_add("/ui/mock-dbus/connection", TestUIFixture, NULL,
             test_ui_fixture_setup, test_mock_dbus_connection, test_ui_fixture_teardown);
  g_test_add("/ui/mock-dbus/sign-event", TestUIFixture, NULL,
             test_ui_fixture_setup, test_mock_dbus_sign_event, test_ui_fixture_teardown);
  g_test_add("/ui/mock-dbus/disconnected", TestUIFixture, NULL,
             test_ui_fixture_setup, test_mock_dbus_disconnected, test_ui_fixture_teardown);

  /* High contrast theme tests */
  g_test_add("/ui/high-contrast/css-class-applied", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_css_class_applied, test_ui_fixture_teardown);
  g_test_add("/ui/high-contrast/variant-inverted", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_variant_inverted, test_ui_fixture_teardown);
  g_test_add("/ui/high-contrast/variant-yellow-on-black", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_variant_yellow_on_black, test_ui_fixture_teardown);
  g_test_add("/ui/high-contrast/focus-indicators", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_focus_indicators, test_ui_fixture_teardown);
  g_test_add("/ui/high-contrast/button-styles", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_button_styles, test_ui_fixture_teardown);
  g_test_add("/ui/high-contrast/adw-style-manager", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_adw_style_manager, test_ui_fixture_teardown);
  g_test_add("/ui/high-contrast/color-scheme-integration", TestUIFixture, NULL,
             test_ui_fixture_setup, test_high_contrast_color_scheme_integration, test_ui_fixture_teardown);

  /* Backup dialog tests */
  g_test_add("/ui/dialog/backup-creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_backup_dialog_creation, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/backup-tab-switching", TestUIFixture, NULL,
             test_ui_fixture_setup, test_backup_dialog_tab_switching, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/backup-export-button-state", TestUIFixture, NULL,
             test_ui_fixture_setup, test_backup_dialog_export_button_state, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/backup-export-format-options", TestUIFixture, NULL,
             test_ui_fixture_setup, test_backup_dialog_export_format_options, test_ui_fixture_teardown);
  g_test_add("/ui/dialog/backup-mnemonic-input", TestUIFixture, NULL,
             test_ui_fixture_setup, test_backup_dialog_mnemonic_input, test_ui_fixture_teardown);

  /* Sidebar-to-page synchronization tests */
  g_test_add("/ui/navigation/sidebar-page-sync", TestUIFixture, NULL,
             test_ui_fixture_setup, test_sidebar_page_sync_on_row_click, test_ui_fixture_teardown);
  g_test_add("/ui/navigation/sidebar-page-bidirectional", TestUIFixture, NULL,
             test_ui_fixture_setup, test_sidebar_page_sync_bidirectional, test_ui_fixture_teardown);

  /* Auth state button tests */
  g_test_add("/ui/auth/locked-button-states", TestUIFixture, NULL,
             test_ui_fixture_setup, test_auth_state_locked_button_states, test_ui_fixture_teardown);
  g_test_add("/ui/auth/unlocked-button-states", TestUIFixture, NULL,
             test_ui_fixture_setup, test_auth_state_unlocked_button_states, test_ui_fixture_teardown);
  g_test_add("/ui/auth/no-profile-button-states", TestUIFixture, NULL,
             test_ui_fixture_setup, test_auth_state_no_profile_button_states, test_ui_fixture_teardown);
  g_test_add("/ui/auth/state-transition", TestUIFixture, NULL,
             test_ui_fixture_setup, test_auth_state_transition, test_ui_fixture_teardown);

  return g_test_run();
}
