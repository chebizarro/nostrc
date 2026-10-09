/* The conversation view (charter G12: PT-2 render side, PT-3, UX-3
 * messages, UX-5, UX-8 states 11-13, D13) on a real GhConversationStore fed
 * through its admission API with fixture rumors: no relay, no signer, no
 * network. A counting fake stands in for a link-preview fetcher, and
 * "open-uri" is intercepted, so nothing is ever launched or fetched.
 * Covered: bubbles (alignment, classes, literal markup, selectable text),
 * runs and sender names, day separators, the delivery indicator and its
 * details for every GhMessageStatus, retry, links (open, confirm, refuse,
 * nostr: copy), previews behind consent and none without a fetcher, expiry,
 * scrolling (stick to the newest, "Jump to Latest", first unread), paging
 * older history and its "Earlier Messages" affordance (unread count,
 * failure without automatic retry),
 * announcements, compact width and the §7.15 states 11-13. Needs a display:
 * it self-skips (77) without one. Waits iterate the main context against a
 * deadline; they never sleep. With GROUNDHOG_TEST_SCREENSHOTS=<dir> the
 * screenshots case renders <dir>/groundhog-g12-*.png (wide and narrow, light
 * and dark, a three-person room and a failed message with its details).
 */
#include "gh-attachment-card.h"
#include "gh-conversation-list.h"
#include "gh-conversation-open-probe-private.h"
#include "gh-window.h"
#include "gh-shell.h"
#include "gh-composer.h"

#include "nostrc-test-gdk-frame.h"
#include "gh-conversation-private.h"
#include "gh-conversation-window.h"
#include "gh-preferences-dialog.h"
#include "blossom-fixture.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include <math.h>
#include "gh-timeline-row.h"
#include "gh-delivery-indicator.h"
#include "gh-message-row.h"
#include "gh-agent-event-row.h"
#include <nostr-gtk-1.0/gn-nostr-reference.h>
#include <nostr-gtk-1.0/gn-og-preview-card.h>
#include "gh-reaction-store.h"
#include "gh-reaction-picker.h"

#include "nostr-event.h"
#include "nostr-keys.h"
#include "nostr-tag.h"
#include "gh-test-active.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void groundhog_register_resource(void);

/* A (1) is the account; B-D (2-4) are the other people. */
#define KEYS 5
static const gchar *const secrets[KEYS] = {
  NULL,
  "0000000000000000000000000000000000000000000000000000000000000001",
  "0000000000000000000000000000000000000000000000000000000000000002",
  "0000000000000000000000000000000000000000000000000000000000000003",
  "0000000000000000000000000000000000000000000000000000000000000004",
};
static gchar *hex[KEYS];

static const gchar *const nprofile =
  "nostr:nprofile1qqsrhuxx8l9ex335q7he0f09aej04zpazpl0ne2cgukyawd24mayt8gpp4mhxue69";

static void
init_keys(void)
{
  for (guint key = 1; key < KEYS; key++) {
    char *pub = nostr_key_get_public(secrets[key]);
    g_assert_nonnull(pub);
    hex[key] = g_strdup(pub);
    free(pub);
  }
}

/* ---- fixture rumors ------------------------------------------------------------- */

/* A kind-14 rumor from author to recipients (key indexes, 0-terminated). */
static GhMessage *
rumor(guint author, const guint *recipients, gint64 created_at, const gchar *content,
      const gchar *subject)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, hex[author]);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  NostrTags *tags = nostr_tags_new(0);
  for (const guint *r = recipients; *r; r++)
    nostr_tags_append(tags, nostr_tag_new("p", hex[*r], NULL));
  if (subject)
    nostr_tags_append(tags, nostr_tag_new("subject", subject, NULL));
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(hex[1], json, &error);
  free(json);
  g_assert_no_error(error);
  return message;
}

/* Admitted as a local echo (no wrap); borrowed: the store holds it. */
static GhMessage *
add_to(GhConversationStore *store, guint author, const guint *recipients, gint64 created_at,
       const gchar *content, const gchar *subject)
{
  g_autoptr(GhMessage) message = rumor(author, recipients, created_at, content, subject);
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_add_message(store, message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  return message;
}

static GhMessage *
add_dm(GhConversationStore *store, guint author, guint recipient, gint64 created_at,
       const gchar *content)
{
  const guint to[] = { recipient, 0 };
  return add_to(store, author, to, created_at, content, NULL);
}

static GhConversation *
room_of(GhConversationStore *store, GhMessage *message)
{
  GhConversation *conversation =
    gh_conversation_store_lookup(store, gh_message_get_room_id(message));
  g_assert_nonnull(conversation);
  return conversation;
}

static gint64
now_seconds(void)
{
  return g_get_real_time() / G_USEC_PER_SEC;
}

/* Local noon today: "Today" whatever the time of the run. */
static gint64
noon_today(void)
{
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autoptr(GDateTime) noon = g_date_time_new_local(g_date_time_get_year(now),
                                                    g_date_time_get_month(now),
                                                    g_date_time_get_day_of_month(now), 12, 0, 0);
  return g_date_time_to_unix(noon);
}

/* ---- waits ------------------------------------------------------------------------ */

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

static gboolean
is_null(gpointer data)
{
  return *(gpointer *)data == NULL;
}

/* ---- widget lookups ---------------------------------------------------------------- */

static gpointer
template_child(gpointer widget, GType type, const char *name)
{
  GObject *child = gtk_widget_get_template_child(GTK_WIDGET(widget), type, name);
  g_assert_nonnull(child);
  return child;
}

static GtkWidget *
find_named_widget(GtkWidget *root, const char *name)
{
  if (g_strcmp0(gtk_widget_get_name(root), name) == 0)
    return root;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child)) {
    GtkWidget *found = find_named_widget(child, name);
    if (found)
      return found;
  }
  return NULL;
}

static gpointer
row_child(GhMessageRow *row, const char *name)
{
  gpointer child = gtk_widget_get_template_child(GTK_WIDGET(row), GH_TYPE_MESSAGE_ROW, name);
  return child ? child : find_named_widget(GTK_WIDGET(row), name);
}

static gpointer
view_child(GhConversationView *view, const char *name)
{
  return template_child(view, GH_TYPE_CONVERSATION_VIEW, name);
}

static gpointer
indicator_child(GhDeliveryIndicator *indicator, const char *name)
{
  return template_child(indicator, GH_TYPE_DELIVERY_INDICATOR, name);
}

/* A click (GtkWidget::activate on a button waits for its pressed look). */
static void
click(gpointer button)
{
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(button)));
  g_signal_emit_by_name(button, "clicked");
}

static gboolean
shown(gpointer widget)
{
  return widget && gtk_widget_get_visible(GTK_WIDGET(widget));
}

static const char *
text_of(gpointer label)
{
  return gtk_label_get_text(GTK_LABEL(label));
}

static void
collect(GtkWidget *widget, GType type, GPtrArray *found)
{
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(c, type))
      g_ptr_array_add(found, c);
    collect(c, type, found);
  }
}

/* A list item widget's message row (its child is a GhTimelineRow), or NULL
 * for a header or a local event. */
static GhMessageRow *
item_row(GtkWidget *item)
{
  GtkWidget *child = gtk_widget_get_first_child(item);
  if (!GH_IS_TIMELINE_ROW(child))
    return NULL;
  GhMessageRow *row = gh_timeline_row_get_message_row(GH_TIMELINE_ROW(child));
  return gh_message_row_get_message(row) ? row : NULL;
}

/* Pictures under widget that show an image: a decoded one (PT-2). A file
 * message's card (G22) keeps an empty, hidden picture until its Download. */
static guint
decoded_pictures(GtkWidget *widget)
{
  g_autoptr(GPtrArray) pictures = g_ptr_array_new();
  collect(widget, GTK_TYPE_PICTURE, pictures);
  guint decoded = 0;
  for (guint i = 0; i < pictures->len; i++)
    decoded += gtk_picture_get_paintable(g_ptr_array_index(pictures, i)) != NULL;
  return decoded;
}

/* The list item widgets holding a bound GhMessageRow, in list order. */
static GPtrArray *
item_widgets(GhConversationView *view)
{
  GPtrArray *items = g_ptr_array_new();
  GtkWidget *list = GTK_WIDGET(gh_conversation_view_get_message_list(view));
  for (GtkWidget *c = gtk_widget_get_first_child(list); c; c = gtk_widget_get_next_sibling(c))
    if (item_row(c))
      g_ptr_array_add(items, c);
  return items;
}

static GtkWidget *
item_for(GhConversationView *view, GhMessage *message)
{
  g_autoptr(GPtrArray) items = item_widgets(view);
  for (guint i = 0; i < items->len; i++) {
    GtkWidget *item = g_ptr_array_index(items, i);
    if (gh_message_row_get_message(item_row(item)) == message)
      return item;
  }
  return NULL;
}

static GhMessageRow *
row_for(GhConversationView *view, GhMessage *message)
{
  GtkWidget *item = item_for(view, message);
  if (!item)
    g_error("no row for \"%s\"", gh_message_get_content(message));
  return item_row(item);
}

typedef struct {
  GhConversationView *view;
  GhMessage *message;
} VisibleRowWait;

static gboolean
row_visible(gpointer data)
{
  VisibleRowWait *wait = data;
  GtkWidget *item = item_for(wait->view, wait->message);
  GtkWidget *scroller = view_child(wait->view, "scroller");
  graphene_rect_t bounds;
  return item && gtk_widget_get_mapped(item) &&
         gtk_widget_compute_bounds(item, scroller, &bounds) &&
         bounds.origin.y + bounds.size.height > 0 &&
         bounds.origin.y < gtk_widget_get_height(scroller);
}

/* The day separator texts, in list order. */
static GPtrArray *
header_texts(GhConversationView *view)
{
  GPtrArray *texts = g_ptr_array_new();
  GtkWidget *list = GTK_WIDGET(gh_conversation_view_get_message_list(view));
  for (GtkWidget *c = gtk_widget_get_first_child(list); c; c = gtk_widget_get_next_sibling(c))
    if (g_str_equal(gtk_widget_get_css_name(c), "header") &&
        GTK_IS_LABEL(gtk_widget_get_first_child(c)))
      g_ptr_array_add(texts, (gpointer)text_of(gtk_widget_get_first_child(c)));
  return texts;
}

static GhTimelineItem *
timeline_item(GhConversationView *view, guint position)
{
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  GhTimelineItem *item = g_list_model_get_item(timeline, position);
  g_assert_nonnull(item);
  g_object_unref(item); /* the timeline keeps it */
  return item;
}

static guint
timeline_length(GhConversationView *view)
{
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  return timeline ? g_list_model_get_n_items(timeline) : 0;
}

/* ---- fake link-preview fetcher ------------------------------------------------------ */

typedef struct {
  gchar *title;
  gchar *description;
} FakePreview;

static void
fake_preview_free(FakePreview *preview)
{
  g_free(preview->title);
  g_free(preview->description);
  g_free(preview);
}

typedef struct {
  GtkWindow *window;
  GhConversationView *view;
  GhConversationStore *store;
  GSettings *settings;
  guint opened;           /* "open-uri" emissions (never launched) */
  gchar *opened_uri;
  gchar *copied;          /* "copy-text" (never on the system clipboard) */
  guint retries;
  GhMessage *retried;
  guint unlocks;
  guint fetches;          /* fetcher calls: anything the network would see */
  gchar *fetched_uri;
  GTask *pending;         /* the fetch waiting for the test to answer */
  guint loads;            /* history loader calls */
} Fixture;

static void
fake_fetch(const gchar *uri, GCancellable *cancellable, GAsyncReadyCallback callback,
           gpointer callback_data, gpointer user_data)
{
  Fixture *f = user_data;
  f->fetches++;
  g_free(f->fetched_uri);
  f->fetched_uri = g_strdup(uri);
  g_assert_null(f->pending);
  f->pending = g_task_new(NULL, cancellable, callback, callback_data);
}

static gboolean
fake_finish(GAsyncResult *result, gchar **title, gchar **description, GError **error,
            gpointer user_data)
{
  (void)user_data;
  FakePreview *preview = g_task_propagate_pointer(G_TASK(result), error);
  if (!preview)
    return FALSE;
  *title = g_steal_pointer(&preview->title);
  *description = g_steal_pointer(&preview->description);
  fake_preview_free(preview);
  return TRUE;
}

static void
answer_fetch(Fixture *f, const gchar *title, const gchar *description)
{
  g_assert_nonnull(f->pending);
  GTask *task = g_steal_pointer(&f->pending);
  if (title) {
    FakePreview *preview = g_new0(FakePreview, 1);
    preview->title = g_strdup(title);
    preview->description = g_strdup(description);
    g_task_return_pointer(task, preview, (GDestroyNotify)fake_preview_free);
  } else {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "timed out");
  }
  g_object_unref(task);
}

/* ---- fixture ------------------------------------------------------------------------ */

static void
on_open_uri(GhConversationView *view, const gchar *uri, Fixture *f)
{
  f->opened++;
  g_free(f->opened_uri);
  f->opened_uri = g_strdup(uri);
  /* Never launch anything from a test. */
  g_signal_stop_emission_by_name(view, "open-uri");
}

/* The system clipboard is shared (on macOS it is the user's own, and a
 * pending item there re-enters the main loop later): never written here. */
static void
on_copy_text(GhConversationView *view, const gchar *text, Fixture *f)
{
  g_free(f->copied);
  f->copied = g_strdup(text);
  g_signal_stop_emission_by_name(view, "copy-text");
}

static void
on_retry(GhConversationView *view, GhMessage *message, Fixture *f)
{
  (void)view;
  f->retries++;
  f->retried = message;
}

static void
on_unlock(GhConversationView *view, Fixture *f)
{
  (void)view;
  f->unlocks++;
}

static void
fixture_setup(Fixture *f, gconstpointer data)
{
  (void)data;
  f->store = gh_conversation_store_new();
  gh_conversation_store_set_account(f->store, hex[1], NULL, NULL, NULL);
  f->settings = g_settings_new("org.nostr.Groundhog");
  g_settings_reset(f->settings, "link-previews");
  g_settings_reset(f->settings, "network-mode");
  /* An AdwWindow, as the application's is one: dialogs present inside it. */
  f->window = GTK_WINDOW(adw_window_new());
  GtkWidget *toasts = adw_toast_overlay_new();
  f->view = GH_CONVERSATION_VIEW(gh_conversation_view_new());
  adw_toast_overlay_set_child(ADW_TOAST_OVERLAY(toasts), GTK_WIDGET(f->view));
  adw_window_set_content(ADW_WINDOW(f->window), toasts);
  gh_conversation_view_set_settings(f->view, f->settings);
  g_signal_connect(f->view, "open-uri", G_CALLBACK(on_open_uri), f);
  g_signal_connect(f->view, "copy-text", G_CALLBACK(on_copy_text), f);
  g_signal_connect(f->view, "retry-requested", G_CALLBACK(on_retry), f);
  g_signal_connect(f->view, "unlock-requested", G_CALLBACK(on_unlock), f);
}

/* Nothing outlives the window: the view releases the conversation. */
static void
fixture_teardown(Fixture *f, gconstpointer data)
{
  (void)data;
  g_clear_object(&f->pending);
  gpointer weak = f->store;
  g_object_add_weak_pointer(G_OBJECT(f->store), &weak);
  gtk_window_destroy(f->window);
  g_object_unref(f->store);
  spin_until(is_null, &weak);
  g_settings_reset(f->settings, "link-previews");
  g_settings_reset(f->settings, "network-mode");
  g_object_unref(f->settings);
  g_free(f->opened_uri);
  g_free(f->copied);
  g_free(f->fetched_uri);
}

/* Mapped, not necessarily active: whether a window becomes active is up to
 * the platform (a macOS app in the background never does). */
static gboolean
is_mapped(gpointer data)
{
  Fixture *f = data;
  return gtk_widget_get_mapped(GTK_WIDGET(f->window));
}

/* Announcements are made only while the window is active (charter §7.14):
 * how many one event should add here. */
static guint
announced_if_active(Fixture *f)
{
  return gtk_window_is_active(f->window) ? 1 : 0;
}

/* Rows are bound: all of a short conversation, some of a long one (the
 * list only makes rows for what is near the view). */
static gboolean
rows_bound(gpointer data)
{
  Fixture *f = data;
  g_autoptr(GPtrArray) items = item_widgets(f->view);
  guint n = timeline_length(f->view);
  return items->len > 0 && (n > 10 || items->len == n);
}

static void
show(Fixture *f, GhConversation *conversation, int width, int height)
{
  gh_conversation_view_set_conversation(f->view, conversation);
  gtk_window_set_default_size(f->window, width, height);
  gtk_window_present(f->window);
  spin_until(is_mapped, f);
  spin_until(rows_bound, f);
  drain_idle();
}

/* ---- bubbles, runs and day separators ------------------------------------------------ */

static gboolean
two_headers(gpointer data)
{
  g_autoptr(GPtrArray) texts = header_texts(GH_CONVERSATION_VIEW(data));
  return texts->len == 2;
}

static void
test_bubbles_runs_and_days(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 noon = noon_today();
  gint64 yesterday = noon - 86400;
  GhMessage *m0 = add_dm(f->store, 2, 1, yesterday, "hi there");
  GhMessage *m1 = add_dm(f->store, 2, 1, yesterday + 120, "are you around?");
  GhMessage *m2 = add_dm(f->store, 1, 2, yesterday + 180,
                         "Yes! See https://example.com/a and <b>this</b> & more");
  GhMessage *m3 = add_dm(f->store, 1, 2, noon, "second day");
  GhMessage *m4 = add_dm(f->store, 1, 2, noon + 60, "same run");
  GhMessage *m5 = add_dm(f->store, 2, 1, noon + 400, "later, not the same run");
  gh_message_set_status(m3, GH_MESSAGE_STATUS_SENT);
  gh_message_set_status(m4, GH_MESSAGE_STATUS_SENDING);
  GhConversation *conversation = room_of(f->store, m0);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 700, 700);

  /* Runs: one sender, one day, at most 5 minutes apart. */
  static const gboolean starts[] = { TRUE, FALSE, TRUE, TRUE, FALSE, TRUE };
  static const gboolean ends[] = { FALSE, TRUE, TRUE, FALSE, TRUE, TRUE };
  g_assert_cmpuint(timeline_length(f->view), ==, 6);
  for (guint i = 0; i < 6; i++) {
    GhTimelineItem *item = timeline_item(f->view, i);
    g_assert_cmpint(gh_timeline_item_get_run_start(item), ==, starts[i]);
    g_assert_cmpint(gh_timeline_item_get_run_end(item), ==, ends[i]);
    g_assert_false(gh_timeline_item_get_show_sender(item)); /* one-to-one */
    g_assert_cmpstr(gh_timeline_item_get_day_label(item), ==, i < 3 ? "Yesterday" : "Today");
  }

  /* One section, and one separator, per local day. */
  guint start = 0, end = 0;
  GtkSectionModel *sections = GTK_SECTION_MODEL(gh_conversation_view_get_timeline(f->view));
  gtk_section_model_get_section(sections, 1, &start, &end);
  g_assert_cmpuint(start, ==, 0);
  g_assert_cmpuint(end, ==, 3);
  gtk_section_model_get_section(sections, 4, &start, &end);
  g_assert_cmpuint(start, ==, 3);
  g_assert_cmpuint(end, ==, 6);
  gtk_section_model_get_section(sections, 6, &start, &end);
  g_assert_cmpuint(start, ==, 6);
  g_assert_cmpuint(end, ==, G_MAXUINT);
  spin_until(two_headers, f->view);
  g_autoptr(GPtrArray) headers = header_texts(f->view);
  g_assert_cmpstr(g_ptr_array_index(headers, 0), ==, "Yesterday");
  g_assert_cmpstr(g_ptr_array_index(headers, 1), ==, "Today");

  /* Others' bubbles at the start, yours at the end, each its colour. */
  GhMessageRow *incoming = row_for(f->view, m0);
  GtkWidget *bubble = row_child(incoming, "bubble");
  g_assert_true(gtk_widget_has_css_class(bubble, "message-bubble"));
  g_assert_true(gtk_widget_has_css_class(bubble, "incoming"));
  g_assert_cmpint(gtk_widget_get_halign(bubble), ==, GTK_ALIGN_START);
  g_assert_false(shown(row_child(incoming, "sender_label")));
  g_assert_false(shown(row_child(incoming, "meta_box"))); /* the run goes on */
  GhMessageRow *outgoing = row_for(f->view, m2);
  bubble = row_child(outgoing, "bubble");
  g_assert_true(gtk_widget_has_css_class(bubble, "outgoing"));
  g_assert_cmpint(gtk_widget_get_halign(bubble), ==, GTK_ALIGN_END);
  g_assert_cmpint(gtk_widget_get_halign(row_child(outgoing, "meta_box")), ==, GTK_ALIGN_END);

  /* PT-3: the text as typed; only the address is a link; selectable. */
  GtkLabel *body = row_child(outgoing, "body_label");
  g_assert_cmpstr(gtk_label_get_text(body), ==, gh_message_get_content(m2));
  g_assert_true(gtk_label_get_selectable(body));
  g_assert_true(gtk_label_get_wrap(body));
  const char *markup = gtk_label_get_label(body);
  g_assert_nonnull(strstr(markup, "<a href=\"https://example.com/a\">https://example.com/a</a>"));
  g_assert_nonnull(strstr(markup, "&lt;b&gt;this&lt;/b&gt; &amp; more"));

  /* The time ends a run; the delivery state shows where it says something. */
  GhMessageRow *end_of_run = row_for(f->view, m1);
  g_assert_true(shown(row_child(end_of_run, "meta_box")));
  g_autofree gchar *time = gh_conversation_row_format_time_of_day(gh_message_get_created_at(m1));
  g_assert_cmpstr(text_of(row_child(end_of_run, "time_label")), ==, time);
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(end_of_run), "run-end"));
  g_assert_false(gtk_widget_has_css_class(GTK_WIDGET(end_of_run), "run-start"));
  GhMessageRow *sent = row_for(f->view, m3);   /* "Sent", mid-run: quiet */
  g_assert_false(shown(row_child(sent, "meta_box")));
  GhMessageRow *sending = row_for(f->view, m4);
  g_assert_true(shown(row_child(sending, "meta_box")));
  GhDeliveryIndicator *indicator = row_child(sending, "delivery");
  g_assert_true(shown(indicator));
  g_assert_cmpstr(text_of(indicator_child(indicator, "status_label")), ==, "Sending…");

  /* UX-3: the list and its items say who, when, what and the state. */
  GtkListView *list = gh_conversation_view_get_message_list(f->view);
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(list)), ==,
                  GTK_ACCESSIBLE_ROLE_LIST);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(list), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Messages");
  g_assert_cmpint(gtk_list_view_get_tab_behavior(list), ==, GTK_LIST_TAB_ITEM);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  GhMessage *const all[] = { m0, m1, m2, m3, m4, m5 };
  for (guint i = 0; i < G_N_ELEMENTS(all); i++) {
    GtkWidget *item = item_for(f->view, all[i]);
    g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(item)), ==,
                    GTK_ACCESSIBLE_ROLE_LIST_ITEM);
    g_autofree gchar *label = gh_message_row_compose_summary(all[i], now);
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        label);
  }
  g_autofree gchar *theirs = gh_message_row_compose_summary(m0, now);
  g_assert_true(g_str_has_prefix(theirs, "npub1"));
  g_assert_nonnull(strstr(theirs, ", Yesterday "));
  g_assert_true(g_str_has_suffix(theirs, ": hi there"));
  g_autofree gchar *mine = gh_message_row_compose_summary(m3, now);
  g_assert_true(g_str_has_prefix(mine, "You, "));
  g_assert_true(g_str_has_suffix(mine, ": second day. Sent."));
  (void)m5;
}

