#include "gh-social-lists-backend.h"
#include "gh-list-editor-page.h"
#include "gh-preferences-dialog.h"
#include "nostrc-test-gdk-frame.h"
#include <nostr-kinds.h>
#include <sqlite3.h>
#define ACCOUNT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
void groundhog_register_resource(void);

static GtkWidget *child(GtkWidget *widget, GType type, const gchar *name)
{
  return GTK_WIDGET(gtk_widget_get_template_child(widget, type, name));
}
static GtkWidget *find_type(GtkWidget *widget, GType type)
{
  if (g_type_is_a(G_OBJECT_TYPE(widget), type)) return widget;
  for (GtkWidget *next = gtk_widget_get_first_child(widget); next; next = gtk_widget_get_next_sibling(next)) {
    GtkWidget *found = find_type(next, type);
    if (found) return found;
  }
  return NULL;
}
static gboolean frame(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget; (void)clock;
  g_main_loop_quit(data);
  return G_SOURCE_REMOVE;
}
static void present(GtkWindow *window)
{
  GMainLoop *loop = g_main_loop_new(NULL, FALSE);
  gtk_widget_add_tick_callback(GTK_WIDGET(window), frame, loop, NULL);
  gtk_window_present(window);
  g_main_loop_run(loop);
  g_main_loop_unref(loop);
}
static void test_real_backend_editor(void)
{
  GError *error = NULL;
  GhStore *store = gh_store_open_ephemeral(ACCOUNT, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhSocialLists) owner = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(owner, store, 1, &error));
  g_assert_no_error(error);
  GhListsBackend backend = gh_social_lists_backend(owner);
  g_autoptr(GNostrListDocument) doc = gh_social_lists_ensure(owner, NOSTR_KIND_INTEREST_SETS, "test", &error);
  g_assert_no_error(error);
  GhListEditorPage *editor = g_object_ref_sink(gh_list_editor_page_new(&backend, doc));
  g_assert_true(gh_list_editor_page_save_tag(editor, "[\"t\",\"one\",\"\",\"extra\"]", TRUE, &error));
  g_assert_no_error(error);
  g_signal_emit_by_name(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "new_entry"), "clicked");
  g_assert_true(gh_list_editor_page_save_tag(editor, "[\"x-extension\",\"opaque\",\"\",\"tail\"]", TRUE, &error));
  g_assert_no_error(error);
  GNostrListDocument *saved = gh_list_editor_page_get_snapshot(editor);
  g_assert_cmpuint(gnostr_list_document_get_local_revision(saved), ==, 2);
  const NostrListDocument *codec = gnostr_list_document_get_codec(saved);
  g_assert_cmpuint(nostr_nip51_document_tag_count(codec, TRUE), ==, 2);
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, TRUE, 0), 3), ==, "extra");
  GtkListView *entries = GTK_LIST_VIEW(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "entries"));
  GtkSingleSelection *selection = GTK_SINGLE_SELECTION(gtk_list_view_get_model(entries));
  gtk_single_selection_set_selected(selection, 1);
  g_signal_emit_by_name(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "move_up"), "clicked");
  codec = gnostr_list_document_get_codec(gh_list_editor_page_get_snapshot(editor));
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, TRUE, 0), 0), ==, "x-extension");
  gtk_single_selection_set_selected(selection, 0);
  g_assert_true(gh_list_editor_page_save_tag(editor, "[\"x-extension\",\"updated\",\"\",\"retained\"]", TRUE, &error));
  g_assert_no_error(error);
  GtkWidget *title = child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "title_row");
  GtkWidget *raw = child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "raw_row");
  const gchar *pending = "[\"x-extension\",\"unsaved\",\"\",\"retained\"]";
  gtk_editable_set_text(GTK_EDITABLE(raw), pending);
  gtk_editable_set_text(GTK_EDITABLE(title), "Local title");
  g_signal_emit_by_name(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "save_metadata"), "clicked");
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(raw)), ==, pending);
  g_assert_cmpuint(gtk_single_selection_get_selected(selection), !=, GTK_INVALID_LIST_POSITION);
  g_signal_emit_by_name(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "save_entry"), "clicked");
  codec = gnostr_list_document_get_codec(gh_list_editor_page_get_snapshot(editor));
  g_assert_cmpuint(nostr_nip51_document_tag_count(codec, TRUE), ==, 2);
  g_assert_cmpstr(nostr_nip51_tag_field(nostr_nip51_document_tag(codec, TRUE, 0), 1), ==, "unsaved");
  /* Removing the title clears its detail field and an unrelated details save
   * must not resurrect it or manufacture other empty metadata tags. */
  gtk_single_selection_set_selected(selection, 0);
  g_signal_emit_by_name(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "remove_entry"), "clicked");
  g_assert_cmpstr(gtk_editable_get_text(GTK_EDITABLE(title)), ==, "");
  g_signal_emit_by_name(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "save_metadata"), "clicked");
  codec = gnostr_list_document_get_codec(gh_list_editor_page_get_snapshot(editor));
  g_assert_cmpuint(nostr_nip51_document_tag_count(codec, FALSE), ==, 1);
  saved = gh_list_editor_page_get_snapshot(editor);
  guint64 revision = gnostr_list_document_get_local_revision(saved);
  g_assert_true(gh_store_exec(store, "CREATE TRIGGER reject_social BEFORE UPDATE ON social_documents BEGIN SELECT RAISE(ABORT,'injected'); END", &error));
  g_assert_false(gh_list_editor_page_save_tag(editor, "[\"t\",\"failure\"]", TRUE, &error));
  g_assert_nonnull(error); g_clear_error(&error);
  g_assert_cmpuint(gnostr_list_document_get_local_revision(gh_list_editor_page_get_snapshot(editor)), ==, revision);
  g_assert_true(g_str_has_prefix(gh_list_editor_page_get_status(editor), "Not saved"));
  g_assert_true(gh_store_exec(store, "DROP TRIGGER reject_social", &error));
  /* Present the actual editor at narrow and desktop widths, event-driven. */
  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_child(window, GTK_WIDGET(editor));
  gtk_window_set_default_size(window, 360, 640);
  present(window);
  g_assert_cmpint(gtk_widget_get_width(GTK_WIDGET(editor)), >=, 360);
  gtk_window_set_default_size(window, 960, 640);
  present(window);
  g_assert_true(gtk_widget_grab_focus(child(GTK_WIDGET(editor), GH_TYPE_LIST_EDITOR_PAGE, "save_entry")));
  gh_social_lists_close(owner);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(editor)));
  g_assert_false(gh_list_editor_page_save_tag(editor, "[\"t\",\"stale\"]", TRUE, &error));
  g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CLOSED); g_clear_error(&error);
  gtk_window_destroy(window);
  g_object_unref(editor);
  gh_store_close(store);
}

