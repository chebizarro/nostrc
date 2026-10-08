#include "gh-conversation-view.h"
#include "gh-conversation-list.h"
#include "gh-conversation-private.h"
#include "gh-mls-poll.h"
#include "gh-nip17-envelope.h"
#include "gh-poll-card.h"
#include "gh-poll-ui.h"
#include "gh-outbox.h"
#include "gh-window.h"

#include "nostr-keys.h"
#include "nostrc-test-gdk-frame.h"

#include <adwaita.h>
#include <stdlib.h>

void groundhog_register_resource(void);

/* The view-only test never sends. These outbox hooks are linked by the
 * production poll UI but are deliberately unreachable in this fixture. */
GObject *gh_account_store_get_outbox(GhAccountStore *store)
{
  (void)store;
  g_assert_not_reached();
  return NULL;
}

GType gh_outbox_get_type(void)
{
  g_assert_not_reached();
  return G_TYPE_OBJECT;
}

GhOutboxItem *gh_outbox_send_poll_event_room(GhOutbox *outbox,
                                              const gchar *const *recipients,
                                              const gchar *event, GError **error)
{
  (void)outbox; (void)recipients; (void)event; (void)error;
  g_assert_not_reached();
  return NULL;
}

static gchar *account;
static gchar *peer;
static const gchar *group_id = "0123456789abcdef";

static GhMessage *
message_from_event(const gchar *event)
{
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_mls(account, group_id, event, &error);
  g_assert_no_error(error);
  g_assert_nonnull(message);
  return message;
}

static GhMlsPoll *
new_poll(gboolean multiple, GhMessage **message)
{
  const gchar *labels[] = { "Coffee", "Tea", "Water" };
  g_autoptr(GError) error = NULL;
  g_autofree gchar *event = gh_mls_poll_build_event(
    account, group_id, g_get_real_time() / G_USEC_PER_SEC, "What to drink?",
    labels, G_N_ELEMENTS(labels), multiple ? GH_MLS_POLL_MULTIPLE_CHOICE :
    GH_MLS_POLL_SINGLE_CHOICE, 0, &error);
  g_assert_no_error(error);
  g_assert_nonnull(event);
  *message = message_from_event(event);
  GhMlsPoll *poll = gh_mls_poll_new_from_event(
    gh_message_get_rumor_id(*message), account, gh_message_get_created_at(*message),
    event, &error);
  g_assert_no_error(error);
  g_assert_nonnull(poll);
  gh_mls_poll_set_local_account(poll, account);
  return poll;
}

static void
admit(GhConversationStore *store, GhMessage *message)
{
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_add_message(store, message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
}

static GtkWidget *
find_card(GtkWidget *root)
{
  if (GH_IS_POLL_CARD(root)) return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *card = find_card(child);
    if (card) return card;
  }
  return NULL;
}

static gboolean
mark_deadline(gpointer data)
{
  *(gboolean *)data = TRUE;
  return G_SOURCE_REMOVE;
}

static void
wait_for_window_or_card(GtkWidget *widget, gboolean card)
{
  gboolean expired = FALSE;
  guint deadline = g_timeout_add_seconds(5, mark_deadline, &expired);
  while (!(card ? find_card(widget) != NULL : gtk_widget_get_mapped(widget)) && !expired)
    g_main_context_iteration(NULL, TRUE);
  if (!expired) g_source_remove(deadline);
  g_assert_false(expired);
}

