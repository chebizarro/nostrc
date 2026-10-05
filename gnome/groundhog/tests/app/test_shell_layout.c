/* Shell layout, actions and status checks for the GTK-only templates
 * (data/ui/gh-window.blp, gh-sidebar-page.blp, gh-content-page.blp,
 * gh-shortcuts-window.blp) and GhStatus. It registers
 * the compiled Groundhog GResource and instantiates GhWindow, GhSidebarPage
 * and GhContentPage exactly as the application does, without GSettings, the
 * signer or a conversation model (the pages are model-agnostic; generic
 * GListStores stand in here, and tests/ui/test_conversation_list.c covers the
 * real store). Charter acceptance covered: UX-1 (360x294 minimum size, mapped
 * collapsed layout), UX-2 (window breakpoint), UX-4 (every accelerator bound
 * and listed in the shortcuts window) and UX-8 (banner states 3, 5-8 through a
 * GhStatus fixture; list states 9 and 10). GLib/GTK/libadwaita warnings are
 * fatal (g_test), so a size or breakpoint warning fails the test. Keyboard
 * order, screen-reader phrasing and high contrast still need the manual GNOME
 * check (charter UX-10).
 */
#include "gh-window.h"

#include <string.h>

#include "nostrc-test-gdk-frame.h"

void groundhog_register_resource(void);

#define APP_ID "org.nostr.Groundhog"

static void
assert_label(gpointer accessible, const char *label)
{
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(accessible),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, label);
}

static gpointer
template_child(gpointer widget, GType type, const char *name)
{
  GObject *child = gtk_widget_get_template_child(GTK_WIDGET(widget), type, name);
  g_assert_nonnull(child);
  return child;
}

static void
assert_status_page(GtkWidget *page)
{
  g_assert_true(ADW_IS_STATUS_PAGE(page));
  g_assert_true(gtk_widget_has_css_class(page, "groundhog-shell-status"));
  g_assert_nonnull(adw_status_page_get_title(ADW_STATUS_PAGE(page)));
  g_assert_nonnull(adw_status_page_get_description(ADW_STATUS_PAGE(page)));
}

static void
assert_stack_pages(GtkStack *stack, gboolean onboarding)
{
  static const char *expected[] = {"conversations", "empty", "no-results", "error"};
  guint n_pages = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(GTK_WIDGET(stack)); c;
       c = gtk_widget_get_next_sibling(c))
    n_pages++;

  for (guint i = 0; i < G_N_ELEMENTS(expected); i++)
    g_assert_nonnull(gtk_stack_get_child_by_name(stack, expected[i]));
  assert_status_page(gtk_stack_get_child_by_name(stack, "empty"));
  assert_status_page(gtk_stack_get_child_by_name(stack, "no-results"));
  assert_status_page(gtk_stack_get_child_by_name(stack, "error"));
  if (onboarding) {
    assert_status_page(gtk_stack_get_child_by_name(stack, "onboarding"));
    /* The account-free build (GROUNDHOG_HAVE_ACCOUNTS=0) shows onboarding. */
    g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "onboarding");
    g_assert_cmpuint(n_pages, ==, 5);
  } else {
    g_assert_null(gtk_stack_get_child_by_name(stack, "onboarding"));
    /* No model bound: the honest empty state, never fabricated rows. */
    g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");
    g_assert_cmpuint(n_pages, ==, 4);
  }
}

static void
test_sidebar_page_structure(void)
{
  GhSidebarPage *page = g_object_ref_sink(g_object_new(GH_TYPE_SIDEBAR_PAGE, NULL));
  GtkStack *stack = gh_sidebar_page_get_stack(page);

  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(page)), ==, "Conversations");
  g_assert_true(ADW_IS_HEADER_BAR(gh_sidebar_page_get_header(page)));
  g_assert_true(adw_header_bar_get_title_widget(gh_sidebar_page_get_header(page)) ==
                GTK_WIDGET(gh_sidebar_page_get_window_title(page)));
  g_assert_cmpstr(adw_window_title_get_title(gh_sidebar_page_get_window_title(page)), ==,
                  "Groundhog");
  /* Standalone builds never set a subtitle; it stays empty, not unset. */
  g_assert_cmpstr(adw_window_title_get_subtitle(gh_sidebar_page_get_window_title(page)), ==, "");
  g_assert_true(GTK_IS_STACK(stack));
  assert_stack_pages(stack, FALSE);

  GtkListView *list = gh_sidebar_page_get_list(page);
  g_assert_true(GTK_IS_LIST_VIEW(list));
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(list)), ==,
                  GTK_ACCESSIBLE_ROLE_LIST);
  /* The Blueprint accessibility blocks reach the widgets' AT context. */
  assert_label(list, "Conversations");
  assert_label(stack, "Conversation list status");

  /* Header controls: every icon-only button has a tooltip and a label. */
  GtkWidget *search = template_child(page, GH_TYPE_SIDEBAR_PAGE, "search_button");
  GtkWidget *new_button = template_child(page, GH_TYPE_SIDEBAR_PAGE, "new_button");
  GtkWidget *menu = template_child(page, GH_TYPE_SIDEBAR_PAGE, "primary_button");
  GtkWidget *back = template_child(page, GH_TYPE_SIDEBAR_PAGE, "back_button");
  assert_label(search, "Search Conversations");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(search), ==, "Search Conversations");
  assert_label(new_button, "New Message");
  g_assert_cmpstr(gtk_widget_get_tooltip_text(new_button), ==, "New Message");
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(new_button)), ==,
                  "win.new-message");
  assert_label(menu, "Main Menu");
  /* F10 opens the main menu. */
  g_assert_true(gtk_menu_button_get_primary(GTK_MENU_BUTTON(menu)));
  assert_label(back, "Back to Conversations");
  g_assert_false(gtk_widget_get_visible(back));
  g_assert_false(adw_banner_get_revealed(gh_sidebar_page_get_banner(page)));

  gh_sidebar_page_show_onboarding(page);
  assert_stack_pages(stack, TRUE);

  g_object_unref(page);
}