static void test_preferences_manager(void)
{
  GError *error = NULL;
  GhStore *store = gh_store_open_ephemeral(ACCOUNT, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhSocialLists) owner = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(owner, store, 1, &error));
  GhListsBackend backend = gh_social_lists_backend(owner);
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  GhPreferencesDialog *prefs = g_object_ref_sink(gh_preferences_dialog_new(settings, 0));
  gh_preferences_dialog_set_social_backend(prefs, &backend);
  GtkWidget *row = child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "social_sync_row");
  g_assert_false(adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
  g_assert_false(gtk_widget_get_sensitive(row));
  /* Even programmatic activation cannot bypass the store-backed backend. */
  adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
  g_assert_false(adw_switch_row_get_active(ADW_SWITCH_ROW(row)));
  g_assert_false(gh_social_lists_get_sync_enabled(owner));
  GtkWindow *window = GTK_WINDOW(adw_window_new());
  gtk_window_set_default_size(window, 960, 640);
  present(window);
  adw_dialog_present(ADW_DIALOG(prefs), GTK_WIDGET(window));
  adw_preferences_dialog_set_visible_page_name(ADW_PREFERENCES_DIALOG(prefs), "account");
  g_signal_emit_by_name(child(GTK_WIDGET(prefs), GH_TYPE_PREFERENCES_DIALOG, "social_lists_row"), "activated");
  present(window);
  GhListsPage *page = GH_LISTS_PAGE(find_type(GTK_WIDGET(prefs), GH_TYPE_LISTS_PAGE));
  g_assert_nonnull(page);
  g_object_ref(page);
  GtkWidget *id = child(GTK_WIDGET(page), GH_TYPE_LISTS_PAGE, "identifier_row");
  GtkWidget *create = child(GTK_WIDGET(page), GH_TYPE_LISTS_PAGE, "create_list");
  gtk_editable_set_text(GTK_EDITABLE(id), "created-in-ui");
  g_assert_true(gtk_widget_get_sensitive(create));
  g_signal_emit_by_name(create, "clicked");
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(gh_social_lists_get_service(owner))), ==, 1);
  g_assert_nonnull(find_type(GTK_WIDGET(prefs), GH_TYPE_LIST_EDITOR_PAGE));
  GtkListView *documents = GTK_LIST_VIEW(child(GTK_WIDGET(page), GH_TYPE_LISTS_PAGE, "documents"));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(gtk_list_view_get_model(documents))), ==, 1);
  g_assert_false(gh_lists_page_kind_visible(NOSTR_KIND_MUTE_LIST));
  g_assert_false(gh_lists_page_kind_visible(NOSTR_KIND_BLOSSOM_SERVER_LIST));
  g_assert_false(gh_lists_page_kind_visible(NOSTR_KIND_PROFILE_BADGE_LIST));
  g_assert_false(gh_lists_page_kind_visible(NOSTR_KIND_EMOJI_SETS));
  g_assert_true(gh_lists_page_kind_visible(NOSTR_KIND_SLASH_COMMAND_DEFINITION));
  gh_social_lists_close(owner);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(page)));
  g_object_unref(page);
  gtk_window_destroy(window);
  g_object_unref(prefs);
  gh_store_close(store);
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  if (!gtk_init_check()) return 77;
  adw_init();
  g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
  groundhog_register_resource();
  g_test_add_func("/groundhog/lists-ui/production-editor", test_real_backend_editor);
  g_test_add_func("/groundhog/lists-ui/preferences-manager", test_preferences_manager);
  return g_test_run();
}