static void
test_multi_party_senders(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 t = noon_today();
  const guint to_ac[] = { 1, 3, 0 }, to_ab[] = { 1, 2, 0 }, to_bc[] = { 2, 3, 0 };
  GhMessage *b0 = add_to(f->store, 2, to_ac, t, "Saturday?", "Weekend Hike");
  GhMessage *b1 = add_to(f->store, 2, to_ac, t + 60, "Weather looks good", NULL);
  GhMessage *c0 = add_to(f->store, 3, to_ab, t + 120, "I'm in", NULL);
  GhMessage *a0 = add_to(f->store, 1, to_bc, t + 180, "Me too", NULL);
  GhConversation *room = room_of(f->store, b0);
  g_assert_true(room_of(f->store, a0) == room);
  gh_conversation_mark_read(room);
  show(f, room, 700, 600);

  /* The first message of each run from someone else carries their name. */
  static const gboolean names[] = { TRUE, FALSE, TRUE, FALSE };
  for (guint i = 0; i < 4; i++)
    g_assert_cmpint(gh_timeline_item_get_show_sender(timeline_item(f->view, i)), ==, names[i]);
  g_autofree gchar *b_name = gh_message_row_display_name(hex[2]);
  g_autofree gchar *c_name = gh_message_row_display_name(hex[3]);
  g_assert_true(g_str_has_prefix(b_name, "npub1"));
  g_assert_nonnull(strstr(b_name, "…"));
  GhMessageRow *row = row_for(f->view, b0);
  g_assert_true(shown(row_child(row, "sender_label")));
  g_assert_cmpstr(text_of(row_child(row, "sender_label")), ==, b_name);
  g_assert_false(shown(row_child(row_for(f->view, b1), "sender_label")));
  row = row_for(f->view, c0);
  g_assert_true(shown(row_child(row, "sender_label")));
  g_assert_cmpstr(text_of(row_child(row, "sender_label")), ==, c_name);
  g_assert_false(shown(row_child(row_for(f->view, a0), "sender_label")));
}

/* ---- delivery indicator (UX-5) ------------------------------------------------------- */

static GhDeliveryReport *
fixture_report(GhMessage *message, gpointer data)
{
  (void)message;
  (void)data;
  GhDeliveryReport *report = gh_delivery_report_new();
  gh_delivery_report_add(report, hex[2], "wss://relay.one.example", TRUE,
                         "Accepted by this relay.");
  gh_delivery_report_add(report, hex[2], "wss://relay.two.example/inbox", FALSE,
                         "This relay blocked it.");
  gh_delivery_report_add(report, NULL, "wss://inbox.self.example", FALSE,
                         "Couldn't reach this relay.");
  report->next_attempt_at = now_seconds() + 600;
  report->self_copy_missing = TRUE;
  report->detail = g_strdup("Accepted for 0 of 1 recipients.");
  return report;
}

static gboolean
popover_mapped(gpointer data)
{
  return gtk_widget_get_mapped(GTK_WIDGET(data));
}

static gboolean
popover_unmapped(gpointer data)
{
  return !gtk_widget_get_mapped(GTK_WIDGET(data));
}

static void
test_delivery_indicator(Fixture *f, gconstpointer data)
{
  (void)data;
  /* UX-5 on the copy itself: every status but NONE has an icon, a label and a
   * description, and nothing claims delivery or reading. */
  g_autoptr(GRegex) claims = g_regex_new("\\b(delivered|read|seen)\\b", G_REGEX_CASELESS, 0,
                                         NULL);
  GEnumClass *statuses = g_type_class_ref(GH_TYPE_MESSAGE_STATUS);
  g_assert_null(g_enum_get_value_by_nick(statuses, "delivered"));
  g_assert_null(g_enum_get_value_by_nick(statuses, "read"));
  for (guint i = 0; i < statuses->n_values; i++) {
    GhMessageStatus status = statuses->values[i].value;
    if (status == GH_MESSAGE_STATUS_NONE) {
      g_assert_null(gh_message_status_get_label(status));
      g_assert_null(gh_delivery_indicator_status_text(status));
      continue;
    }
    const gchar *label = gh_message_status_get_label(status);
    const gchar *description = gh_message_status_get_accessible_description(status);
    g_assert_true(label && *label && description && *description);
    g_assert_true(gh_message_status_get_icon_name(status) != NULL);
    g_assert_false(g_regex_match(claims, label, 0, NULL));
    g_assert_false(g_regex_match(claims, description, 0, NULL));
    /* nostrc-lff5: a room's description never speaks of one recipient. */
    const gchar *room = gh_message_status_get_accessible_description_for(status, 3);
    g_assert_true(room && *room);
    g_assert_false(g_regex_match(claims, room, 0, NULL));
    g_assert_null(strstr(room, "The recipient"));
    g_assert_null(strstr(room, "the recipient's"));
    g_assert_cmpstr(gh_message_status_get_accessible_description_for(status, 1), ==, description);
  }
  g_assert_cmpstr(gh_message_status_get_accessible_description_for(
                    GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX, 2), ==,
                  "Can't send. No one in this conversation has set up private messaging yet.");

  gint64 t = noon_today();
  GhMessage *theirs = add_dm(f->store, 2, 1, t, "hello");
  GhMessage *mine = add_dm(f->store, 1, 2, t + 600, "hi back");
  GhConversation *conversation = room_of(f->store, mine);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 700, 600);
  GhDeliveryIndicator *their_indicator = row_child(row_for(f->view, theirs), "delivery");
  GhMessageRow *row = row_for(f->view, mine);
  GhDeliveryIndicator *indicator = row_child(row, "delivery");
  GtkWidget *retry = row_child(row, "retry_button");
  /* Incoming, and own with no outbox record (NONE): no indicator. */
  g_assert_false(shown(their_indicator));
  g_assert_false(shown(indicator));

  GtkWidget *item = item_for(f->view, mine);
  for (guint i = 0; i < statuses->n_values; i++) {
    GhMessageStatus status = statuses->values[i].value;
    if (status == GH_MESSAGE_STATUS_NONE)
      continue;
    gh_message_set_status(mine, status);
    g_assert_cmpint(gh_delivery_indicator_get_status(indicator), ==, status);
    g_assert_true(shown(indicator));
    const char *icon = gtk_image_get_icon_name(indicator_child(indicator, "status_icon"));
    g_assert_cmpstr(icon, ==, gh_message_status_get_icon_name(status));
    GtkWidget *status_label = indicator_child(indicator, "status_label");
    g_assert_cmpint(shown(status_label), ==, status != GH_MESSAGE_STATUS_SENT);
    if (status != GH_MESSAGE_STATUS_SENT)
      g_assert_cmpstr(text_of(status_label), ==, gh_message_status_get_label(status));
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(indicator_child(indicator, "button")),
                                        GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        gh_message_status_get_label(status));
    g_assert_cmpint(shown(retry), ==, status == GH_MESSAGE_STATUS_NOT_SENT ||
                                      status == GH_MESSAGE_STATUS_PARTIALLY_SENT);
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *label = gh_message_row_compose_summary(mine, now);
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(item), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        label);
    g_assert_nonnull(strstr(label, gh_message_status_get_label(status)));
  }
  g_type_class_unref(statuses);

  /* "Not sent": an assertive announcement, and Try Again asks for a retry
   * (the outbox's to do) with this message; the button is focusable. */
  gh_message_set_status(mine, GH_MESSAGE_STATUS_SENDING);
  guint assertive = gh_conversation_view_get_announcements(
    f->view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
  guint step = announced_if_active(f);
  gh_message_set_status(mine, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_cmpuint(gh_conversation_view_get_announcements(
                     f->view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH), ==, assertive + step);
  if (step)
    g_assert_cmpstr(gh_conversation_view_get_last_announcement(f->view), ==, "Message not sent");
  gh_message_set_status(mine, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  g_assert_cmpuint(gh_conversation_view_get_announcements(
                     f->view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH), ==, assertive + 2 * step);
  if (step)
    g_assert_cmpstr(gh_conversation_view_get_last_announcement(f->view), ==,
                    gh_message_status_get_accessible_description(
                      GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX));
  gh_message_set_status(mine, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_true(shown(retry));
  g_assert_true(gtk_widget_get_focusable(retry));
  click(retry);
  g_assert_cmpuint(f->retries, ==, 1);
  g_assert_true(f->retried == mine);
  g_assert_true(g_str_has_suffix(text_of(indicator_child(indicator, "status_label")), "Not sent"));
  g_assert_true(gtk_widget_has_css_class(indicator_child(indicator, "status_label"), "error"));

  /* Details with nothing known: said so, plus what acceptance means. */
  GtkWidget *details = indicator_child(indicator, "details");
  gh_delivery_indicator_show_details(indicator);
  spin_until(popover_mapped, details);
  g_assert_cmpstr(text_of(indicator_child(indicator, "details_title")), ==, "Not sent");
  g_assert_cmpstr(text_of(indicator_child(indicator, "details_summary")), ==,
                  gh_message_status_get_accessible_description(GH_MESSAGE_STATUS_NOT_SENT));
  g_assert_true(shown(indicator_child(indicator, "details_unavailable")));
  g_assert_false(shown(indicator_child(indicator, "relay_list")));
  g_assert_nonnull(strstr(text_of(indicator_child(indicator, "details_honesty")),
                          "can't tell when anyone receives or reads it"));
  gh_delivery_indicator_hide_details(indicator);
  spin_until(popover_unmapped, details);
  /* A fresh popup surface for the next opening: GTK 4.22's macOS backend
   * thaws a re-shown popup surface it never froze again (a critical). */
  gtk_widget_unrealize(details);

  /* Details from the outbox: each receiver's relays and what each said. */
  gh_conversation_view_set_delivery_report_func(f->view, fixture_report, NULL, NULL);
  gh_delivery_indicator_show_details(indicator);
  spin_until(popover_mapped, details);
  GtkWidget *relays = indicator_child(indicator, "relay_list");
  g_assert_true(shown(relays));
  g_assert_false(shown(indicator_child(indicator, "details_unavailable")));
  static const char *hosts[] = { "relay.one.example", "relay.two.example", "inbox.self.example" };
  static const char *outcomes[] = { "Accepted by this relay.", "This relay blocked it.",
                                    "Couldn't reach this relay." };
  g_autofree gchar *b_name = gh_message_row_display_name(hex[2]);
  g_autofree gchar *b_header = g_strdup_printf("%s · 1 of 2 relays accepted", b_name);
  const char *headers[] = { b_header, NULL, "Your other devices · 0 of 1 relay accepted" };
  guint n = 0;
  for (GtkWidget *c = gtk_widget_get_first_child(relays); c; c = gtk_widget_get_next_sibling(c)) {
    if (!ADW_IS_ACTION_ROW(c))
      continue;
    g_assert_cmpuint(n, <, 3);
    g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(c)), ==, hosts[n]);
    g_assert_cmpstr(adw_action_row_get_subtitle(ADW_ACTION_ROW(c)), ==, outcomes[n]);
    g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(c)));
    GtkWidget *header = gtk_list_box_row_get_header(GTK_LIST_BOX_ROW(c));
    if (headers[n])
      g_assert_cmpstr(text_of(header), ==, headers[n]);
    else
      g_assert_null(header);
    n++;
  }
  g_assert_cmpuint(n, ==, 3);
  /* The outbox's own sentence replaces the generic description. */
  g_assert_cmpstr(text_of(indicator_child(indicator, "details_summary")), ==,
                  "Accepted for 0 of 1 recipients.");
  g_assert_true(shown(indicator_child(indicator, "details_note")));
  g_assert_cmpstr(text_of(indicator_child(indicator, "details_note")), ==,
                  gh_message_status_get_self_copy_note());
  g_assert_true(shown(indicator_child(indicator, "details_retry")));
  g_assert_true(g_str_has_prefix(text_of(indicator_child(indicator, "details_retry")),
                                 "Groundhog will try again at "));
  gh_delivery_indicator_hide_details(indicator);
  spin_until(popover_unmapped, details);
}

/* ---- G21: a kind-15 file message (PT-2/AT-7 render side) ------------------------------ */

/* Until G22's card, a file message reads "Photo" or "File": its Blossom URL is
 * neither shown as a link nor a preview candidate, and nothing is fetched or
 * decoded, even with link previews on. */
static void
test_file_message(Fixture *f, gconstpointer data)
{
  (void)data;
  gh_conversation_view_set_link_preview_fetcher(f->view, fake_fetch, fake_finish, f, NULL);
  g_settings_set_boolean(f->settings, "link-previews", TRUE);
  GhNip17File file = { 0 };
  file.url = (gchar *)"https://blossom.example.com/7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730";
  file.file_type = (gchar *)"image/jpeg";
  file.nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  g_strlcpy(file.x, "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730",
            sizeof file.x);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gh_nip17_file_rumor_new(hex[2], hex[1], &file, noon_today(), 0, NULL,
                                                   &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) m = gh_message_new_from_rumor(hex[1], json, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(f->store, m, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  GhConversation *conversation = room_of(f->store, m);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 700, 600);
  GhMessageRow *row = row_for(f->view, m);
  GtkLabel *body = row_child(row, "body_label");
  g_assert_cmpstr(gtk_label_get_text(body), ==, "Photo");
  g_assert_null(strstr(gtk_label_get_label(body), "href"));
  g_assert_null(strstr(gtk_label_get_label(body), "blossom.example.com"));
  g_assert_false(shown(row_child(row, "preview_box")));
  g_assert_cmpuint(f->fetches, ==, 0);
  g_assert_cmpuint(decoded_pictures(GTK_WIDGET(f->view)), ==, 0);
  /* G22: the card says what the file is in place of the body; with no
   * attachment service in this window it offers no Download. */
  g_assert_false(shown(GTK_WIDGET(body)));
  g_assert_true(shown(row_child(row, "attachment_slot")));
  GhAttachmentCard *card = row_child(row, "attachment_card");
  g_assert_nonnull(strstr(gh_attachment_card_get_summary(card), "Photo"));
  g_assert_null(gh_attachment_card_get_transfer(card));
  g_settings_reset(f->settings, "link-previews");
}

/* ---- links and previews (PT-2 render side, PT-3, D13) ---------------------------------- */

static void
test_markdown_copy_identity(Fixture *f, gconstpointer data)
{
  (void)data;
  GhMessage *message = add_dm(f->store, 2, 1, noon_today(),
                              "**Hello** [site](https://example.com/x)");
  GhConversation *conversation = room_of(f->store, message);
  show(f, conversation, 700, 600);
  GhMessageRow *row = row_for(f->view, message);
  GtkLabel *body = row_child(row, "body_label");
  g_assert_nonnull(strstr(gtk_label_get_label(body), "<b>Hello</b>"));
  g_assert_nonnull(strstr(gtk_label_get_text(body),
                          "site (https://example.com/x)"));
  GtkWidget *copy = row_child(row, "copy_button");
  g_assert_true(shown(copy));
  click(copy);
  g_assert_cmpstr(f->copied, ==, gtk_label_get_text(body));
  g_free(f->copied);
  f->copied = NULL;
  g_autofree gchar *old_id = g_strdup(gh_message_get_rumor_id(message));
  gh_conversation_view_set_conversation(f->view, NULL);
  gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.copy-message", "s", old_id);
  g_assert_null(f->copied);
}

static void
test_reference_card(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *uri = "nostr:note1zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsglnzgl";
  GhMessage *message = add_dm(f->store, 2, 1, noon_today(), uri);
  g_autoptr(GnNostrReference) reference = gn_nostr_reference_parse(uri);
  g_assert_nonnull(reference);
  reference->kind = 1;
  g_autofree gchar *repost = gn_nostr_build_repost_template(reference, NULL);
  g_assert_nonnull(repost);
  GhMessage *repost_message = add_dm(f->store, 2, 1, noon_today() + 1, repost);
  g_assert_cmpstr(gh_message_get_content(repost_message), ==, repost);
  g_autoptr(GnNostrRepostDescriptor) parsed_repost =
    gn_nostr_repost_descriptor_parse(repost, FALSE);
  g_assert_nonnull(parsed_repost);
  GhConversation *conversation = room_of(f->store, message);
  show(f, conversation, 700, 600);
  GhMessageRow *row = row_for(f->view, message);
  const gchar *cached_uri = NULL;
  const gchar *label = NULL;
  g_assert_true(gh_conversation_view_get_reference(f->view, message, &cached_uri, &label));
  g_assert_cmpstr(cached_uri, ==, uri);
  g_assert_nonnull(strstr(label, "Nostr note"));
  g_assert_true(shown(row_child(row, "reference_box")));
  g_assert_true(shown(row_child(row, "share_reference_button")));
  const gchar *repost_label = NULL;
  g_assert_true(gh_conversation_view_get_reference(f->view, repost_message,
                                                     NULL, &repost_label));
  g_assert_nonnull(strstr(repost_label, "Reposted Nostr note"));
  g_assert_true(shown(row_child(row_for(f->view, repost_message), "reference_box")));
  g_assert_cmpuint(f->fetches, ==, 0);
}

static void
test_share_reference(Fixture *f, gconstpointer data)
{
  (void)data;
  const gchar *uri = "nostr:note1zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zyg3zygsglnzgl";
  GhMessage *message = add_dm(f->store, 2, 1, noon_today(), uri);
  GhConversation *conversation = room_of(f->store, message);
  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, f->store, NULL);
  GhContentPage *content = gh_window_get_content(window);
  GhConversationView *view = GH_CONVERSATION_VIEW(gh_content_page_get_view(content));
  GhComposer *composer = gh_content_page_get_composer(content);
  g_assert_true(GH_IS_CONVERSATION_VIEW(view));
  gh_conversation_view_set_conversation(view, conversation);
  gh_composer_set_text(composer, "hello");
  gtk_widget_activate_action(GTK_WIDGET(view), "conversation.share-reference", "s",
                             gh_message_get_rumor_id(message));
  GtkTextBuffer *buffer = gtk_text_view_get_buffer(gh_composer_get_text_view(composer));
  GtkTextIter start, end;
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree gchar *draft = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_autofree gchar *expected = g_strdup_printf("hello %s", uri);
  g_assert_cmpstr(draft, ==, expected);
  g_assert_true(GH_IS_CONVERSATION_VIEW(view));
  gh_conversation_view_set_conversation(view, NULL);
  gtk_widget_activate_action(GTK_WIDGET(view), "conversation.share-reference", "s",
                             gh_message_get_rumor_id(message));
  gtk_text_buffer_get_bounds(buffer, &start, &end);
  g_autofree gchar *unchanged = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
  g_assert_cmpstr(unchanged, ==, expected);
  gtk_window_destroy(GTK_WINDOW(window));
}