static void
test_own_poll_card_on_open(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  g_autoptr(GhMessage) poll_message = NULL;
  g_autoptr(GhMlsPoll) poll = new_poll(FALSE, &poll_message);
  admit(store, poll_message);
  GhConversation *conversation = gh_conversation_store_lookup(
    store, gh_message_get_room_id(poll_message));
  g_assert_nonnull(conversation);
  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, store, NULL);
  GhPollUiConfig config = { 0 };
  gh_poll_ui_attach(window, &config);
  gtk_window_present(GTK_WINDOW(window));
  GhConversationView *view = GH_CONVERSATION_VIEW(
    gh_content_page_get_view(gh_window_get_content(window)));
  wait_for_window_or_card(GTK_WIDGET(window), FALSE);
  g_assert_true(gh_window_open_item(window, conversation));
  wait_for_window_or_card(GTK_WIDGET(view), TRUE);
  GtkWidget *card = find_card(GTK_WIDGET(view));
  wait_for_window_or_card(card, FALSE);
  GhMlsPoll *on_screen = gh_poll_card_get_poll(GH_POLL_CARD(card));
  g_assert_cmpstr(gh_mls_poll_get_question(on_screen), ==, "What to drink?");
  const gchar *choices[] = { gh_mls_poll_get_option(on_screen, 0)->id };
  g_autoptr(GError) error = NULL;
  g_autofree gchar *vote_event = gh_mls_poll_build_vote_event(
    account, group_id, gh_message_get_created_at(poll_message) + 1,
    gh_message_get_rumor_id(poll_message), choices, 1, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) vote = message_from_event(vote_event);
  admit(store, vote);
  g_assert_true(find_card(GTK_WIDGET(view)) == card);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(on_screen), ==, 1);
  g_assert_cmpuint(g_list_model_get_n_items(gh_conversation_view_get_timeline(view)), ==, 1);
  gtk_window_destroy(GTK_WINDOW(window));
}

static void
test_unread_vote_on_open(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  g_autoptr(GhMessage) poll_message = NULL;
  g_autoptr(GhMlsPoll) poll = new_poll(FALSE, &poll_message);
  admit(store, poll_message);
  const gchar *choices[] = { gh_mls_poll_get_option(poll, 0)->id };
  g_autoptr(GError) error = NULL;
  g_autofree gchar *vote_event = gh_mls_poll_build_vote_event(
    peer, group_id, gh_message_get_created_at(poll_message) + 1,
    gh_message_get_rumor_id(poll_message), choices, 1, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) vote = message_from_event(vote_event);
  admit(store, vote);
  GhConversation *conversation = gh_conversation_store_lookup(
    store, gh_message_get_room_id(poll_message));
  g_assert_nonnull(conversation);
  guint first = 0;
  g_assert_cmpuint(gh_conversation_get_listed_unread(conversation, &first), ==, 1);
  g_assert_cmpuint(first, ==, 1); /* the unread vote has no visible row */

  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, store, NULL);
  GhPollUiConfig config = { 0 };
  gh_poll_ui_attach(window, &config);
  gtk_window_present(GTK_WINDOW(window));
  GhConversationView *view = GH_CONVERSATION_VIEW(
    gh_content_page_get_view(gh_window_get_content(window)));
  wait_for_window_or_card(GTK_WIDGET(window), FALSE);
  g_assert_true(gh_window_open_item(window, conversation));
  wait_for_window_or_card(GTK_WIDGET(view), TRUE);
  GtkWidget *card = find_card(GTK_WIDGET(view));
  wait_for_window_or_card(card, FALSE);
  g_assert_cmpuint(g_list_model_get_n_items(gh_conversation_view_get_timeline(view)), ==, 1);
  gtk_window_destroy(GTK_WINDOW(window));
}

static void
test_own_poll_and_vote_timeline(void)
{
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  g_autoptr(GhMessage) poll_message = NULL;
  g_autoptr(GhMlsPoll) poll = new_poll(FALSE, &poll_message);
  admit(store, poll_message);
  GhConversation *conversation = gh_conversation_store_lookup(
    store, gh_message_get_room_id(poll_message));
  g_assert_nonnull(conversation);
  GhConversationView *view = GH_CONVERSATION_VIEW(g_object_ref_sink(
    gh_conversation_view_new()));
  gh_conversation_view_set_conversation(view, conversation);
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 1);
  g_autoptr(GhTimelineItem) first = g_list_model_get_item(timeline, 0);
  g_assert_true(gh_timeline_item_get_message(first) == poll_message);
  g_assert_cmpint(gh_message_get_kind(gh_timeline_item_get_message(first)), ==,
                  GH_MLS_POLL_KIND);

  const gchar *choices[] = { gh_mls_poll_get_option(poll, 0)->id };
  g_autoptr(GError) error = NULL;
  g_autofree gchar *vote_event = gh_mls_poll_build_vote_event(
    peer, group_id, gh_message_get_created_at(poll_message) + 1,
    gh_message_get_rumor_id(poll_message), choices, 1, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) vote_message = message_from_event(vote_event);
  admit(store, vote_message);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 2);
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 1);
  g_autoptr(GhTimelineItem) still_first = g_list_model_get_item(timeline, 0);
  g_assert_true(gh_timeline_item_get_message(still_first) == poll_message);
  g_object_unref(view);
}

