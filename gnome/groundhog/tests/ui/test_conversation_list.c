/* The conversation list of the Groundhog shell and the conversation it shows
 * (charter G11: UX-1, UX-3 list, UX-8), on a real GhConversationStore fed
 * through its admission API with fixture rumors: no relay, no signer, no
 * network. Covered: list order, unread badges and bold titles, message
 * requests kept apart (entry, request mode, accepting), search by title and
 * npub, "no results", selection showing the conversation's messages (marked
 * read locally), previous/next, the empty state and status banner, the
 * accessible labels of the list and message items, the 360x294 collapsed
 * layout and its navigation, and the inbox banner wiring on a real account,
 * relay-list and inbox stack with no relay configured. Needs a display: it
 * self-skips (77) without one. Waits iterate the main context against a
 * deadline; they never sleep. With GROUNDHOG_TEST_SCREENSHOTS=<dir> the
 * screenshots case also renders the fixture to PNGs (wide and 360x294, light
 * and dark); otherwise it is skipped.
 */
#include "gh-conversation-list.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-timeline-row.h"
#include "gh-message-row.h"
#include "gh-identity.h"
#include "gh-inbox-status.h"

#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include "nostr/nip19/nip19.h"

#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>

void groundhog_register_resource(void);

/* A (1) is the account; B-E (2-5) write to it. */
#define KEYS 6
static const gchar *const secrets[KEYS] = {
  NULL,
  "0000000000000000000000000000000000000000000000000000000000000001",
  "0000000000000000000000000000000000000000000000000000000000000002",
  "0000000000000000000000000000000000000000000000000000000000000003",
  "0000000000000000000000000000000000000000000000000000000000000004",
  "0000000000000000000000000000000000000000000000000000000000000005",
};
static gchar *hex[KEYS];
static gchar *npub[KEYS];

static void
init_keys(void)
{
  for (guint key = 1; key < KEYS; key++) {
    char *pub = nostr_key_get_public(secrets[key]);
    g_assert_nonnull(pub);
    hex[key] = g_strdup(pub);
    free(pub);
    guint8 bytes[32];
    char *encoded = NULL;
    g_assert_true(nostr_hex2bin(bytes, hex[key], sizeof bytes));
    g_assert_cmpint(nostr_nip19_encode_npub(bytes, &encoded), ==, 0);
    npub[key] = g_strdup(encoded);
    free(encoded);
  }
}

/* ---- fixture rumors ---------------------------------------------------------- */

static GhMessage *
message(guint author, guint recipient, gint64 created_at, const gchar *content,
        const gchar *subject)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, hex[author]);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", hex[recipient], NULL));
  if (subject)
    nostr_tags_append(tags, nostr_tag_new("subject", subject, NULL));
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *result = gh_message_new_from_rumor(hex[1], json, &error);
  free(json);
  g_assert_no_error(error);
  return result;
}

static GhMessage *
add(GhConversationStore *store, guint author, guint recipient, gint64 created_at,
    const gchar *content, const gchar *subject)
{
  g_autoptr(GhMessage) m = message(author, recipient, created_at, content, subject);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_add_message(store, m, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  return m; /* borrowed: the store holds it */
}

static GhConversation *
room(GhConversationStore *store, guint peer)
{
  const gchar *a = hex[1], *b = hex[peer];
  g_autofree gchar *id = strcmp(a, b) < 0 ? g_strjoin(",", a, b, NULL)
                                          : g_strjoin(",", b, a, NULL);
  GhConversation *conversation = gh_conversation_store_lookup(store, id);
  g_assert_nonnull(conversation);
  return conversation;
}

/* ---- waits ------------------------------------------------------------------- */

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
  if (expired)
    g_error("condition waited for at line %d did not hold within 5s", line);
  g_source_remove(timer);
}
#define spin_until(pred, data) spin_until_at((pred), (data), __LINE__)

static void
drain_idle(void)
{
  for (int i = 0; i < 200 && g_main_context_iteration(NULL, FALSE); i++)
    ;
}

/* ---- widget lookups ---------------------------------------------------------- */

static gpointer
template_child(gpointer widget, GType type, const char *name)
{
  GObject *child = gtk_widget_get_template_child(GTK_WIDGET(widget), type, name);
  g_assert_nonnull(child);
  return child;
}

static void
collect(GtkWidget *widget, GType type, GPtrArray *found)
{
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(c, type))
      g_ptr_array_add(found, c);
    else
      collect(c, type, found);
  }
}

/* The bound rows of the sidebar list, in list order. */
static GPtrArray *
rows_of(GhSidebarPage *sidebar)
{
  g_autoptr(GPtrArray) all = g_ptr_array_new();
  GPtrArray *bound = g_ptr_array_new();
  collect(GTK_WIDGET(gh_sidebar_page_get_list(sidebar)), GH_TYPE_CONVERSATION_ROW, all);
  for (guint i = 0; i < all->len; i++)
    if (gh_conversation_row_get_conversation(g_ptr_array_index(all, i)))
      g_ptr_array_add(bound, g_ptr_array_index(all, i));
  return bound;
}

static GhConversationRow *
row_for(GhSidebarPage *sidebar, GhConversation *conversation)
{
  g_autoptr(GPtrArray) rows = rows_of(sidebar);
  for (guint i = 0; i < rows->len; i++)
    if (gh_conversation_row_get_conversation(g_ptr_array_index(rows, i)) == conversation)
      return g_ptr_array_index(rows, i);
  g_error("no row for %s", gh_conversation_get_title(conversation));
  return NULL;
}

static const char *
row_text(GhConversationRow *row, const char *name)
{
  return gtk_label_get_text(template_child(row, GH_TYPE_CONVERSATION_ROW, name));
}

static gboolean
row_shown(GhConversationRow *row, const char *name)
{
  return gtk_widget_get_visible(template_child(row, GH_TYPE_CONVERSATION_ROW, name));
}

/* The conversation view the binder installs in the content page (G12). */
static GhConversationView *
view_of(GhContentPage *content)
{
  return GH_CONVERSATION_VIEW(gh_content_page_get_view(content));
}

/* The messages of the conversation the content page shows, or NULL. */
static GListModel *
shown_messages(GhContentPage *content)
{
  GhConversation *conversation = gh_conversation_view_get_conversation(view_of(content));
  return conversation ? G_LIST_MODEL(conversation) : NULL;
}

