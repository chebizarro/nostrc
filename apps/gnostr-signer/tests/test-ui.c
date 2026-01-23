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
  return g_object_new(mock_approval_dialog_get_type(), NULL);
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
  return g_object_new(mock_create_profile_dialog_get_type(), NULL);
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
 * Test Main
 * =========================================================================== */

int main(int argc, char *argv[]) {
  /* Initialize GTK test framework */
  gtk_test_init(&argc, &argv, NULL);

  /* Window creation and destruction tests */
  g_test_add("/ui/window/creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_creation, test_ui_fixture_teardown);
  g_test_add("/ui/window/destruction", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_destruction, test_ui_fixture_teardown);
  g_test_add("/ui/window/components", TestUIFixture, NULL,
             test_ui_fixture_setup, test_window_components, test_ui_fixture_teardown);

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
  g_test_add("/ui/dialog/create-profile-creation", TestUIFixture, NULL,
             test_ui_fixture_setup, test_create_profile_dialog_creation, test_ui_fixture_teardown);

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

  return g_test_run();
}