/* On screen: libadwaita 1.5 ignores a close before the dialog is mapped. */
static gboolean
dialog_presented(gpointer dialog)
{
  return gtk_widget_get_root(GTK_WIDGET(dialog)) != NULL &&
         gtk_widget_get_mapped(GTK_WIDGET(dialog));
}

/* Asks again until the dialog has left the window: libadwaita 1.5 drops a
 * close that comes too soon after presenting. */
static gboolean
dialog_gone(gpointer dialog)
{
  if (gtk_widget_get_root(GTK_WIDGET(dialog)) == NULL)
    return TRUE;
  adw_dialog_force_close(ADW_DIALOG(dialog));
  return FALSE;
}

static void
close_dialog(AdwDialog *dialog)
{
  spin_until(dialog_gone, dialog);
}

static void
test_links(Fixture *f, gconstpointer data)
{
  (void)data;
  gh_conversation_view_set_link_preview_fetcher(f->view, fake_fetch, fake_finish, f, NULL);
  g_autofree gchar *text = g_strdup_printf(
    "Photo https://img.example/cat.png, page https://example.com/a, me %s, "
    "and javascript:alert(1) data:text/html,x file:///etc/passwd", nprofile);
  GhMessage *m = add_dm(f->store, 2, 1, noon_today(), text);
  GhConversation *conversation = room_of(f->store, m);
  gh_conversation_mark_read(conversation);
  gh_conversation_accept(conversation);
  show(f, conversation, 700, 600);
  GhMessageRow *row = row_for(f->view, m);

  /* PT-2: rendering fetches nothing and decodes no image. */
  g_assert_cmpuint(f->fetches, ==, 0);
  g_assert_cmpuint(decoded_pictures(GTK_WIDGET(f->view)), ==, 0);
  g_assert_false(shown(row_child(row, "attachment_slot")));
  g_assert_true(shown(row_child(row, "preview_box")));

  /* PT-3: three links, each showing itself; the dangerous ones are text. */
  GtkLabel *body = row_child(row, "body_label");
  const char *markup = gtk_label_get_label(body);
  g_autoptr(GRegex) anchors = g_regex_new("<a href=", 0, 0, NULL);
  g_autoptr(GMatchInfo) match = NULL;
  guint count = 0;
  for (g_regex_match(anchors, markup, 0, &match); g_match_info_matches(match);
       g_match_info_next(match, NULL))
    count++;
  g_assert_cmpuint(count, ==, 3);
  g_assert_null(strstr(markup, "href=\"javascript"));
  g_assert_null(strstr(markup, "href=\"data"));
  g_assert_null(strstr(markup, "href=\"file"));

  /* A click on a plain https link goes through the policy (GTK's default,
   * which would open anything, never runs) and out through "open-uri". */
  gboolean handled = FALSE;
  g_signal_emit_by_name(body, "activate-link", "https://example.com/a", &handled);
  g_assert_true(handled);
  g_assert_cmpuint(f->opened, ==, 1);
  g_assert_cmpstr(f->opened_uri, ==, "https://example.com/a");

  /* http: confirmation first, showing the full address. */
  AdwAlertDialog *link_dialog = view_child(f->view, "link_dialog");
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.open-link", "s",
                                           "http://example.com/x"));
  g_assert_cmpuint(f->opened, ==, 1);
  spin_until(dialog_presented, link_dialog);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(link_dialog), "http://example.com/x"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(link_dialog), "isn't secure"));
  g_signal_emit_by_name(link_dialog, "response", "link-open");
  g_assert_cmpuint(f->opened, ==, 2);
  g_assert_cmpstr(f->opened_uri, ==, "http://example.com/x");
  close_dialog(ADW_DIALOG(link_dialog));

  /* An IDN look-alike: confirmation shows punycode; cancelling opens nothing. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.open-link", "s",
                                           "https://\xd0\xb0pple.com/login"));
  spin_until(dialog_presented, link_dialog);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(link_dialog),
                          "https://xn--pple-43d.com/login"));
  g_signal_emit_by_name(link_dialog, "response", "link-cancel");
  g_assert_cmpuint(f->opened, ==, 2);
  close_dialog(ADW_DIALOG(link_dialog));

  /* In Tor mode a plain https link is confirmed as well: the browser does
   * not use Groundhog's Tor connection (W16 review #4). */
  g_settings_set_string(f->settings, "network-mode", "tor");
  g_signal_emit_by_name(body, "activate-link", "https://example.com/a", &handled);
  g_assert_true(handled);
  g_assert_cmpuint(f->opened, ==, 2);
  spin_until(dialog_presented, link_dialog);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(link_dialog),
                          "doesn't use Groundhog's Tor connection"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(link_dialog), "https://example.com/a"));
  g_signal_emit_by_name(link_dialog, "response", "link-open");
  g_assert_cmpuint(f->opened, ==, 3);
  g_assert_cmpstr(f->opened_uri, ==, "https://example.com/a");
  close_dialog(ADW_DIALOG(link_dialog));
  g_settings_set_string(f->settings, "network-mode", "system");

  /* Refused outright: no dialog, nothing opened. */
  static const char *refused[] = { "javascript:alert(1)", "data:text/html,x",
                                   "file:///etc/passwd", "ftp://files.example/x" };
  for (guint i = 0; i < G_N_ELEMENTS(refused); i++) {
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.open-link", "s",
                                             refused[i]));
    drain_idle();
    g_assert_false(dialog_presented(link_dialog));
  }
  g_assert_cmpuint(f->opened, ==, 3);

  /* A nostr: address is copied: never fetched, never handed to an app. */
  g_signal_emit_by_name(body, "activate-link", nprofile, &handled);
  g_assert_true(handled);
  g_assert_cmpstr(f->copied, ==, nprofile);
  g_assert_cmpuint(f->opened, ==, 3);
  g_assert_cmpuint(f->fetches, ==, 0);
}

static GhLinkPreviewState
preview_state(Fixture *f, GhMessage *message)
{
  return gh_conversation_view_get_link_preview(f->view, message, NULL, NULL);
}

static gboolean
not_loading(gpointer data)
{
  Fixture *f = data;
  return f->pending == NULL;
}

typedef struct {
  Fixture *f;
  GhMessage *message;
  GhLinkPreviewState state;
} StateWait;

static gboolean
state_is(gpointer data)
{
  StateWait *wait = data;
  return preview_state(wait->f, wait->message) == wait->state;
}

static void
test_link_previews(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 t = noon_today();
  GhMessage *news = add_dm(f->store, 2, 1, t, "read https://news.example/story please");
  GhMessage *plain = add_dm(f->store, 2, 1, t + 600, "only http://old.example here");
  GhMessage *second = add_dm(f->store, 2, 1, t + 1200, "and https://second.example/x");
  GhMessage *third = add_dm(f->store, 2, 1, t + 1800, "last https://third.example/y");
  GhConversation *conversation = room_of(f->store, news);
  gh_conversation_mark_read(conversation);
  gh_conversation_accept(conversation);
  show(f, conversation, 700, 700);
  AdwAlertDialog *dialog = view_child(f->view, "preview_dialog");
  GtkCheckButton *dont_ask = view_child(f->view, "preview_dont_ask");
  g_assert_false(g_settings_get_boolean(f->settings, "link-previews"));

  /* No fetcher (this build): nothing is offered, and asking anyway neither
   * asks consent nor keeps any (W13b review, non-blocking #1). */
  GhMessageRow *row = row_for(f->view, news);
  GtkWidget *button = row_child(row, "preview_button");
  g_assert_null(button);
  g_assert_false(gh_conversation_view_get_previews_available(f->view));
  g_assert_false(shown(row_child(row, "preview_box")));
  gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.show-preview", "s",
                             gh_message_get_rumor_id(news));
  drain_idle();
  g_assert_false(dialog_presented(dialog));
  g_assert_cmpint(preview_state(f, news), ==, GH_LINK_PREVIEW_NONE);
  g_assert_false(g_settings_get_boolean(f->settings, "link-previews"));
  g_assert_cmpuint(f->fetches, ==, 0);

  /* With a fetcher: offered only for https, never loaded before asked. */
  gh_conversation_view_set_link_preview_fetcher(f->view, fake_fetch, fake_finish, f, NULL);
  g_assert_true(shown(row_child(row, "preview_box")));
  button = row_child(row, "preview_button");
  g_assert_true(shown(button));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, "Show Preview");
  g_assert_false(shown(row_child(row_for(f->view, plain), "preview_box")));
  g_assert_cmpint(preview_state(f, news), ==, GH_LINK_PREVIEW_NONE);

  /* D13: the tap asks first, naming the host and the network mode. */
  click(button);
  spin_until(dialog_presented, dialog);
  g_assert_cmpint(preview_state(f, news), ==, GH_LINK_PREVIEW_ASKING);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(dialog), "news.example"));
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(dialog), "your IP address"));
  g_assert_true(shown(dont_ask));
  g_signal_emit_by_name(dialog, "response", "preview-cancel");
  close_dialog(ADW_DIALOG(dialog));
  g_assert_cmpint(preview_state(f, news), ==, GH_LINK_PREVIEW_NONE);
  g_assert_cmpuint(f->fetches, ==, 0);

  /* In Tor mode: "Don't ask again" turns the setting on. */
  g_settings_set_string(f->settings, "network-mode", "tor");
  row = row_for(f->view, second);
  button = row_child(row, "preview_button");
  click(button);
  spin_until(dialog_presented, dialog);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(dialog), "second.example through Tor"));
  gtk_check_button_set_active(dont_ask, TRUE);
  g_signal_emit_by_name(dialog, "response", "preview-show");
  close_dialog(ADW_DIALOG(dialog));
  g_assert_true(g_settings_get_boolean(f->settings, "link-previews"));
  g_assert_cmpuint(f->fetches, ==, 1);
  g_assert_cmpstr(f->fetched_uri, ==, "https://second.example/x");
  g_assert_cmpint(preview_state(f, second), ==, GH_LINK_PREVIEW_LOADING);
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, "Loading Preview…");
  g_assert_false(gtk_widget_get_sensitive(button));
  answer_fetch(f, "Second Story", "What happened next");
  StateWait loaded = { f, second, GH_LINK_PREVIEW_LOADED };
  spin_until(state_is, &loaded);
  GtkWidget *card = row_child(row, "og_card");
  g_assert_true(shown(card));
  GtkWidget *card_title = gtk_widget_get_first_child(card);
  GtkWidget *card_description = gtk_widget_get_next_sibling(card_title);
  g_assert_cmpstr(text_of(card_title), ==, "Second Story");
  g_assert_cmpstr(text_of(card_description), ==, "What happened next");
  g_assert_false(shown(button));

  /* Allowed now: a tap fetches without asking; a failure can be retried. */
  row = row_for(f->view, third);
  button = row_child(row, "preview_button");
  click(button);
  g_assert_false(dialog_presented(dialog));
  g_assert_cmpuint(f->fetches, ==, 2);
  answer_fetch(f, NULL, NULL);
  StateWait failed = { f, third, GH_LINK_PREVIEW_FAILED };
  spin_until(state_is, &failed);
  g_assert_cmpstr(text_of(row_child(row, "preview_text")), ==, "The preview couldn't be loaded.");
  g_assert_true(shown(button));
  g_assert_true(gtk_widget_get_sensitive(button));
  spin_until(not_loading, f);
}

static void
test_sender_scoped_consent(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 t = noon_today();
  const guint to_ac[] = { 1, 3, 0 }, to_ab[] = { 1, 2, 0 };
  GhMessage *first = add_to(f->store, 2, to_ac, t,
                            "https://sender-two.example/one", "Weekend Hike");
  GhMessage *same_sender = add_to(f->store, 2, to_ac, t + 60,
                                  "https://sender-two.example/two", NULL);
  GhMessage *other_sender = add_to(f->store, 3, to_ab, t + 120,
                                   "https://sender-three.example/one", NULL);
  GhConversation *conversation = room_of(f->store, first);
  g_assert_true(room_of(f->store, other_sender) == conversation);
  gh_conversation_accept(conversation);
  show(f, conversation, 700, 700);
  gh_conversation_view_set_link_preview_fetcher(f->view, fake_fetch, fake_finish, f, NULL);
  AdwAlertDialog *dialog = view_child(f->view, "preview_dialog");
  GtkCheckButton *dont_ask = view_child(f->view, "preview_dont_ask");

  click(row_child(row_for(f->view, first), "preview_button"));
  spin_until(dialog_presented, dialog);
  g_assert_cmpuint(f->fetches, ==, 0);
  gtk_check_button_set_active(dont_ask, TRUE);
  g_signal_emit_by_name(dialog, "response", "preview-show");
  close_dialog(ADW_DIALOG(dialog));
  g_assert_cmpuint(f->fetches, ==, 1);
  answer_fetch(f, "First", NULL);
  StateWait first_loaded = { f, first, GH_LINK_PREVIEW_LOADED };
  spin_until(state_is, &first_loaded);

  click(row_child(row_for(f->view, same_sender), "preview_button"));
  g_assert_false(dialog_presented(dialog));
  g_assert_cmpuint(f->fetches, ==, 2);
  answer_fetch(f, "Second", NULL);
  StateWait second_loaded = { f, same_sender, GH_LINK_PREVIEW_LOADED };
  spin_until(state_is, &second_loaded);

  /* A group member cannot inherit another member's permission. */
  click(row_child(row_for(f->view, other_sender), "preview_button"));
  spin_until(dialog_presented, dialog);
  g_assert_cmpuint(f->fetches, ==, 2);
  g_signal_emit_by_name(dialog, "response", "preview-cancel");
  close_dialog(ADW_DIALOG(dialog));

  /* A different conversation cannot inherit the first one's setting, even
   * though the global switch is now on. Returning also asks afresh. */
  GhMessage *different = add_dm(f->store, 4, 1, t + 180,
                                "https://sender-four.example/one");
  GhConversation *different_room = room_of(f->store, different);
  gh_conversation_accept(different_room);
  gh_conversation_view_set_conversation(f->view, different_room);
  gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.show-preview", "s",
                             gh_message_get_rumor_id(different));
  spin_until(dialog_presented, dialog);
  g_assert_cmpuint(f->fetches, ==, 2);
  g_signal_emit_by_name(dialog, "response", "preview-cancel");
  close_dialog(ADW_DIALOG(dialog));
  gh_conversation_view_set_conversation(f->view, conversation);
  gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.show-preview", "s",
                             gh_message_get_rumor_id(same_sender));
  spin_until(dialog_presented, dialog);
  g_assert_cmpuint(f->fetches, ==, 2);
  g_signal_emit_by_name(dialog, "response", "preview-cancel");
  close_dialog(ADW_DIALOG(dialog));
  g_settings_reset(f->settings, "link-previews");
}

typedef struct { GObject parent_instance; } TestPictureSource;
typedef struct { GObjectClass parent_class; } TestPictureSourceClass;
G_DEFINE_TYPE(TestPictureSource, test_picture_source, G_TYPE_OBJECT)
static void test_picture_source_class_init(TestPictureSourceClass *klass)
{
  g_signal_new("profile-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0,
               NULL, NULL, NULL, G_TYPE_NONE, 1, G_TYPE_STRING);
}
static void test_picture_source_init(TestPictureSource *self) { (void)self; }

/* Same local HTTP fixture as the attachment tests; the injected transport
 * changes only https to loopback http. Parsing, bounds, cancellation and the
 * real GhNetHttp request run normally. Production refuses private addresses. */
static void
web_local_get(gpointer data, const gchar *uri, gsize limit, GCancellable *cancel,
              GAsyncReadyCallback callback, gpointer user_data)
{
  g_assert_true(g_str_has_prefix(uri, "https://127.0.0.1:"));
  g_autofree gchar *local = g_strconcat("http://", uri + strlen("https://"), NULL);
  gh_net_http_get_accept_async(data, local, "*/*", limit, cancel, callback, user_data);
}
static GBytes *web_local_finish(gpointer data, GAsyncResult *result, GError **error)
{ return gh_net_http_get_finish(data, result, error); }
static gchar *web_picture_uri(const gchar *pubkey, gpointer data)
{
  /* Only the peer (key 2) has a picture; the account's own messages none. */
  if (g_strcmp0(pubkey, hex[2]) != 0)
    return NULL;
  return g_strdup(g_object_get_data(data, "picture"));
}
typedef struct { Fixture *f; GhMessage *message; GhWebKind kind; } WebWait;
static gboolean web_loaded(gpointer data)
{
  WebWait *wait = data;
  if (wait->kind == GH_WEB_PREVIEW)
    return preview_state(wait->f, wait->message) == GH_LINK_PREVIEW_LOADED;
  GhLinkPreviewState state;
  return gh_conversation_view_get_web_texture(wait->f->view, wait->message, wait->kind, &state) != NULL;
}
static gboolean web_held(gpointer data) { return blossom_fixture_held(data) > 0; }