static GtkWidget *
message_list_of(GhContentPage *content)
{
  return GTK_WIDGET(gh_conversation_view_get_message_list(view_of(content)));
}

/* A list item widget's bound message row (its child is a GhTimelineRow),
 * or NULL for a day separator or a local event. */
static GhMessageRow *
item_row(GtkWidget *item)
{
  GtkWidget *child = gtk_widget_get_first_child(item);
  if (!GH_IS_TIMELINE_ROW(child))
    return NULL;
  GhMessageRow *row = gh_timeline_row_get_message_row(GH_TIMELINE_ROW(child));
  return gh_message_row_get_message(row) ? row : NULL;
}

/* The list item widgets of the message list: each is a GtkListItem's
 * widget whose child holds a bound GhMessageRow (day separators are
 * skipped). */
static GPtrArray *
message_items(GhContentPage *content)
{
  GPtrArray *items = g_ptr_array_new();
  GtkWidget *list = message_list_of(content);
  for (GtkWidget *c = gtk_widget_get_first_child(list); c; c = gtk_widget_get_next_sibling(c))
    if (item_row(c))
      g_ptr_array_add(items, c);
  return items;
}

static GListModel *
list_model(GhSidebarPage *sidebar)
{
  return G_LIST_MODEL(gtk_list_view_get_model(gh_sidebar_page_get_list(sidebar)));
}

/* ---- fixture ----------------------------------------------------------------- */

typedef struct {
  GhConversationStore *store;
  GhWindow *window;
  GhSidebarPage *sidebar;
  GhContentPage *content;
  GhConversation *ab, *ac, *ad, *ae;
  gint64 now;
} Fixture;

/* A→B then B→A (accepted, 1 unread, newest); A→C with a subject (accepted,
 * read); D→A and E→A (message requests: nobody on the account wrote). */
static void
fixture_setup(Fixture *f, gconstpointer data)
{
  (void)data;
  f->now = g_get_real_time() / G_USEC_PER_SEC;
  f->store = gh_conversation_store_new();
  gh_conversation_store_set_account(f->store, hex[1], NULL, NULL, NULL);
  add(f->store, 1, 2, f->now - 7200, "hey B", NULL);
  add(f->store, 2, 1, f->now - 60, "reply from B\nsecond line", NULL);
  add(f->store, 1, 3, f->now - 3 * 86400, "hi C", "Book Club");
  add(f->store, 4, 1, f->now - 600, "are you there?", NULL);
  add(f->store, 5, 1, f->now - 10 * 86400, "hello from E", NULL);
  f->ab = room(f->store, 2);
  f->ac = room(f->store, 3);
  f->ad = room(f->store, 4);
  f->ae = room(f->store, 5);

  f->window = gh_window_new(NULL);
  f->sidebar = gh_window_get_sidebar(f->window);
  f->content = gh_window_get_content(f->window);
  gh_conversation_list_attach(f->window, f->store, NULL);
  GhStatus *status = gh_window_get_status(f->window);
  gh_status_set_account_active(status, TRUE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  gh_status_set_inbox(status, GH_STATUS_INBOX_LIVE, NULL);
}

static gboolean
is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

/* The window's binder must release the store: nothing outlives the window. */
static void
fixture_teardown(Fixture *f, gconstpointer data)
{
  (void)data;
  gpointer weak = f->store;
  g_object_add_weak_pointer(G_OBJECT(f->store), &weak);
  gtk_window_destroy(GTK_WINDOW(f->window));
  g_object_unref(f->store);
  spin_until(is_null, &weak);
}

static void
present(Fixture *f, int width, int height)
{
  gtk_window_set_default_size(GTK_WINDOW(f->window), width, height);
  gtk_window_present(GTK_WINDOW(f->window));
}

static gboolean
has_rows(gpointer data)
{
  Fixture *f = data;
  g_autoptr(GPtrArray) rows = rows_of(f->sidebar);
  return gtk_widget_get_mapped(GTK_WIDGET(f->sidebar)) &&
         rows->len == g_list_model_get_n_items(list_model(f->sidebar));
}

/* ---- tests ------------------------------------------------------------------- */

static void
assert_list(Fixture *f, GhConversation *const *expected, guint n)
{
  GListModel *model = list_model(f->sidebar);
  g_assert_cmpuint(g_list_model_get_n_items(model), ==, n);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhConversation) item = g_list_model_get_item(model, i);
    g_assert_true(item == expected[i]);
  }
}