static void
test_private_transport_poll(void)
{
  g_autoptr(GError) error = NULL;
  const gchar *labels[] = { "Coffee", "Tea" };
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *poll_event = gh_mls_poll_build_event(
    account, NULL, now, "Private poll?", labels, 2,
    GH_MLS_POLL_SINGLE_CHOICE, 0, &error);
  g_assert_no_error(error);
  const gchar *recipients[] = { peer, NULL };
  g_autofree gchar *poll_rumor = gh_nip17_rumor_new_poll_room(
    account, recipients, poll_event, now, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) poll_message = gh_message_new_from_rumor(account, poll_rumor, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_message_get_kind(poll_message), ==, GH_MLS_POLL_KIND);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    gh_message_get_rumor_id(poll_message), account, now, poll_rumor, &error);
  g_assert_no_error(error);
  g_assert_nonnull(poll);
  const gchar *choices[] = { gh_mls_poll_get_option(poll, 0)->id };
  g_autofree gchar *vote_event = gh_mls_poll_build_vote_event(
    account, NULL, now + 1, gh_message_get_rumor_id(poll_message), choices, 1, &error);
  g_assert_no_error(error);
  g_autofree gchar *vote_rumor = gh_nip17_rumor_new_poll_room(
    account, recipients, vote_event, now + 1, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) vote_message = gh_message_new_from_rumor(account, vote_rumor, &error);
  g_assert_no_error(error);
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  admit(store, poll_message);
  GhConversation *conversation = gh_conversation_store_lookup(
    store, gh_message_get_room_id(poll_message));
  GhConversationView *view = GH_CONVERSATION_VIEW(g_object_ref_sink(
    gh_conversation_view_new()));
  gh_conversation_view_set_conversation(view, conversation);
  admit(store, vote_message);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 2);
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 1);
  g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, 0);
  g_assert_true(gh_timeline_item_get_message(item) == poll_message);
  g_object_unref(view);
}

static void
test_nip29_transport_poll(void)
{
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *poll_event = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[[\"h\",\"coffee\"],[\"poll\",\"1068\"],"
    "[\"option\",\"0\",\"Coffee\"],[\"option\",\"1\",\"Tea\"],"
    "[\"polltype\",\"singlechoice\"]],\"content\":\"Group poll?\"}",
    account, now);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) poll_message = gh_message_new_from_nip29_event(
    account, "wss://nos.lol", poll_event, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_message_get_kind(poll_message), ==, GH_MLS_POLL_KIND);
  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    gh_message_get_rumor_id(poll_message), account, now, poll_event, &error);
  g_assert_no_error(error);
  g_assert_nonnull(poll);
  g_autofree gchar *vote_event = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[[\"h\",\"coffee\"],[\"poll\",\"1018\"],"
    "[\"e\",\"%s\"],[\"response\",\"0\"]],\"content\":\"\"}",
    account, now + 1, gh_message_get_rumor_id(poll_message));
  g_autoptr(GhMessage) vote_message = gh_message_new_from_nip29_event(
    account, "wss://nos.lol", vote_event, &error);
  g_assert_no_error(error);
  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  admit(store, poll_message);
  GhConversation *conversation = gh_conversation_store_lookup(
    store, gh_message_get_room_id(poll_message));
  GhConversationView *view = GH_CONVERSATION_VIEW(g_object_ref_sink(
    gh_conversation_view_new()));
  gh_conversation_view_set_conversation(view, conversation);
  admit(store, vote_message);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 2);
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 1);
  g_autoptr(GhTimelineItem) item = g_list_model_get_item(timeline, 0);
  g_assert_true(gh_timeline_item_get_message(item) == poll_message);
  g_object_unref(view);
}

typedef struct {
  guint calls;
  gchar **choices;
} VoteCapture;