static void
test_web_consent(Fixture *f, gconstpointer data)
{
  GhWebKind kind = GPOINTER_TO_UINT(data);
  const gchar *key = gh_web_content_setting(kind);
  g_settings_set_boolean(f->settings, key, FALSE);
  g_settings_set_string(f->settings, "network-mode", "none");
  BlossomFixture *server = blossom_fixture_new();
  g_autoptr(GBytes) body = NULL;
  if (kind == GH_WEB_PREVIEW) {
    const gchar *html = "<html><head><meta property='og:title' content='Local story'>"
      "<meta property='og:description' content='Only head text'>"
      "<meta property='og:image' content='https://127.0.0.1:1/not-fetched'></head>"
      "<body><img src='https://127.0.0.1:1/not-fetched'></body></html>";
    body = g_bytes_new(html, strlen(html));
  } else {
    guint8 pixel[] = { 255, 0, 0, 255 };
    g_autoptr(GBytes) pixels = g_bytes_new(pixel, sizeof pixel);
    g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, pixels, 4));
    body = gdk_texture_save_to_png_bytes(texture);
  }
  g_autofree gchar *hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
  blossom_fixture_put_blob(server, hash, body);
  g_autofree gchar *uri = g_strdup_printf("https://127.0.0.1:%u/%s", blossom_fixture_port(server), hash);
  GhMessage *message = add_dm(f->store, 2, 1, noon_today(), uri);
  GhConversation *conversation = room_of(f->store, message);
  show(f, conversation, 700, 700);
  g_autoptr(GhNetHttp) http = gh_net_http_new(f->settings);
  static const GhHttpTransport transport = { web_local_get, web_local_finish };
  gh_conversation_view_enable_web_content(f->view, &transport, http);
  g_autoptr(GObject) source = g_object_new(test_picture_source_get_type(), NULL);
  if (kind != GH_WEB_PICTURE)
    g_object_set_data_full(source, "picture", g_strdup(uri), g_free);
  gh_conversation_view_set_picture_source(f->view, web_picture_uri, source);
  g_autoptr(GhPreferencesDialog) prefs = g_object_ref_sink(gh_preferences_dialog_new(f->settings,
    GH_PREFERENCES_FEATURE_LINK_PREVIEWS | GH_PREFERENCES_FEATURE_REMOTE_IMAGES |
    GH_PREFERENCES_FEATURE_PROFILE_PICTURES));
  AdwSwitchRow *setting = ADW_SWITCH_ROW(gh_preferences_dialog_get_key_widget(prefs, key));
  g_assert_true(gtk_widget_get_sensitive(GTK_WIDGET(setting)));
  g_assert_false(adw_switch_row_get_active(setting));
  drain_idle();
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  const gchar *button_name = kind == GH_WEB_PREVIEW ? "preview_button"
    : kind == GH_WEB_IMAGE ? "image_button" : "picture_button";
  GtkWidget *button = row_child(row_for(f->view, message), button_name);
  AdwAlertDialog *dialog = view_child(f->view, "preview_dialog");
  /* A Message Request offers no web action; even direct action activation
   * cannot trigger consent or traffic. Acceptance reveals the action. */
  g_assert_true(gh_conversation_get_is_request(conversation));
  g_assert_false(shown(kind == GH_WEB_PREVIEW
    ? row_child(row_for(f->view, message), "preview_box") : button));
  g_autofree gchar *request_id = g_strconcat(kind == GH_WEB_PREVIEW ? "" :
    kind == GH_WEB_IMAGE ? "image:" : "picture:", gh_message_get_rumor_id(message), NULL);
  gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.show-preview", "s", request_id);
  drain_idle();
  g_assert_false(dialog_presented(dialog));
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  gh_conversation_accept(conversation);
  if (kind == GH_WEB_PICTURE) {
    g_assert_false(shown(button));
    /* Kind-0 metadata can arrive after the row: expose the action then. */
    g_object_set_data_full(source, "picture", g_strdup(uri), g_free);
    g_signal_emit_by_name(source, "profile-changed", hex[2]);
  }
  button = row_child(row_for(f->view, message), button_name);
  g_assert_true(shown(button));
  click(button);
  spin_until(dialog_presented, dialog);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(dialog), "127.0.0.1"));
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  g_signal_emit_by_name(dialog, "response", "preview-cancel");
  close_dialog(ADW_DIALOG(dialog));
  drain_idle();
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  /* A global preference alone must never grant consent. */
  adw_switch_row_set_active(setting, TRUE);
  click(button);
  spin_until(dialog_presented, dialog);
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  gtk_check_button_set_active(view_child(f->view, "preview_dont_ask"), TRUE);
  g_signal_emit_by_name(dialog, "response", "preview-show");
  close_dialog(ADW_DIALOG(dialog));
  WebWait loaded = { f, message, kind };
  spin_until(web_loaded, &loaded);
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 1);
  if (kind == GH_WEB_PREVIEW) {
    const gchar *title = NULL;
    gh_conversation_view_get_link_preview(f->view, message, &title, NULL);
    g_assert_cmpstr(title, ==, "Local story");
    g_assert_cmpstr(gh_conversation_view_get_og_image_uri(f->view, message), ==,
                    "https://127.0.0.1:1/not-fetched");
    /* Metadata never fetches artwork; an explicit artwork action asks for
     * separate consent, and Cancel sends no second request. */
    GnOgPreviewCard *card = GN_OG_PREVIEW_CARD(
      row_child(row_for(f->view, message), "og_card"));
    g_assert_true(GN_IS_OG_PREVIEW_CARD(card));
    gn_og_preview_card_request_image(card);
    spin_until(dialog_presented, dialog);
    g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 1);
    g_signal_emit_by_name(dialog, "response", "preview-cancel");
    close_dialog(ADW_DIALOG(dialog));
  }
  /* Revocation clears results and consent. A pending request cannot restore
   * the old result after its callback arrives, even in the same room. */
  adw_switch_row_set_active(setting, FALSE);
  g_assert_false(web_loaded(&loaded));
  blossom_fixture_set_hold(server, TRUE);
  click(row_child(row_for(f->view, message), button_name));
  spin_until(dialog_presented, dialog);
  gtk_check_button_set_active(view_child(f->view, "preview_dont_ask"), TRUE);
  g_signal_emit_by_name(dialog, "response", "preview-show");
  close_dialog(ADW_DIALOG(dialog));
  spin_until(web_held, server);
  adw_switch_row_set_active(setting, FALSE);
  blossom_fixture_release_held(server);
  drain_idle();
  g_assert_false(web_loaded(&loaded));
  /* Returning to a room doesn't restore permission, even with the preference on. */
  gh_conversation_view_set_conversation(f->view, NULL);
  gh_conversation_view_set_conversation(f->view, conversation);
  adw_switch_row_set_active(setting, TRUE);
  g_autofree gchar *id = g_strconcat(kind == GH_WEB_PREVIEW ? "" : kind == GH_WEB_IMAGE ? "image:" : "picture:",
                                    gh_message_get_rumor_id(message), NULL);
  gtk_widget_activate_action(GTK_WIDGET(f->view), "conversation.show-preview", "s", id);
  spin_until(dialog_presented, dialog);
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 2);
  g_signal_emit_by_name(dialog, "response", "preview-cancel");
  close_dialog(ADW_DIALOG(dialog));
  gh_conversation_view_set_conversation(f->view, NULL);
  gh_conversation_view_enable_web_content(f->view, NULL, NULL);
  g_settings_reset(f->settings, key);
  blossom_fixture_free(server);
}

/* An allow for one message's profile picture (owner report: it loaded for
 * that message only) loads the same picture under the sender's other
 * messages, with one fetch; a message by someone else keeps its button. */
static void
test_web_allow_covers_sender(Fixture *f, gconstpointer data)
{
  (void)data;
  g_settings_set_boolean(f->settings, "load-profile-pictures", FALSE);
  g_settings_set_string(f->settings, "network-mode", "none");
  BlossomFixture *server = blossom_fixture_new();
  guint8 pixel[] = { 0, 255, 0, 255 };
  g_autoptr(GBytes) pixels = g_bytes_new(pixel, sizeof pixel);
  g_autoptr(GdkTexture) texture = GDK_TEXTURE(gdk_memory_texture_new(1, 1, GDK_MEMORY_R8G8B8A8, pixels, 4));
  g_autoptr(GBytes) body = gdk_texture_save_to_png_bytes(texture);
  g_autofree gchar *hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, body);
  blossom_fixture_put_blob(server, hash, body);
  g_autofree gchar *uri = g_strdup_printf("https://127.0.0.1:%u/%s", blossom_fixture_port(server), hash);
  GhMessage *first = add_dm(f->store, 2, 1, noon_today() - 60, "first");
  GhMessage *second = add_dm(f->store, 2, 1, noon_today() - 30, "second");
  GhMessage *mine = add_dm(f->store, 1, 2, noon_today(), "mine");
  GhConversation *conversation = room_of(f->store, first);
  gh_conversation_accept(conversation);
  show(f, conversation, 700, 700);
  g_autoptr(GhNetHttp) http = gh_net_http_new(f->settings);
  static const GhHttpTransport transport = { web_local_get, web_local_finish };
  gh_conversation_view_enable_web_content(f->view, &transport, http);
  g_autoptr(GObject) source = g_object_new(test_picture_source_get_type(), NULL);
  g_object_set_data_full(source, "picture", g_strdup(uri), g_free);
  gh_conversation_view_set_picture_source(f->view, web_picture_uri, source);
  drain_idle();
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  /* No separate bubble button: only the visible run-end avatar offers the
   * action, and creating its menu must not fetch the picture. */
  g_assert_null(row_child(row_for(f->view, first), "picture_button"));
  GhMessageRow *avatar_row = row_for(f->view, second);
  gtk_widget_activate_action(GTK_WIDGET(avatar_row), "message.avatar-menu", NULL);
  GtkWidget *button = row_child(avatar_row, "picture_menu_button");
  g_assert_true(shown(button));
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 0);
  AdwAlertDialog *dialog = view_child(f->view, "preview_dialog");
  click(button);
  spin_until(dialog_presented, dialog);
  g_signal_emit_by_name(dialog, "response", "preview-show");
  close_dialog(ADW_DIALOG(dialog));
  WebWait loaded = { f, first, GH_WEB_PICTURE };
  spin_until(web_loaded, &loaded);
  WebWait other = { f, second, GH_WEB_PICTURE };
  spin_until(web_loaded, &other);
  g_assert_cmpuint(blossom_fixture_count(server, "GET"), ==, 1); /* shared, not refetched */
  g_assert_null(row_child(row_for(f->view, first), "picture_button"));
  g_assert_null(row_child(row_for(f->view, second), "picture_button"));
  gtk_widget_activate_action(GTK_WIDGET(avatar_row), "message.avatar-menu", NULL);
  g_assert_false(shown(row_child(avatar_row, "avatar_menu")));
  WebWait own = { f, mine, GH_WEB_PICTURE };
  g_assert_false(web_loaded(&own)); /* another sender: nothing loaded for it */
  gh_conversation_view_set_conversation(f->view, NULL);
  gh_conversation_view_enable_web_content(f->view, NULL, NULL);
  g_settings_reset(f->settings, "load-profile-pictures");
  blossom_fixture_free(server);
}

typedef struct { Fixture *f; GhMessage *message; } PictureFailure;
static gboolean
picture_failed(gpointer data)
{
  PictureFailure *wait = data;
  GhLinkPreviewState state = GH_LINK_PREVIEW_NONE;
  gh_conversation_view_get_web_texture(wait->f->view, wait->message, GH_WEB_PICTURE, &state);
  return state == GH_LINK_PREVIEW_FAILED;
}

static void
test_failed_picture_has_no_row_error(Fixture *f, gconstpointer data)
{
  (void)data;
  g_settings_set_string(f->settings, "network-mode", "none");
  BlossomFixture *server = blossom_fixture_new();
  g_autofree gchar *uri = g_strdup_printf("https://127.0.0.1:%u/missing.png",
                                          blossom_fixture_port(server));
  GhMessage *first = add_dm(f->store, 2, 1, noon_today() - 60, "first");
  GhMessage *second = add_dm(f->store, 2, 1, noon_today() - 30, "second");
  GhConversation *conversation = room_of(f->store, first);
  gh_conversation_accept(conversation);
  show(f, conversation, 700, 700);
  g_autoptr(GhNetHttp) http = gh_net_http_new(f->settings);
  static const GhHttpTransport transport = { web_local_get, web_local_finish };
  gh_conversation_view_enable_web_content(f->view, &transport, http);
  g_autoptr(GObject) source = g_object_new(test_picture_source_get_type(), NULL);
  g_object_set_data_full(source, "picture", g_strdup(uri), g_free);
  gh_conversation_view_set_picture_source(f->view, web_picture_uri, source);
  GhMessageRow *avatar_row = row_for(f->view, second);
  gtk_widget_activate_action(GTK_WIDGET(avatar_row), "message.avatar-menu", NULL);
  GtkWidget *button = row_child(avatar_row, "picture_menu_button");
  g_assert_true(shown(button));
  click(button);
  AdwAlertDialog *dialog = view_child(f->view, "preview_dialog");
  spin_until(dialog_presented, dialog);
  g_signal_emit_by_name(dialog, "response", "preview-show");
  close_dialog(ADW_DIALOG(dialog));
  PictureFailure failed = { f, second };
  spin_until(picture_failed, &failed);
  drain_idle();
  g_assert_false(shown(row_child(row_for(f->view, first), "web_error")));
  g_assert_false(shown(row_child(row_for(f->view, second), "web_error")));
  gh_conversation_view_set_conversation(f->view, NULL);
  gh_conversation_view_enable_web_content(f->view, NULL, NULL);
  blossom_fixture_free(server);
}

static GtkAdjustment *vadjustment(GhConversationView *view);

/* Opt-in diagnostic: mapped GhWindow, real sidebar selection, exact-size
 * A/B rooms. The endpoint is two stable post-paint observations, not a
 * compositor presentation acknowledgement. No fixture creation is timed. */
typedef struct {
  GhConversationView *view;
  GhConversationOpenProbe *probe;
  gboolean settled;
  gboolean expired;
  gboolean had_stable;
  gdouble value, upper, page;
  guint visible;
  guint64 identities;
} OpenFrameWait;

static gboolean
open_frame_stable(OpenFrameWait *wait)
{
  GtkWidget *scroller = view_child(wait->view, "scroller");
  GtkWidget *list = GTK_WIDGET(gh_conversation_view_get_message_list(wait->view));
  GtkAdjustment *adj = vadjustment(wait->view);
  if (gh_conversation_open_probe_scroll_pending(wait->view) ||
      gtk_widget_get_width(scroller) <= 0 || gtk_widget_get_height(scroller) <= 0 ||
      gtk_adjustment_get_page_size(adj) <= 0) {
    wait->had_stable = FALSE;
    return FALSE;
  }
  gdouble value = gtk_adjustment_get_value(adj);
  gdouble upper = gtk_adjustment_get_upper(adj);
  gdouble page = gtk_adjustment_get_page_size(adj);
  if (upper - page - value > 4.0) {
    wait->had_stable = FALSE;
    return FALSE;
  }
  guint visible = 0;
  guint64 identities = 0;
  for (GtkWidget *item = gtk_widget_get_first_child(list); item;
       item = gtk_widget_get_next_sibling(item)) {
    if (!gtk_widget_get_mapped(item))
      continue;
    graphene_rect_t bounds;
    if (!gtk_widget_compute_bounds(item, scroller, &bounds) ||
        bounds.origin.y + bounds.size.height <= 0 ||
        bounds.origin.y >= gtk_widget_get_height(scroller))
      continue;
    GtkWidget *child = gtk_widget_get_first_child(item);
    if (!GH_IS_TIMELINE_ROW(child)) {
      if (g_strcmp0(gtk_widget_get_css_name(item), "header") == 0)
        continue;
      wait->had_stable = FALSE;
      return FALSE;
    }
    GhTimelineItem *timeline_item = gh_timeline_row_get_item(GH_TIMELINE_ROW(child));
    if (!timeline_item) {
      wait->had_stable = FALSE;
      return FALSE;
    }
    GhMessage *message = gh_timeline_item_get_message(timeline_item);
    if (!message)
      continue;
    GhMessageRow *row = gh_timeline_row_get_message_row(GH_TIMELINE_ROW(child));
    if (gh_message_row_get_message(row) != message) {
      wait->had_stable = FALSE;
      return FALSE;
    }
    visible++;
    identities = identities * 1315423911u + (guintptr)message;
  }
  if (!visible) {
    wait->had_stable = FALSE;
    return FALSE;
  }
  gboolean stable = wait->had_stable && visible == wait->visible &&
    identities == wait->identities && ABS(value - wait->value) <= 1.0 &&
    ABS(upper - wait->upper) <= 1.0 && ABS(page - wait->page) <= 1.0;
  wait->value = value;
  wait->upper = upper;
  wait->page = page;
  wait->visible = visible;
  wait->identities = identities;
  wait->had_stable = TRUE;
  return stable;
}

static void
on_open_frame(GdkFrameClock *clock, OpenFrameWait *wait)
{
  (void)clock;
  if (!wait->probe->entry_us)
    return;
  gint64 now = g_get_monotonic_time();
  if (!wait->probe->first_paint_us)
    wait->probe->first_paint_us = now;
  if (!wait->probe->first_allocation_us &&
      gtk_adjustment_get_page_size(vadjustment(wait->view)) > 0)
    wait->probe->first_allocation_us = now;
  if (open_frame_stable(wait)) {
    wait->probe->settled_us = now;
    wait->settled = TRUE;
  } else {
    gtk_widget_queue_draw(GTK_WIDGET(wait->view));
  }
}

static gboolean
open_frame_timeout(gpointer data)
{
  ((OpenFrameWait *)data)->expired = TRUE;
  return G_SOURCE_REMOVE;
}

static void
wait_open_settled(GhConversationView *view, GhConversationOpenProbe *probe)
{
  OpenFrameWait wait = { .view = view, .probe = probe };
  GdkFrameClock *clock = gtk_widget_get_frame_clock(GTK_WIDGET(view));
  g_assert_nonnull(clock);
  gulong handler = g_signal_connect(clock, "after-paint", G_CALLBACK(on_open_frame), &wait);
  guint timeout = g_timeout_add_seconds(10, open_frame_timeout, &wait);
  gtk_widget_queue_draw(GTK_WIDGET(view));
  while (!wait.settled && !wait.expired)
    g_main_context_iteration(NULL, TRUE);
  g_signal_handler_disconnect(clock, handler);
  if (!wait.expired)
    g_source_remove(timeout);
  if (wait.expired) {
    GtkAdjustment *adj = vadjustment(view);
    g_error("open generation %" G_GUINT64_FORMAT " did not settle: pending=%d "
            "value=%.2f upper=%.2f page=%.2f visible=%u binds=%u",
            probe->generation, gh_conversation_open_probe_scroll_pending(view),
            gtk_adjustment_get_value(adj), gtk_adjustment_get_upper(adj),
            gtk_adjustment_get_page_size(adj), wait.visible, probe->bind_count);
  }
}

static guint
open_sample_count(const char *name, guint fallback)
{
  const char *value = g_getenv(name);
  if (!value || !*value)
    return fallback;
  char *end = NULL;
  unsigned long result = strtoul(value, &end, 10);
  g_assert_true(end != value && !*end && result > 0 && result <= 1000);
  return (guint)result;
}

static double
open_ms(gint64 end, gint64 start)
{
  return end && start && end >= start ? (end - start) / 1000.0 : -1.0;
}

static void
print_open_sample(FILE *csv, const char *session, guint size, const char *phase,
                  guint sample, gboolean warmup, gboolean active,
                  const GhConversationOpenProbe *p)
{
  g_assert_cmpint(p->settled_us, >, p->entry_us);
  g_autofree char *line = g_strdup_printf(
    "%s,%u,%s,%u,%u,%" G_GINT64_FORMAT ",%" G_GINT64_FORMAT
    ",%u,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%.3f,%u,%u,%u,%u,%u",
    session, size, phase, sample, warmup, p->entry_us, p->settled_us,
    active, p->trace, open_ms(p->settled_us, p->entry_us),
    open_ms(p->cleanup_end_us, p->entry_us),
    open_ms(p->timeline_end_us, p->timeline_start_us),
    open_ms(p->attach_end_us, p->timeline_end_us),
    open_ms(p->return_us, p->entry_us),
    open_ms(p->first_bind_us, p->entry_us),
    open_ms(p->last_bind_us, p->entry_us),
    open_ms(p->first_allocation_us, p->entry_us),
    open_ms(p->first_paint_us, p->entry_us),
    open_ms(p->settled_us, p->last_scroll_us),
    p->bind_count, p->bind_wall_us / 1000.0, p->bind_wall_max_us / 1000.0,
    p->adjustment_changes, p->older_requests, p->timeline_items,
    p->open_scroll_count, p->pin_count);
  g_autofree char *extended = g_strdup_printf(
    "%s,%u,%.3f,%u,%.3f,%u,%u,%u,%u,%u,%u,%u", line,
    p->row_construct_count, p->row_construct_wall_us / 1000.0,
    p->row_dispose_count, p->row_dispose_wall_us / 1000.0,
    p->plain_row_widgets, p->optional_reactions, p->optional_attachments,
    p->optional_polls, p->optional_replies, p->optional_links, p->optional_audio);
  g_print("OPEN_CSV,%s\n", extended);
  if (csv) {
    fprintf(csv, "%s\n", extended);
    fflush(csv);
  }
}