static void
test_rows_order_and_badges(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f, 900, 600);
  spin_until(has_rows, f);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  GtkStack *stack = gh_sidebar_page_get_stack(f->sidebar);

  /* Accepted conversations only, newest activity first. */
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "conversations");
  GhConversation *const order[] = { f->ab, f->ac };
  assert_list(f, order, 2);
  g_autoptr(GPtrArray) rows = rows_of(f->sidebar);
  g_assert_true(gh_conversation_row_get_conversation(g_ptr_array_index(rows, 0)) == f->ab);
  g_assert_true(gh_conversation_row_get_conversation(g_ptr_array_index(rows, 1)) == f->ac);

  /* Unread: bold title and an accent badge with the count. */
  GhConversationRow *ab = row_for(f->sidebar, f->ab);
  const char *title = gh_conversation_get_title(f->ab);
  g_assert_true(g_str_has_prefix(title, "npub1"));
  g_assert_cmpstr(row_text(ab, "title_label"), ==, title);
  g_assert_true(row_shown(ab, "unread_badge"));
  g_assert_cmpstr(row_text(ab, "unread_badge"), ==, "1");
  g_assert_true(gtk_widget_has_css_class(
    template_child(ab, GH_TYPE_CONVERSATION_ROW, "title_label"), "groundhog-unread"));
  g_autofree char *ab_time = gh_conversation_row_format_time(f->now - 60, now);
  g_assert_cmpstr(row_text(ab, "time_label"), ==, ab_time);
  g_assert_false(row_shown(ab, "request_label"));
  /* Initials only: no picture is ever loaded (PD-2). */
  AdwAvatar *avatar = template_child(ab, GH_TYPE_CONVERSATION_ROW, "avatar");
  g_assert_cmpstr(adw_avatar_get_text(avatar), ==, title);
  g_assert_true(adw_avatar_get_show_initials(avatar));
  g_assert_null(adw_avatar_get_custom_image(avatar));

  /* Previews are off until the settings key exists (G01): no message text,
   * in the row or in its accessible label. */
  g_assert_cmpstr(row_text(ab, "preview_label"), ==, "");
  g_autofree char *ab_label = g_strdup_printf("%s. Private conversation. 1 unread. %s",
                                              title, ab_time);
  g_assert_cmpstr(gh_conversation_row_get_summary(ab), ==, ab_label);
  GtkWidget *item = gtk_widget_get_parent(GTK_WIDGET(ab));
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(item)), ==,
                  GTK_ACCESSIBLE_ROLE_LIST_ITEM);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      ab_label);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(gh_sidebar_page_get_list(f->sidebar)),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, "Conversations");

  /* Read, with a subject: plain title, no badge. */
  GhConversationRow *ac = row_for(f->sidebar, f->ac);
  g_assert_cmpstr(row_text(ac, "title_label"), ==, "Book Club");
  g_assert_false(row_shown(ac, "unread_badge"));
  g_assert_false(gtk_widget_has_css_class(
    template_child(ac, GH_TYPE_CONVERSATION_ROW, "title_label"), "groundhog-unread"));
  g_autofree char *ac_time = gh_conversation_row_format_time(f->now - 3 * 86400, now);
  g_autofree char *ac_label = g_strdup_printf("Book Club. Private conversation. %s", ac_time);
  g_assert_cmpstr(gh_conversation_row_get_summary(ac), ==, ac_label);

  /* With previews on: the newest message's first line, also in the label. */
  gh_sidebar_page_set_show_previews(f->sidebar, TRUE);
  g_assert_cmpstr(row_text(ab, "preview_label"), ==, "reply from B");
  g_autofree char *ab_preview = g_strdup_printf("%s. reply from B", ab_label);
  g_assert_cmpstr(gh_conversation_row_get_summary(ab), ==, ab_preview);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      ab_preview);
  gh_sidebar_page_set_show_previews(f->sidebar, FALSE);
  g_assert_cmpstr(row_text(ab, "preview_label"), ==, "");

  /* The requests are counted apart, never listed among conversations. */
  GtkWidget *requests = template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "requests_button");
  g_assert_true(gtk_widget_get_visible(requests));
  g_assert_cmpstr(gtk_label_get_text(template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE,
                                                    "requests_count")), ==, "2");
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(requests), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Message Requests, 2 conversations");

  /* New activity moves a conversation to the top and updates its row. */
  add(f->store, 3, 1, f->now - 5, "news from C", NULL);
  GhConversation *const moved[] = { f->ac, f->ab };
  assert_list(f, moved, 2);
  spin_until(has_rows, f);
  ac = row_for(f->sidebar, f->ac);
  g_assert_true(row_shown(ac, "unread_badge"));
  g_assert_cmpstr(row_text(ac, "unread_badge"), ==, "1");
}

static void
test_requests_are_separate(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f, 900, 600);
  spin_until(has_rows, f);

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->sidebar), "sidebar.show-requests",
                                           "b", TRUE));
  GhConversation *const requests[] = { f->ad, f->ae };
  assert_list(f, requests, 2);
  spin_until(has_rows, f);
  GhConversationRow *ad = row_for(f->sidebar, f->ad);
  g_assert_true(row_shown(ad, "request_label"));
  g_assert_cmpstr(row_text(ad, "request_label"), ==, "Request");
  g_assert_true(g_strstr_len(gh_conversation_row_get_summary(ad), -1,
                             ". Message request. 1 unread. ") != NULL);

  /* A request opens in the Message Requests page (G18), not the
   * conversation view; the header says what it is. */
  g_assert_true(gh_sidebar_page_select_relative(f->sidebar, 1));
  g_assert_null(shown_messages(f->content));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(gh_content_page_get_stack(f->content)), ==,
                  "requests");
  g_assert_true(gh_requests_view_get_request(gh_conversation_list_get_requests_view(f->window)) ==
                f->ad);
  g_assert_cmpstr(adw_window_title_get_subtitle(gh_content_page_get_window_title(f->content)),
                  ==, "Message request · end-to-end encrypted");

  /* Accepting (locally) moves it to the conversations, in activity order;
   * the selection it had in the requests list does not carry over. */
  gh_conversation_accept(f->ad);
  GhConversation *const remaining[] = { f->ae };
  assert_list(f, remaining, 1);
  g_assert_null(gh_sidebar_page_get_selected(f->sidebar));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(gh_content_page_get_stack(f->content)), ==,
                  "none");

  /* Back to the conversations (the header's back button). */
  g_assert_true(gh_sidebar_page_get_show_requests(f->sidebar));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->sidebar), "sidebar.show-requests",
                                           "b", FALSE));
  GhConversation *const accepted[] = { f->ab, f->ad, f->ac };
  assert_list(f, accepted, 3);
  g_assert_cmpstr(gtk_label_get_text(template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE,
                                                    "requests_count")), ==, "1");

  /* Replying also makes a conversation of the last request, and with none
   * left the entry disappears. */
  add(f->store, 1, 5, f->now - 1, "hi E", NULL);
  GtkWidget *entry = template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "requests_button");
  g_assert_false(gtk_widget_get_visible(entry));
  g_assert_cmpuint(g_list_model_get_n_items(list_model(f->sidebar)), ==, 4);
}

static void search(Fixture *f, const char *text);

/* Charter §7.9 (W13 review #4): a request is titled, in its row, avatar,
 * accessible label and header, by its sender's npub. The subject it carries
 * is text the sender chose: shown only as secondary text until accepted,
 * and still found by search. */