static void
count_new_message(GhWindow *window, gpointer data)
{
  (void)window;
  (*(guint *)data)++;
}

static GListStore *
store_of(guint n)
{
  GListStore *store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
  for (guint i = 0; i < n; i++) {
    g_autofree char *name = g_strdup_printf("item %u", i);
    g_autoptr(GtkStringObject) item = gtk_string_object_new(name);
    g_list_store_append(store, item);
  }
  return store;
}

static GtkWidget *
visible_page(GhSidebarPage *page)
{
  return gtk_stack_get_visible_child(gh_sidebar_page_get_stack(page));
}

/* The sidebar's list states (charter §7.15 #9, #10) and the Message
 * Requests entry, driven by generic models: the page knows no model type. */
static void
test_sidebar_list_states(void)
{
  GhSidebarPage *page = g_object_ref_sink(g_object_new(GH_TYPE_SIDEBAR_PAGE, NULL));
  GtkStack *stack = gh_sidebar_page_get_stack(page);
  g_autoptr(GListStore) conversations = store_of(0);
  g_autoptr(GListStore) requests = store_of(0);
  GtkWidget *requests_button = template_child(page, GH_TYPE_SIDEBAR_PAGE, "requests_button");
  GtkLabel *count = template_child(page, GH_TYPE_SIDEBAR_PAGE, "requests_count");

  gh_sidebar_page_set_models(page, G_LIST_MODEL(conversations), G_LIST_MODEL(requests));
  /* #9 No conversations: [New Message] (charter §7.15, G18). */
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");
  g_assert_cmpstr(adw_status_page_get_title(ADW_STATUS_PAGE(visible_page(page))), ==,
                  "No Conversations");
  GtkWidget *start = adw_status_page_get_child(ADW_STATUS_PAGE(visible_page(page)));
  g_assert_true(GTK_IS_BUTTON(start));
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(start)), ==, "win.new-message");
  g_assert_true(gh_sidebar_page_get_focus_target(page) == start);

  /* Requests alone still list: the entry is the way to them. */
  g_autoptr(GtkStringObject) request = gtk_string_object_new("request");
  g_list_store_append(requests, request);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "conversations");
  g_assert_true(gtk_widget_get_visible(requests_button));
  g_assert_cmpstr(gtk_label_get_text(count), ==, "1");
  assert_label(requests_button, "Message Requests, 1 conversation");
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(requests_button)), ==,
                  "sidebar.show-requests");

  g_autoptr(GtkStringObject) conversation = gtk_string_object_new("conversation");
  g_list_store_append(conversations, conversation);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(page), "sidebar.show-requests", "b",
                                           TRUE));
  g_assert_true(gh_sidebar_page_get_show_requests(page));
  g_assert_false(gtk_widget_get_visible(requests_button));
  g_assert_true(gtk_widget_get_visible(template_child(page, GH_TYPE_SIDEBAR_PAGE,
                                                      "back_button")));
  g_assert_false(gtk_widget_get_visible(template_child(page, GH_TYPE_SIDEBAR_PAGE,
                                                       "new_button")));
  g_assert_cmpstr(adw_window_title_get_title(gh_sidebar_page_get_window_title(page)), ==,
                  "Requests");
  /* The visible list is the requests; selection follows it. */
  g_assert_true(gh_sidebar_page_select_relative(page, 1));
  g_assert_true(gh_sidebar_page_get_selected(page) == (gpointer)request);
  g_assert_false(gh_sidebar_page_select_relative(page, 1));

  /* The last request leaving (accepted) returns to the conversations. */
  g_list_store_remove(requests, 0);
  g_assert_false(gh_sidebar_page_get_show_requests(page));
  g_assert_null(gh_sidebar_page_get_selected(page));
  g_assert_cmpstr(adw_window_title_get_title(gh_sidebar_page_get_window_title(page)), ==,
                  "Groundhog");
  g_assert_false(gtk_widget_get_visible(requests_button));
  g_assert_true(gtk_widget_get_visible(template_child(page, GH_TYPE_SIDEBAR_PAGE,
                                                      "new_button")));

  /* Previous/next move through the visible list, stopping at the ends. */
  g_autoptr(GtkStringObject) second = gtk_string_object_new("second");
  g_list_store_append(conversations, second);
  g_assert_true(gh_sidebar_page_select_relative(page, -1));
  g_assert_true(gh_sidebar_page_get_selected(page) == (gpointer)second);
  g_assert_true(gh_sidebar_page_select_relative(page, -1));
  g_assert_true(gh_sidebar_page_get_selected(page) == (gpointer)conversation);
  g_assert_false(gh_sidebar_page_select_relative(page, -1));
  gh_sidebar_page_unselect(page);
  g_assert_null(gh_sidebar_page_get_selected(page));

  /* An account page wins over every list state, and nothing is selectable
   * through it. */
  g_autoptr(GtkWidget) account = g_object_ref_sink(adw_status_page_new());
  gtk_stack_add_named(stack, account, "account-test");
  gh_sidebar_page_set_account_page(page, "account-test");
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "account-test");
  g_assert_false(gh_sidebar_page_select_relative(page, 1));
  g_assert_null(gh_sidebar_page_get_focus_target(page));
  gh_sidebar_page_set_account_page(page, NULL);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "conversations");

  g_object_unref(page);
}