static void
test_open_timing(Fixture *f, gconstpointer data)
{
  (void)f;
  (void)data;
  if (!g_getenv("GROUNDHOG_TEST_TIMING")) {
    g_test_skip("GROUNDHOG_TEST_TIMING unset");
    return;
  }
  guint pairs = open_sample_count("GROUNDHOG_TEST_PAIRS", 30);
  guint warmups = open_sample_count("GROUNDHOG_TEST_WARMUPS", 5);
  gboolean trace = g_strcmp0(g_getenv("GROUNDHOG_TEST_TRACE"), "0") != 0;
  const char *session = g_getenv("GROUNDHOG_TEST_SESSION");
  if (!session || !*session)
    session = "session-1";
  const char *path = g_getenv("GROUNDHOG_TEST_TIMING_CSV");
  FILE *csv = path ? fopen(path, "w") : NULL;
  if (path)
    g_assert_nonnull(csv);
  const char *header = "session,size,phase,sample,warmup,start_us,settled_us,window_active,trace,settled_ms,cleanup_ms,timeline_ms,attach_ms,set_return_ms,first_bind_ms,last_bind_ms,first_allocation_ms,first_paint_ms,scroll_to_settle_ms,bind_count,bind_wall_ms,bind_wall_max_ms,adjustment_changes,older_requests,timeline_items,open_scroll_count,pin_count,row_construct_count,row_construct_wall_ms,row_dispose_count,row_dispose_wall_ms,plain_row_widgets,optional_reactions,optional_attachments,optional_polls,optional_replies,optional_links,optional_audio";
  g_print("OPEN_CSV_HEADER,%s\n", header);
  if (csv)
    fprintf(csv, "%s\n", header);
  const guint orders[][3] = {{50, 200, 1000}, {200, 1000, 50}, {1000, 50, 200}};
  const char *order_env = g_getenv("GROUNDHOG_TEST_ORDER");
  g_assert_true(!order_env || g_str_equal(order_env, "0") ||
                g_str_equal(order_env, "1") || g_str_equal(order_env, "2"));
  guint order = order_env ? (guint)atoi(order_env) : 0;
  for (guint k = 0; k < G_N_ELEMENTS(orders[0]); k++) {
    guint size = orders[order][k];
    g_autoptr(GhConversationStore) store = gh_conversation_store_new();
    gh_conversation_store_set_account(store, hex[1], NULL, NULL, NULL);
    GhMessage *a_first = NULL, *b_first = NULL;
    gint64 base = noon_today() - 200000;
    for (guint i = 0; i < size; i++) {
      g_autoptr(GString) long_body = NULL;
      if (size == 1000 && i % 50 == 0) {
        long_body = g_string_sized_new(4300);
        while (long_body->len <= 4300)
          g_string_append(long_body, " a long message with ordinary words and punctuation.");
      }
      g_autofree char *a_text = g_strdup_printf("A message %u, **ordinary text** with a few words to wrap%s%s", i,
                                               i % 20 == 0 ? " https://example.com/a" : "",
                                               long_body ? long_body->str : "");
      g_autofree char *b_text = g_strdup_printf("B message %u, _ordinary text_ with a few words to wrap %s%s", i,
                                               i % 20 == 0 ? nprofile : "",
                                               long_body ? long_body->str : "");
      GhMessage *a = add_dm(store, i % 2 ? 2 : 1, i % 2 ? 1 : 2,
                            base + 10000 + i * 60, a_text);
      GhMessage *b = add_dm(store, i % 2 ? 3 : 1, i % 2 ? 1 : 3,
                            base + i * 60, b_text);
      if (!a_first) a_first = a;
      if (!b_first) b_first = b;
    }
    GhConversation *a = room_of(store, a_first);
    GhConversation *b = room_of(store, b_first);
    g_assert_true(a != b);
    gh_conversation_accept(a);
    gh_conversation_accept(b);
    gh_conversation_mark_read(a);
    gh_conversation_mark_read(b);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(a)), ==, size);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(b)), ==, size);
    /* The store-backed opener presents only its newest window, not all
     * seeded messages. The in-memory fixture enables that policy explicitly. */
    gh_conversation_window_enable(a);
    gh_conversation_window_enable(b);
    for (guint sample = 0; sample < warmups + pairs; sample++) {
      gboolean warmup = sample < warmups;
      GhWindow *window = gh_window_new(NULL);
      GhSidebarPage *sidebar = gh_window_get_sidebar(window);
      gh_conversation_list_attach(window, store, NULL);
      GhConversationView *view = GH_CONVERSATION_VIEW(gh_content_page_get_view(gh_window_get_content(window)));
      g_assert_nonnull(view);
      GhStatus *status = gh_window_get_status(window);
      gh_status_set_account_active(status, TRUE);
      gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
      gh_status_set_inbox(status, GH_STATUS_INBOX_LIVE, NULL);
      gtk_window_set_default_size(GTK_WINDOW(window), 900, 700);
      gtk_window_present(GTK_WINDOW(window));
      while (!gtk_widget_get_mapped(GTK_WIDGET(window)))
        g_main_context_iteration(NULL, TRUE);
      gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
      drain_idle();
      GhConversationOpenProbe *probe = gh_conversation_open_probe_arm(view, trace);
      g_assert_true(gh_sidebar_page_select_relative(sidebar, 1));
      g_assert_true(gh_sidebar_page_get_selected(sidebar) == a);
      wait_open_settled(view, probe);
      GhConversationOpenProbe first = *probe;
      if (trace && size == 1000)
        g_assert_cmpuint(first.bind_count, <, 300);
      print_open_sample(csv, session, size, "first", sample, warmup,
                        gtk_window_is_active(GTK_WINDOW(window)), &first);
      probe = gh_conversation_open_probe_arm(view, FALSE);
      g_assert_true(gh_sidebar_page_select_relative(sidebar, 1));
      g_assert_true(gh_sidebar_page_get_selected(sidebar) == b);
      wait_open_settled(view, probe);
      probe = gh_conversation_open_probe_arm(view, trace);
      g_assert_true(gh_sidebar_page_select_relative(sidebar, -1));
      g_assert_true(gh_sidebar_page_get_selected(sidebar) == a);
      wait_open_settled(view, probe);
      GhConversationOpenProbe reopen = *probe;
      if (trace && size == 1000)
        g_assert_cmpuint(reopen.bind_count, <, 300);
      print_open_sample(csv, session, size, "reopen", sample, warmup,
                        gtk_window_is_active(GTK_WINDOW(window)), &reopen);
      gh_conversation_open_probe_disarm(view);
      gtk_window_destroy(GTK_WINDOW(window));
      drain_idle();
    }
  }
  if (csv)
    fclose(csv);
}

/* ---- expiry ---------------------------------------------------------------------------- */

static gboolean
timeline_has_two(gpointer data)
{
  return timeline_length(GH_CONVERSATION_VIEW(data)) == 2;
}

/* Expiry has one source of truth (charter §3.7, G07): the conversation. The
 * store never lists or admits an expired message and GhExpiry (tested in
 * groundhog-expiry) takes one out of the model when it expires; the view
 * shows exactly what the conversation holds and keeps no timer of its own. */
static void
test_expiry(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 now = now_seconds();
  GhMessage *first = add_dm(f->store, 2, 1, now - 100, "stays");
  GhMessage *later = add_dm(f->store, 2, 1, now - 50, "disappears in an hour");
  GhMessage *soon = add_dm(f->store, 2, 1, now - 10, "disappears in a moment");
  gh_message_set_expires_at(later, now + 3600);
  gh_message_set_expires_at(soon, now + 60);
  GhConversation *conversation = room_of(f->store, soon);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 600, 500);

  /* Disappearing messages carry the timer icon and say so. */
  g_assert_cmpuint(timeline_length(f->view), ==, 3);
  GhMessageRow *row = row_for(f->view, later);
  g_assert_true(shown(row_child(row, "timer_icon")));
  g_assert_true(shown(row_child(row, "meta_box")));
  g_assert_nonnull(strstr(gh_message_row_get_summary(row), "Disappearing message."));
  g_assert_true(shown(row_child(row_for(f->view, soon), "timer_icon")));
  g_assert_false(shown(row_child(row_for(f->view, first), "timer_icon")));

  /* When it expires the purge takes it out of the conversation (as
   * gh_store_conversations_purge() does), and the view follows at once. */
  g_autofree gchar *soon_id = g_strdup(gh_message_get_rumor_id(soon));
  g_assert_true(gh_conversation_store_remove_message(f->store, soon_id));
  spin_until(timeline_has_two, f->view);
  g_assert_true(gh_timeline_item_get_message(timeline_item(f->view, 0)) == first);
  g_assert_true(gh_timeline_item_get_message(timeline_item(f->view, 1)) == later);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 2);
  g_assert_cmpstr(gh_conversation_get_preview(conversation), ==, "disappears in an hour");
}

/* ---- scrolling ---------------------------------------------------------------------------- */

static GtkAdjustment *
vadjustment(GhConversationView *view)
{
  return gtk_scrolled_window_get_vadjustment(view_child(view, "scroller"));
}

static gdouble
to_bottom(GhConversationView *view)
{
  GtkAdjustment *adj = vadjustment(view);
  return gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj) -
         gtk_adjustment_get_value(adj);
}

static gboolean
at_bottom(gpointer data)
{
  GhConversationView *view = data;
  GtkAdjustment *adj = vadjustment(view);
  return gtk_adjustment_get_upper(adj) > gtk_adjustment_get_page_size(adj) &&
         to_bottom(view) <= 4 && gh_conversation_view_get_at_latest(view);
}

static gboolean
scrolled_up(gpointer data)
{
  GhConversationView *view = data;
  return !gh_conversation_view_get_at_latest(view) &&
         shown(view_child(view, "jump_button"));
}

static void
fill(GhConversationStore *store, gint64 from, guint count)
{
  for (guint i = 0; i < count; i++) {
    g_autofree gchar *text = g_strdup_printf("message %u", i);
    if (i % 2)
      add_dm(store, 1, 2, from + i * 600, text);
    else
      add_dm(store, 2, 1, from + i * 600, text);
  }
}

static void
test_scrolling(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 start = noon_today() - 40 * 600;
  fill(f->store, start, 40);
  GhMessage *first = add_dm(f->store, 2, 1, start - 600, "first");
  GhConversation *conversation = room_of(f->store, first);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 480, 360);

  /* Opens at the newest message and stays there. */
  spin_until(at_bottom, f->view);
  GtkWidget *jump = view_child(f->view, "jump_button");
  g_assert_false(shown(jump));

  /* Scrolled up: new messages below are counted on "Jump to Latest". */
  gtk_adjustment_set_value(vadjustment(f->view), 0);
  spin_until(scrolled_up, f->view);
  guint polite = gh_conversation_view_get_announcements(
    f->view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  GhTestActiveSpan span;
  gh_test_active_span_begin(&span, f->window);
  GhMessage *news = add_dm(f->store, 2, 1, noon_today() + 60, "something new");
  drain_idle();
  gboolean either = FALSE;
  guint step = gh_test_active_span_expect(&span, 1, &either);
  g_assert_false(gh_conversation_view_get_at_latest(f->view));
  g_assert_cmpuint(gh_conversation_view_get_new_below(f->view), ==, 1);
  GtkWidget *count = view_child(f->view, "jump_count");
  g_assert_true(shown(count));
  g_assert_cmpstr(text_of(count), ==, "1");
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(jump), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      "Jump to Latest, 1 new message");
  /* Announced politely, in full, if the window was active (nostrc-9g6e:
   * another test's window may have taken activation meanwhile). */
  guint made = gh_conversation_view_get_announcements(
    f->view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM) - polite;
  if (either)
    g_assert_cmpuint(made, <=, 1);
  else
    g_assert_cmpuint(made, ==, step);
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree gchar *announced = gh_message_row_compose_summary(news, now);
  if (made)
    g_assert_cmpstr(gh_conversation_view_get_last_announcement(f->view), ==, announced);

  /* Jump to Latest brings the newest into view and clears the count. */
  click(jump);
  spin_until(at_bottom, f->view);
  g_assert_cmpuint(gh_conversation_view_get_new_below(f->view), ==, 0);
  g_assert_false(shown(jump));

  /* At the bottom, new messages keep it there. */
  add_dm(f->store, 2, 1, noon_today() + 120, "and another");
  drain_idle();
  spin_until(at_bottom, f->view);
  g_assert_cmpuint(gh_conversation_view_get_new_below(f->view), ==, 0);

  /* Your own message (a local echo) always brings you to it. */
  gtk_adjustment_set_value(vadjustment(f->view), 0);
  spin_until(scrolled_up, f->view);
  add_dm(f->store, 1, 2, noon_today() + 180, "my reply");
  spin_until(at_bottom, f->view);
}

static gboolean
unread_below(gpointer data)
{
  GhConversationView *view = data;
  return shown(view_child(view, "jump_button")) &&
         gh_conversation_view_get_new_below(view) == 6 &&
         gtk_adjustment_get_upper(vadjustment(view)) > 0;
}

static void
test_opens_at_first_unread(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 start = noon_today() - 50 * 600;
  fill(f->store, start, 30);
  GhMessage *read = add_dm(f->store, 1, 2, start + 30 * 600, "read up to here");
  GhConversation *conversation = room_of(f->store, read);
  gh_conversation_mark_read(conversation);
  GhMessage *first_unread = NULL;
  for (guint i = 0; i < 6; i++) {
    g_autofree gchar *text = g_strdup_printf("unread %u", i);
    GhMessage *m = add_dm(f->store, 2, 1, start + (31 + i) * 600, text);
    if (!first_unread)
      first_unread = m;
  }
  g_assert_cmpuint(gh_conversation_get_unread_count(conversation), ==, 6);
  gtk_window_set_default_size(f->window, 480, 300);
  gtk_window_present(f->window);
  spin_until(is_mapped, f);
  gh_conversation_view_set_conversation(f->view, conversation);

  /* The first unread message is in view, the newer ones counted below. */
  spin_until(unread_below, f->view);
  drain_idle();
  GtkWidget *item = item_for(f->view, first_unread);
  g_assert_nonnull(item);
  graphene_point_t point;
  g_assert_true(gtk_widget_compute_point(item, view_child(f->view, "scroller"),
                                         &GRAPHENE_POINT_INIT(0, 0), &point));
  g_assert_cmpfloat(point.y, >=, -1);
  g_assert_cmpfloat(point.y, <, gtk_adjustment_get_page_size(vadjustment(f->view)));
  g_assert_false(gh_conversation_view_get_at_latest(f->view));
}

/* ---- older history ---------------------------------------------------------------------- */

static void
load_older(GhConversationView *view, GhConversation *conversation, gpointer data)
{
  Fixture *f = data;
  g_assert_true(gh_conversation_view_get_conversation(view) == conversation);
  f->loads++;
}

static gboolean
loading(gpointer data)
{
  Fixture *f = data;
  return f->loads == 1 && gh_conversation_view_get_loading_older(f->view);
}

static void
test_load_older(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 t = noon_today();
  const guint to_a[] = { 1, 0 }, to_b[] = { 2, 0 };
  g_autoptr(GPtrArray) older = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GPtrArray) newest = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < 3; i++) {
    g_autofree gchar *old_text = g_strdup_printf("older %u", i);
    g_autofree gchar *new_text = g_strdup_printf("newest %u", i);
    g_ptr_array_add(older, rumor(i % 2 ? 1 : 2, i % 2 ? to_b : to_a, t - 3600 + i * 60,
                                 old_text, NULL));
    g_ptr_array_add(newest, rumor(i % 2 ? 1 : 2, i % 2 ? to_b : to_a, t + i * 60, new_text,
                                  NULL));
  }
  GhMessage *floor = g_ptr_array_index(newest, 0);
  GhMessage *last = g_ptr_array_index(newest, 2);
  GhConversationState state = {
    .accepted = TRUE,
    .has_marker = TRUE,
    .marker_created_at = gh_message_get_created_at(last),
    .marker_id = gh_message_get_rumor_id(last),
    .has_older = TRUE,
    .floor_created_at = gh_message_get_created_at(floor),
    .floor_id = gh_message_get_rumor_id(floor),
  };
  const gchar *room_id = gh_message_get_room_id(floor);
  GhConversation *conversation = gh_conversation_store_restore(f->store, room_id, newest, &state);
  g_assert_nonnull(conversation);
  g_assert_true(gh_conversation_get_has_older(conversation));
  gh_conversation_view_set_history_loader(f->view, load_older, f, NULL);

  /* Near the top of a conversation with older history: one request, shown
   * as "Loading earlier messages" until the loader finishes. */
  show(f, conversation, 600, 600);
  spin_until(loading, f);
  g_assert_true(shown(view_child(f->view, "loading_box")));
  gtk_adjustment_set_value(vadjustment(f->view), 0);
  drain_idle();
  g_assert_cmpuint(f->loads, ==, 1);

  state.has_older = FALSE;
  g_assert_true(gh_conversation_store_restore(f->store, room_id, older, &state) == conversation);
  gh_conversation_view_finish_loading_older(f->view);
  g_assert_false(gh_conversation_view_get_loading_older(f->view));
  g_assert_false(shown(view_child(f->view, "loading_box")));
  g_assert_cmpuint(timeline_length(f->view), ==, 6);
  g_assert_true(gh_timeline_item_get_message(timeline_item(f->view, 0)) ==
                g_ptr_array_index(older, 0));
  drain_idle();
  gtk_adjustment_set_value(vadjustment(f->view), 0);
  drain_idle();
  /* Nothing older remains: no more requests. */
  g_assert_cmpuint(f->loads, ==, 1);
}

/* Frames drawn since a count was started: layout has run in between. */
static gboolean
count_tick(GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  (void)widget;
  (void)clock;
  (*(guint *)data)++;
  return G_SOURCE_CONTINUE;
}

static gboolean
ticks_reached(gpointer data)
{
  return *(guint *)data >= 6;
}

static void
wait_frames(GtkWidget *widget)
{
  guint ticks = 0;
  guint id = gtk_widget_add_tick_callback(widget, count_tick, &ticks, NULL);
  spin_until(ticks_reached, &ticks);
  gtk_widget_remove_tick_callback(widget, id);
}

/* The topmost row of `messages` wholly on screen, and where it is. */
static GhMessage *
topmost_on_screen(GhConversationView *view, GPtrArray *messages, gdouble *y)
{
  GtkWidget *scroller = view_child(view, "scroller");
  for (guint i = 0; i < messages->len; i++) {
    GhMessage *message = g_ptr_array_index(messages, i);
    GtkWidget *item = item_for(view, message);
    GhMessageRow *row = item ? item_row(item) : NULL;
    graphene_rect_t bounds;
    if (row && gtk_widget_get_mapped(GTK_WIDGET(row)) &&
        gtk_widget_compute_bounds(GTK_WIDGET(row), scroller, &bounds) &&
        bounds.origin.y >= 0) {
      *y = bounds.origin.y;
      return message;
    }
  }
  return NULL;
}

static void
assert_window_bound(GListModel *model, guint position, guint removed, guint added,
                    gpointer data)
{
  (void)position;
  (void)removed;
  (void)added;
  (void)data;
  g_assert_cmpuint(g_list_model_get_n_items(model), <=, GH_CONVERSATION_WINDOW_MAX);
}

/* Traverse 1,000 stored messages in both directions without retaining the
 * whole history. The fixture array stands in for the encrypted store; the
 * view sees only the GhConversation window. */