static void
test_request_subject_secondary(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f, 900, 600);
  spin_until(has_rows, f);
  add(f->store, 5, 1, f->now - 30, "claim it now", "You won a prize");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->sidebar), "sidebar.show-requests",
                                           "b", TRUE));
  spin_until(has_rows, f);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  GhConversationRow *ae = row_for(f->sidebar, f->ae);
  const char *title = gh_conversation_get_title(f->ae);
  g_assert_true(g_str_has_prefix(title, "npub1"));
  g_assert_cmpstr(row_text(ae, "title_label"), ==, title);
  AdwAvatar *avatar = template_child(ae, GH_TYPE_CONVERSATION_ROW, "avatar");
  g_assert_cmpstr(adw_avatar_get_text(avatar), ==, title);
  /* Secondary text even with previews off (it is not the message body). */
  g_assert_cmpstr(row_text(ae, "preview_label"), ==, "You won a prize");
  g_autofree char *time = gh_conversation_row_format_time(f->now - 30, now);
  g_autofree char *label = g_strdup_printf(
    "%s. Message request. Subject: You won a prize. 2 unread. %s", title, time);
  g_assert_cmpstr(gh_conversation_row_get_summary(ae), ==, label);
  GtkWidget *item = gtk_widget_get_parent(GTK_WIDGET(ae));
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label);
  /* With previews: the subject, then the newest message, in both. */
  gh_sidebar_page_set_show_previews(f->sidebar, TRUE);
  g_assert_cmpstr(row_text(ae, "preview_label"), ==, "You won a prize — claim it now");
  g_autofree char *with_preview = g_strdup_printf("%s. claim it now", label);
  g_assert_cmpstr(gh_conversation_row_get_summary(ae), ==, with_preview);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      with_preview);
  gh_sidebar_page_set_show_previews(f->sidebar, FALSE);

  /* The header: the npub as the title, the quoted subject in the subtitle. */
  g_assert_true(gh_sidebar_page_select_relative(f->sidebar, 1));
  g_assert_true(gh_requests_view_get_request(gh_conversation_list_get_requests_view(f->window)) ==
                f->ae);
  AdwWindowTitle *header = gh_content_page_get_window_title(f->content);
  g_assert_cmpstr(adw_window_title_get_title(header), ==, title);
  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(f->content)), ==, title);
  g_assert_cmpstr(adw_window_title_get_subtitle(header), ==,
                  "“You won a prize” · Message request · end-to-end encrypted");
  /* A later subject changes the secondary text, never the title. */
  add(f->store, 5, 1, f->now - 20, "last chance", "Final notice");
  g_assert_cmpstr(adw_window_title_get_title(header), ==, title);
  g_assert_cmpstr(adw_window_title_get_subtitle(header), ==,
                  "“Final notice” · Message request · end-to-end encrypted");
  g_assert_cmpstr(row_text(ae, "title_label"), ==, title);
  g_assert_cmpstr(row_text(ae, "preview_label"), ==, "Final notice");
  search(f, "final NOTICE");
  GhConversation *const found[] = { f->ae };
  assert_list(f, found, 1);
  search(f, "");

  /* Accepted, the subject names the conversation. */
  gh_conversation_accept(f->ae);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->sidebar), "sidebar.show-requests",
                                           "b", FALSE));
  spin_until(has_rows, f);
  GhConversationRow *accepted = row_for(f->sidebar, f->ae);
  g_assert_cmpstr(row_text(accepted, "title_label"), ==, "Final notice");
  g_assert_cmpstr(row_text(accepted, "preview_label"), ==, "");
  g_assert_true(g_str_has_prefix(gh_conversation_row_get_summary(accepted),
                                 "Final notice. Private conversation. "));
}

static gboolean
search_text_is(gpointer data)
{
  GhSidebarPage *sidebar = data;
  return g_strcmp0(gh_sidebar_page_get_search_text(sidebar),
                   g_object_get_data(G_OBJECT(sidebar), "want")) == 0;
}

static void
search(Fixture *f, const char *text)
{
  GtkEditable *entry = template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "search_entry");
  g_object_set_data_full(G_OBJECT(f->sidebar), "want", g_strstrip(g_strdup(text)), g_free);
  gtk_editable_set_text(entry, text);
  spin_until(search_text_is, f->sidebar);
}

static void
test_search(Fixture *f, gconstpointer data)
{
  (void)data;
  GtkStack *stack = gh_sidebar_page_get_stack(f->sidebar);
  GtkWidget *requests = template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "requests_button");
  GtkLabel *count = template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "requests_count");

  /* By title (a subject), ignoring case. */
  search(f, "book CLUB");
  GhConversation *const book[] = { f->ac };
  assert_list(f, book, 1);
  g_assert_false(gtk_widget_get_visible(requests));

  /* By a participant's full npub, which the abbreviated title hides. */
  search(f, npub[2]);
  GhConversation *const by_npub[] = { f->ab };
  assert_list(f, by_npub, 1);

  /* A matching request is counted in the entry, not listed. */
  search(f, npub[4]);
  assert_list(f, NULL, 0);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "conversations");
  g_assert_true(gtk_widget_get_visible(requests));
  g_assert_cmpstr(gtk_label_get_text(count), ==, "1");

  /* #10: nothing matches. */
  search(f, "no such conversation");
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "no-results");
  g_assert_true(gh_sidebar_page_get_focus_target(f->sidebar) ==
                template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "search_entry"));

  search(f, "");
  GhConversation *const all[] = { f->ab, f->ac };
  assert_list(f, all, 2);
  g_assert_cmpstr(gtk_label_get_text(count), ==, "2");
}

static gboolean
has_message_items(gpointer data)
{
  Fixture *f = data;
  g_autoptr(GPtrArray) items = message_items(f->content);
  GListModel *messages = shown_messages(f->content);
  return messages && gtk_widget_get_mapped(message_list_of(f->content)) &&
         items->len == g_list_model_get_n_items(messages);
}

/* A message row shows its text as typed (markup never interpreted, charter
 * PT-3) and its list item carries the composed accessible label; the
 * bubbles themselves are tests/ui/test_conversation_view.c's. */
static void
assert_message_item(GtkWidget *item, GhMessage *message)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *label = gh_message_row_compose_summary(message, now);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label);
  GhMessageRow *row = item_row(item);
  g_assert_true(gh_message_row_get_message(row) == message);
  GtkLabel *body = template_child(row, GH_TYPE_MESSAGE_ROW, "body_label");
  g_assert_cmpstr(gtk_label_get_text(body), ==, gh_message_get_content(message));
}

