#include "gh-emoji-images.h"
#include "gh-reaction-picker.h"
#include "gh-composer.h"
#include "gh-nip17-envelope.h"
#include "gh-social-lists.h"
#include "gh-message-row.h"
#include "gh-conversation-view.h"
#include "gh-conversation-store.h"
#include "nostrc-test-gdk-frame.h"
#include "nostrc-test-bus.h"
#include <string.h>
#include <json.h>
void groundhog_register_resource(void);
#define ACCOUNT "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define PEER "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"

static GPtrArray *tags_new(void)
{
  GPtrArray *tags = gh_emoji_tags_new();
  const gchar *tag[] = {"emoji", "test-face", "https://example.com/face.png", NULL};
  g_ptr_array_add(tags, g_strdupv((gchar **)tag));
  return tags;
}
static void test_tags(void)
{
  g_autoptr(GPtrArray) tags = tags_new();
  const gchar *peers[] = {PEER, NULL};
  g_autofree gchar *id = NULL;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gh_nip17_rumor_new_room_with_tags(ACCOUNT, peers,
    "hello :test-face:", tags, 1700000000, 0, &id, &error);
  g_assert_no_error(error); g_assert_nonnull(json);
  g_autoptr(GPtrArray) parsed = gh_emoji_parse(json);
  g_assert_cmpuint(parsed->len, ==, 1);
  gchar **tag = g_ptr_array_index(parsed, 0);
  g_assert_cmpstr(tag[2], ==, "https://example.com/face.png");
  NostrEvent *event = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(event, json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_validate_id(event, NULL), ==, NOSTR_EVENT_VALIDATION_OK);
  nostr_event_free(event);
  g_autoptr(GPtrArray) unused = gh_emoji_tags_for_text(tags, "hello");
  g_assert_cmpuint(unused->len, ==, 0);
}
static void test_length(void)
{
  gchar name[64]; memset(name, 'a', 62); name[62] = 0;
  const gchar *tag[] = {"emoji", name, "https://example.com/image.png", NULL};
  g_assert_true(gh_emoji_tag_valid(tag));
  name[62] = 'a'; name[63] = 0;
  g_assert_false(gh_emoji_tag_valid(tag));
  const gchar *invalid[] = {"emoji", "bad", "javascript:bad", NULL};
  g_assert_false(gh_emoji_tag_valid(invalid));
}
static guint fetches;
static gboolean valid_artwork;
static void get(gpointer data, const gchar *uri, gsize max, GCancellable *cancel,
                GAsyncReadyCallback callback, gpointer user_data)
{
  (void)data; (void)uri; (void)max;
  fetches++;
  GTask *task = g_task_new(NULL, cancel, callback, user_data);
  GBytes *bytes;
  if (valid_artwork) {
    static const guint8 pixel[] = {255, 0, 0, 255};
    g_autoptr(GBytes) pixels = g_bytes_new_static(pixel, sizeof pixel);
    g_autoptr(GdkTexture) texture = gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, pixels, 4);
    bytes = gdk_texture_save_to_png_bytes(texture);
  } else bytes = g_bytes_new_static("not an image", 12);
  g_task_return_pointer(task, bytes, (GDestroyNotify)g_bytes_unref);
  g_object_unref(task);
}
static GBytes *finish(gpointer data, GAsyncResult *result, GError **error)
{
  (void)data; return g_task_propagate_pointer(G_TASK(result), error);
}
static const GhHttpTransport transport = {get, finish};
static void test_no_fetch(void)
{
  fetches = 0;
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_boolean(settings, "load-remote-images", TRUE);
  g_autoptr(GhWebContent) web = gh_web_content_new(settings, &transport, NULL);
  g_autoptr(GPtrArray) tags = tags_new();
  GtkWidget *images = g_object_ref_sink(gh_emoji_images_new(tags, NULL, settings, web));
  GtkWidget *message = g_object_ref_sink(gh_emoji_images_new(tags, "hello :test-face:", settings, web));
  while (g_main_context_iteration(NULL, FALSE));
  g_assert_cmpuint(fetches, ==, 0);
  g_assert_false(gh_emoji_images_has_artwork(GH_EMOJI_IMAGES(message)));
  gh_emoji_images_revoke(GH_EMOJI_IMAGES(images));
  g_object_unref(images); g_object_unref(message);
  g_settings_reset(settings, "load-remote-images");
}
static GtkWidget *find_widget(GtkWidget *widget, GType type, const gchar *label)
{
  if (g_type_is_a(G_OBJECT_TYPE(widget), type) && (!label ||
      (GTK_IS_BUTTON(widget) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label)) ||
      !g_strcmp0(gtk_widget_get_tooltip_text(widget), label))) return widget;
  for (GtkWidget *child = gtk_widget_get_first_child(widget); child; child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_widget(child, type, label);
    if (found) return found;
  }
  return NULL;
}
static void changed(GhEmojiImages *images, guint *count) { (void)images; (*count)++; }
static void test_artwork(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autoptr(GhWebContent) web = gh_web_content_new(settings, &transport, NULL);
  g_autoptr(GPtrArray) tags = tags_new();
  for (guint good = 0; good < 2; good++) {
    valid_artwork = good;
    fetches = 0;
    GtkWidget *window = adw_window_new();
    GtkWidget *images = gh_emoji_images_new(tags, "hello :test-face:", settings, web);
    adw_window_set_content(ADW_WINDOW(window), images);
    gtk_window_present(GTK_WINDOW(window));
    guint changes = 0;
    g_signal_connect(images, "changed", G_CALLBACK(changed), &changes);
    GtkWidget *button = find_widget(images, GTK_TYPE_BUTTON, "Load Custom Emoji");
    g_assert_nonnull(button);
    g_signal_emit_by_name(button, "clicked");
    AdwDialog *dialog = adw_window_get_visible_dialog(ADW_WINDOW(window));
    g_assert_true(ADW_IS_ALERT_DIALOG(dialog));
    g_assert_cmpuint(fetches, ==, 0);
    g_assert_nonnull(strstr(adw_alert_dialog_get_body(ADW_ALERT_DIALOG(dialog)), "example.com"));
    g_assert_cmpstr(adw_alert_dialog_get_default_response(ADW_ALERT_DIALOG(dialog)), ==, "cancel");
    GtkWidget *allow = find_widget(GTK_WIDGET(dialog), GTK_TYPE_BUTTON, "_Load Images");
    g_assert_nonnull(allow);
    g_signal_emit_by_name(allow, "clicked");
    while (!changes) g_main_context_iteration(NULL, TRUE);
    g_assert_cmpuint(fetches, ==, 1);
    g_assert_cmpint(gh_emoji_images_has_artwork(GH_EMOJI_IMAGES(images)), ==, good);
    GtkWidget *inline_text = find_widget(images, GTK_TYPE_TEXT_VIEW, NULL);
    g_assert_cmpint(inline_text != NULL, ==, good);
    gh_emoji_images_revoke(GH_EMOJI_IMAGES(images));
    g_assert_false(gh_emoji_images_has_artwork(GH_EMOJI_IMAGES(images)));
    gtk_window_destroy(GTK_WINDOW(window));
    while (g_main_context_iteration(NULL, FALSE));
  }
}
static void test_picker_composer(void)
{
  GhStore *store = gh_store_open_ephemeral(ACCOUNT, NULL, NULL);
  g_autoptr(GhSocialLists) lists = gh_social_lists_new(NULL);
  g_assert_true(gh_social_lists_bind_store(lists, store, 1, NULL));
  g_autoptr(GError) error = NULL;
  g_autoptr(GNostrListDocument) doc = gh_social_lists_import(lists,
    "{\"kind\":10030,\"identifier\":\"\",\"public\":[],\"private\":[[\"emoji\",\"test-face\",\"https://example.com/face.png\"]]}", &error);
  g_assert_no_error(error); g_assert_nonnull(doc);
  g_autoptr(GPtrArray) catalog = gh_emoji_catalog(lists);
  g_assert_cmpuint(catalog->len, ==, 1);
  g_assert_cmpstr(((GhEmojiSet *)g_ptr_array_index(catalog, 0))->title, ==, "Favorites");
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_autoptr(GhWebContent) web = gh_web_content_new(settings, &transport, NULL);
  GtkWidget *window = adw_window_new();
  GtkWidget *composer = gh_composer_new();
  gh_composer_set_emoji_source(GH_COMPOSER(composer), lists, settings, web);
  adw_window_set_content(ADW_WINDOW(window), composer);
  gtk_window_present(GTK_WINDOW(window));
  GtkWidget *button = GTK_WIDGET(gtk_widget_get_template_child(composer, GH_TYPE_COMPOSER, "emoji_button"));
  gtk_menu_button_popup(GTK_MENU_BUTTON(button));
  GtkWidget *picker = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(button)));
  GtkWidget *custom = find_widget(picker, GTK_TYPE_BUTTON, ":test-face:");
  g_assert_nonnull(custom);
  fetches = 0;
  g_signal_emit_by_name(custom, "clicked");
  g_autofree gchar *text = gh_composer_dup_text(GH_COMPOSER(composer));
  g_assert_cmpstr(text, ==, ":test-face:");
  g_autoptr(GPtrArray) selected = gh_composer_dup_emoji_tags(GH_COMPOSER(composer));
  g_assert_cmpuint(selected->len, ==, 1);
  g_assert_cmpuint(fetches, ==, 0);
  g_autoptr(GNostrListDocument) refreshed = gh_social_lists_import(lists,
    "{\"kind\":30030,\"identifier\":\"another-set\",\"public\":[[\"emoji\",\"test-face\",\"https://example.com/new.png\"]],\"private\":[]}", &error);
  g_assert_no_error(error);
  g_assert_nonnull(refreshed);
  g_autoptr(GPtrArray) retained = gh_composer_dup_emoji_tags(GH_COMPOSER(composer));
  g_assert_cmpuint(retained->len, ==, 1);
  g_assert_cmpstr(((gchar **)g_ptr_array_index(retained, 0))[2], ==, "https://example.com/face.png");
  gh_social_lists_close(lists);
  g_autoptr(GPtrArray) cleared = gh_composer_dup_emoji_tags(GH_COMPOSER(composer));
  g_assert_cmpuint(cleared->len, ==, 0);
  gtk_window_destroy(GTK_WINDOW(window));
  gh_store_close(store);
  while (g_main_context_iteration(NULL, FALSE));
}