static void
test_window_long_scroll(Fixture *f, gconstpointer data)
{
  (void)data;
  const guint to_a[] = { 1, 0 };
  gint64 base = noon_today() - 100000;
  g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GPtrArray) page = g_ptr_array_new_with_free_func(g_object_unref);
  gboolean seen_older[1000] = { FALSE };
  gboolean seen_newer[1000] = { FALSE };
  for (guint i = 0; i < 1000; i++) {
    g_autofree gchar *body = g_strdup_printf("stored window message %u", i);
    g_ptr_array_add(all, rumor(2, to_a, base + i * 60, body, NULL));
  }
  for (guint i = 970; i < 1000; i++) {
    g_ptr_array_add(page, g_object_ref(g_ptr_array_index(all, i)));
    seen_older[i] = TRUE;
  }
  GhMessage *floor = g_ptr_array_index(all, 970);
  const gchar *room_id = gh_message_get_room_id(floor);
  GhConversationState state = {
    .accepted = TRUE,
    .has_older = TRUE,
    .floor_created_at = gh_message_get_created_at(floor),
    .floor_id = gh_message_get_rumor_id(floor),
  };
  GhConversation *conversation = gh_conversation_store_restore(f->store, room_id, page, &state);
  gh_conversation_window_enable(conversation);
  g_signal_connect(conversation, "items-changed", G_CALLBACK(assert_window_bound), NULL);
  show(f, conversation, 480, 300);
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==,
                   GH_CONVERSATION_WINDOW_OPEN);
  GtkAdjustment *adj = vadjustment(f->view);
  gtk_adjustment_set_value(adj, gtk_adjustment_get_page_size(adj) / 4);
  wait_frames(GTK_WIDGET(f->view));
  gdouble before_y = 0;
  GhMessage *anchor = topmost_on_screen(f->view, page, &before_y);
  g_assert_nonnull(anchor);
  for (guint end = 970; end > 0;) {
    guint start = end > GH_CONVERSATION_WINDOW_PAGE ? end - GH_CONVERSATION_WINDOW_PAGE : 0;
    g_ptr_array_set_size(page, 0);
    for (guint i = start; i < end; i++) {
      g_assert_false(seen_older[i]);
      seen_older[i] = TRUE;
      g_ptr_array_add(page, g_object_ref(g_ptr_array_index(all, i)));
    }
    floor = g_ptr_array_index(all, start);
    state.has_older = start > 0;
    state.floor_created_at = gh_message_get_created_at(floor);
    state.floor_id = gh_message_get_rumor_id(floor);
    g_assert_true(gh_conversation_store_restore(f->store, room_id, page, &state) == conversation);
    g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), <=,
                     GH_CONVERSATION_WINDOW_MAX);
    if (end == 970) {
      wait_frames(GTK_WIDGET(f->view));
      GhMessageRow *row = row_for(f->view, anchor);
      graphene_rect_t bounds;
      g_assert_nonnull(row);
      g_assert_true(gtk_widget_compute_bounds(GTK_WIDGET(row),
        view_child(f->view, "scroller"), &bounds));
      g_assert_cmpfloat(fabs(bounds.origin.y - before_y), <=, 1);
    }
    end = start;
  }
  for (guint i = 0; i < 1000; i++)
    g_assert_true(seen_older[i]);
  g_assert_true(gh_conversation_get_has_newer(conversation));
  /* The oldest window is contiguous. Every adjacent newer page advances
   * exactly once, including across each far-end trim. */
  guint cursor = 0;
  for (guint i = 0; i < g_list_model_get_n_items(G_LIST_MODEL(conversation)); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(G_LIST_MODEL(conversation), i);
    guint index = (guint)((gh_message_get_created_at(message) - base) / 60);
    seen_newer[index] = TRUE;
    cursor = index;
  }
  gboolean checked_append_anchor = FALSE;
  while (cursor < 999) {
    guint start = cursor + 1;
    guint end = MIN(start + GH_CONVERSATION_WINDOW_PAGE, 1000);
    g_ptr_array_set_size(page, 0);
    for (guint i = start; i < end; i++) {
      g_assert_false(seen_newer[i]);
      seen_newer[i] = TRUE;
      g_ptr_array_add(page, g_object_ref(g_ptr_array_index(all, i)));
    }
    g_autoptr(GPtrArray) visible_before = NULL;
    GhMessage *append_anchor = NULL;
    gdouble append_y = 0;
    if (!checked_append_anchor &&
        g_list_model_get_n_items(G_LIST_MODEL(conversation)) + page->len >
          GH_CONVERSATION_WINDOW_MAX) {
      visible_before = g_ptr_array_new_with_free_func(g_object_unref);
      guint count = g_list_model_get_n_items(G_LIST_MODEL(conversation));
      for (guint i = 0; i < count; i++)
        g_ptr_array_add(visible_before,
          g_list_model_get_item(G_LIST_MODEL(conversation), i));
      wait_frames(GTK_WIDGET(f->view));
      gdouble view_page = gtk_adjustment_get_page_size(adj);
      gtk_adjustment_set_value(adj,
        MAX(0, gtk_adjustment_get_upper(adj) - view_page * 1.1));
      wait_frames(GTK_WIDGET(f->view));
      append_anchor = topmost_on_screen(f->view, visible_before, &append_y);
      g_assert_nonnull(append_anchor);
    }
    gh_conversation_window_add_newer(conversation, page, end < 1000);
    if (append_anchor) {
      wait_frames(GTK_WIDGET(f->view));
      GhMessageRow *row = row_for(f->view, append_anchor);
      graphene_rect_t bounds;
      g_assert_nonnull(row);
      g_assert_true(gtk_widget_compute_bounds(GTK_WIDGET(row),
        view_child(f->view, "scroller"), &bounds));
      g_assert_cmpfloat(fabs(bounds.origin.y - append_y), <=, 1);
      checked_append_anchor = TRUE;
    }
    guint n = g_list_model_get_n_items(G_LIST_MODEL(conversation));
    g_assert_cmpuint(n, <=, GH_CONVERSATION_WINDOW_MAX);
    for (guint i = 1; i < n; i++) {
      g_autoptr(GhMessage) left = g_list_model_get_item(G_LIST_MODEL(conversation), i - 1);
      g_autoptr(GhMessage) right = g_list_model_get_item(G_LIST_MODEL(conversation), i);
      g_assert_cmpint(gh_message_compare(left, right), <, 0);
    }
    cursor = end - 1;
  }
  for (guint i = 0; i < 1000; i++)
    g_assert_true(seen_newer[i]);
  g_assert_false(gh_conversation_get_has_newer(conversation));
  g_assert_true(checked_append_anchor);
}

/* "Earlier Messages" shows only within a page of the top of what is listed
 * (nostrc-l1kn6.1), not over the latest messages; clicking it lists the
 * older page above while the messages on screen stay where they are. */
static void
test_earlier_button_near_top(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 t = noon_today();
  const guint to_a[] = { 1, 0 }, to_b[] = { 2, 0 };
  g_autoptr(GPtrArray) older = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GPtrArray) newest = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < 30; i++) {
    g_autofree gchar *text = g_strdup_printf("newest %u", i);
    g_ptr_array_add(newest, rumor(i % 2 ? 1 : 2, i % 2 ? to_b : to_a, t + i * 600, text, NULL));
  }
  for (guint i = 0; i < 10; i++) {
    g_autofree gchar *text = g_strdup_printf("older %u", i);
    g_ptr_array_add(older, rumor(2, to_a, t - 36000 + i * 60, text, NULL));
  }
  GhMessage *floor = g_ptr_array_index(newest, 0);
  const gchar *room_id = gh_message_get_room_id(floor);
  GhConversationState state = {
    .accepted = TRUE,
    .has_older = TRUE,
    .floor_created_at = gh_message_get_created_at(floor),
    .floor_id = gh_message_get_rumor_id(floor),
  };
  GhConversation *conversation = gh_conversation_store_restore(f->store, room_id, newest, &state);
  show(f, conversation, 480, 300);
  gh_conversation_view_scroll_to_latest(f->view);
  spin_until(at_bottom, f->view);
  wait_frames(GTK_WIDGET(f->view));
  gh_conversation_view_set_history_loader(f->view, load_older, f, NULL);
  GtkWidget *button = view_child(f->view, "older_button");
  GtkAdjustment *adj = vadjustment(f->view);
  gdouble page = gtk_adjustment_get_page_size(adj);
  g_assert_cmpfloat(gtk_adjustment_get_upper(adj), >, 3 * page);

  /* At the latest messages: nothing about earlier ones covers them. */
  g_assert_false(shown(button));

  /* Within a page of the top: offered, not yet asked for. */
  gtk_adjustment_set_value(adj, page * 3 / 4);
  wait_frames(GTK_WIDGET(f->view));
  g_assert_true(shown(button));
  g_assert_cmpuint(f->loads, ==, 0);
  /* Clear of the scrollbar on the end edge, at the top. */
  g_assert_cmpint(gtk_widget_get_valign(button), ==, GTK_ALIGN_START);
  g_assert_cmpint(gtk_widget_get_halign(button), ==, GTK_ALIGN_CENTER);
  g_assert_cmpint(gtk_widget_get_margin_end(button), >=, 18);

  /* Further down again: gone. */
  gtk_adjustment_set_value(adj, page * 2);
  wait_frames(GTK_WIDGET(f->view));
  g_assert_false(shown(button));
  gtk_adjustment_set_value(adj, page / 2);
  wait_frames(GTK_WIDGET(f->view));
  g_assert_true(shown(button));

  /* Clicked: the older page is listed above; the reader's place holds. */
  gdouble before_y = 0;
  GhMessage *anchor = topmost_on_screen(f->view, newest, &before_y);
  g_assert_nonnull(anchor);
  gdouble before_value = gtk_adjustment_get_value(adj);
  click(button);
  g_assert_cmpuint(f->loads, ==, 1);
  g_assert_false(shown(button));
  g_assert_true(shown(view_child(f->view, "loading_box")));
  state.has_older = FALSE;
  g_assert_true(gh_conversation_store_restore(f->store, room_id, older, &state) == conversation);
  gh_conversation_view_finish_loading_older(f->view);
  g_assert_false(shown(view_child(f->view, "loading_box")));
  wait_frames(GTK_WIDGET(f->view));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(conversation)), ==, 40);
  GhMessageRow *row = row_for(f->view, anchor);
  g_assert_nonnull(row);
  graphene_rect_t bounds;
  g_assert_true(gtk_widget_compute_bounds(GTK_WIDGET(row), view_child(f->view, "scroller"),
                                          &bounds));
  g_assert_cmpfloat(fabs(bounds.origin.y - before_y), <=, 1);
  g_assert_cmpfloat(gtk_adjustment_get_value(adj), >, before_value);
  g_assert_false(gh_conversation_view_get_at_latest(f->view));
  /* Nothing older remains: neither offered nor asked for at the top. */
  g_assert_false(shown(button));
  gtk_adjustment_set_value(adj, 0);
  wait_frames(GTK_WIDGET(f->view));
  g_assert_false(shown(button));
  g_assert_cmpuint(f->loads, ==, 1);
}

/* ---- earlier messages (W13b review B1) ----------------------------------------------- */

static void
fail_older(GhConversationView *view, GhConversation *conversation, gpointer data)
{
  Fixture *f = data;
  g_assert_true(gh_conversation_view_get_conversation(view) == conversation);
  f->loads++;
  gh_conversation_view_fail_loading_older(view);
}

static gboolean
older_failed(gpointer data)
{
  Fixture *f = data;
  return f->loads >= 1 && gh_conversation_view_get_older_failed(f->view);
}

static gboolean
loading_again(gpointer data)
{
  Fixture *f = data;
  return f->loads == 3 && gh_conversation_view_get_loading_older(f->view);
}

static const char *
older_label(Fixture *f)
{
  return adw_button_content_get_label(view_child(f->view, "older_content"));
}

/* Older history that is not listed is said, with how much of it is unread;
 * the button lists it as scrolling to the top does; a failure is said and
 * retried only from the button (scrolling would spin). */
static void
test_earlier_messages(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 t = noon_today();
  const guint to_a[] = { 1, 0 };
  g_autoptr(GPtrArray) older = g_ptr_array_new_with_free_func(g_object_unref);
  g_autoptr(GPtrArray) newest = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < 3; i++) {
    g_autofree gchar *old_text = g_strdup_printf("older %u", i);
    g_autofree gchar *new_text = g_strdup_printf("newest %u", i);
    g_ptr_array_add(older, rumor(2, to_a, t - 3600 + i * 60, old_text, NULL));
    g_ptr_array_add(newest, rumor(2, to_a, t + i * 60, new_text, NULL));
  }
  GhMessage *floor = g_ptr_array_index(newest, 0);
  /* Nothing read yet: the 3 listed messages and the 3 older ones. */
  GhConversationState state = {
    .accepted = TRUE,
    .unread = 6,
    .has_older = TRUE,
    .floor_created_at = gh_message_get_created_at(floor),
    .floor_id = gh_message_get_rumor_id(floor),
  };
  const gchar *room_id = gh_message_get_room_id(floor);
  GhConversation *conversation = gh_conversation_store_restore(f->store, room_id, newest, &state);
  g_assert_cmpuint(gh_conversation_get_unread_count(conversation), ==, 6);
  guint first = 99;
  g_assert_cmpuint(gh_conversation_get_listed_unread(conversation, &first), ==, 3);
  g_assert_cmpuint(first, ==, 0);
  GtkWidget *button = view_child(f->view, "older_button");

  /* Without a loader it is said, not offered. */
  gh_conversation_view_set_conversation(f->view, conversation);
  g_assert_true(shown(button));
  g_assert_false(gtk_widget_get_sensitive(button));
  g_assert_cmpstr(older_label(f), ==, "Earlier Messages Can't Be Shown");
  gh_conversation_view_set_conversation(f->view, NULL);
  g_assert_false(shown(button));

  /* A failing loader (no store): the view opens at the top, where the
   * unread older messages are, asks once and says it couldn't. */
  gh_conversation_view_set_history_loader(f->view, fail_older, f, NULL);
  show(f, conversation, 600, 600);
  spin_until(older_failed, f);
  g_assert_cmpuint(f->loads, ==, 1);
  g_assert_true(shown(button));
  g_assert_false(shown(view_child(f->view, "loading_box")));
  g_assert_cmpstr(older_label(f), ==, "Couldn't Load Earlier Messages");
  gtk_adjustment_set_value(vadjustment(f->view), 1);
  drain_idle();
  gtk_adjustment_set_value(vadjustment(f->view), 0);
  drain_idle();
  g_assert_cmpuint(f->loads, ==, 1);
  click(button);
  g_assert_cmpuint(f->loads, ==, 2);
  g_assert_true(gh_conversation_view_get_older_failed(f->view));

  /* A working loader: the button counts what is unread up there, and the
   * view asks for it as it opens at the top. */
  gh_conversation_view_set_history_loader(f->view, load_older, f, NULL);
  gh_conversation_view_set_conversation(f->view, NULL);
  gh_conversation_view_set_conversation(f->view, conversation);
  g_assert_false(gh_conversation_view_get_older_failed(f->view));
  g_assert_cmpstr(older_label(f), ==, "3 Unread Earlier Messages");
  spin_until(loading_again, f);
  g_assert_false(shown(button));
  g_assert_true(shown(view_child(f->view, "loading_box")));

  /* Listed: nothing older remains, so nothing is offered. */
  state.has_older = FALSE;
  g_assert_true(gh_conversation_store_restore(f->store, room_id, older, &state) == conversation);
  gh_conversation_view_finish_loading_older(f->view);
  g_assert_false(shown(button));
  g_assert_false(shown(view_child(f->view, "loading_box")));
  g_assert_cmpuint(gh_conversation_get_listed_unread(conversation, NULL), ==, 6);
}

/* ---- charter §7.15 states 11-13, compact width, keyboard --------------------------------- */

static void
test_states(Fixture *f, gconstpointer data)
{
  (void)data;
  GhMessage *m = add_dm(f->store, 2, 1, noon_today(), "see https://example.com/z");
  GhConversation *conversation = room_of(f->store, m);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 600, 500);

  /* 11: the recipient has no message inbox. */
  AdwBanner *banner = view_child(f->view, "banner");
  g_assert_false(adw_banner_get_revealed(banner));
  gh_conversation_view_set_recipient_without_inbox(f->view, "Alice");
  g_assert_true(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_banner_get_title(banner), ==, "Alice hasn't set up private messaging yet");
  gh_conversation_view_set_recipient_without_inbox(f->view, NULL);
  g_assert_false(adw_banner_get_revealed(banner));
  /* nostrc-lff5: a room where nobody has one says so, over a name. */
  gh_conversation_view_set_room_without_inbox(f->view, TRUE);
  g_assert_true(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_banner_get_title(banner), ==,
                  "No one in this conversation has set up private messaging yet");
  gh_conversation_view_set_recipient_without_inbox(f->view, "Alice");
  g_assert_cmpstr(adw_banner_get_title(banner), ==,
                  "No one in this conversation has set up private messaging yet");
  gh_conversation_view_set_room_without_inbox(f->view, FALSE);
  g_assert_true(adw_banner_get_revealed(banner));
  g_assert_cmpstr(adw_banner_get_title(banner), ==, "Alice hasn't set up private messaging yet");
  gh_conversation_view_set_recipient_without_inbox(f->view, NULL);
  g_assert_false(adw_banner_get_revealed(banner));

  /* 12: messages waiting for Grotto, with a focusable Unlock. */
  GtkWidget *locked = view_child(f->view, "locked_row");
  GtkWidget *unlock = view_child(f->view, "unlock_button");
  g_assert_false(shown(locked));
  gh_conversation_view_set_locked_messages(f->view, 12);
  g_assert_true(shown(locked));
  g_assert_cmpstr(text_of(view_child(f->view, "locked_label")), ==,
                  "Waiting for Grotto to unlock 12 messages");
  g_assert_true(gtk_widget_get_focusable(unlock));
  click(unlock);
  g_assert_cmpuint(f->unlocks, ==, 1);
  gh_conversation_view_set_locked_messages(f->view, 1);
  g_assert_cmpstr(text_of(view_child(f->view, "locked_label")), ==,
                  "Waiting for Grotto to unlock 1 message");
  gh_conversation_view_set_locked_messages(f->view, 0);
  g_assert_false(shown(locked));

  /* 13: an encrypted-group message that cannot be decrypted yet. */
  GhMessageRow *row = GH_MESSAGE_ROW(g_object_ref_sink(gh_message_row_new()));
  gh_message_row_set_message(row, m);
  /* Outside a view there is no preview fetcher, so no preview either. */
  g_assert_false(shown(row_child(row, "preview_box")));
  gh_message_row_set_undecryptable(row, TRUE);
  g_assert_cmpstr(text_of(row_child(row, "body_label")), ==, "Unable to decrypt yet");
  g_assert_false(shown(row_child(row, "preview_box")));
  g_assert_nonnull(strstr(gh_message_row_get_summary(row), ": Unable to decrypt yet"));
  g_assert_null(strstr(gh_message_row_get_summary(row), "example.com"));
  g_object_unref(row);

  /* 14: an encrypted-group message the group withdrew when it resolved a
   * conflict (nostrc-xrza): marked, its text gone from the bubble and the
   * summary, no preview -- live, as the mark arrives. */
  g_autofree gchar *inner = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[],\"content\":\"see https://example.com/x\"}", hex[2], noon_today());
  g_autoptr(GError) bad = NULL;
  g_autoptr(GhMessage) group_message =
    gh_message_new_from_mls(hex[1], "0123456789abcdef", inner, &bad);
  g_assert_no_error(bad);
  row = GH_MESSAGE_ROW(g_object_ref_sink(gh_message_row_new()));
  gh_message_row_set_message(row, group_message);
  g_assert_cmpstr(text_of(row_child(row, "body_label")), ==, "see https://example.com/x");
  gh_message_set_withdrawn(group_message, TRUE);
  g_assert_cmpstr(text_of(row_child(row, "body_label")), ==,
                  "This message was withdrawn when the group resolved a conflict");
  g_assert_true(gtk_widget_has_css_class(row_child(row, "body_label"), "groundhog-withdrawn"));
  g_assert_false(shown(row_child(row, "preview_box")));
  g_assert_nonnull(strstr(gh_message_row_get_summary(row),
                          ": This message was withdrawn when the group resolved a conflict"));
  g_assert_null(strstr(gh_message_row_get_summary(row), "example.com"));
  g_object_unref(row);
}

static gboolean
compact_is(gpointer data)
{
  Fixture *f = data;
  return gh_conversation_view_get_compact(f->view) ==
         GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f->view), "want"));
}

static void
test_compact_and_keyboard(Fixture *f, gconstpointer data)
{
  (void)data;
  GhMessage *m = add_dm(f->store, 1, 2, noon_today(),
                        "A longer message that wraps over more than one line in a bubble");
  GhConversation *conversation = room_of(f->store, m);
  g_object_set_data(G_OBJECT(f->view), "want", GINT_TO_POINTER(TRUE));
  show(f, conversation, 400, 500);
  /* Below 480sp: narrower bubbles (charter §7.12). */
  spin_until(compact_is, f);
  GhMessageRow *row = row_for(f->view, m);
  g_assert_cmpint(gtk_label_get_max_width_chars(row_child(row, "body_label")), ==, 32);
  int min_width = 0, min_height = 0;
  gtk_widget_measure(GTK_WIDGET(f->view), GTK_ORIENTATION_HORIZONTAL, -1, &min_width, NULL, NULL,
                     NULL);
  gtk_widget_measure(GTK_WIDGET(f->view), GTK_ORIENTATION_VERTICAL, -1, &min_height, NULL, NULL,
                     NULL);
  g_assert_cmpint(min_width, <=, 300);
  g_assert_cmpint(min_height, <=, 150);

  /* Keyboard: focusing the view focuses the list; arrows move between
   * messages and Tab stays within one (tab-behavior item). The window's
   * focus, not the focus state flags: GTK clears those whenever the window
   * is not active, and another test's window can take activation at any
   * moment (nostrc-9g6e). */
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(f->view)));
  GtkWidget *list = GTK_WIDGET(gh_conversation_view_get_message_list(f->view));
  GtkWidget *focus = gtk_root_get_focus(gtk_widget_get_root(list));
  g_assert_nonnull(focus);
  g_assert_true(focus == list || gtk_widget_is_ancestor(focus, list));
  g_assert_true(gtk_widget_get_focusable(item_for(f->view, m)));
}