static void
test_selection_shows_messages(Fixture *f, gconstpointer data)
{
  (void)data;
  present(f, 900, 600);
  spin_until(has_rows, f);
  GtkStack *content_stack = gh_content_page_get_stack(f->content);
  AdwWindowTitle *title = gh_content_page_get_window_title(f->content);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(content_stack), ==, "none");
  g_assert_cmpuint(gh_conversation_get_unread_count(f->ab), ==, 1);

  /* Alt+Down from nothing selects the first conversation. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), "win.next-conversation",
                                           NULL));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ab);
  g_assert_true(shown_messages(f->content) == G_LIST_MODEL(f->ab));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(content_stack), ==, "conversation");
  g_assert_cmpstr(adw_window_title_get_title(title), ==, gh_conversation_get_title(f->ab));
  g_assert_cmpstr(adw_window_title_get_subtitle(title), ==, "Private · end-to-end encrypted");
  g_assert_cmpstr(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(f->content)), ==,
                  gh_conversation_get_title(f->ab));
  /* Opening it is reading it (locally only); the badge goes. */
  g_assert_cmpuint(gh_conversation_get_unread_count(f->ab), ==, 0);
  g_assert_false(row_shown(row_for(f->sidebar, f->ab), "unread_badge"));

  /* Oldest first, plain text, each with "Sender, time: text". */
  spin_until(has_message_items, f);
  g_autoptr(GPtrArray) items = message_items(f->content);
  g_assert_cmpuint(items->len, ==, 2);
  g_autoptr(GhMessage) first = g_list_model_get_item(G_LIST_MODEL(f->ab), 0);
  g_autoptr(GhMessage) second = g_list_model_get_item(G_LIST_MODEL(f->ab), 1);
  g_assert_cmpstr(gh_message_get_content(first), ==, "hey B");
  assert_message_item(g_ptr_array_index(items, 0), first);
  assert_message_item(g_ptr_array_index(items, 1), second);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree char *own = gh_message_row_compose_summary(first, now);
  g_assert_true(g_str_has_prefix(own, "You, "));
  g_assert_true(g_str_has_suffix(own, ": hey B"));
  g_autofree char *theirs = gh_message_row_compose_summary(second, now);
  g_assert_true(g_str_has_prefix(theirs, "npub1"));
  g_assert_cmpint(gtk_accessible_get_accessible_role(
                    GTK_ACCESSIBLE(g_ptr_array_index(items, 0))), ==,
                  GTK_ACCESSIBLE_ROLE_LIST_ITEM);

  /* Markup in a message is shown as typed, never interpreted. */
  add(f->store, 2, 1, f->now - 2, "<b>not bold</b> & <a href='x'>", NULL);
  spin_until(has_message_items, f);
  g_autoptr(GPtrArray) more = message_items(f->content);
  g_autoptr(GhMessage) markup = g_list_model_get_item(G_LIST_MODEL(f->ab), 2);
  assert_message_item(g_ptr_array_index(more, 2), markup);

  /* Next and previous. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), "win.next-conversation",
                                           NULL));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ac);
  g_assert_cmpstr(adw_window_title_get_title(title), ==, "Book Club");
  g_assert_false(gh_sidebar_page_select_relative(f->sidebar, 1));

  /* The shown conversation moving to the top keeps it selected and shown. */
  add(f->store, 3, 1, f->now - 1, "moving up", NULL);
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ac);
  g_assert_true(shown_messages(f->content) == G_LIST_MODEL(f->ac));
  GhConversation *const moved[] = { f->ac, f->ab };
  assert_list(f, moved, 2);

  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window),
                                           "win.previous-conversation", NULL));
  g_assert_false(gh_sidebar_page_select_relative(f->sidebar, -1));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ac);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), "win.next-conversation",
                                           NULL));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ab);

  gh_sidebar_page_unselect(f->sidebar);
  g_assert_cmpstr(gtk_stack_get_visible_child_name(content_stack), ==, "none");
  g_assert_cmpstr(adw_window_title_get_title(title), ==, "Messages");
}

static void
test_empty_and_banner(Fixture *f, gconstpointer data)
{
  (void)data;
  GtkStack *stack = gh_sidebar_page_get_stack(f->sidebar);
  GhStatus *status = gh_window_get_status(f->window);
  AdwBanner *banner = gh_sidebar_page_get_banner(f->sidebar);
  g_assert_false(adw_banner_get_revealed(banner));
  g_assert_true(gh_sidebar_page_select_relative(f->sidebar, 1));

  /* Another account: every conversation of the previous one goes first,
   * the selection and the shown messages with them. */
  gh_conversation_store_set_account(f->store, hex[2], NULL, NULL, NULL);
  g_assert_cmpuint(g_list_model_get_n_items(list_model(f->sidebar)), ==, 0);
  g_assert_null(gh_sidebar_page_get_selected(f->sidebar));
  g_assert_null(shown_messages(f->content));
  g_assert_false(gtk_widget_get_visible(template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE,
                                                       "requests_button")));
  /* #9 */
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");

  /* #7 while empty: one banner, next to the list it explains. */
  gh_status_set_inbox(status, GH_STATUS_INBOX_MISSING, NULL);
  g_assert_true(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_banner_get_title(banner), ==,
                  "Set up private messaging so people can reach you");
  gh_status_set_inbox(status, GH_STATUS_INBOX_ERROR, "The DM seen-set is unusable: disk");
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "error");
  g_assert_cmpstr(adw_status_page_get_description(
                    ADW_STATUS_PAGE(gtk_stack_get_visible_child(stack))), ==,
                  "The DM seen-set is unusable: disk");
  gh_status_set_inbox(status, GH_STATUS_INBOX_LIVE, NULL);
  g_assert_false(adw_banner_get_revealed(banner));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "empty");
}

static gboolean
is_collapsed_with_rows(gpointer data)
{
  Fixture *f = data;
  return adw_navigation_split_view_get_collapsed(gh_window_get_split(f->window)) &&
         has_rows(f);
}

static gboolean
content_shown(gpointer data)
{
  Fixture *f = data;
  return gtk_widget_get_mapped(GTK_WIDGET(f->content)) && has_message_items(f) &&
         gtk_widget_get_width(message_list_of(f->content)) > 0;
}

static gboolean
sidebar_shown(gpointer data)
{
  Fixture *f = data;
  return gtk_widget_get_mapped(GTK_WIDGET(f->sidebar)) &&
         !gtk_widget_get_mapped(GTK_WIDGET(f->content));
}

static void
assert_fits(Fixture *f)
{
  int min_width = 0, min_height = 0;
  gtk_widget_measure(GTK_WIDGET(f->window), GTK_ORIENTATION_HORIZONTAL, -1, &min_width, NULL,
                     NULL, NULL);
  gtk_widget_measure(GTK_WIDGET(f->window), GTK_ORIENTATION_VERTICAL, 360, &min_height, NULL,
                     NULL, NULL);
  g_assert_cmpint(min_width, <=, 360);
  g_assert_cmpint(min_height, <=, 294);
}

/* UX-1 at the list level: the smallest window collapses to a working list,
 * a conversation opens on selection, back returns to the list, and the
 * same conversation reopens. */