static gboolean
search_text_is(gpointer data)
{
  GhSidebarPage *page = data;
  const char *want = g_object_get_data(G_OBJECT(page), "want");
  return g_strcmp0(gh_sidebar_page_get_search_text(page), want) == 0;
}

static gboolean
deadline_hit(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
spin_until_at(gboolean (*pred)(gpointer), gpointer data, int line)
{
  gboolean expired = FALSE;
  guint timer = g_timeout_add_seconds(5, deadline_hit, &expired);
  while (!pred(data) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (expired) {
    if (GH_IS_WINDOW(data))
      g_printerr("window %dx%d mapped=%d collapsed=%d\n", gtk_widget_get_width(data),
                 gtk_widget_get_height(data), gtk_widget_get_mapped(data),
                 adw_navigation_split_view_get_collapsed(gh_window_get_split(data)));
    g_error("condition waited for at line %d did not hold within 5s", line);
  }
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

/* #10 No results: the search entry's text becomes the page's search-text
 * (trimmed, after the entry's own search delay) and an empty filtered list
 * shows "no-results", whose explicit focus target is the search entry. */
static void
test_sidebar_search_states(void)
{
  GhSidebarPage *page = g_object_ref_sink(g_object_new(GH_TYPE_SIDEBAR_PAGE, NULL));
  GtkStack *stack = gh_sidebar_page_get_stack(page);
  g_autoptr(GListStore) conversations = store_of(0);
  g_autoptr(GListStore) requests = store_of(0);
  GtkWidget *entry = template_child(page, GH_TYPE_SIDEBAR_PAGE, "search_entry");
  gh_sidebar_page_set_models(page, G_LIST_MODEL(conversations), G_LIST_MODEL(requests));
  assert_label(entry, "Search conversations");

  gtk_editable_set_text(GTK_EDITABLE(entry), "  alice ");
  g_object_set_data(G_OBJECT(page), "want", (gpointer)"alice");
  spin_until(search_text_is, page);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "no-results");
  g_assert_cmpstr(adw_status_page_get_title(ADW_STATUS_PAGE(visible_page(page))), ==,
                  "No Results");
  g_assert_true(gh_sidebar_page_get_focus_target(page) == entry);

  gtk_editable_set_text(GTK_EDITABLE(entry), "");
  g_object_set_data(G_OBJECT(page), "want", (gpointer)"");
  spin_until(search_text_is, page);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");
  g_assert_true(gh_sidebar_page_get_focus_target(page) ==
                adw_status_page_get_child(ADW_STATUS_PAGE(visible_page(page))));

  g_object_unref(page);
}

static void
test_content_page_structure(void)
{
  GhContentPage *page = g_object_ref_sink(g_object_new(GH_TYPE_CONTENT_PAGE, NULL));
  GtkStack *stack = gh_content_page_get_stack(page);
  GhComposer *composer = gh_content_page_get_composer(page);

  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(page)), ==, "Messages");
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "none");
  GtkWidget *none = gtk_stack_get_visible_child(stack);
  assert_status_page(none);
  g_assert_cmpstr(adw_status_page_get_title(ADW_STATUS_PAGE(none)), ==,
                  "No Conversation Selected");
  /* The conversation view is the typed layer's (G12, tested in
   * tests/ui/test_conversation_view.c); any widget stands in for it here. */
  g_assert_null(gh_content_page_get_view(page));
  g_assert_false(gh_content_page_focus_conversation(page));
  GtkWidget *view = gtk_label_new("view");
  gh_content_page_set_view(page, view);
  g_assert_true(gh_content_page_get_view(page) == view);

  /* The composer (charter G13) sits under the conversation; the reason
   * sending is unavailable is shown in its place (gh-send-ui.c sets it). */
  g_assert_true(GH_IS_COMPOSER(composer));
  g_assert_null(gh_composer_get_disabled_reason(composer));
  gh_composer_set_disabled_reason(composer, "Read-only: test");
  gh_content_page_set_conversation_shown(page, TRUE);
  g_assert_true(gh_content_page_get_conversation_shown(page));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "conversation");
  g_assert_true(gtk_widget_is_ancestor(view, gtk_stack_get_visible_child(stack)));
  g_assert_true(gtk_widget_is_ancestor(GTK_WIDGET(composer),
                                       gtk_stack_get_visible_child(stack)));
  g_assert_true(gtk_widget_is_ancestor(
    GTK_WIDGET(composer), template_child(page, GH_TYPE_CONTENT_PAGE, "conversation_bin")));
  g_assert_cmpstr(gh_composer_get_disabled_reason(composer), ==, "Read-only: test");
  gh_composer_set_disabled_reason(composer, NULL);
  g_assert_null(gh_composer_get_disabled_reason(composer));

  gh_content_page_set_title(page, "Alice", "Private · end-to-end encrypted");
  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(page)), ==, "Alice");
  g_assert_cmpstr(gh_content_page_get_title(page), ==,
                  "Alice");
  g_assert_cmpstr(gh_content_page_get_subtitle(page), ==,
                  "Private · end-to-end encrypted");
  gh_content_page_set_title(page, NULL, NULL);
  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(page)), ==, "Messages");

  gh_content_page_set_conversation_shown(page, FALSE);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "none");
  g_assert_false(gh_content_page_focus_conversation(page));

  g_object_unref(page);
}