static void
test_wide_is_not_compact(Fixture *f, gconstpointer data)
{
  (void)data;
  GhMessage *m = add_dm(f->store, 1, 2, noon_today(), "wide");
  g_object_set_data(G_OBJECT(f->view), "want", GINT_TO_POINTER(FALSE));
  show(f, room_of(f->store, m), 900, 500);
  spin_until(compact_is, f);
  g_assert_cmpint(gtk_label_get_max_width_chars(row_child(row_for(f->view, m), "body_label")), ==,
                  60);
}

static void
test_day_format(void)
{
  g_autoptr(GTimeZone) zone = g_time_zone_new_local();
  g_autoptr(GDateTime) now = g_date_time_new(zone, 2026, 9, 28, 9, 30, 0);
  g_autoptr(GDateTime) early = g_date_time_new(zone, 2026, 9, 28, 0, 5, 0);
  g_autofree gchar *today = gh_conversation_view_format_day(early, now);
  g_assert_cmpstr(today, ==, "Today");
  g_autoptr(GDateTime) late = g_date_time_new(zone, 2026, 9, 27, 23, 55, 0);
  g_autofree gchar *yesterday = gh_conversation_view_format_day(late, now);
  g_assert_cmpstr(yesterday, ==, "Yesterday");
  g_autoptr(GDateTime) three = g_date_time_add_days(now, -3);
  g_autofree gchar *weekday = g_date_time_format(three, "%A");
  g_autofree gchar *three_days = gh_conversation_view_format_day(three, now);
  g_assert_cmpstr(three_days, ==, weekday);
  g_autoptr(GDateTime) month = g_date_time_add_days(now, -30);
  g_autofree gchar *expected = g_date_time_format(month, "%A, %-d %B");
  g_autofree gchar *this_year = gh_conversation_view_format_day(month, now);
  g_assert_cmpstr(this_year, ==, expected);
  g_autoptr(GDateTime) last_year = g_date_time_add_years(now, -1);
  g_autofree gchar *expected_year = g_date_time_format(last_year, "%-d %B %Y");
  g_autofree gchar *other_year = gh_conversation_view_format_day(last_year, now);
  g_assert_cmpstr(other_year, ==, expected_year);
}

/* ---- screenshots (opt-in evidence) ------------------------------------------------------ */

/* The window as drawn, plus an open popover (a surface of its own) where
 * it sits, like a screenshot of the screen. */
static void
save_png(GtkWidget *window, GtkWidget *popover, const char *dir, const char *name)
{
  GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(window));
  int width = gdk_surface_get_width(surface);
  int height = gdk_surface_get_height(surface);
  double wx = 0, wy = 0;
  gtk_native_get_surface_transform(GTK_NATIVE(window), &wx, &wy);
  GtkSnapshot *snapshot = gtk_snapshot_new();
  g_autoptr(GdkPaintable) window_paintable = gtk_widget_paintable_new(window);
  gtk_snapshot_save(snapshot);
  gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(wx, wy));
  gdk_paintable_snapshot(window_paintable, snapshot, gtk_widget_get_width(window),
                         gtk_widget_get_height(window));
  gtk_snapshot_restore(snapshot);
  if (popover && gtk_widget_get_mapped(popover)) {
    GdkSurface *popup = gtk_native_get_surface(GTK_NATIVE(popover));
    double px = 0, py = 0;
    gtk_native_get_surface_transform(GTK_NATIVE(popover), &px, &py);
    g_autoptr(GdkPaintable) popover_paintable = gtk_widget_paintable_new(popover);
    gtk_snapshot_save(snapshot);
    gtk_snapshot_translate(snapshot,
                           &GRAPHENE_POINT_INIT(gdk_popup_get_position_x(GDK_POPUP(popup)) + px,
                                                gdk_popup_get_position_y(GDK_POPUP(popup)) + py));
    gdk_paintable_snapshot(popover_paintable, snapshot, gtk_widget_get_width(popover),
                           gtk_widget_get_height(popover));
    gtk_snapshot_restore(snapshot);
  }
  g_autoptr(GskRenderNode) node = gtk_snapshot_free_to_node(snapshot);
  g_assert_nonnull(node);
  GskRenderer *renderer = gtk_native_get_renderer(GTK_NATIVE(window));
  g_autoptr(GdkTexture) texture =
    gsk_renderer_render_texture(renderer, node, &GRAPHENE_RECT_INIT(0, 0, width, height));
  g_autofree char *path = g_strdup_printf("%s/groundhog-g12-%s.png", dir, name);
  g_assert_true(gdk_texture_save_to_png(texture, path));
  g_test_message("saved %s (%dx%d)", path, width, height);
}

typedef struct {
  GhConversationStore *store;
  GhConversation *dm;      /* B: days, runs, a link, statuses */
  GhConversation *group;   /* A, B, C: "Weekend Hike" */
  GhConversation *failed;  /* D: a message that was not sent */
  GhMessage *failed_message;
} Scenes;

static void
scenes_build(Scenes *s)
{
  s->store = gh_conversation_store_new();
  gh_conversation_store_set_account(s->store, hex[1], NULL, NULL, NULL);
  gint64 noon = noon_today();
  gint64 yesterday = noon - 86400 - 3 * 3600;
  add_dm(s->store, 2, 1, yesterday, "Hey! Are we still on for coffee tomorrow?");
  add_dm(s->store, 2, 1, yesterday + 40, "I found a new place near the station");
  GhMessage *y = add_dm(s->store, 1, 2, yesterday + 300, "Yes, looking forward to it ☕");
  gh_message_set_status(y, GH_MESSAGE_STATUS_SENT);
  add_dm(s->store, 2, 1, noon - 1800, "Here's the menu: https://cafe.example/menu");
  add_dm(s->store, 2, 1, noon - 1780, "They open at 9. <b>Tags</b> show as typed.");
  GhMessage *a = add_dm(s->store, 1, 2, noon - 900, "Perfect, see you at 9:30");
  gh_message_set_status(a, GH_MESSAGE_STATUS_SENT);
  GhMessage *b = add_dm(s->store, 1, 2, noon - 880, "I'll grab a table by the window");
  gh_message_set_status(b, GH_MESSAGE_STATUS_SENDING);
  s->dm = room_of(s->store, a);
  gh_conversation_mark_read(s->dm);

  const guint to_ac[] = { 1, 3, 0 }, to_ab[] = { 1, 2, 0 }, to_bc[] = { 2, 3, 0 };
  add_to(s->store, 2, to_ac, noon - 7200, "Hike on Saturday? Forecast is sunny", "Weekend Hike");
  add_to(s->store, 2, to_ac, noon - 7150, "Trailhead at 8, about 12 km", NULL);
  add_to(s->store, 3, to_ab, noon - 6900, "Count me in! I'll bring snacks", NULL);
  GhMessage *g = add_to(s->store, 1, to_bc, noon - 6600, "Me too. I can drive two people", NULL);
  gh_message_set_status(g, GH_MESSAGE_STATUS_SENT);
  add_to(s->store, 3, to_ab, noon - 6500, "Great, thanks!", NULL);
  s->group = room_of(s->store, g);
  gh_conversation_mark_read(s->group);

  add_dm(s->store, 4, 1, noon - 5400, "Can you send me the address?");
  s->failed_message = add_dm(s->store, 1, 4, noon - 5000, "Sure: 12 Market Street, 2nd floor");
  gh_message_set_status(s->failed_message, GH_MESSAGE_STATUS_NOT_SENT);
  s->failed = room_of(s->store, s->failed_message);
  gh_conversation_mark_read(s->failed);
}

static GhDeliveryReport *
failed_report(GhMessage *message, gpointer data)
{
  (void)message;
  (void)data;
  GhDeliveryReport *report = gh_delivery_report_new();
  gh_delivery_report_add(report, hex[4], "wss://inbox.relay.example", FALSE,
                         "This relay only accepts messages from signed-in users.");
  gh_delivery_report_add(report, hex[4], "wss://relay.backup.example", FALSE,
                         "Couldn't reach this relay.");
  gh_delivery_report_add(report, NULL, "wss://my.inbox.example", TRUE,
                         "Accepted by this relay.");
  report->detail = g_strdup("None of the recipient's message relays accepted it. "
                            "One needs you to sign in; the other couldn't be reached.");
  return report;
}

typedef struct {
  GhWindow *window;
  GhConversationView *view;
  GtkWidget *popover;
} Shot;

static gboolean
shot_ready(gpointer data)
{
  Shot *shot = data;
  if (!gtk_widget_get_mapped(GTK_WIDGET(shot->window)))
    return FALSE;
  if (shot->popover && !gtk_widget_get_mapped(shot->popover))
    return FALSE;
  g_autoptr(GPtrArray) items = item_widgets(shot->view);
  return items->len > 0 && gtk_widget_get_mapped(GTK_WIDGET(shot->view)) &&
         gh_conversation_view_get_at_latest(shot->view) && to_bottom(shot->view) <= 4;
}

static void
take_shot(Scenes *s, const char *dir, GhConversation *conversation, int width, int height,
          gboolean details, const char *name)
{
  Shot shot = { gh_window_new(NULL), NULL, NULL };
  GhSidebarPage *sidebar = gh_window_get_sidebar(shot.window);
  GtkWidget *account = gtk_menu_button_new();
  gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(account), "avatar-default-symbolic");
  adw_header_bar_pack_start(gh_sidebar_page_get_header(sidebar), account);
  GhContentPage *content = gh_window_get_content(shot.window);
  gh_conversation_list_attach(shot.window, s->store, NULL);
  GhStatus *status = gh_window_get_status(shot.window);
  gh_status_set_account_active(status, TRUE);
  gh_status_set_signer(status, GH_STATUS_SIGNER_AVAILABLE);
  gh_status_set_inbox(status, GH_STATUS_INBOX_LIVE, NULL);
  gh_sidebar_page_set_show_previews(sidebar, TRUE);
  shot.view = GH_CONVERSATION_VIEW(gh_content_page_get_view(content));
  gh_conversation_view_set_delivery_report_func(shot.view, failed_report, NULL, NULL);
  gtk_window_set_default_size(GTK_WINDOW(shot.window), width, height);
  gtk_window_present(GTK_WINDOW(shot.window));
  while (gh_sidebar_page_get_selected(sidebar) != conversation)
    g_assert_true(gtk_widget_activate_action(GTK_WIDGET(shot.window), "win.next-conversation",
                                             NULL));
  if (details) {
    Shot wait = shot;
    spin_until(shot_ready, &wait);
    GhDeliveryIndicator *indicator = row_child(row_for(shot.view, s->failed_message), "delivery");
    gh_delivery_indicator_show_details(indicator);
    shot.popover = indicator_child(indicator, "details");
  }
  spin_until(shot_ready, &shot);
  drain_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(shot.window));
  if (shot.popover)
    gtk_test_widget_wait_for_draw(shot.popover);
  save_png(GTK_WIDGET(shot.window), shot.popover, dir, name);
  gtk_window_destroy(GTK_WINDOW(shot.window));
  drain_idle();
}

static void
test_screenshots(void)
{
  const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
  if (!dir || !*dir) {
    g_test_skip("GROUNDHOG_TEST_SCREENSHOTS is not set");
    return;
  }
  /* Re-parsing the theme on a scheme switch can warn on some GTK builds;
   * criticals stay fatal. */
  GLogLevelFlags fatal = g_log_set_always_fatal(G_LOG_FATAL_MASK | G_LOG_LEVEL_CRITICAL);
  Scenes s = { 0 };
  scenes_build(&s);
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
    g_autofree char *narrow = g_strdup_printf("narrow-%s", schemes[i].name);
    take_shot(&s, dir, s.dm, 960, 680, FALSE, wide);
    take_shot(&s, dir, s.dm, 360, 640, FALSE, narrow);
  }
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_LIGHT);
  take_shot(&s, dir, s.group, 960, 680, FALSE, "group-light");
  take_shot(&s, dir, s.failed, 960, 680, TRUE, "failed-details-light");
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_FORCE_DARK);
  take_shot(&s, dir, s.failed, 960, 680, TRUE, "failed-details-dark");
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
  g_object_unref(s.store);
  g_log_set_always_fatal(fatal);
}

/* The visible children of the meta line, in order. */
static GPtrArray *
meta_children(GtkWidget *meta)
{
  GPtrArray *children = g_ptr_array_new();
  for (GtkWidget *child = gtk_widget_get_first_child(meta); child;
       child = gtk_widget_get_next_sibling(child))
    if (gtk_widget_get_visible(child))
      g_ptr_array_add(children, child);
  return children;
}

static graphene_rect_t
bounds_in(GtkWidget *widget, GtkWidget *target)
{
  graphene_rect_t bounds;
  g_assert_true(gtk_widget_compute_bounds(widget, target, &bounds));
  return bounds;
}

static gboolean
row_laid_out(gpointer data)
{
  GhMessageRow *row = data;
  GtkWidget *meta = row_child(row, "meta_box");
  return gtk_widget_get_mapped(meta) && gtk_widget_get_width(meta) > 0;
}

static guint
chip_count(GtkWidget *bar)
{
  g_autoptr(GPtrArray) chips = g_ptr_array_new();
  collect(bar, GTK_TYPE_BUTTON, chips);
  return chips->len;
}

/* One meta line for both sides, mirrored (nostrc-l1kn6.5): the status
 * (time, and on your own messages the receipt) at the bubble's edge, the
 * reaction chips next to it, React outermost; no gaps, and the same with no,
 * one or several reactions:
 *   incoming  [time][chips…][React]
 *   own       [React][chips…][time][receipt]
 * The chips are created only once there are reactions, and go again. */
static void
test_reaction_meta_layout(void)
{
  static const gchar *const emojis[] = { "👍", "❤️", "😂" };
  static const guint reactors[] = { 2, 3, 1 };
  const guint counts[] = { 0, 1, 3 };
  for (guint author = 1; author <= 2; author++) {
    gboolean own = author == 1;
    for (guint c = 0; c < G_N_ELEMENTS(counts); c++) {
      guint n = counts[c];
      g_autoptr(GhReactionStore) reactions = gh_reaction_store_new();
      gh_reaction_store_set_account(reactions, hex[1], NULL, NULL, NULL);
      g_autoptr(GhMessage) message = rumor(author, own ? (const guint[]){ 2, 0 }
                                                       : (const guint[]){ 1, 0 },
                                           now_seconds(), "reacted message", NULL);
      if (own)
        gh_message_set_status(message, GH_MESSAGE_STATUS_SENT);
      GtkWindow *window = GTK_WINDOW(gtk_window_new());
      gtk_window_set_default_size(window, 640, 300);
      GhMessageRow *row = GH_MESSAGE_ROW(gh_message_row_new());
      gtk_window_set_child(window, GTK_WIDGET(row));
      gh_message_row_set_message(row, message);
      gh_message_row_set_run(row, TRUE, FALSE);
      GtkWidget *meta = row_child(row, "meta_box");
      GtkWidget *status = row_child(row, "status_box");
      GtkWidget *react = row_child(row, "react_button");
      GtkWidget *bubble = row_child(row, "bubble");
      /* Mid-run, plainly sent, unreacted: no meta line at all. */
      g_assert_false(gtk_widget_get_visible(meta));
      g_assert_null(row_child(row, "reaction_bar"));

      for (guint i = 0; i < n; i++) {
        g_autofree gchar *reaction_id = g_strdup_printf("reaction-%u-%u-%u", author, n, i);
        g_autoptr(GhReaction) reaction = gh_reaction_new(gh_message_get_rumor_id(message),
          reaction_id, hex[reactors[i]], emojis[i], now_seconds(),
          gh_message_get_room_id(message));
        g_assert_true(gh_reaction_store_admit(reactions, reaction, NULL));
      }
      gh_message_row_set_reaction_summary(row,
        gh_reaction_store_lookup(reactions, gh_message_get_rumor_id(message)));
      GtkWidget *bar = row_child(row, "reaction_bar");
      if (n == 0) {
        g_assert_null(bar);
        g_assert_false(gtk_widget_get_visible(meta));
        gh_message_row_set_run(row, TRUE, TRUE); /* the end of a run shows it */
      } else {
        g_assert_nonnull(bar);
        g_assert_true(gtk_widget_get_visible(bar));
        g_assert_cmpuint(chip_count(bar), ==, n);
      }
      g_assert_true(gtk_widget_get_visible(meta));
      g_assert_true(gtk_widget_get_visible(status));
      g_assert_true(gtk_widget_get_visible(react));
      g_assert_false(shown(row_child(row, "retry_button")));
      g_assert_cmpint(gtk_widget_get_halign(meta), ==, own ? GTK_ALIGN_END : GTK_ALIGN_START);

      /* The order, outward from the bubble's edge. */
      g_autoptr(GPtrArray) order = meta_children(meta);
      g_assert_cmpuint(order->len, ==, n ? 3 : 2);
      GtkWidget *edge = g_ptr_array_index(order, own ? order->len - 1 : 0);
      GtkWidget *outer = g_ptr_array_index(order, own ? 0 : order->len - 1);
      g_assert_true(edge == status);
      g_assert_true(outer == react);
      if (n)
        g_assert_true(g_ptr_array_index(order, 1) == bar);
      /* The status reads the same on both sides: time, then any receipt. */
      GtkWidget *time = row_child(row, "time_label");
      GtkWidget *receipt = row_child(row, "delivery");
      g_assert_true(gtk_widget_get_parent(time) == status);
      g_assert_cmpint(gtk_widget_get_visible(receipt), ==, own);

      /* Laid out: flush to the bubble's edge, nothing between the parts. */
      gtk_window_present(window);
      spin_until(row_laid_out, row);
      wait_frames(GTK_WIDGET(row));
      GtkWidget *target = GTK_WIDGET(row);
      graphene_rect_t bubble_box = bounds_in(bubble, target);
      graphene_rect_t edge_box = bounds_in(edge, target);
      if (own)
        g_assert_cmpfloat(fabs(edge_box.origin.x + edge_box.size.width -
                               (bubble_box.origin.x + bubble_box.size.width)), <=, 1);
      else
        g_assert_cmpfloat(fabs(edge_box.origin.x - bubble_box.origin.x), <=, 1);
      for (guint i = 1; i < order->len; i++) {
        graphene_rect_t left = bounds_in(g_ptr_array_index(order, i - 1), target);
        graphene_rect_t right = bounds_in(g_ptr_array_index(order, i), target);
        gdouble gap = right.origin.x - (left.origin.x + left.size.width);
        g_assert_cmpfloat(fabs(gap - 4), <=, 1);
      }
      graphene_rect_t time_box = bounds_in(time, target);
      if (own) {
        graphene_rect_t receipt_box = bounds_in(receipt, target);
        g_assert_cmpfloat(time_box.origin.x + time_box.size.width, <=, receipt_box.origin.x);
        g_assert_cmpfloat(fabs(receipt_box.origin.x + receipt_box.size.width -
                               (edge_box.origin.x + edge_box.size.width)), <=, 1);
      } else {
        g_assert_cmpfloat(fabs(time_box.origin.x - edge_box.origin.x), <=, 1);
      }
      /* The chips sit on the meta line, below the bubble. */
      g_assert_cmpfloat(edge_box.origin.y, >=, bubble_box.origin.y + bubble_box.size.height);
      /* The chips fill the bar, in one line, spaced like the rest: no
       * room is kept beside them. */
      if (bar) {
        g_autoptr(GPtrArray) chips = g_ptr_array_new();
        collect(bar, GTK_TYPE_BUTTON, chips);
        graphene_rect_t bar_box = bounds_in(bar, target);
        graphene_rect_t first = bounds_in(g_ptr_array_index(chips, 0), target);
        graphene_rect_t last = bounds_in(g_ptr_array_index(chips, chips->len - 1), target);
        g_assert_cmpfloat(fabs(first.origin.x - bar_box.origin.x), <=, 1);
        g_assert_cmpfloat(fabs(last.origin.x + last.size.width -
                               (bar_box.origin.x + bar_box.size.width)), <=, 1);
        /* Not stretched to the line's height (the receipt's is taller):
         * as tall as the tallest chip (your own has a border). */
        int line_height = 0;
        for (guint i = 0; i < chips->len; i++) {
          int chip_height = 0;
          gtk_widget_measure(g_ptr_array_index(chips, i), GTK_ORIENTATION_VERTICAL, -1, NULL,
                             &chip_height, NULL, NULL);
          line_height = MAX(line_height, chip_height);
        }
        g_assert_cmpfloat(fabs(bar_box.size.height - line_height), <=, 1);
        graphene_rect_t status_box = bounds_in(status, target);
        g_assert_cmpfloat(fabs(first.origin.y + first.size.height / 2 -
                               (status_box.origin.y + status_box.size.height / 2)), <=, 1);
        for (guint i = 1; i < chips->len; i++) {
          graphene_rect_t left = bounds_in(g_ptr_array_index(chips, i - 1), target);
          graphene_rect_t right = bounds_in(g_ptr_array_index(chips, i), target);
          g_assert_cmpfloat(fabs(right.origin.y - left.origin.y), <=, 1);
          g_assert_cmpfloat(fabs(right.origin.x - (left.origin.x + left.size.width) - 4), <=, 1);
        }
      }
      const char *dir = g_getenv("GROUNDHOG_TEST_SCREENSHOTS");
      if (dir) {
        gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
        g_autofree gchar *name = g_strdup_printf("meta-%s-%u-reactions",
                                                 own ? "own" : "incoming", n);
        save_png(GTK_WIDGET(window), NULL, dir, name);
      }

      /* Reactions gone: the chips go too. */
      for (guint i = 0; i < n; i++) {
        g_autofree gchar *reaction_id = g_strdup_printf("reaction-%u-%u-%u", author, n, i);
        g_assert_true(gh_reaction_store_remove(reactions, reaction_id, NULL));
      }
      g_assert_null(row_child(row, "reaction_bar"));
      gtk_window_destroy(window);
    }
  }
}