static void
test_collapsed_360x294(Fixture *f, gconstpointer data)
{
  (void)data;
  AdwNavigationSplitView *split = gh_window_get_split(f->window);
  gh_composer_set_disabled_reason(gh_content_page_get_composer(f->content),
                                  "Read-only: the Nostr signer service is not installed or running");
  gh_status_set_inbox(gh_window_get_status(f->window), GH_STATUS_INBOX_UNREACHABLE, NULL);
  present(f, 360, 294);
  spin_until(is_collapsed_with_rows, f);
  drain_idle();
  assert_fits(f);
  g_assert_false(adw_navigation_split_view_get_show_content(split));
  g_autoptr(GPtrArray) rows = rows_of(f->sidebar);
  for (guint i = 0; i < rows->len; i++) {
    int width = gtk_widget_get_width(g_ptr_array_index(rows, i));
    g_assert_cmpint(width, >, 0);
    g_assert_cmpint(width, <=, 360);
  }

  /* Moving through the collapsed list as the arrow keys do selects without
   * opening, and marks nothing read (nothing is on screen). */
  g_assert_true(gh_sidebar_page_select_relative(f->sidebar, 1));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ab);
  g_assert_false(adw_navigation_split_view_get_show_content(split));
  g_assert_cmpuint(gh_conversation_get_unread_count(f->ab), ==, 1);
  g_assert_true(gh_sidebar_page_select_relative(f->sidebar, 1));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ac);
  g_assert_true(gh_sidebar_page_select_relative(f->sidebar, -1));

  /* Enter (activate) opens it; now on screen, it is read. */
  g_signal_emit_by_name(gh_sidebar_page_get_list(f->sidebar), "activate", 0u);
  g_assert_true(adw_navigation_split_view_get_show_content(split));
  g_assert_cmpuint(gh_conversation_get_unread_count(f->ab), ==, 0);
  spin_until(content_shown, f);
  drain_idle();
  assert_fits(f);
  GtkWidget *messages = message_list_of(f->content);
  g_assert_cmpint(gtk_widget_get_width(messages), >, 0);
  g_assert_cmpint(gtk_widget_get_width(messages), <=, 360);

  /* Back to the list clears the selection, so the same row reopens, here
   * through Alt+Down's action, which opens as a press does. */
  adw_navigation_split_view_set_show_content(split, FALSE);
  g_assert_null(gh_sidebar_page_get_selected(f->sidebar));
  spin_until(sidebar_shown, f);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), "win.next-conversation",
                                           NULL));
  g_assert_true(gh_sidebar_page_get_selected(f->sidebar) == f->ab);
  g_assert_true(adw_navigation_split_view_get_show_content(split));

  /* Ctrl+F brings the list (with its search) back on screen. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->window), "win.search", NULL));
  g_assert_false(adw_navigation_split_view_get_show_content(split));
  g_assert_true(gtk_search_bar_get_search_mode(
    template_child(f->sidebar, GH_TYPE_SIDEBAR_PAGE, "search_bar")));
  drain_idle();
  assert_fits(f);

  /* Message Requests (back button, no New Message) also fits. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->sidebar), "sidebar.show-requests", "b",
                                           TRUE));
  spin_until(is_collapsed_with_rows, f);
  drain_idle();
  assert_fits(f);
}

/* ---- screenshots (opt-in evidence) ------------------------------------------ */

/* The whole window as it is drawn, client-side frame and shadow included
 * (the node's own bounds), like a screenshot of it. */
static void
save_png(GtkWidget *window, const char *dir, const char *name)
{
  g_autoptr(GdkPaintable) paintable = gtk_widget_paintable_new(window);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  gdk_paintable_snapshot(paintable, snapshot, gdk_paintable_get_intrinsic_width(paintable),
                         gdk_paintable_get_intrinsic_height(paintable));
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  graphene_rect_t bounds;
  gsk_render_node_get_bounds(node, &bounds);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture = gsk_renderer_render_texture(renderer, node, &bounds);
  g_autofree char *path = g_strdup_printf("%s/groundhog-g11-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s (window %dx%d)", path, gtk_widget_get_width(window),
                 gtk_widget_get_height(window));
}

typedef enum { SCENE_CONVERSATION, SCENE_LIST, SCENE_REQUESTS } Scene;

typedef struct {
  GhWindow *window;
  gboolean want_messages;
} Shot;

static gboolean
shot_ready(gpointer data)
{
  Shot *shot = data;
  GhSidebarPage *sidebar = gh_window_get_sidebar(shot->window);
  GhContentPage *content = gh_window_get_content(shot->window);
  GListModel *model = G_LIST_MODEL(gtk_list_view_get_model(gh_sidebar_page_get_list(sidebar)));
  g_autoptr(GPtrArray) rows = rows_of(sidebar);
  if (!gtk_widget_get_mapped(GTK_WIDGET(shot->window)))
    return FALSE;
  if (gtk_widget_get_mapped(GTK_WIDGET(sidebar)) && rows->len != g_list_model_get_n_items(model))
    return FALSE;
  if (!shot->want_messages)
    return TRUE;
  g_autoptr(GPtrArray) items = message_items(content);
  GListModel *messages = shown_messages(content);
  return messages && gtk_widget_get_mapped(GTK_WIDGET(content)) &&
         items->len == g_list_model_get_n_items(messages);
}

/* One fresh window per shot: a mapped window keeps its size. */
static void
take_shot(Fixture *f, const char *dir, int width, int height, Scene scene, const char *name)
{
  Shot shot = { gh_window_new(NULL), scene == SCENE_CONVERSATION };
  GhSidebarPage *sidebar = gh_window_get_sidebar(shot.window);
  GhStatus *status = gh_window_get_status(shot.window);
  gh_conversation_list_attach(shot.window, f->store, NULL);
  gh_status_set_account_active(status, TRUE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  gh_status_set_inbox(status, scene == SCENE_LIST ? GH_STATUS_INBOX_BACKFILLING
                                                  : GH_STATUS_INBOX_LIVE, NULL);
  gh_sidebar_page_set_show_previews(sidebar, TRUE);
  adw_window_title_set_subtitle(gh_sidebar_page_get_window_title(sidebar), "Test");
  if (scene == SCENE_CONVERSATION)
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(shot.window), "win.next-conversation",
                                             NULL));
  if (scene == SCENE_REQUESTS)
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(sidebar), "sidebar.show-requests", "b",
                                             TRUE));
  gtk_window_set_default_size(GTK_WINDOW(shot.window), width, height);
  gtk_window_present(GTK_WINDOW(shot.window));
  spin_until(shot_ready, &shot);
  drain_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(shot.window));
  save_png(GTK_WIDGET(shot.window), dir, name);
  gtk_window_destroy(GTK_WINDOW(shot.window));
}