static void test_requests(void)
{
  g_autoptr(GPtrArray) tags = tags_new();
  const gchar *recipients[] = {ACCOUNT, NULL};
  g_autofree gchar *json = gh_nip17_rumor_new_room_with_tags(PEER, recipients,
    "hello :test-face:", tags, g_get_real_time() / G_USEC_PER_SEC, 0, NULL, NULL);
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(ACCOUNT, json, NULL);
  g_assert_nonnull(message);
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, ACCOUNT, NULL, NULL, NULL);
  g_assert_cmpint(gh_conversation_store_add_message(store, message, NULL), ==, GH_CONVERSATION_ADD_NEW);
  GhConversation *room = gh_conversation_store_lookup(store, gh_message_get_room_id(message));
  g_assert_true(gh_conversation_get_is_request(room));
  GtkWidget *window = adw_window_new();
  GtkWidget *view = gh_conversation_view_new();
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  gh_conversation_view_set_settings(GH_CONVERSATION_VIEW(view), settings);
  gh_conversation_view_enable_web_content(GH_CONVERSATION_VIEW(view), &transport, NULL);
  gh_conversation_view_set_conversation(GH_CONVERSATION_VIEW(view), room);
  adw_window_set_content(ADW_WINDOW(window), view);
  gtk_window_present(GTK_WINDOW(window));
  fetches = 0;
  while (!find_widget(view, GH_TYPE_MESSAGE_ROW, NULL)) g_main_context_iteration(NULL, TRUE);
  g_assert_null(find_widget(view, GH_TYPE_EMOJI_IMAGES, NULL));
  g_assert_null(find_widget(view, GTK_TYPE_BUTTON, "Load Custom Emoji"));
  g_assert_cmpuint(fetches, ==, 0);
  gh_conversation_view_set_conversation(GH_CONVERSATION_VIEW(view), NULL);
  gh_conversation_accept(room);
  gh_conversation_view_set_conversation(GH_CONVERSATION_VIEW(view), room);
  while (!find_widget(view, GH_TYPE_EMOJI_IMAGES, NULL)) g_main_context_iteration(NULL, TRUE);
  g_assert_nonnull(find_widget(view, GTK_TYPE_BUTTON, "Load Custom Emoji"));
  g_assert_cmpuint(fetches, ==, 0);
  gtk_window_destroy(GTK_WINDOW(window));
  while (g_main_context_iteration(NULL, FALSE));
}

int main(int argc, char **argv)
{
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  if (!nostrc_test_bus_available()) return 77;
  NostrcTestBus *bus = nostrc_test_bus_new(NOSTRC_TEST_BUS_FLAGS_NONE);
  nostrc_test_bus_up(bus);
  if (!gtk_init_check()) { nostrc_test_bus_down(bus); return 77; }
  adw_init(); nostr_json_init(); groundhog_register_resource();
  nostrc_test_bus_add_func("/emoji/nip17-tags", test_tags);
  nostrc_test_bus_add_func("/emoji/length-cap", test_length);
  nostrc_test_bus_add_func("/emoji/artwork-consent-and-fallback", test_artwork);
  nostrc_test_bus_add_func("/emoji/picker-composer", test_picker_composer);
  nostrc_test_bus_add_func("/privacy/emoji/requests-never-load", test_requests);
  nostrc_test_bus_add_func("/privacy/emoji/no-default-fetch", test_no_fetch);
  int result = g_test_run();
  nostrc_test_bus_down(bus);
  return result;
}