static void
capture_vote(GhPollCard *card, const gchar **choices, guint n_choices, VoteCapture *capture)
{
  (void)card;
  capture->calls++;
  g_clear_pointer(&capture->choices, g_strfreev);
  capture->choices = g_new0(gchar *, n_choices + 1);
  for (guint i = 0; i < n_choices; i++)
    capture->choices[i] = g_strdup(choices[i]);
}

static GPtrArray *
option_checks(GtkWidget *root)
{
  GPtrArray *checks = g_ptr_array_new();
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    if (GTK_IS_CHECK_BUTTON(child))
      g_ptr_array_add(checks, child);
    g_autoptr(GPtrArray) nested = option_checks(child);
    for (guint i = 0; i < nested->len; i++)
      g_ptr_array_add(checks, g_ptr_array_index(nested, i));
  }
  return checks;
}

static guint
visible_progress_bars(GtkWidget *root)
{
  guint count = GTK_IS_PROGRESS_BAR(root) && gtk_widget_get_visible(root);
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child))
    count += visible_progress_bars(child);
  return count;
}

static gboolean
has_vote_status(GtkWidget *root)
{
  if (GTK_IS_LABEL(root) &&
      g_strstr_len(gtk_label_get_text(GTK_LABEL(root)), -1, "You voted"))
    return TRUE;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child))
    if (has_vote_status(child)) return TRUE;
  return FALSE;
}

static GtkButton *
find_vote_button(GtkWidget *root)
{
  if (GTK_IS_BUTTON(root) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), "Vote") == 0)
    return GTK_BUTTON(root);
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkButton *found = find_vote_button(child);
    if (found) return found;
  }
  return NULL;
}