static void
test_window_structure(void)
{
  GhWindow *window = gh_window_new(NULL);
  AdwNavigationSplitView *split = gh_window_get_split(window);

  g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(window)), ==, "Groundhog");
  /* The window's icon is the application id (qp24.8.6). */
  g_assert_cmpstr(gtk_window_get_icon_name(GTK_WINDOW(window)), ==, APP_ID);
  int width = 0, height = 0;
  gtk_window_get_default_size(GTK_WINDOW(window), &width, &height);
  g_assert_cmpint(width, ==, 900);
  g_assert_cmpint(height, ==, 600);
  /* UX-1: the template itself requests the GNOME HIG minimum. */
  gtk_widget_get_size_request(GTK_WIDGET(window), &width, &height);
  g_assert_cmpint(width, ==, 360);
  g_assert_cmpint(height, ==, 294);

  g_assert_true(adw_application_window_get_content(ADW_APPLICATION_WINDOW(window)) ==
                GTK_WIDGET(gh_window_get_toasts(window)));
  /* The root stack holds the split view as "main"; full-window flows such
   * as onboarding (G14) are added to it by their owners. */
  GtkStack *root = gh_window_get_root_stack(window);
  g_assert_true(adw_toast_overlay_get_child(gh_window_get_toasts(window)) == GTK_WIDGET(root));
  g_assert_true(gtk_stack_get_child_by_name(root, "main") == GTK_WIDGET(split));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(root), ==, "main");
  g_assert_true(adw_navigation_split_view_get_sidebar(split) ==
                ADW_NAVIGATION_PAGE(gh_window_get_sidebar(window)));
  g_assert_true(adw_navigation_split_view_get_content(split) ==
                ADW_NAVIGATION_PAGE(gh_window_get_content(window)));
  g_assert_cmpfloat(adw_navigation_split_view_get_min_sidebar_width(split), ==, 280);
  g_assert_cmpfloat(adw_navigation_split_view_get_max_sidebar_width(split), ==, 380);
  g_assert_false(adw_navigation_split_view_get_show_content(split));
  g_assert_false(adw_navigation_split_view_get_collapsed(split));
  assert_stack_pages(gh_sidebar_page_get_stack(gh_window_get_sidebar(window)), FALSE);
  g_assert_true(GH_IS_STATUS(gh_window_get_status(window)));

  /* The window actions exist; new-message is disabled until a New Message
   * flow is attached (charter G18), then runs it while enabled. */
  GActionGroup *actions = G_ACTION_GROUP(window);
  static const char *win_actions[] = {
    "search", "previous-conversation", "next-conversation", "new-message",
    "show-help-overlay",
  };
  for (guint i = 0; i < G_N_ELEMENTS(win_actions); i++)
    g_assert_true(g_action_group_has_action(actions, win_actions[i]));
  g_assert_false(g_action_group_get_action_enabled(actions, "new-message"));
  guint new_message_runs = 0;
  gh_window_set_new_message_handler(window, count_new_message, &new_message_runs, NULL);
  g_assert_true(g_action_group_get_action_enabled(actions, "new-message"));
  g_action_group_activate_action(actions, "new-message", NULL);
  g_assert_cmpuint(new_message_runs, ==, 1);
  gh_window_set_new_message_enabled(window, FALSE);
  g_assert_false(g_action_group_get_action_enabled(actions, "new-message"));
  gh_window_set_new_message_enabled(window, TRUE);
  gh_window_set_new_message_handler(window, NULL, NULL, NULL);
  g_assert_false(g_action_group_get_action_enabled(actions, "new-message"));
  g_assert_true(g_action_group_get_action_enabled(actions, "search"));
  g_assert_true(GTK_IS_SHORTCUTS_WINDOW(
    gtk_application_window_get_help_overlay(GTK_APPLICATION_WINDOW(window))));

  /* Ctrl+F's action reveals the search bar. */
  GtkSearchBar *bar = template_child(gh_window_get_sidebar(window), GH_TYPE_SIDEBAR_PAGE,
                                     "search_bar");
  g_assert_false(gtk_search_bar_get_search_mode(bar));
  g_action_group_activate_action(actions, "search", NULL);
  g_assert_true(gtk_search_bar_get_search_mode(bar));
  g_assert_true(gtk_search_bar_get_key_capture_widget(bar) == GTK_WIDGET(window));

  gtk_window_destroy(GTK_WINDOW(window));
}

/* ---- UX-4 shortcuts ------------------------------------------------------- */