typedef struct {
  guint calls;
  GhMessage *target;
  gchar *emoji;
  gboolean add;
} ReactCall;

static void
capture_react(GhConversation *conversation, GhMessage *target, const gchar *emoji,
              gboolean add, gpointer data)
{
  ReactCall *call = data;
  g_assert_cmpstr(gh_conversation_get_room_id(conversation), ==,
                  gh_message_get_room_id(target));
  call->calls++;
  call->target = target;
  g_free(call->emoji);
  call->emoji = g_strdup(emoji);
  call->add = add;
}

static void
test_reaction_view_binding(Fixture *f, gconstpointer data)
{
  (void)data;
  GhMessage *target = add_dm(f->store, 2, 1, noon_today(), "react here");
  GhConversation *conversation = room_of(f->store, target);
  g_autoptr(GhReactionStore) reactions = gh_reaction_store_new();
  gh_reaction_store_set_account(reactions, hex[1], NULL, NULL, NULL);
  g_autoptr(GhReaction) reaction = gh_reaction_new(gh_message_get_rumor_id(target),
    "view-reaction", hex[2], "👍", now_seconds(), gh_message_get_room_id(target));
  g_assert_true(gh_reaction_store_admit(reactions, reaction, NULL));
  gh_conversation_view_set_reaction_store(f->view, reactions);
  ReactCall call = { 0 };
  gh_conversation_view_set_reaction_func(f->view, capture_react, &call, NULL);
  show(f, conversation, 640, 360);
  GhMessageRow *row = row_for(f->view, target);

  /* A known reaction must survive the initial message + summary bindings. */
  GtkWidget *bar = row_child(row, "reaction_bar");
  g_assert_nonnull(bar);
  g_assert_true(shown(bar));
  g_assert_true(shown(row_child(row, "meta_box")));
  g_assert_cmpuint(chip_count(bar), ==, 1);

  /* The button-to-picker-to-view action must preserve target and emoji. */
  click(row_child(row, "react_button"));
  g_autoptr(GPtrArray) pickers = g_ptr_array_new();
  collect(GTK_WIDGET(row), GH_TYPE_REACTION_PICKER, pickers);
  g_assert_cmpuint(pickers->len, ==, 1);
  g_assert_true(gtk_widget_get_mapped(g_ptr_array_index(pickers, 0)));
  g_signal_emit_by_name(g_ptr_array_index(pickers, 0), "emoji-picked", "❤️");
  g_assert_cmpuint(call.calls, ==, 1);
  g_assert_true(call.target == target);
  g_assert_cmpstr(call.emoji, ==, "❤️");
  g_assert_true(call.add);

  g_assert_true(gh_reaction_store_remove(reactions, "view-reaction", NULL));
  g_assert_null(row_child(row, "reaction_bar"));
  g_autoptr(GhReaction) later = gh_reaction_new(gh_message_get_rumor_id(target),
    "view-reaction-later", hex[2], "👍", now_seconds(), gh_message_get_room_id(target));
  g_assert_true(gh_reaction_store_admit(reactions, later, NULL));
  gh_conversation_view_set_conversation(f->view, NULL);
  gh_conversation_view_set_conversation(f->view, conversation);
  spin_until(rows_bound, f);
  drain_idle();
  row = row_for(f->view, target);
  g_assert_cmpuint(chip_count(row_child(row, "reaction_bar")), ==, 1);
  gh_conversation_view_set_reaction_store(f->view, NULL);
  g_assert_null(row_child(row, "reaction_bar"));
  gh_conversation_view_set_reaction_store(f->view, reactions);
  g_assert_cmpuint(chip_count(row_child(row, "reaction_bar")), ==, 1);
  g_clear_pointer(&call.emoji, g_free);
}

static guint
widget_tree_count(GtkWidget *root)
{
  guint count = 1;
  for (GtkWidget *child = gtk_widget_get_first_child(root); child;
       child = gtk_widget_get_next_sibling(child))
    count += widget_tree_count(child);
  return count;
}

static GhMessage *
recycle_reply_message(void)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 9);
  nostr_event_set_pubkey(event, hex[1]);
  nostr_event_set_created_at(event, noon_today());
  nostr_event_set_content(event, "reply B");
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("h", "recycle-group", NULL));
  nostr_tags_append(tags, nostr_tag_new("e",
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef", "", "reply", NULL));
  nostr_event_set_tags(event, tags);
  event->id = nostr_event_get_id(event);
  g_autofree gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_nip29_event(hex[1],
    "wss://relay.example.com", json, &error);
  g_assert_no_error(error);
  g_assert_nonnull(gh_message_get_reply_to_id(message));
  return message;
}

static void
test_recycled_scroll_directions(Fixture *f, gconstpointer data)
{
  (void)data;
  gint64 start = noon_today() - 300 * 60;
  GhNip17File file = { 0 };
  file.url = (gchar *)"https://blossom.example.com/7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730";
  file.file_type = (gchar *)"image/jpeg";
  file.nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  g_strlcpy(file.x, "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730",
            sizeof file.x);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = gh_nip17_file_rumor_new(hex[2], hex[1], &file,
    start, 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) first = gh_message_new_from_rumor(hex[1], json, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(f->store, first, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  fill(f->store, start + 60, 300);
  GhMessage *last = add_dm(f->store, 2, 1, start + 301 * 600, "last plain row");
  GhConversation *conversation = room_of(f->store, first);
  gh_conversation_mark_read(conversation);
  show(f, conversation, 480, 360);
  VisibleRowWait first_visible = { f->view, first };
  VisibleRowWait last_visible = { f->view, last };
  guint last_position = timeline_length(f->view) - 1;
  gtk_list_view_scroll_to(gh_conversation_view_get_message_list(f->view),
                          last_position, GTK_LIST_SCROLL_NONE, NULL);
  spin_until(at_bottom, f->view);
  spin_until(row_visible, &last_visible);
  guint plain_widgets = widget_tree_count(GTK_WIDGET(row_for(f->view, last)));
  for (guint pass = 0; pass < 3; pass++) {
    gtk_list_view_scroll_to(gh_conversation_view_get_message_list(f->view),
                            0, GTK_LIST_SCROLL_NONE, NULL);
    gtk_adjustment_set_value(vadjustment(f->view), 0);
    spin_until(scrolled_up, f->view);
    spin_until(row_visible, &first_visible);
    drain_idle();
    g_assert_nonnull(row_child(row_for(f->view, first), "attachment_card"));
    gtk_list_view_scroll_to(gh_conversation_view_get_message_list(f->view),
                            last_position, GTK_LIST_SCROLL_NONE, NULL);
    gtk_adjustment_set_value(vadjustment(f->view),
                             gtk_adjustment_get_upper(vadjustment(f->view)));
    spin_until(at_bottom, f->view);
    spin_until(row_visible, &last_visible);
    drain_idle();
    GhMessageRow *row = row_for(f->view, last);
    g_assert_null(row_child(row, "attachment_card"));
    g_assert_cmpstr(text_of(row_child(row, "body_label")), ==, "last plain row");
    g_assert_cmpuint(widget_tree_count(GTK_WIDGET(row)), ==, plain_widgets);
  }
}

static void
test_recycled_optional_subtrees(void)
{
  const guint to[] = { 1, 0 };
  g_autoptr(GhMessage) a = rumor(2, to, noon_today(), "plain A", NULL);
  g_autoptr(GhMessage) b = recycle_reply_message();
  GhNip17File file = { 0 };
  file.url = (gchar *)"https://blossom.example.com/7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730";
  file.file_type = (gchar *)"image/jpeg";
  file.nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  g_strlcpy(file.x, "7d865e959b2466918c9863afca942d0fb89d7c9ac0c99bafc3749504ded97730",
            sizeof file.x);
  g_autoptr(GError) error = NULL;
  g_autofree gchar *file_json = gh_nip17_file_rumor_new(hex[2], hex[1], &file,
    noon_today(), 0, NULL, &error);
  g_assert_no_error(error);
  g_autoptr(GhMessage) attachment = gh_message_new_from_rumor(hex[1], file_json, &error);
  g_assert_no_error(error);
  GhMessageRow *row = GH_MESSAGE_ROW(g_object_ref_sink(gh_message_row_new()));
  gh_message_row_set_message(row, a);
  guint plain_widgets = widget_tree_count(GTK_WIDGET(row));
  g_assert_null(row_child(row, "reply_button"));
  g_assert_null(row_child(row, "attachment_card"));
  g_assert_null(row_child(row, "reaction_bar"));
  g_assert_null(row_child(row, "preview_button"));
  g_assert_cmpstr(text_of(row_child(row, "body_label")), ==, "plain A");

  for (guint pass = 0; pass < 3; pass++) {
    /* Forward/backward recycling: A→reply→A→file→A→poll→A. */
    gh_message_row_set_message(row, b);
    g_assert_nonnull(row_child(row, "reply_button"));
    g_assert_nonnull(strstr(gh_message_row_get_summary(row), "In reply"));
    gh_message_set_expires_at(b, now_seconds() + 3600);
    g_assert_true(shown(row_child(row, "timer_icon")));
    gh_message_row_set_message(row, a);
    g_assert_null(row_child(row, "reply_button"));
    g_assert_false(shown(row_child(row, "timer_icon")));
    g_assert_null(strstr(gh_message_row_get_summary(row), "In reply"));
    g_assert_cmpuint(widget_tree_count(GTK_WIDGET(row)), ==, plain_widgets);

    gh_message_row_set_message(row, attachment);
    g_assert_nonnull(row_child(row, "attachment_card"));
    gh_message_row_set_message(row, a);
    g_assert_null(row_child(row, "attachment_card"));
    g_assert_cmpuint(widget_tree_count(GTK_WIDGET(row)), ==, plain_widgets);

    gh_message_row_set_message(row, b);
    gh_message_row_set_poll_widget(row, gtk_label_new("poll options"));
    g_assert_true(shown(row_child(row, "poll_slot")));
    gh_message_row_set_message(row, a);
    g_assert_false(shown(row_child(row, "poll_slot")));
    g_assert_cmpuint(widget_tree_count(GTK_WIDGET(row)), ==, plain_widgets);
  }
  g_autoptr(GhReactionStore) reactions = gh_reaction_store_new();
  gh_reaction_store_set_account(reactions, hex[1], NULL, NULL, NULL);
  g_autoptr(GhReaction) reaction = gh_reaction_new(gh_message_get_rumor_id(a),
    "recycle-reaction", hex[2], "👍", now_seconds(), gh_message_get_room_id(a));
  g_assert_true(gh_reaction_store_admit(reactions, reaction, NULL));
  gh_message_row_set_reaction_summary(row,
    gh_reaction_store_lookup(reactions, gh_message_get_rumor_id(a)));
  g_assert_nonnull(row_child(row, "reaction_bar"));
  gh_message_row_set_message(row, b);
  g_assert_null(row_child(row, "reaction_bar"));
  gh_message_row_set_message(row, a);
  g_assert_cmpuint(widget_tree_count(GTK_WIDGET(row)), ==, plain_widgets);

  g_autofree gchar *inner = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[],\"content\":\"withdrawn B\"}", hex[2], noon_today());
  g_autoptr(GhMessage) withdrawn = gh_message_new_from_mls(hex[1],
    "0123456789abcdef", inner, &error);
  g_assert_no_error(error);
  gh_message_row_set_message(row, withdrawn);
  gh_message_set_withdrawn(withdrawn, TRUE);
  g_assert_null(row_child(row, "reply_button"));
  g_assert_nonnull(strstr(gh_message_row_get_summary(row), gh_message_withdrawn_text()));
  gh_message_row_set_message(row, a);
  g_assert_cmpstr(text_of(row_child(row, "body_label")), ==, "plain A");
  g_assert_null(strstr(gh_message_row_get_summary(row), "reply B"));
  g_assert_cmpuint(widget_tree_count(GTK_WIDGET(row)), ==, plain_widgets);
  g_object_unref(row);
}

/* nostrc-gofet.10: forced-RTL smoke. Under an RTL UI an incoming bubble sits
 * at the right (start) edge, and the body label takes its own text's
 * direction, falling back to the UI's for neutral text. */
static gboolean
row_mapped(gpointer row)
{
  return gtk_widget_get_mapped(GTK_WIDGET(row));
}

static void
test_rtl_message_row(void)
{
  const guint to[] = { 1, 0 };
  GtkTextDirection saved = gtk_widget_get_default_direction();
  gtk_widget_set_default_direction(GTK_TEXT_DIR_RTL);
  g_autoptr(GhMessage) arabic = rumor(2, to, noon_today(), "مرحبا بالعالم", NULL);
  g_autoptr(GhMessage) latin = rumor(2, to, noon_today(), "hello **world**", NULL);
  g_autoptr(GhMessage) neutral = rumor(2, to, noon_today(), "12345 🙂", NULL);
  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  gtk_window_set_default_size(window, 640, 240);
  GhMessageRow *row = GH_MESSAGE_ROW(gh_message_row_new());
  gtk_window_set_child(window, GTK_WIDGET(row));
  GtkWidget *body = row_child(row, "body_label");

  gh_message_row_set_message(row, arabic);
  g_assert_cmpint(gtk_widget_get_direction(body), ==, GTK_TEXT_DIR_RTL);
  gh_message_row_set_message(row, latin);
  g_assert_cmpint(gtk_widget_get_direction(body), ==, GTK_TEXT_DIR_LTR);
  gh_message_row_set_message(row, neutral);
  g_assert_cmpint(gtk_widget_get_direction(body), ==, GTK_TEXT_DIR_RTL);
  g_assert_cmpint(gtk_widget_get_direction(row_child(row, "sender_label")), !=, GTK_TEXT_DIR_NONE);

  gh_message_row_set_message(row, arabic);
  gtk_window_present(window);
  spin_until(row_mapped, row);
  drain_idle();
  graphene_rect_t bubble, whole;
  g_assert_true(gtk_widget_compute_bounds(row_child(row, "bubble"), GTK_WIDGET(window), &bubble));
  g_assert_true(gtk_widget_compute_bounds(GTK_WIDGET(row), GTK_WIDGET(window), &whole));
  /* Start edge is the right edge under RTL. */
  g_assert_cmpfloat(bubble.origin.x + bubble.size.width, >, whole.origin.x + whole.size.width / 2);
  gtk_window_destroy(window);
  gtk_widget_set_default_direction(saved);
}

/* nostrc-gofet.10: icon-only and new alpha-6 actions have accessible names. */
static void
test_message_action_accessibility(void)
{
  const guint to[] = { 1, 0 };
  g_autoptr(GhMessage) message = rumor(2, to, noon_today(), "plain", NULL);
  GhMessageRow *row = GH_MESSAGE_ROW(g_object_ref_sink(gh_message_row_new()));
  gh_message_row_set_message(row, message);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(row_child(row, "copy_button")),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, "Copy message");
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(row_child(row, "reference_copy_button")),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, "Copy Nostr address");
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(row_child(row, "react_button")),
                                      GTK_ACCESSIBLE_PROPERTY_LABEL, "Add Reaction");
  for (guint i = 0; i < 2; i++) {
    const char *name = i ? "quote_reference_button" : "repost_reference_button";
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(row_child(row, name)),
      GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
      "Opens a preview before anything is published publicly");
  }
  GtkWidget *avatar = row_child(row, "avatar");
  g_assert_true(gtk_test_accessible_has_property(GTK_ACCESSIBLE(avatar),
                                                 GTK_ACCESSIBLE_PROPERTY_LABEL));
  g_assert_true(gtk_test_accessible_has_property(GTK_ACCESSIBLE(avatar),
                                                 GTK_ACCESSIBLE_PROPERTY_DESCRIPTION));
  g_object_unref(row);

  GtkWidget *agent = g_object_ref_sink(GTK_WIDGET(gh_agent_event_row_new()));
  g_assert_cmpint(gtk_accessible_get_accessible_role(GTK_ACCESSIBLE(agent)), ==,
                  GTK_ACCESSIBLE_ROLE_GROUP);
  g_assert_cmpint(gtk_accessible_get_accessible_role(
                    GTK_ACCESSIBLE(gtk_widget_get_first_child(agent))), ==,
                  GTK_ACCESSIBLE_ROLE_PRESENTATION);
  g_object_unref(agent);
}

int
main(int argc, char **argv)
{
  if (!gtk_init_check()) {
    g_printerr("groundhog-conversation-view test skipped: no graphical display\n");
    return 77;
  }
  adw_init();
  groundhog_register_resource();
  /* The application's stylesheet, as main.c loads it, so sizes include it. */
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* 96 dpi makes 1sp one pixel everywhere; no animations. */
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
               "gtk-enable-animations", FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
  init_keys();

  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
#define ADD(path, func) \
  g_test_add("/groundhog/conversation-view/" path, Fixture, NULL, fixture_setup, func, \
             fixture_teardown)
  ADD("bubbles-runs-and-days", test_bubbles_runs_and_days);
  ADD("multi-party-senders", test_multi_party_senders);
  ADD("delivery-indicator", test_delivery_indicator);
  ADD("links", test_links);
  ADD("markdown-copy-identity", test_markdown_copy_identity);
  ADD("reference-card", test_reference_card);
  ADD("share-reference", test_share_reference);
  ADD("link-previews", test_link_previews);
  ADD("sender-scoped-consent", test_sender_scoped_consent);
  ADD("file-message", test_file_message);
  ADD("expiry", test_expiry);
  ADD("scrolling", test_scrolling);
  ADD("opens-at-first-unread", test_opens_at_first_unread);
  ADD("load-older", test_load_older);
  ADD("earlier-button-near-top", test_earlier_button_near_top);
  ADD("window-long-scroll", test_window_long_scroll);
  ADD("earlier-messages", test_earlier_messages);
  ADD("states-11-13", test_states);
  ADD("compact-and-keyboard", test_compact_and_keyboard);
  ADD("wide-is-not-compact", test_wide_is_not_compact);
  for (guint kind = 0; kind <= GH_WEB_IMAGE; kind++) {
    g_autofree gchar *path = g_strdup_printf("/groundhog/conversation-view/web-consent/%u-%s", kind,
                                         gh_web_content_setting(kind));
    g_test_add(path, Fixture, GUINT_TO_POINTER(kind), fixture_setup, test_web_consent, fixture_teardown);
  }
  ADD("web-allow-covers-sender", test_web_allow_covers_sender);
  ADD("failed-picture-has-no-row-error", test_failed_picture_has_no_row_error);
  ADD("open-timing", test_open_timing);
  ADD("recycled-scroll-directions", test_recycled_scroll_directions);
  ADD("reaction-view-binding", test_reaction_view_binding);
#undef ADD
  g_test_add_func("/groundhog/conversation-view/recycled-optional-subtrees", test_recycled_optional_subtrees);
  g_test_add_func("/groundhog/conversation-view/rtl-message-row", test_rtl_message_row);
  g_test_add_func("/groundhog/conversation-view/action-accessibility", test_message_action_accessibility);
  g_test_add_func("/groundhog/conversation-view/reaction-meta-layout", test_reaction_meta_layout);
  g_test_add_func("/groundhog/conversation-view/day-format", test_day_format);
  g_test_add_func("/groundhog/conversation-view/screenshots", test_screenshots);
  int status = g_test_run();
  for (guint key = 1; key < KEYS; key++)
    g_free(hex[key]);
  return status;
}