/* With GROUNDHOG_TEST_SCREENSHOTS=<dir>, renders the fixture wide and at the
 * 360x294 minimum, light and dark, to <dir>/groundhog-g11-*.png. Previews are
 * switched on here only to show them; they default to off. */
static void
test_screenshots(Fixture *f, gconstpointer data)
{
  (void)data;
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  /* Switching the color scheme re-parses the theme, and some GTK builds
   * (e.g. Homebrew GTK 4.22 with libadwaita 1.9) warn about libadwaita's own
   * CSS then: warnings are not fatal while taking screenshots (criticals
   * still are). */
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  AdwStyleManager *style = adw_style_manager_get_default();
  static const struct {
    AdwColorScheme scheme;
    const char *name;
  } schemes[] = {
    { ADW_COLOR_SCHEME_FORCE_LIGHT, "light" },
    { ADW_COLOR_SCHEME_FORCE_DARK, "dark" },
  };
  for (guint i = 0; i < G_N_ELEMENTS(schemes); i++) {
    adw_style_manager_set_color_scheme(style, schemes[i].scheme);
    g_autofree char *wide = g_strdup_printf("wide-%s", schemes[i].name);
    g_autofree char *list = g_strdup_printf("narrow-list-%s", schemes[i].name);
    g_autofree char *conversation = g_strdup_printf("narrow-conversation-%s", schemes[i].name);
    g_autofree char *requests = g_strdup_printf("narrow-requests-%s", schemes[i].name);
    take_shot(f, dir, 900, 600, SCENE_CONVERSATION, wide);
    take_shot(f, dir, 360, 294, SCENE_LIST, list);
    take_shot(f, dir, 360, 294, SCENE_CONVERSATION, conversation);
    take_shot(f, dir, 360, 294, SCENE_REQUESTS, requests);
  }
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  g_log_set_always_fatal(fatal);
}

static void
test_inbox_status_mapping(void)
{
  static const struct {
    GhDmInboxState inbox;
    GhAccountRelaysState relays;
    gboolean has_relays;
    GhStatusInbox want;
  } cases[] = {
    { GH_DM_INBOX_INACTIVE, GH_ACCOUNT_RELAYS_INACTIVE, FALSE, GH_STATUS_INBOX_INACTIVE },
    { GH_DM_INBOX_NO_INBOX_RELAYS, GH_ACCOUNT_RELAYS_NO_SOURCES, FALSE,
      GH_STATUS_INBOX_NO_SOURCES },
    { GH_DM_INBOX_NO_INBOX_RELAYS, GH_ACCOUNT_RELAYS_INACTIVE, FALSE, GH_STATUS_INBOX_LOOKING },
    { GH_DM_INBOX_NO_INBOX_RELAYS, GH_ACCOUNT_RELAYS_DISCOVERING, FALSE,
      GH_STATUS_INBOX_LOOKING },
    { GH_DM_INBOX_NO_INBOX_RELAYS, GH_ACCOUNT_RELAYS_UNREACHABLE, FALSE,
      GH_STATUS_INBOX_LOOKUP_FAILED },
    { GH_DM_INBOX_NO_INBOX_RELAYS, GH_ACCOUNT_RELAYS_COMPLETE, FALSE, GH_STATUS_INBOX_MISSING },
    { GH_DM_INBOX_CONNECTING, GH_ACCOUNT_RELAYS_COMPLETE, TRUE, GH_STATUS_INBOX_CONNECTING },
    /* An admitted inbox list keeps working when the lookup relays fail. */
    { GH_DM_INBOX_BACKFILLING, GH_ACCOUNT_RELAYS_UNREACHABLE, TRUE,
      GH_STATUS_INBOX_BACKFILLING },
    { GH_DM_INBOX_LIVE, GH_ACCOUNT_RELAYS_COMPLETE, TRUE, GH_STATUS_INBOX_LIVE },
    { GH_DM_INBOX_ERROR, GH_ACCOUNT_RELAYS_COMPLETE, TRUE, GH_STATUS_INBOX_UNREACHABLE },
    { GH_DM_INBOX_ERROR, GH_ACCOUNT_RELAYS_COMPLETE, FALSE, GH_STATUS_INBOX_ERROR },
  };
  for (guint i = 0; i < G_N_ELEMENTS(cases); i++)
    g_assert_cmpint(gh_inbox_status_map(cases[i].inbox, cases[i].relays, cases[i].has_relays),
                    ==, cases[i].want);
}

static GPtrArray *
fake_identities(gpointer data, GError **error)
{
  (void)data;
  (void)error;
  GPtrArray *ids = g_ptr_array_new_with_free_func((GDestroyNotify)gh_identity_info_free);
  GhIdentityInfo *info = g_new0(GhIdentityInfo, 1);
  info->npub = g_strdup(npub[1]);
  info->label = g_strdup("Test");
  g_ptr_array_add(ids, info);
  return ids;
}

static gboolean
is_unselected(gpointer data)
{
  return gh_account_controller_get_state(data) == GH_ACCOUNT_STATE_UNSELECTED;
}

static gboolean
inbox_is_no_sources(gpointer data)
{
  return gh_status_get_inbox(data) == GH_STATUS_INBOX_NO_SOURCES;
}

static void
remove_tree(const gchar *dir)
{
  g_autoptr(GDir) handle = g_dir_open(dir, 0, NULL);
  const gchar *name;
  while (handle && (name = g_dir_read_name(handle))) {
    g_autofree gchar *path = g_build_filename(dir, name, NULL);
    g_assert_cmpint(g_remove(path), ==, 0);
  }
  g_assert_cmpint(g_rmdir(dir), ==, 0);
}

/* The executable's wiring (src/main.c) on the real account, relay-list and
 * inbox objects: with the default settings (no discovery relay) nothing is
 * contacted, and the banner says why nothing can arrive. */