static void
collect_shortcuts(GtkWidget *widget, GPtrArray *accels)
{
  if (GTK_IS_SHORTCUTS_SHORTCUT(widget)) {
    g_autofree char *accelerator = NULL;
    g_object_get(widget, "accelerator", &accelerator, NULL);
    g_auto(GStrv) parts = g_strsplit(accelerator ? accelerator : "", " ", -1);
    for (guint i = 0; parts[i]; i++) {
      guint key = 0;
      GdkModifierType mods = 0;
      if (!*parts[i])
        continue;
      g_assert_true(gtk_accelerator_parse(parts[i], &key, &mods));
      g_ptr_array_add(accels, gtk_accelerator_name(key, mods));
    }
  }
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c))
    collect_shortcuts(c, accels);
}

static gboolean
contains(GPtrArray *strings, const char *value)
{
  for (guint i = 0; i < strings->len; i++)
    if (g_str_equal(g_ptr_array_index(strings, i), value))
      return TRUE;
  return FALSE;
}

static char *
normalized(const char *accel)
{
  guint key = 0;
  GdkModifierType mods = 0;
  g_assert_true(gtk_accelerator_parse(accel, &key, &mods));
  return gtk_accelerator_name(key, mods);
}

static void
test_shortcuts_bound_and_listed(void)
{
  /* Charter §7.13 as far as G11 implements it: action, accelerators. */
  static const struct {
    const char *action;
    const char *accels[3];
  } required[] = {
    { "win.search", { "<Control>f", NULL } },
    { "win.previous-conversation", { "<Alt>Up", "<Control>Page_Up", NULL } },
    { "win.next-conversation", { "<Alt>Down", "<Control>Page_Down", NULL } },
    { "win.new-message", { "<Control>n", NULL } },
    { "win.conversation-info", { "<Control>i", NULL } },
    { "win.show-help-overlay", { "<Control>question", NULL } },
    { "app.preferences", { "<Control>comma", NULL } },
    { "window.close", { "<Control>w", NULL } },
    { "app.quit", { "<Control>q", NULL } },
  };
  /* Accelerators and actions need no registration (which would reach for a
   * session bus); the window's help overlay is the one it ships. */
  g_autoptr(GtkApplication) app =
    gtk_application_new(APP_ID ".ShellTest", G_APPLICATION_NON_UNIQUE);
  gh_window_setup_application(app);
  g_assert_nonnull(g_action_map_lookup_action(G_ACTION_MAP(app), "quit"));

  GhWindow *window = gh_window_new(NULL);
  g_autoptr(GPtrArray) listed = g_ptr_array_new_with_free_func(g_free);
  collect_shortcuts(GTK_WIDGET(gtk_application_window_get_help_overlay(
                      GTK_APPLICATION_WINDOW(window))), listed);
  g_assert_cmpuint(listed->len, >, 0);

  for (guint i = 0; i < G_N_ELEMENTS(required); i++) {
    g_auto(GStrv) bound = gtk_application_get_accels_for_action(app, required[i].action);
    g_assert_cmpuint(g_strv_length(bound), ==, g_strv_length((GStrv)required[i].accels));
    for (guint j = 0; required[i].accels[j]; j++) {
      g_autofree char *want = normalized(required[i].accels[j]);
      g_autofree char *have = normalized(bound[j]);
      g_assert_cmpstr(have, ==, want);
    }
  }

  /* Every bound accelerator is listed, and every listed one is bound (F10
   * is the primary menu button's, checked in the sidebar test; Shift+F10
   * and Menu the conversation list's, in test_conversation_menu.c). */
  g_autoptr(GPtrArray) all_bound = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) actions = gtk_application_list_action_descriptions(app);
  for (guint i = 0; actions[i]; i++) {
    g_auto(GStrv) bound = gtk_application_get_accels_for_action(app, actions[i]);
    for (guint j = 0; bound[j]; j++) {
      char *name = normalized(bound[j]);
      if (!contains(listed, name))
        g_error("%s (%s) is bound but not in gh-shortcuts-window", name, actions[i]);
      g_ptr_array_add(all_bound, name);
    }
  }
  g_autofree char *f10 = normalized("F10");
  g_autofree char *shift_f10 = normalized("<Shift>F10");
  g_autofree char *menu_key = normalized("Menu");
  g_assert_true(contains(listed, shift_f10) && contains(listed, menu_key));
  for (guint i = 0; i < listed->len; i++) {
    const char *name = g_ptr_array_index(listed, i);
    if (!g_str_equal(name, f10) && !g_str_equal(name, shift_f10) &&
        !g_str_equal(name, menu_key) && !contains(all_bound, name))
      g_error("gh-shortcuts-window lists %s, which nothing binds", name);
  }

  /* The accelerators reach the window's actions. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(window), "win.search", NULL));
  gtk_window_destroy(GTK_WINDOW(window));
}

/* ---- UX-8 banners --------------------------------------------------------- */