static void
test_card_selection(gconstpointer data)
{
  gboolean multiple = GPOINTER_TO_INT(data);
  g_autoptr(GhMessage) message = NULL;
  g_autoptr(GhMlsPoll) poll = new_poll(multiple, &message);
  GhPollCard *card = GH_POLL_CARD(g_object_ref_sink(gh_poll_card_new()));
  gh_poll_card_set_poll(card, poll);
  VoteCapture capture = { 0 };
  g_signal_connect(card, "vote-cast", G_CALLBACK(capture_vote), &capture);
  g_autoptr(GPtrArray) checks = option_checks(GTK_WIDGET(card));
  g_assert_cmpuint(checks->len, ==, 3);
  g_assert_cmpuint(visible_progress_bars(GTK_WIDGET(card)), ==, 0);
  for (guint i = 0; i < checks->len; i++)
    g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(g_ptr_array_index(checks, i))),
                    ==, multiple ? GTK_ACCESSIBLE_ROLE_CHECKBOX : GTK_ACCESSIBLE_ROLE_RADIO);
  GtkButton *vote = find_vote_button(GTK_WIDGET(card));
  g_assert_nonnull(vote);
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(vote)));
  gtk_check_button_set_active(g_ptr_array_index(checks, 0), TRUE);
  g_assert_cmpuint(capture.calls, ==, 0);
  if (multiple) {
    gtk_check_button_set_active(g_ptr_array_index(checks, 1), TRUE);
    g_assert_true(gtk_check_button_get_active(g_ptr_array_index(checks, 0)));
  } else {
    gtk_check_button_set_active(g_ptr_array_index(checks, 1), TRUE);
    g_assert_false(gtk_check_button_get_active(g_ptr_array_index(checks, 0)));
  }
  g_assert_cmpuint(capture.calls, ==, 0);
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(vote)));
  g_signal_emit_by_name(vote, "clicked");
  g_assert_cmpuint(capture.calls, ==, 1);
  g_assert_cmpuint(g_strv_length(capture.choices), ==, multiple ? 2 : 1);
  g_assert_cmpstr(capture.choices[0], ==, gh_mls_poll_get_option(poll, multiple ? 0 : 1)->id);
  if (multiple)
    g_assert_cmpstr(capture.choices[1], ==, gh_mls_poll_get_option(poll, 1)->id);
  g_assert_true(gh_mls_poll_apply_vote(poll, account,
    (const gchar **)capture.choices, gh_message_get_created_at(message) + 1,
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
  g_assert_cmpuint(visible_progress_bars(GTK_WIDGET(card)), ==, 3);
  g_assert_true(has_vote_status(GTK_WIDGET(card)));
  g_assert_true(gtk_check_button_get_active(g_ptr_array_index(checks, 1)));
  if (multiple)
    g_assert_true(gtk_check_button_get_active(g_ptr_array_index(checks, 0)));
  g_assert_false(gtk_widget_get_sensitive(GTK_WIDGET(vote)));
  g_strfreev(capture.choices);
  g_object_unref(card);
}

static void
assert_live_local_echo(gconstpointer data)
{
  GhConversationBackend backend = GPOINTER_TO_INT(data);
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  const gchar *labels[] = { "Coffee", "Tea" };
  g_autoptr(GError) error = NULL;
  g_autofree gchar *poll_event = backend == GH_CONVERSATION_BACKEND_NIP29
    ? g_strdup_printf(
        "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
        "\"tags\":[[\"h\",\"coffee\"],[\"poll\",\"1068\"],"
        "[\"option\",\"0\",\"Coffee\"],[\"option\",\"1\",\"Tea\"],"
        "[\"polltype\",\"singlechoice\"]],\"content\":\"Live poll?\"}",
        account, now)
    : gh_mls_poll_build_event(account,
        backend == GH_CONVERSATION_BACKEND_MLS ? group_id : NULL,
        now, "Live poll?", labels, 2, GH_MLS_POLL_SINGLE_CHOICE, 0, &error);
  g_assert_no_error(error);
  g_assert_nonnull(poll_event);

  g_autofree gchar *poll_rumor = NULL;
  g_autoptr(GhMessage) poll_message = NULL;
  const gchar *recipients[] = { peer, NULL };
  if (backend == GH_CONVERSATION_BACKEND_MLS)
    poll_message = message_from_event(poll_event);
  else if (backend == GH_CONVERSATION_BACKEND_NIP29)
    poll_message = gh_message_new_from_nip29_event(account, "wss://nos.lol",
                                                    poll_event, &error);
  else {
    poll_rumor = gh_nip17_rumor_new_poll_room(account, recipients, poll_event,
                                               now, 0, NULL, &error);
    g_assert_no_error(error);
    poll_message = gh_message_new_from_rumor(account, poll_rumor, &error);
  }
  g_assert_no_error(error);
  g_assert_nonnull(poll_message);
  g_assert_cmpint(gh_message_get_kind(poll_message), ==, GH_MLS_POLL_KIND);

  g_autoptr(GhMlsPoll) poll = gh_mls_poll_new_from_event(
    gh_message_get_rumor_id(poll_message), account, now,
    gh_message_get_rumor_json(poll_message), &error);
  g_assert_no_error(error);
  g_assert_nonnull(poll);
  const gchar *choices[] = { gh_mls_poll_get_option(poll, 0)->id };
  g_autofree gchar *vote_event = backend == GH_CONVERSATION_BACKEND_NIP29
    ? g_strdup_printf(
        "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
        "\"tags\":[[\"h\",\"coffee\"],[\"poll\",\"1018\"],"
        "[\"e\",\"%s\"],[\"response\",\"%s\"]],\"content\":\"\"}",
        account, now + 1, gh_message_get_rumor_id(poll_message), choices[0])
    : gh_mls_poll_build_vote_event(account,
        backend == GH_CONVERSATION_BACKEND_MLS ? group_id : NULL,
        now + 1, gh_message_get_rumor_id(poll_message), choices, 1, &error);
  g_assert_no_error(error);
  g_assert_nonnull(vote_event);
  g_autofree gchar *vote_rumor = NULL;
  g_autoptr(GhMessage) vote_message = NULL;
  if (backend == GH_CONVERSATION_BACKEND_MLS)
    vote_message = message_from_event(vote_event);
  else if (backend == GH_CONVERSATION_BACKEND_NIP29)
    vote_message = gh_message_new_from_nip29_event(account, "wss://nos.lol",
                                                    vote_event, &error);
  else {
    vote_rumor = gh_nip17_rumor_new_poll_room(account, recipients, vote_event,
                                               now + 1, 0, NULL, &error);
    g_assert_no_error(error);
    vote_message = gh_message_new_from_rumor(account, vote_rumor, &error);
  }
  g_assert_no_error(error);
  g_assert_nonnull(vote_message);
  g_assert_cmpint(gh_message_get_kind(vote_message), ==, GH_MLS_POLL_VOTE_KIND);

  g_autoptr(GhConversationStore) store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, account, NULL, NULL, NULL);
  GhConversation *conversation = backend == GH_CONVERSATION_BACKEND_NIP17
    ? gh_conversation_store_open_room(store, recipients, &error)
    : gh_conversation_store_ensure_group(store,
        gh_message_get_room_id(poll_message), "Live poll room");
  g_assert_no_error(error);
  g_assert_nonnull(conversation);
  g_assert_cmpint(gh_conversation_get_backend(conversation), ==, backend);
  g_assert_cmpstr(gh_conversation_get_room_id(conversation), ==,
                  gh_message_get_room_id(poll_message));
  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, store, NULL);
  GhPollUiConfig config = { 0 };
  gh_poll_ui_attach(window, &config);
  gtk_window_present(GTK_WINDOW(window));
  GhConversationView *view = GH_CONVERSATION_VIEW(
    gh_content_page_get_view(gh_window_get_content(window)));
  wait_for_window_or_card(GTK_WIDGET(window), FALSE);
  gh_conversation_view_set_conversation(view, conversation);
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 0);
  g_assert_null(find_card(GTK_WIDGET(view)));

  /* Each message is the sender's local echo, admitted while this room stays open. */
  admit(store, poll_message);
  wait_for_window_or_card(GTK_WIDGET(view), TRUE);
  GtkWidget *card = find_card(GTK_WIDGET(view));
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 1);
  GhMlsPoll *on_screen = gh_poll_card_get_poll(GH_POLL_CARD(card));
  g_assert_cmpstr(gh_mls_poll_get_question(on_screen), ==, "Live poll?");
  g_assert_cmpuint(gh_mls_poll_get_total_voters(on_screen), ==, 0);

  admit(store, vote_message);
  g_assert_true(find_card(GTK_WIDGET(view)) == card);
  g_assert_cmpuint(gh_mls_poll_get_total_voters(on_screen), ==, 1);
  g_assert_true(has_vote_status(card));
  g_assert_cmpuint(g_list_model_get_n_items(timeline), ==, 1);
  gtk_window_destroy(GTK_WINDOW(window));
}

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) return 77;
  adw_init();
  groundhog_register_resource();
  char *public_key = nostr_key_get_public(
    "0000000000000000000000000000000000000000000000000000000000000001");
  account = g_strdup(public_key);
  free(public_key);
  public_key = nostr_key_get_public(
    "0000000000000000000000000000000000000000000000000000000000000002");
  peer = g_strdup(public_key);
  free(public_key);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  g_test_add_func("/groundhog/poll/own-poll-card-on-open", test_own_poll_card_on_open);
  g_test_add_func("/groundhog/poll/unread-vote-on-open", test_unread_vote_on_open);
  g_test_add_func("/groundhog/poll/own-poll-and-vote-timeline", test_own_poll_and_vote_timeline);
  g_test_add_func("/groundhog/poll/nip17-own-poll-and-vote", test_private_transport_poll);
  g_test_add_func("/groundhog/poll/nip29-own-poll-and-vote", test_nip29_transport_poll);
  g_test_add_data_func("/groundhog/poll/mls-live-echo-while-open", GINT_TO_POINTER(GH_CONVERSATION_BACKEND_MLS), assert_live_local_echo);
  g_test_add_data_func("/groundhog/poll/nip17-live-echo-while-open", GINT_TO_POINTER(GH_CONVERSATION_BACKEND_NIP17), assert_live_local_echo);
  g_test_add_data_func("/groundhog/poll/nip29-live-echo-while-open", GINT_TO_POINTER(GH_CONVERSATION_BACKEND_NIP29), assert_live_local_echo);
  g_test_add_data_func("/groundhog/poll/single-choice-card", GINT_TO_POINTER(FALSE), test_card_selection);
  g_test_add_data_func("/groundhog/poll/multi-choice-card", GINT_TO_POINTER(TRUE), test_card_selection);
  int result = g_test_run();
  g_free(account);
  g_free(peer);
  return result;
}