static void
test_inbox_status_wiring(void)
{
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  g_settings_set_string(settings, "current-npub", "");
  g_auto(GStrv) sources = g_settings_get_strv(settings, "discovery-relays");
  g_assert_cmpuint(g_strv_length(sources), ==, 0);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *state_dir = g_dir_make_tmp("groundhog-conversation-list-XXXXXX", &error);
  g_assert_no_error(error);

  GhAccountController *accounts =
    gh_account_controller_new_full(settings, NULL, fake_identities, NULL);
  GhAccountRelays *relays = gh_account_relays_new(accounts, settings, NULL, NULL);
  GhConversationStore *store = gh_conversation_store_new();
  GhDmInbox *inbox = gh_dm_inbox_new(accounts, relays, store, state_dir, NULL, NULL, NULL);
  GhWindow *window = gh_window_new(NULL);
  GhStatus *status = gh_window_get_status(window);
  gh_conversation_list_attach(window, store, settings);
  gh_inbox_status_attach(status, inbox, relays);
  g_assert_cmpint(gh_status_get_inbox(status), ==, GH_STATUS_INBOX_INACTIVE);

  spin_until(is_unselected, accounts);
  g_assert_true(gh_account_controller_select(accounts, npub[1], &error));
  g_assert_no_error(error);
  spin_until(inbox_is_no_sources, status);
  g_assert_cmpint(gh_dm_inbox_get_state(inbox), ==, GH_DM_INBOX_NO_INBOX_RELAYS);
  g_assert_null(gh_dm_inbox_get_relays(inbox));
  g_assert_cmpstr(gh_conversation_store_get_account(store), ==, hex[1]);

  gh_status_set_account_active(status, TRUE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  AdwBanner *banner = gh_sidebar_page_get_banner(gh_window_get_sidebar(window));
  g_assert_true(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_banner_get_title(banner), ==,
                  "No relay is set up yet, so Groundhog can't receive messages");
  /* Previews follow the show-message-previews key (charter §7.11, G01). */
  g_assert_cmpint(gh_sidebar_page_get_show_previews(gh_window_get_sidebar(window)), ==,
                  g_settings_get_boolean(settings, "show-message-previews"));
  g_settings_set_boolean(settings, "show-message-previews", FALSE);
  g_assert_false(gh_sidebar_page_get_show_previews(gh_window_get_sidebar(window)));
  g_settings_set_boolean(settings, "show-message-previews", TRUE);
  g_assert_true(gh_sidebar_page_get_show_previews(gh_window_get_sidebar(window)));

  /* Teardown in the executable's order: inbox, relays, accounts. */
  gtk_window_destroy(GTK_WINDOW(window));
  g_object_run_dispose(G_OBJECT(inbox));
  g_object_unref(inbox);
  g_object_unref(store);
  g_object_run_dispose(G_OBJECT(relays));
  g_object_unref(relays);
  gpointer weak = accounts;
  g_object_add_weak_pointer(G_OBJECT(accounts), &weak);
  g_object_run_dispose(G_OBJECT(accounts));
  g_object_unref(accounts);
  spin_until(is_null, &weak);
  remove_tree(state_dir);
}

static void
test_time_format(void)
{
  g_autoptr(GTimeZone) zone = g_time_zone_new_local();
  g_autoptr(GDateTime) now = g_date_time_new(zone, 2026, 9, 28, 15, 30, 0);
  gint64 at = g_date_time_to_unix(now);
  g_autofree char *today = gh_conversation_row_format_time(at - 3600, now);
  g_assert_cmpstr(today, ==, "14:30");
  g_autofree char *yesterday = gh_conversation_row_format_time(at - 86400, now);
  g_assert_cmpstr(yesterday, ==, "Yesterday");
  g_autoptr(GDateTime) three = g_date_time_add_days(now, -3);
  g_autofree char *weekday = g_date_time_format(three, "%A");
  g_autofree char *three_days = gh_conversation_row_format_time(at - 3 * 86400, now);
  g_assert_cmpstr(three_days, ==, weekday);
  g_autoptr(GDateTime) month = g_date_time_add_days(now, -30);
  g_autofree char *date = g_date_time_format(month, "%x");
  g_autofree char *old = gh_conversation_row_format_time(at - 30 * 86400, now);
  g_assert_cmpstr(old, ==, date);
  g_autofree char *never = gh_conversation_row_format_time(0, now);
  g_assert_cmpstr(never, ==, "");

  g_autofree char *message_today = gh_conversation_row_format_message_time(at - 60, now);
  g_assert_cmpstr(message_today, ==, "15:29");
  g_autofree char *message_yesterday =
    gh_conversation_row_format_message_time(at - 86400 - 60, now);
  g_assert_cmpstr(message_yesterday, ==, "Yesterday 15:29");
}

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) {
    g_printerr("groundhog-conversation-list test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  /* The application's stylesheet, as main.c loads it, so sizes include it. */
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* 96 dpi makes 1sp one pixel on every platform (GTK on macOS reports 72);
   * without animations a navigation completes in one layout. */
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
               "gtk-enable-animations", FALSE, NULL);
  /* GNOME's window controls (close only), the desktop the 360x294 minimum is
   * for (charter §7.12); GTK's own default adds minimize and maximize, which
   * need about 5 px more in the collapsed sidebar header (follow-up bead). */
  g_object_set(gtk_settings_get_default(), "gtk-decoration-layout", "appmenu:close", NULL);
  init_keys();

  g_test_init(&argc, &argv, NULL);
#define ADD(path, func) \
  g_test_add("/groundhog/conversation-list/" path, Fixture, NULL, fixture_setup, func, \
             fixture_teardown)
  ADD("rows-order-and-badges", test_rows_order_and_badges);
  ADD("requests-are-separate", test_requests_are_separate);
  ADD("request-subject-secondary", test_request_subject_secondary);
  ADD("search", test_search);
  ADD("selection-shows-messages", test_selection_shows_messages);
  ADD("empty-and-banner", test_empty_and_banner);
  ADD("collapsed-360x294", test_collapsed_360x294);
  ADD("screenshots", test_screenshots);
#undef ADD
  g_test_add_func("/groundhog/conversation-list/inbox-status-mapping",
                  test_inbox_status_mapping);
  g_test_add_func("/groundhog/conversation-list/inbox-status-wiring", test_inbox_status_wiring);
  g_test_add_func("/groundhog/conversation-list/time-format", test_time_format);
  int status = g_test_run();
  for (guint key = 1; key < KEYS; key++) {
    g_free(hex[key]);
    g_free(npub[key]);
  }
  return status;
}