static void
assert_banner(GhWindow *window, GhStatusBanner banner, const char *title)
{
  GhStatus *status = gh_window_get_status(window);
  AdwBanner *widget = gh_sidebar_page_get_banner(gh_window_get_sidebar(window));
  g_assert_cmpint(gh_status_get_banner(status), ==, banner);
  g_assert_cmpint(adw_banner_get_revealed(widget), ==, banner != GH_STATUS_BANNER_NONE);
  if (banner == GH_STATUS_BANNER_NONE)
    return;
  g_assert_cmpstr(adw_banner_get_title(widget), ==, title);
  g_assert_cmpstr(gh_status_banner_get_title(banner), ==, title);
  /* The "no inbox relays" banners offer [Set Up], the onboarding inbox step
   * (G14), and "Can't reach Tor" [Network Settings] (G09). No other banner
   * here offers a button whose destination does not exist yet. */
  const char *button = adw_banner_get_button_label(widget);
  if (banner == GH_STATUS_BANNER_TOR_UNREACHABLE || banner == GH_STATUS_BANNER_TOR_UNAVAILABLE) {
    g_assert_cmpstr(button, ==, "Network Settings");
    g_assert_cmpstr(gh_status_banner_get_action(banner), ==, GH_STATUS_ACTION_NETWORK_SETTINGS);
    g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(widget)), ==,
                    "app.network-settings");
    return;
  }
  if (banner == GH_STATUS_BANNER_INBOX_MISSING || banner == GH_STATUS_BANNER_NO_RELAYS) {
    g_assert_cmpstr(button, ==, "Set Up");
    g_assert_cmpstr(gh_status_banner_get_action(banner), ==, GH_STATUS_ACTION_SETUP_INBOX);
    g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(widget)), ==,
                    "win.setup-inbox");
    return;
  }
  g_assert_true(button == NULL || *button == '\0');
  g_assert_null(gh_status_banner_get_action(banner));
}

static void
test_status_banners(void)
{
  GhWindow *window = gh_window_new(NULL);
  GhStatus *status = gh_window_get_status(window);
  GtkStack *stack = gh_sidebar_page_get_stack(gh_window_get_sidebar(window));

  /* Without an active account the account pages speak; no banner. */
  gh_status_set_network_available(status, FALSE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_UNAVAILABLE);
  assert_banner(window, GH_STATUS_BANNER_NONE, NULL);

  gh_status_set_account_active(status, TRUE);
  /* A build without G09 and network-mode tor connects to nothing, online or
   * not (nostrc-6v0i), so it outranks Offline, whose "when you're back
   * online" would not come true. */
  gh_status_set_tor_unavailable(status, TRUE);
  assert_banner(window, GH_STATUS_BANNER_TOR_UNAVAILABLE,
                "Tor isn't available in this build — Groundhog won't connect until you "
                "choose another network setting");
  gh_status_set_tor_unavailable(status, FALSE);
  /* #5, then: it explains every relay failure below it. */
  assert_banner(window, GH_STATUS_BANNER_OFFLINE,
                "Offline — new messages will arrive when you're back online");
  gh_status_set_network_available(status, TRUE);
  /* #3: messages are unlocked through the signer, so without it nothing new
   * can be read either (the charter's "you can read" would overclaim). */
  assert_banner(window, GH_STATUS_BANNER_SIGNER_UNAVAILABLE,
                "Nostr Signer isn't running — messages can't be unlocked or sent");
  gh_status_set_signer(status, GH_STATUS_SIGNER_NO_BUS);
  assert_banner(window, GH_STATUS_BANNER_SIGNER_NO_BUS,
                "Nostr Signer and notifications are unavailable: no session bus is available");
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  gh_status_set_bus_unresponsive(TRUE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_NO_BUS);
  assert_banner(window, GH_STATUS_BANNER_SIGNER_NO_BUS,
                "Nostr Signer and notifications are unavailable: the session bus isn't responding");
  gh_status_set_bus_unresponsive(FALSE);
  /* #6 outranks the signer (G09: Tor mode with nothing at the Tor address). */
  gh_status_set_tor_unreachable(status, TRUE);
  assert_banner(window, GH_STATUS_BANNER_TOR_UNREACHABLE,
                "Can't reach Tor — Groundhog won't connect without it");
  gh_status_set_tor_unreachable(status, FALSE);
  /* An activatable signer is started by the bus on first use: no banner. */
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  assert_banner(window, GH_STATUS_BANNER_NONE, NULL);

  static const struct {
    GhStatusInbox inbox;
    GhStatusBanner banner;
    const char *title;
  } inbox[] = {
    { GH_STATUS_INBOX_NO_SOURCES, GH_STATUS_BANNER_NO_RELAYS,
      "No relay is set up yet, so Groundhog can't receive messages" },
    { GH_STATUS_INBOX_LOOKING, GH_STATUS_BANNER_LOOKING,
      "Looking for your message relays…" },
    { GH_STATUS_INBOX_LOOKUP_FAILED, GH_STATUS_BANNER_LOOKUP_FAILED,
      "Can't reach your relays to find where your messages arrive" },
    /* #7 */
    { GH_STATUS_INBOX_MISSING, GH_STATUS_BANNER_INBOX_MISSING,
      "Set up private messaging so people can reach you" },
    { GH_STATUS_INBOX_CONNECTING, GH_STATUS_BANNER_CONNECTING,
      "Connecting to your message relays…" },
    { GH_STATUS_INBOX_BACKFILLING, GH_STATUS_BANNER_BACKFILLING,
      "Checking for new messages…" },
    { GH_STATUS_INBOX_LIVE, GH_STATUS_BANNER_NONE, NULL },
    /* #8 */
    { GH_STATUS_INBOX_UNREACHABLE, GH_STATUS_BANNER_INBOX_UNREACHABLE,
      "Can't reach your message relays" },
    { GH_STATUS_INBOX_ERROR, GH_STATUS_BANNER_INBOX_ERROR,
      "Can't receive messages on this device" },
    { GH_STATUS_INBOX_INACTIVE, GH_STATUS_BANNER_NONE, NULL },
  };
  for (guint i = 0; i < G_N_ELEMENTS(inbox); i++) {
    gh_status_set_inbox(status, inbox[i].inbox, NULL);
    assert_banner(window, inbox[i].banner, inbox[i].title);
    g_assert_cmpint(gh_status_banner_is_problem(inbox[i].banner), ==,
                    inbox[i].banner != GH_STATUS_BANNER_NONE &&
                    inbox[i].banner != GH_STATUS_BANNER_LOOKING &&
                    inbox[i].banner != GH_STATUS_BANNER_CONNECTING &&
                    inbox[i].banner != GH_STATUS_BANNER_BACKFILLING);
  }

  /* A local failure with nothing listed shows the error page, with the
   * receive path's own explanation. */
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");
  gh_status_set_inbox(status, GH_STATUS_INBOX_ERROR, "The DM seen-set is unusable: test");
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "error");
  AdwStatusPage *error_page = ADW_STATUS_PAGE(gtk_stack_get_visible_child(stack));
  g_assert_cmpstr(adw_status_page_get_description(error_page), ==,
                  "The DM seen-set is unusable: test");
  g_assert_cmpstr(gh_status_get_inbox_error(status), ==, "The DM seen-set is unusable: test");
  gh_status_set_inbox(status, GH_STATUS_INBOX_LIVE, "ignored");
  g_assert_null(gh_status_get_inbox_error(status));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");
  g_assert_cmpstr(adw_status_page_get_description(error_page), ==,
                  "Groundhog can't receive messages on this device right now.");

  gtk_window_destroy(GTK_WINDOW(window));
}

/* ---- UX-1 / UX-2 layout ---------------------------------------------------- */

static gboolean
is_laid_out(gpointer data)
{
  GtkWidget *window = data;
  return gtk_widget_get_mapped(window) && gtk_widget_get_width(window) > 0;
}

static gboolean
is_collapsed(gpointer data)
{
  return is_laid_out(data) && adw_navigation_split_view_get_collapsed(
                                gh_window_get_split(GH_WINDOW(data)));
}

static gboolean
is_not_collapsed(gpointer data)
{
  return !is_collapsed(data);
}

static gboolean
is_expanded(gpointer data)
{
  return is_laid_out(data) && !adw_navigation_split_view_get_collapsed(
                                 gh_window_get_split(GH_WINDOW(data)));
}

/* No size request is added here: the template's own 360x294 must suffice
 * (the nostrc-qp24.8.4 warning tolerance is gone; warnings are fatal). */
static GhWindow *
new_mapped_window(int width, int height)
{
  GhWindow *window = gh_window_new(NULL);
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  gtk_window_present(GTK_WINDOW(window));
  return window;
}

static void
drain_idle(void)
{
  for (int i = 0; i < 100 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

static void
test_breakpoint_collapses_below_600sp(void)
{
  GhWindow *wide = new_mapped_window(900, 600);
  spin_until(is_expanded, wide);
  drain_idle();
  g_assert_null(adw_application_window_get_current_breakpoint(ADW_APPLICATION_WINDOW(wide)));
  g_assert_false(adw_navigation_split_view_get_collapsed(gh_window_get_split(wide)));
  gtk_window_destroy(GTK_WINDOW(wide));

  GhWindow *narrow = new_mapped_window(590, 600);
  spin_until(is_collapsed, narrow);
  AdwBreakpoint *breakpoint =
    adw_application_window_get_current_breakpoint(ADW_APPLICATION_WINDOW(narrow));
  g_assert_nonnull(breakpoint);
  g_autofree char *condition =
    adw_breakpoint_condition_to_string(adw_breakpoint_get_condition(breakpoint));
  g_assert_cmpstr(condition, ==, "max-width: 600sp");
  gtk_window_destroy(GTK_WINDOW(narrow));
}

/* W29 (nostrc-lol6): a long conversation title is readable at every width.
 * Wide: the header label shows it (middle-ellipsized only when it truly
 * cannot fit). Narrow (<= 600sp): the header label gives way to a full-width
 * two-line label that wraps and never ellipsizes, so the title the owner
 * saw cut to uselessness at 360px reads in full. */
static void
test_narrow_title_not_truncated(void)
{
  const gchar *title = "Weekend hiking plans with the whole Groundhog burrow crew 2026";
  GhWindow *window = new_mapped_window(360, 294);
  spin_until(is_collapsed, window);
  GhContentPage *page = gh_window_get_content(window);
  gh_content_page_set_title(page, title, NULL);
  gh_content_page_set_conversation_shown(page, TRUE);
  /* Collapsed, the content page is only laid out once it is shown. */
  adw_navigation_split_view_set_show_content(gh_window_get_split(window), TRUE);
  drain_idle();
  GtkLabel *narrow = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(page),
                                                             GH_TYPE_CONTENT_PAGE,
                                                             "narrow_title_label"));
  GtkLabel *header = GTK_LABEL(gtk_widget_get_template_child(GTK_WIDGET(page),
                                                             GH_TYPE_CONTENT_PAGE,
                                                             "title_label"));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(narrow)));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(header)));
  g_assert_cmpstr(gtk_label_get_text(narrow), ==, title);
  PangoLayout *layout = gtk_label_get_layout(narrow);
  g_assert_false(pango_layout_is_ellipsized(layout));
  g_assert_cmpint(pango_layout_get_line_count(layout), <=, 2);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(narrow)), >, 0);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(narrow)), <=, 360);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(narrow)), ==, title);

  /* Wide again: the header label takes over, with the full text available. */
  gtk_window_set_default_size(GTK_WINDOW(window), 900, 600);
  drain_idle();
  spin_until(is_not_collapsed, window);
  drain_idle();
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(header)));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(narrow)));
  g_assert_cmpstr(gtk_label_get_text(header), ==, title);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(GTK_WIDGET(header)), ==, title);
  gtk_window_destroy(GTK_WINDOW(window));
}

/* UX-1: mapped at the 360x294 minimum, the window is collapsed, the sidebar
 * is laid out within it, and its measured minimum does not exceed it. */
static void
test_minimum_size_layout(void)
{
  GhWindow *window = new_mapped_window(360, 294);
  spin_until(is_collapsed, window);
  drain_idle();
  int min_width = 0, min_height = 0;
  gtk_widget_measure(GTK_WIDGET(window), GTK_ORIENTATION_HORIZONTAL, -1, &min_width, NULL,
                     NULL, NULL);
  gtk_widget_measure(GTK_WIDGET(window), GTK_ORIENTATION_VERTICAL, 360, &min_height, NULL,
                     NULL, NULL);
  g_assert_cmpint(min_width, <=, 360);
  g_assert_cmpint(min_height, <=, 294);
  GtkWidget *sidebar = GTK_WIDGET(gh_window_get_sidebar(window));
  g_assert_true(gtk_widget_get_mapped(sidebar));
  g_assert_cmpint(gtk_widget_get_width(sidebar), <=, 360);
  g_assert_cmpint(gtk_widget_get_width(sidebar), >, 0);

  /* Every banner still fits: the longest one, revealed at the minimum. */
  GhStatus *status = gh_window_get_status(window);
  gh_status_set_account_active(status, TRUE);
  gh_status_set_inbox(status, GH_STATUS_INBOX_LOOKUP_FAILED, NULL);
  drain_idle();
  gtk_widget_measure(GTK_WIDGET(window), GTK_ORIENTATION_VERTICAL, 360, &min_height, NULL,
                     NULL, NULL);
  g_assert_cmpint(min_height, <=, 294);
  gtk_window_destroy(GTK_WINDOW(window));
}

/* Building the same templates under a forced RTL default direction must not
 * trip a GTK critical. This proves construction is bidi-safe; it cannot
 * prove correct visual mirroring, which still needs a manual GNOME check
 * with an RTL locale. */
static void
test_rtl_construction_is_safe(void)
{
  GtkTextDirection previous = gtk_widget_get_default_direction();
  gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);

  GhWindow *window = gh_window_new(NULL);
  GhSidebarPage *sidebar = gh_window_get_sidebar(window);
  gh_sidebar_page_show_onboarding(sidebar);
  assert_stack_pages(gh_sidebar_page_get_stack(sidebar), TRUE);
  g_assert_nonnull(gh_window_get_content(window));
  gtk_window_destroy(GTK_WINDOW(window));

  gtk_widget_set_default_direction(previous);
}

int
main(int argc, char **argv)
{
  /* Matches groundhog-launch's own skip convention (see CMakeLists.txt):
   * a missing display is an environment limitation, not a test failure. */
  if (!gtk_init_check()) {
    g_printerr("groundhog-shell test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  /* The application's stylesheet, as main.c loads it, so sizes include it. */
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* 96 dpi makes 1sp one pixel on every platform (GTK on macOS reports 72),
   * so the charter's widths (UX-2: 900 split, 590 collapsed) apply as is;
   * without animations a transition completes in one layout. */
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
               "gtk-enable-animations", FALSE, NULL);
  /* GNOME's window controls (close only), the desktop the 360x294 minimum is
   * for (charter §7.12); GTK's own default adds minimize and maximize, which
   * need about 5 px more in the collapsed sidebar header (follow-up bead). */
  g_object_set(gtk_settings_get_default(), "gtk-decoration-layout", "appmenu:close", NULL);

  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/groundhog/shell/sidebar-page-structure", test_sidebar_page_structure);
  g_test_add_func("/groundhog/shell/sidebar-list-states", test_sidebar_list_states);
  g_test_add_func("/groundhog/shell/sidebar-search-states", test_sidebar_search_states);
  g_test_add_func("/groundhog/shell/content-page-structure", test_content_page_structure);
  g_test_add_func("/groundhog/shell/window-structure", test_window_structure);
  g_test_add_func("/groundhog/shell/shortcuts-bound-and-listed",
                  test_shortcuts_bound_and_listed);
  g_test_add_func("/groundhog/shell/status-banners", test_status_banners);
  g_test_add_func("/groundhog/shell/breakpoint-collapses-below-600sp",
                  test_breakpoint_collapses_below_600sp);
  g_test_add_func("/groundhog/shell/minimum-size-layout", test_minimum_size_layout);
  g_test_add_func("/groundhog/shell/narrow-title-not-truncated", test_narrow_title_not_truncated);
  g_test_add_func("/groundhog/shell/rtl-construction-is-safe", test_rtl_construction_is_safe);
  return g_test_run();
}
