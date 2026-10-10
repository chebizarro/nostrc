/* The composer and send wiring (charter G13: UX-7, OB-1 UI side, UX-1/UX-2
 * for the composer, §7.14 focus incl. nostrc-qp24.8.1, the §7.15 composer
 * states, and every G12 seam wired). First the widget alone (keys, bounds,
 * compact, disabled page, drafts timer, emoji, focus), then the application's
 * wiring (gh-send-ui.c) on a real account stack (tests/ui/send-stack.h): the
 * mock signer on a private bus, FakeSecret (H5), SQLCipher stores in private
 * directories, recording relay transports (H1) and a scripted recipient
 * inbox directory, so every relay answer is the test's. Needs a display:
 * it self-skips (77) without one. Waits iterate the main context against a
 * deadline; they never sleep. With GROUNDHOG_TEST_SCREENSHOTS=<dir> the
 * screenshots case renders <dir>/groundhog-g13-*.png (compose, waiting,
 * sending, failed, sent; light and dark; narrow). */
#include "gh-test-signer.h"
#include "send-stack.h"

#include "gh-conversation-private.h"
#include "gh-delivery-indicator.h"
#include "gh-expiry.h"
#include "gh-message-row.h"
#include "gh-test-active.h"

#include <string.h>

#include "nostrc-test-gdk-frame.h"

#define DISCOVERY "wss://discovery.test.invalid"
#define INBOX_A   "wss://inbox-a.test.invalid"   /* the account's own (key 1) */
#define INBOX_B   "wss://inbox-b.test.invalid"   /* key 2's */
#define INBOX_C   "wss://inbox-c.test.invalid"   /* key 3's, once set up */

static GhTestBus bus;
static GhTestSigner signer;

/* ---- recording relay transports (H1) ---------------------------------------------- */

typedef struct {
  GhRelayScope *scope;
  gchar *url;
  gboolean discovery;
  gboolean closed;
} Req;

static GPtrArray *reqs; /* Req: every REQ ever opened */

static void
req_free(gpointer data)
{
  Req *req = data;
  gh_relay_scope_unref(req->scope);
  g_free(req->url);
  g_free(req);
}

static gpointer
rec_open(GhRelayScope *scope, const gchar *url, const NostrFilters *filters, gpointer data,
         GError **error)
{
  (void)data;
  (void)error;
  Req *req = g_new0(Req, 1);
  req->scope = gh_relay_scope_ref(scope);
  req->url = g_strdup(url);
  req->discovery = nostr_filter_kinds_len(&filters->filters[0]) == 2;
  g_ptr_array_add(reqs, req);
  return req;
}

static void
rec_close(gpointer handle, gpointer data)
{
  (void)data;
  ((Req *)handle)->closed = TRUE;
}

static const GhRelayTransport rec_transport = { rec_open, rec_close };

static gboolean
rec_send_auth(gpointer handle, const gchar *json, gpointer data, GError **error)
{
  (void)handle; (void)json; (void)data; (void)error;
  return TRUE;
}

static void
rec_resubscribe(gpointer handle, gpointer data)
{
  (void)handle; (void)data;
}

static const GhRelayAuthTransport rec_auth = { rec_send_auth, rec_resubscribe };

static Req *
open_req(const gchar *url, gboolean discovery)
{
  for (guint i = reqs->len; i > 0; i--) {
    Req *req = g_ptr_array_index(reqs, i - 1);
    if (!req->closed && req->discovery == discovery && g_str_equal(req->url, url))
      return req;
  }
  return NULL;
}

static gboolean
inbox_req_open(gpointer data)
{
  (void)data;
  return open_req(INBOX_A, FALSE) != NULL;
}

/* Publishing: every wrap handed to a relay, answered as scripted. */
typedef enum { PUB_ACCEPT, PUB_HOLD, PUB_REFUSE } PubMode;

typedef struct {
  GhRelayPublish *publish;
  gchar *url;
  gchar *event_id;
  gchar *p;          /* the wrap's receiver */
  gboolean closed;
  gboolean answered;
} Pub;

static struct {
  GPtrArray *opens; /* Pub */
  PubMode mode;
  const gchar *refuse_url;     /* refused with refuse_message whatever the mode */
  const gchar *refuse_message;
} pubs;

static void
pub_free(gpointer data)
{
  Pub *pub = data;
  gh_relay_publish_unref(pub->publish);
  g_free(pub->url);
  g_free(pub->event_id);
  g_free(pub->p);
  g_free(pub);
}

static void
pub_answer(Pub *pub, gboolean accepted, const gchar *message)
{
  if (pub->closed || pub->answered)
    return;
  pub->answered = TRUE;
  gh_relay_publish_ok(pub->publish, pub->url, pub->event_id, accepted, message);
}

static gboolean
pub_answer_idle(gpointer data)
{
  Pub *pub = data;
  if (pubs.refuse_url && g_str_equal(pub->url, pubs.refuse_url))
    pub_answer(pub, FALSE, pubs.refuse_message);
  else if (pubs.mode == PUB_ACCEPT)
    pub_answer(pub, TRUE, "");
  else if (pubs.mode == PUB_REFUSE)
    pub_answer(pub, FALSE, "blocked: not today");
  return G_SOURCE_REMOVE;
}

static gpointer
pub_open(GhRelayPublish *publish, const gchar *url, const gchar *event_json, gpointer data,
         GError **error)
{
  (void)data;
  (void)error;
  NostrEvent *wrap = nostr_event_new();
  g_assert_cmpint(nostr_event_deserialize_compact(wrap, event_json, NULL), ==, 1);
  g_assert_cmpint(nostr_event_get_kind(wrap), ==, 1059);
  Pub *pub = g_new0(Pub, 1);
  char *p = nostr_nip59_get_recipient(wrap);
  pub->p = g_strdup(p);
  free(p);
  nostr_event_free(wrap);
  pub->publish = gh_relay_publish_ref(publish);
  pub->url = g_strdup(url);
  pub->event_id = g_strdup(gh_relay_publish_get_event_id(publish));
  g_ptr_array_add(pubs.opens, pub);
  if (pubs.mode != PUB_HOLD || pubs.refuse_url)
    g_idle_add(pub_answer_idle, pub);
  return pub;
}

static void
pub_close(gpointer handle, gpointer data)
{
  (void)data;
  ((Pub *)handle)->closed = TRUE;
}

static const GhRelayPublishTransport pub_transport = { pub_open, pub_close };

static void
answer_held(gboolean accepted, const gchar *message)
{
  for (guint i = 0; i < pubs.opens->len; i++)
    pub_answer(g_ptr_array_index(pubs.opens, i), accepted, message);
}

static guint
pubs_to(const gchar *url, const gchar *p)
{
  guint n = 0;
  for (guint i = 0; i < pubs.opens->len; i++) {
    Pub *pub = g_ptr_array_index(pubs.opens, i);
    n += g_str_equal(pub->url, url) && g_str_equal(pub->p, p);
  }
  return n;
}

/* ---- a scripted recipient inbox directory (GhInboxResolver) ------------------------ */

#define DIR_TYPE_RESOLVER (dir_resolver_get_type())
G_DECLARE_FINAL_TYPE(DirResolver, dir_resolver, DIR, RESOLVER, GObject)

struct _DirResolver {
  GObject parent_instance;
  GHashTable *inboxes; /* pubkey hex -> relay URL */
};

static void
dir_resolve_async(GhInboxResolver *resolver, const gchar *pubkey, GCancellable *cancellable,
                  GAsyncReadyCallback callback, gpointer data)
{
  DirResolver *self = DIR_RESOLVER(resolver);
  GTask *task = g_task_new(resolver, cancellable, callback, data);
  GhInboxResult *result = g_new0(GhInboxResult, 1);
  const gchar *url = g_hash_table_lookup(self->inboxes, pubkey);
  result->recipient = g_strdup(pubkey);
  result->sources = result->answered = 1;
  result->status = url ? GH_INBOX_FOUND : GH_INBOX_NOT_FOUND;
  if (url) {
    result->relays = g_new0(gchar *, 2);
    result->relays[0] = g_strdup(url);
  }
  g_task_return_pointer(task, result, (GDestroyNotify)gh_inbox_result_free);
  g_object_unref(task);
}

static GhInboxResult *
dir_resolve_finish(GhInboxResolver *resolver, GAsyncResult *result, GError **error)
{
  (void)resolver;
  return g_task_propagate_pointer(G_TASK(result), error);
}

static void
dir_resolver_iface_init(GhInboxResolverInterface *iface)
{
  iface->resolve_async = dir_resolve_async;
  iface->resolve_finish = dir_resolve_finish;
}

G_DEFINE_FINAL_TYPE_WITH_CODE(DirResolver, dir_resolver, G_TYPE_OBJECT,
  G_IMPLEMENT_INTERFACE(GH_TYPE_INBOX_RESOLVER, dir_resolver_iface_init))

static void
dir_resolver_finalize(GObject *object)
{
  g_hash_table_unref(DIR_RESOLVER(object)->inboxes);
  G_OBJECT_CLASS(dir_resolver_parent_class)->finalize(object);
}

static void
dir_resolver_class_init(DirResolverClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = dir_resolver_finalize;
}

static void
dir_resolver_init(DirResolver *self)
{
  self->inboxes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

/* ---- fixture ------------------------------------------------------------------------ */

typedef struct {
  SendStack s;
  DirResolver *directory;
  gint64 list_time;
} Fixture;

static void
fixture_init(Fixture *f, GDBusConnection *signer_bus)
{
  reqs = g_ptr_array_new_with_free_func(req_free);
  pubs.opens = g_ptr_array_new_with_free_func(pub_free);
  pubs.mode = PUB_ACCEPT;
  pubs.refuse_url = NULL;
  signer.hold = FALSE;
  signer.deny = FALSE;
  f->directory = g_object_new(DIR_TYPE_RESOLVER, NULL);
  g_hash_table_insert(f->directory->inboxes, g_strdup(stack_hex[2]), g_strdup(INBOX_B));
  g_hash_table_insert(f->directory->inboxes, g_strdup(stack_hex[1]), g_strdup(INBOX_A));
  const gchar *discovery[] = { DISCOVERY, NULL };
  send_stack_init(&f->s, 1, signer_bus, discovery);
  f->s.scope_transport = &rec_transport;
  f->s.auth_transport = &rec_auth;
  f->s.publish_transport = &pub_transport;
  f->s.resolver = GH_INBOX_RESOLVER(f->directory);
  f->list_time = 1700000000;
}

/* The account's own kind-10050 (INBOX_A), delivered on its discovery REQ;
 * the inbox then subscribes there. */
static void
own_inbox_list(Fixture *f)
{
  Req *discovery = open_req(DISCOVERY, TRUE);
  g_assert_nonnull(discovery);
  const gchar *urls[] = { INBOX_A, NULL };
  g_autofree gchar *list = stack_inbox_list(f->s.key, ++f->list_time, urls);
  gh_relay_scope_event(discovery->scope, DISCOVERY, list);
  gh_relay_scope_eose(discovery->scope, DISCOVERY);
  gh_test_spin_until(inbox_req_open, NULL);
}

/* Up (or up again: a restart), with the window. */
static void
fixture_up(Fixture *f, gint width, gint height)
{
  send_stack_up(&f->s);
  g_assert_cmpint(gh_account_store_get_state(f->s.store), ==, GH_ACCOUNT_STORE_OPEN);
  own_inbox_list(f);
  send_stack_window(&f->s, width, height);
  gh_test_run_until_idle();
}

static void
fixture_down(Fixture *f)
{
  send_stack_down(&f->s);
  /* Anything the signer still holds belonged to the closed stack. */
  signer.deny = TRUE;
  gh_test_signer_release_all(&signer);
  signer.deny = FALSE;
  GhTestSenders check = { &bus, &signer };
  gh_test_spin_until(gh_test_signer_senders_closed, &check);
}

static void
fixture_clear(Fixture *f)
{
  if (f->s.accounts)
    fixture_down(f);
  send_stack_clear(&f->s);
  g_clear_object(&f->directory);
  g_clear_pointer(&reqs, g_ptr_array_unref);
  g_clear_pointer(&pubs.opens, g_ptr_array_unref);
  signer.hold = FALSE;
  signer.deny = FALSE;
}

typedef struct {
  GhConversationStore *model;
  gchar *room;
} RoomWait;

static gboolean
room_listed(gpointer data)
{
  RoomWait *wait = data;
  return gh_conversation_store_lookup(wait->model, wait->room) != NULL;
}

/* key `from` writes to the account (and `others`, 0-terminated, may be NULL)
 * on its inbox relay; returns the room. */
static GhConversation *
receive(Fixture *f, guint from, const guint *others, const gchar *content)
{
  guint recipients[8] = { f->s.key, 0 };
  guint members[10] = { from, f->s.key, 0 };
  for (guint i = 0; others && others[i]; i++) {
    recipients[i + 1] = others[i];
    recipients[i + 2] = 0;
    members[i + 2] = others[i];
    members[i + 3] = 0;
  }
  Req *req = open_req(INBOX_A, FALSE);
  g_assert_nonnull(req);
  /* Later calls write later messages, whatever the second. */
  static gint64 offset;
  g_autofree gchar *wrap = stack_craft_wrap(from, f->s.key, recipients,
                                            g_get_real_time() / G_USEC_PER_SEC - 600 + offset++,
                                            content);
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  RoomWait wait = { f->s.model, stack_room(members) };
  gh_test_spin_until(room_listed, &wait);
  GhConversation *conversation = gh_conversation_store_lookup(f->s.model, wait.room);
  g_free(wait.room);
  /* Listed as a conversation, not a message request (as after Accept). */
  gh_conversation_accept(conversation);
  return conversation;
}

typedef struct {
  GhMessage *message;
  GhMessageStatus status;
} StatusWait;

static gboolean
status_is(gpointer data)
{
  StatusWait *wait = data;
  return gh_message_get_status(wait->message) == wait->status;
}

/* Approves every signer request as it comes until the message has status. */
static gboolean
approve_until_status(gpointer data)
{
  StatusWait *wait = data;
  if (signer.held->len)
    gh_test_signer_release_all(&signer);
  return status_is(wait);
}

static void
wait_status(GhMessage *message, GhMessageStatus status)
{
  StatusWait wait = { message, status };
  gh_test_spin_until(status_is, &wait);
}

static void
approve_until(GhMessage *message, GhMessageStatus status)
{
  StatusWait wait = { message, status };
  gh_test_spin_until(approve_until_status, &wait);
}

static gboolean
signer_waiting(gpointer data)
{
  (void)data;
  return signer.held->len > 0;
}

static GtkWidget *
template_child(gpointer widget, GType type, const gchar *name)
{
  GObject *child = gtk_widget_get_template_child(GTK_WIDGET(widget), type, name);
  g_assert_nonnull(child);
  return GTK_WIDGET(child);
}

static gboolean
text_view_focused(gpointer data)
{
  GhComposer *composer = data;
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(composer));
  return root && gtk_root_get_focus(root) == GTK_WIDGET(gh_composer_get_text_view(composer));
}

static gboolean
is_mapped(gpointer data)
{
  return gtk_widget_get_mapped(GTK_WIDGET(data));
}

static gboolean
widget_visible(gpointer data)
{
  return gtk_widget_get_visible(GTK_WIDGET(data));
}

/* ---- the widget alone ------------------------------------------------------------- */

typedef struct {
  guint sends;
  gchar *last;
  gboolean accept;
  guint drafts;
  gchar *draft;
} Sink;

static gboolean
sink_send(GhComposer *composer, const gchar *text, Sink *sink)
{
  sink->sends++;
  g_free(sink->last);
  sink->last = g_strdup(text);
  if (!sink->accept)
    gh_composer_set_error(composer, "Storage is full, so this message was not sent.");
  return sink->accept;
}

static void
sink_draft(GhComposer *composer, const gchar *text, Sink *sink)
{
  (void)composer;
  sink->drafts++;
  g_free(sink->draft);
  sink->draft = g_strdup(text);
}

static void
sink_clear(Sink *sink)
{
  g_free(sink->last);
  g_free(sink->draft);
}

static GhComposer *
lone_composer(GtkWindow **window_out, Sink *sink)
{
  GtkWindow *window = GTK_WINDOW(gtk_window_new());
  GhComposer *composer = GH_COMPOSER(gh_composer_new());
  gtk_window_set_child(window, GTK_WIDGET(composer));
  gtk_window_set_default_size(window, 480, 120);
  g_signal_connect(composer, "send", G_CALLBACK(sink_send), sink);
  g_signal_connect(composer, "draft-changed", G_CALLBACK(sink_draft), sink);
  *window_out = window;
  return composer;
}

static gchar *
text_of(GhComposer *composer)
{
  return gh_composer_dup_text(composer);
}

/* UX-7: Enter sends, Shift+Enter is a newline, Ctrl+Enter always sends. */
static void
test_keys(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);

  stack_type(composer, "Hello");
  g_assert_true(gh_composer_get_enter_sends(composer));
  /* Shift+Enter goes to the text view (a newline); nothing is sent. */
  g_assert_false(stack_press(composer, GDK_KEY_Return, GDK_SHIFT_MASK));
  g_assert_cmpuint(sink.sends, ==, 0);
  /* Enter sends, and the composer is emptied without a draft report. */
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_cmpuint(sink.sends, ==, 1);
  g_assert_cmpstr(sink.last, ==, "Hello");
  g_autofree gchar *after = text_of(composer);
  g_assert_cmpstr(after, ==, "");
  g_assert_false(gh_composer_get_draft_pending(composer));

  /* The keypad Enter too. */
  stack_type(composer, "Two");
  g_assert_true(stack_press(composer, GDK_KEY_KP_Enter, 0));
  g_assert_cmpuint(sink.sends, ==, 2);

  /* With "Send with Enter" off, Enter is a newline; Ctrl+Enter still sends. */
  gh_composer_set_enter_sends(composer, FALSE);
  stack_type(composer, "Three");
  g_assert_false(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_cmpuint(sink.sends, ==, 2);
  g_assert_true(stack_press(composer, GDK_KEY_Return, GDK_CONTROL_MASK));
  g_assert_cmpuint(sink.sends, ==, 3);
  g_assert_cmpstr(sink.last, ==, "Three");

  /* Enter on nothing but whitespace sends nothing (and inserts nothing). */
  gh_composer_set_enter_sends(composer, TRUE);
  stack_type(composer, "  \n\t ");
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_cmpuint(sink.sends, ==, 3);
  /* Other keys are the text view's. */
  g_assert_false(stack_press(composer, GDK_KEY_a, 0));

  gtk_window_destroy(window);
  sink_clear(&sink);
}

static gboolean
reject_long(const gchar *text, gpointer data)
{
  (void)data;
  return strlen(text) <= 10;
}

/* UX-7: blank text leaves Send insensitive; a body over the bound shows an
 * inline error and is never truncated; a refused send keeps the text. */
static void
test_bounds(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);
  GtkWidget *send_button = template_child(composer, GH_TYPE_COMPOSER, "send_button");
  GtkWidget *error_label = template_child(composer, GH_TYPE_COMPOSER, "error_label");
  GtkWidget *placeholder = template_child(composer, GH_TYPE_COMPOSER, "placeholder");

  g_assert_false(gh_composer_get_can_send(composer));
  g_assert_false(gtk_widget_is_sensitive(send_button));
  g_assert_true(gtk_widget_get_visible(placeholder));
  stack_type(composer, " \n ");
  g_assert_false(gh_composer_get_can_send(composer));
  g_assert_false(gtk_widget_is_sensitive(send_button));
  g_assert_false(gtk_widget_get_visible(placeholder));
  g_assert_false(gh_composer_send(composer));
  g_assert_cmpuint(sink.sends, ==, 0);
  stack_type(composer, "hi");
  g_assert_true(gh_composer_get_can_send(composer));
  g_assert_true(gtk_widget_is_sensitive(send_button));
  gh_composer_set_text(composer, NULL);
  g_assert_true(gtk_widget_get_visible(placeholder));

  /* The default bound: 60 000 bytes fit, one more does not. */
  g_autofree gchar *fits = g_strnfill(GH_COMPOSER_DEFAULT_MAX_BYTES, 'x');
  gh_composer_set_text(composer, fits);
  g_assert_false(gh_composer_get_too_long(composer));
  g_assert_true(gh_composer_get_can_send(composer));
  g_assert_false(gtk_widget_get_visible(error_label));
  stack_type(composer, "y");
  g_assert_true(gh_composer_get_too_long(composer));
  g_assert_false(gh_composer_get_can_send(composer));
  g_assert_false(gtk_widget_is_sensitive(send_button));
  g_assert_true(gtk_widget_get_visible(error_label));
  g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(error_label)), "too long"));
  g_assert_false(gh_composer_send(composer));
  g_assert_cmpuint(sink.sends, ==, 0);
  g_autofree gchar *kept = text_of(composer);
  g_assert_cmpuint(strlen(kept), ==, GH_COMPOSER_DEFAULT_MAX_BYTES + 1); /* not truncated */

  /* The owner's measure replaces the default. */
  gh_composer_set_text(composer, "short");
  gh_composer_set_length_func(composer, reject_long, NULL, NULL);
  g_assert_true(gh_composer_get_can_send(composer));
  stack_type(composer, " and longer");
  g_assert_true(gh_composer_get_too_long(composer));
  gh_composer_set_length_func(composer, NULL, NULL, NULL);
  g_assert_false(gh_composer_get_too_long(composer));

  /* A send the owner could not queue keeps the text and says why; the next
   * edit clears the message. */
  sink.accept = FALSE;
  gh_composer_set_text(composer, "keep me");
  g_assert_false(gh_composer_send(composer));
  g_assert_cmpuint(sink.sends, ==, 1);
  g_autofree gchar *still = text_of(composer);
  g_assert_cmpstr(still, ==, "keep me");
  g_assert_true(gtk_widget_get_visible(error_label));
  g_assert_cmpstr(gh_composer_get_error(composer), ==,
                  "Storage is full, so this message was not sent.");
  stack_type(composer, "!");
  g_assert_null(gh_composer_get_error(composer));
  g_assert_false(gtk_widget_get_visible(error_label));

  gtk_window_destroy(window);
  sink_clear(&sink);
}

/* The disabled page: the reason in the composer's place, the text kept. */
static void
test_disabled(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);
  GtkStack *stack = GTK_STACK(template_child(composer, GH_TYPE_COMPOSER, "composer_stack"));
  GtkWidget *reason = template_child(composer, GH_TYPE_COMPOSER, "disabled_reason");
  GtkWidget *button = template_child(composer, GH_TYPE_COMPOSER, "disabled_button");

  stack_type(composer, "draft");
  g_assert_true(gh_composer_get_can_send(composer));
  gh_composer_set_disabled_reason(composer, "Message storage is locked.");
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "disabled");
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(reason)), ==, "Message storage is locked.");
  g_assert_false(gh_composer_get_can_send(composer));
  g_assert_false(gh_composer_send(composer));
  g_assert_false(stack_press(composer, GDK_KEY_Return, 0) && sink.sends > 0);
  g_assert_cmpuint(sink.sends, ==, 0);
  g_assert_false(gtk_widget_get_visible(button));
  gh_composer_set_disabled_action(composer, "_Check Again", "win.check");
  g_assert_true(gtk_widget_get_visible(button));
  g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, "_Check Again");
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(button)), ==, "win.check");
  gh_composer_set_disabled_action(composer, NULL, NULL);
  g_assert_false(gtk_widget_get_visible(button));

  gh_composer_set_disabled_reason(composer, "");
  g_assert_null(gh_composer_get_disabled_reason(composer));
  g_assert_cmpstr(gtk_stack_get_visible_child_name(stack), ==, "edit");
  g_autofree gchar *text = text_of(composer);
  g_assert_cmpstr(text, ==, "draft");
  g_assert_true(gh_composer_get_can_send(composer));

  gtk_window_destroy(window);
  sink_clear(&sink);
}

/* Compact hides the emoji button (Ctrl+. still works); fewer lines make a
 * lower entry. */
static void
test_compact_and_lines(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);
  GtkWidget *emoji = template_child(composer, GH_TYPE_COMPOSER, "emoji_button");
  GtkScrolledWindow *scroller =
    GTK_SCROLLED_WINDOW(template_child(composer, GH_TYPE_COMPOSER, "scroller"));
  gtk_window_present(window);
  gh_test_spin_until(is_mapped, composer);

  g_assert_false(gh_composer_get_compact(composer));
  g_assert_true(gtk_widget_get_visible(emoji));
  gh_composer_set_compact(composer, TRUE);
  g_assert_false(gtk_widget_get_visible(emoji));
  g_assert_true(gtk_widget_has_css_class(GTK_WIDGET(composer), "compact"));
  gh_composer_set_compact(composer, FALSE);
  g_assert_true(gtk_widget_get_visible(emoji));

  g_assert_cmpuint(gh_composer_get_max_lines(composer), ==, GH_COMPOSER_DEFAULT_MAX_LINES);
  gint six = gtk_scrolled_window_get_max_content_height(scroller);
  gh_composer_set_max_lines(composer, 3);
  gint three = gtk_scrolled_window_get_max_content_height(scroller);
  g_assert_cmpint(six, >, 0);
  g_assert_cmpint(three, <, six);
  g_assert_cmpint(three, >, 0);

  gtk_window_destroy(window);
  sink_clear(&sink);
}

static gboolean
draft_reported(gpointer data)
{
  return ((Sink *)data)->drafts > 0;
}

/* Drafts are reported 1 s after the last edit, or at once on a flush; a
 * programmatic text reports nothing. The emoji chooser inserts at the
 * cursor. */
static void
test_draft_timer_and_emoji(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);

  gh_composer_set_text(composer, "restored");
  g_assert_false(gh_composer_get_draft_pending(composer));
  stack_type(composer, " and edited");
  g_assert_true(gh_composer_get_draft_pending(composer));
  gint64 typed = g_get_monotonic_time();
  gh_test_spin_until(draft_reported, &sink);
  g_assert_cmpint(g_get_monotonic_time() - typed, >=, (GH_COMPOSER_DRAFT_DELAY_MS - 50) * 1000);
  g_assert_cmpuint(sink.drafts, ==, 1);
  g_assert_cmpstr(sink.draft, ==, "restored and edited");

  stack_type(composer, "!");
  gh_composer_flush_draft(composer);
  g_assert_cmpuint(sink.drafts, ==, 2);
  g_assert_cmpstr(sink.draft, ==, "restored and edited!");
  gh_composer_flush_draft(composer); /* nothing pending */
  g_assert_cmpuint(sink.drafts, ==, 2);

  /* The chooser is made when the button first opens it (nostrc-boq9.5). */
  GtkMenuButton *emoji_button =
    GTK_MENU_BUTTON(template_child(composer, GH_TYPE_COMPOSER, "emoji_button"));
  g_assert_null(gtk_menu_button_get_popover(emoji_button));
  gtk_menu_button_popup(emoji_button);
  GtkEmojiChooser *chooser = GTK_EMOJI_CHOOSER(gtk_menu_button_get_popover(emoji_button));
  g_assert_nonnull(chooser);
  g_signal_emit_by_name(chooser, "emoji-picked", "🦫");
  g_autofree gchar *text = text_of(composer);
  g_assert_cmpstr(text, ==, "restored and edited!🦫");

  /* A pending draft is still reported when the composer goes away. */
  gtk_window_destroy(window);
  g_assert_cmpuint(sink.drafts, ==, 3);
  g_assert_cmpstr(sink.draft, ==, "restored and edited!🦫");
  sink_clear(&sink);
}

/* §7.14: after a send focus stays in (or returns to) the text view, e.g.
 * when the Send button was reached with Tab. */
static void
test_focus_after_send(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);
  gtk_window_present(window);
  gh_test_spin_until(is_mapped, composer);
  GtkWidget *send_button = template_child(composer, GH_TYPE_COMPOSER, "send_button");
  g_assert_false(gtk_widget_get_focus_on_click(send_button));

  stack_type(composer, "one");
  g_assert_true(gtk_widget_grab_focus(send_button));
  g_assert_true(gtk_widget_activate_action(send_button, "composer.send", NULL));
  g_assert_cmpuint(sink.sends, ==, 1);
  g_assert_true(text_view_focused(composer));

  stack_type(composer, "two");
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_true(text_view_focused(composer));
  /* The composer itself hands focus to its entry. */
  gtk_window_set_focus(window, NULL);
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(composer)));
  g_assert_true(text_view_focused(composer));

  gtk_window_destroy(window);
  sink_clear(&sink);
}

/* ---- layout (UX-1, UX-2) ------------------------------------------------------------ */

/* An in-memory model with one conversation (key 2 writes to key 1). */
static GhConversationStore *
layout_model(void)
{
  GhConversationStore *store = gh_conversation_store_new();
  gh_conversation_store_set_account(store, stack_hex[1], NULL, NULL, NULL);
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, stack_hex[2]);
  nostr_event_set_created_at(event, g_get_real_time() / G_USEC_PER_SEC - 300);
  nostr_event_set_content(event, "Are we still on for Saturday?");
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", stack_hex[1], NULL)));
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(stack_hex[1], json, &error);
  free(json);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(store, message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  /* Listed as a conversation, not a message request. */
  gh_conversation_accept(gh_conversation_store_lookup(store, gh_message_get_room_id(message)));
  return store;
}

typedef struct {
  GhComposer *composer;
  gboolean compact;
  guint max_lines;
} LayoutWait;

static gboolean
layout_is(gpointer data)
{
  LayoutWait *wait = data;
  return gtk_widget_get_mapped(GTK_WIDGET(wait->composer)) &&
         gh_composer_get_compact(wait->composer) == wait->compact &&
         gh_composer_get_max_lines(wait->composer) == wait->max_lines;
}

static GhWindow *
layout_window(GhConversationStore *store, gint width, gint height)
{
  GhWindow *window = gh_window_new(NULL);
  gh_conversation_list_attach(window, store, NULL);
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  gtk_window_present(GTK_WINDOW(window));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(window), "win.next-conversation", NULL));
  return window;
}

static void
assert_layout(GhConversationStore *store, gint width, gint height, gboolean compact,
              guint max_lines)
{
  GhWindow *window = layout_window(store, width, height);
  LayoutWait wait = { gh_content_page_get_composer(gh_window_get_content(window)), compact,
                      max_lines };
  gh_test_spin_until(layout_is, &wait);
  gtk_window_destroy(GTK_WINDOW(window));
  gh_test_run_until_idle();
}

/* UX-2 (composer): content width 470 -> compact; content height 350 ->
 * three lines; a wide, tall window -> neither. */
static void
test_breakpoints(void)
{
  g_autoptr(GhConversationStore) store = layout_model();
  assert_layout(store, 960, 680, FALSE, GH_COMPOSER_DEFAULT_MAX_LINES);
  assert_layout(store, 470, 680, TRUE, GH_COMPOSER_DEFAULT_MAX_LINES);
  assert_layout(store, 960, 400, FALSE, 3);
  assert_layout(store, 470, 400, TRUE, 3);
}

static gboolean
send_allocated(gpointer data)
{
  GtkWidget *send = data;
  return gtk_widget_get_mapped(send) && gtk_widget_get_width(send) > 0 &&
         gtk_widget_get_height(send) > 0;
}

/* The widget lies inside the window, whole. */
static void
assert_inside(GtkWidget *widget, GtkWidget *window)
{
  graphene_rect_t bounds;
  g_assert_true(gtk_widget_compute_bounds(widget, window, &bounds));
  g_assert_cmpfloat(bounds.origin.x, >=, 0);
  g_assert_cmpfloat(bounds.origin.y, >=, 0);
  g_assert_cmpfloat(bounds.origin.x + bounds.size.width, <=, gtk_widget_get_width(window) + 0.5);
  g_assert_cmpfloat(bounds.origin.y + bounds.size.height, <=,
                    gtk_widget_get_height(window) + 0.5);
}

/* UX-1 (composer): at 360x294 the conversation shows with a compact,
 * three-line composer whose Send button is allocated inside the window, and
 * a long disabled reason wraps to every line it needs (libadwaita 1.5 sized a
 * wrapping label in a bottom bar as one line: nostrc-qp24.48 #5). */
static void
test_minimum_size(void)
{
  g_autoptr(GhConversationStore) store = layout_model();
  GhWindow *window = layout_window(store, 360, 294);
  GhComposer *composer = gh_content_page_get_composer(gh_window_get_content(window));
  LayoutWait wait = { composer, TRUE, 3 };
  gh_test_spin_until(layout_is, &wait);
  GtkWidget *send = template_child(composer, GH_TYPE_COMPOSER, "send_button");
  gh_test_spin_until(send_allocated, send);
  assert_inside(send, GTK_WIDGET(window));
  assert_inside(GTK_WIDGET(composer), GTK_WIDGET(window));

  gh_composer_set_disabled_reason(
    composer, "Read-only: the Nostr signer service is not installed or running, so "
              "nothing can be sent until it is.");
  GtkWidget *reason = template_child(composer, GH_TYPE_COMPOSER, "disabled_reason");
  gh_test_spin_until(is_mapped, reason);
  gh_test_run_until_idle();
  gtk_test_widget_wait_for_draw(GTK_WIDGET(window));
  gint width = gtk_widget_get_width(reason);
  gint min = 0, one_line = 0;
  gtk_widget_measure(reason, GTK_ORIENTATION_VERTICAL, width, &min, NULL, NULL, NULL);
  gtk_widget_measure(reason, GTK_ORIENTATION_VERTICAL, -1, &one_line, NULL, NULL, NULL);
  g_assert_cmpint(min, >, one_line);                        /* it wraps here */
  g_assert_cmpint(gtk_widget_get_height(reason), >=, min);  /* and is not clipped */
  assert_inside(reason, GTK_WIDGET(window));

  gtk_window_destroy(GTK_WINDOW(window));
  gh_test_run_until_idle();
}

/* ---- the wiring on a real account stack --------------------------------------------- */

static GhDeliveryReport *
report_of(Fixture *f, GhMessage *message)
{
  return gh_conversation_view_dup_delivery_report(send_stack_view(&f->s), message);
}

/* UX-7, §3.5, §3.6, §7.14: Enter queues the text (T-enqueue before any
 * signer call), the message shows at once and its status follows the
 * outbox honestly; delivery details come from the outbox; focus stays in
 * the composer; the stored draft is cleared. */
static void
test_send_echo_status(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  send_stack_select(&f.s, bob);
  GhComposer *composer = send_stack_composer(&f.s);
  GhConversationView *view = send_stack_view(&f.s);
  g_assert_null(gh_composer_get_disabled_reason(composer));

  signer.hold = TRUE;
  pubs.mode = PUB_HOLD;
  guint calls = signer.calls;
  guint before = g_list_model_get_n_items(G_LIST_MODEL(bob));
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(composer)));
  stack_type(composer, "Hello Bob");
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));

  /* Shown at once, before any approval: the local echo of the queued rumor. */
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(bob)), ==, before + 1);
  GhMessage *mine = stack_newest(bob);
  g_assert_true(gh_message_is_self(mine));
  g_assert_cmpstr(gh_message_get_content(mine), ==, "Hello Bob");
  g_assert_cmpint(gh_message_get_status(mine), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  g_assert_cmpuint(signer.calls, ==, calls); /* T-enqueue came first */
  GListModel *timeline = gh_conversation_view_get_timeline(view);
  g_autoptr(GhTimelineItem) last =
    g_list_model_get_item(timeline, g_list_model_get_n_items(timeline) - 1);
  g_assert_true(gh_timeline_item_get_message(last) == mine);
  g_autofree gchar *text = gh_composer_dup_text(composer);
  g_assert_cmpstr(text, ==, "");
  g_autofree gchar *draft = send_stack_draft(&f.s, gh_conversation_get_room_id(bob));
  g_assert_null(draft);
  g_assert_true(text_view_focused(composer));
  gh_test_spin_until(signer_waiting, NULL);

  /* Approved: sealed and handed to Bob's relay and our own, none answered. */
  approve_until(mine, GH_MESSAGE_STATUS_SENDING);
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, 1);
  g_assert_cmpuint(pubs_to(INBOX_A, stack_hex[1]), ==, 1);
  {
    g_autoptr(GhDeliveryReport) report = report_of(&f, mine);
    g_assert_nonnull(report);
    g_assert_cmpuint(report->targets->len, ==, 2);
    for (guint i = 0; i < report->targets->len; i++)
      g_assert_false(((GhDeliveryTarget *)g_ptr_array_index(report->targets, i))->accepted);
  }

  answer_held(TRUE, "");
  wait_status(mine, GH_MESSAGE_STATUS_SENT);
  g_autoptr(GhDeliveryReport) report = report_of(&f, mine);
  g_assert_nonnull(report);
  g_assert_cmpuint(report->targets->len, ==, 2);
  GhDeliveryTarget *to_bob = g_ptr_array_index(report->targets, 0);
  GhDeliveryTarget *to_self = g_ptr_array_index(report->targets, 1);
  g_assert_cmpstr(to_bob->recipient, ==, stack_hex[2]);
  g_assert_cmpstr(to_bob->relay_url, ==, INBOX_B);
  g_assert_true(to_bob->accepted);
  g_assert_nonnull(to_bob->outcome);
  g_assert_null(to_self->recipient);
  g_assert_cmpstr(to_self->relay_url, ==, INBOX_A);
  g_assert_nonnull(report->detail);
  g_assert_false(report->self_copy_missing);
  /* Incoming messages have no details. */
  g_autoptr(GhMessage) first = g_list_model_get_item(G_LIST_MODEL(bob), 0);
  g_assert_null(report_of(&f, first));
  fixture_clear(&f);
}

/* Bob's relay keeps answering "error:" (transient, three times): the
 * message retries on the outbox's schedule (on a fake clock), then says "Not
 * sent" (announced assertively); Try Again republishes the same stored wrap
 * (never re-sealed) and it is sent. */
static void
test_failed_retry(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  f.s.clock = gh_clock_new_fake(g_get_real_time());
  fixture_up(&f, 960, 680);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  send_stack_select(&f.s, bob);
  GhComposer *composer = send_stack_composer(&f.s);
  GhConversationView *view = send_stack_view(&f.s);
  guint assertive = gh_conversation_view_get_announcements(
    view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
  GhTestActiveSpan span;
  gh_test_active_span_begin(&span, GTK_WINDOW(f.s.window));

  pubs.refuse_url = INBOX_B;
  pubs.refuse_message = "error: try again later";
  stack_type(composer, "Did this arrive?");
  g_assert_true(gh_composer_send(composer));
  GhMessage *mine = stack_find(bob, "Did this arrive?");
  wait_status(mine, GH_MESSAGE_STATUS_RETRYING);
  g_assert_cmpuint(signer.calls, >, 0);
  guint calls = signer.calls;
  for (guint round = 0; round < 8 && gh_message_get_status(mine) != GH_MESSAGE_STATUS_NOT_SENT;
       round++) {
    gh_clock_fake_advance(f.s.clock, (gint64)20 * 60 * G_USEC_PER_SEC);
    gh_test_run_until_idle();
  }
  wait_status(mine, GH_MESSAGE_STATUS_NOT_SENT);
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, GH_MESSAGE_STATUS_ERROR_ATTEMPTS);
  gboolean either = FALSE;
  guint step = gh_test_active_span_expect(&span, 1, &either);
  guint made = gh_conversation_view_get_announcements(
    view, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH) - assertive;
  if (either) /* nostrc-9g6e */
    g_assert_cmpuint(made, <=, 1);
  else
    g_assert_cmpuint(made, ==, step);

  pubs.refuse_url = NULL;
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(view), "conversation.retry-message", "s",
                                           gh_message_get_rumor_id(mine)));
  wait_status(mine, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, GH_MESSAGE_STATUS_ERROR_ATTEMPTS + 1);
  g_assert_cmpuint(signer.calls, ==, calls); /* republished, not re-sealed */
  const gchar *first = NULL;
  for (guint i = 0; i < pubs.opens->len; i++) {
    Pub *pub = g_ptr_array_index(pubs.opens, i);
    if (!g_str_equal(pub->url, INBOX_B))
      continue;
    if (!first)
      first = pub->event_id;
    g_assert_cmpstr(pub->event_id, ==, first);
  }
  fixture_clear(&f);
}

typedef struct {
  SendStack *s;
  const gchar *room;
  const gchar *draft;
} DraftWait;

static gboolean
draft_stored(gpointer data)
{
  DraftWait *wait = data;
  g_autofree gchar *draft = send_stack_draft(wait->s, wait->room);
  return g_strcmp0(draft, wait->draft) == 0;
}

static void
assert_composer_text(GhComposer *composer, const gchar *expected)
{
  g_autofree gchar *text = gh_composer_dup_text(composer);
  g_assert_cmpstr(text, ==, expected);
}

/* UX-7: a draft is saved 1 s after the last edit and on a switch, restored on
 * return, and survives a restart of the encrypted store. */
static void
test_drafts(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  GhConversation *carol = receive(&f, 3, NULL, "Hi from Carol");
  g_autofree gchar *bob_room = g_strdup(gh_conversation_get_room_id(bob));
  g_autofree gchar *carol_room = g_strdup(gh_conversation_get_room_id(carol));
  GhComposer *composer = send_stack_composer(&f.s);

  send_stack_select(&f.s, bob);
  stack_type(composer, "Draft for Bob");
  DraftWait saved = { &f.s, bob_room, "Draft for Bob" };
  gh_test_spin_until(draft_stored, &saved); /* the 1 s debounce */

  stack_type(composer, " (more)");
  send_stack_select(&f.s, carol); /* saved at once on the switch */
  g_autofree gchar *bob_draft = send_stack_draft(&f.s, bob_room);
  g_assert_cmpstr(bob_draft, ==, "Draft for Bob (more)");
  assert_composer_text(composer, "");
  stack_type(composer, "Carol draft");
  send_stack_select(&f.s, bob);
  assert_composer_text(composer, "Draft for Bob (more)");
  g_autofree gchar *carol_draft = send_stack_draft(&f.s, carol_room);
  g_assert_cmpstr(carol_draft, ==, "Carol draft");

  /* A restart: the drafts come back from the encrypted store. */
  fixture_down(&f);
  fixture_up(&f, 960, 680);
  composer = send_stack_composer(&f.s);
  bob = gh_conversation_store_lookup(f.s.model, bob_room);
  carol = gh_conversation_store_lookup(f.s.model, carol_room);
  g_assert_nonnull(bob);
  g_assert_nonnull(carol);
  send_stack_select(&f.s, carol);
  assert_composer_text(composer, "Carol draft");
  send_stack_select(&f.s, bob);
  assert_composer_text(composer, "Draft for Bob (more)");
  fixture_clear(&f);
}

/* OB-1 (UI side): a message queued while the signer had not answered is
 * shown again after a restart as "Waiting for approval", the signer is asked
 * again exactly once, and the draft does not come back. Settled messages say
 * "Sent" after a restart too. */
static void
test_restart_queued(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  g_autofree gchar *room = g_strdup(gh_conversation_get_room_id(bob));
  send_stack_select(&f.s, bob);
  GhComposer *composer = send_stack_composer(&f.s);

  stack_type(composer, "Sent before");
  g_assert_true(gh_composer_send(composer));
  GhMessage *sent = stack_find(bob, "Sent before");
  wait_status(sent, GH_MESSAGE_STATUS_SENT);
  g_autofree gchar *sent_id = g_strdup(gh_message_get_rumor_id(sent));

  signer.hold = TRUE;
  stack_type(composer, "Queued");
  g_assert_true(gh_composer_send(composer));
  GhMessage *queued = stack_find(bob, "Queued");
  g_autofree gchar *queued_id = g_strdup(gh_message_get_rumor_id(queued));
  g_assert_cmpint(gh_message_get_status(queued), ==, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  gh_test_spin_until(signer_waiting, NULL);

  fixture_down(&f); /* the approval is revoked; the rows stay */
  guint publishes = pubs_to(INBOX_B, stack_hex[2]);
  fixture_up(&f, 960, 680);
  composer = send_stack_composer(&f.s);
  bob = gh_conversation_store_lookup(f.s.model, room);
  g_assert_nonnull(bob);
  send_stack_select(&f.s, bob);
  queued = gh_conversation_lookup_message(bob, queued_id);
  sent = gh_conversation_lookup_message(bob, sent_id);
  g_assert_nonnull(queued);
  g_assert_nonnull(sent);
  wait_status(queued, GH_MESSAGE_STATUS_WAITING_FOR_SIGNER);
  wait_status(sent, GH_MESSAGE_STATUS_SENT);
  gh_test_spin_until(signer_waiting, NULL);
  gh_test_run_until_idle();
  g_assert_cmpuint(signer.held->len, ==, 1); /* asked again, once */
  assert_composer_text(composer, "");
  g_autofree gchar *draft = send_stack_draft(&f.s, room);
  g_assert_null(draft);

  approve_until(queued, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, publishes + 1);
  fixture_clear(&f);
}

/* A note to self goes as one wrap to the account's own inbox. */
static void
test_note_to_self(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhConversation *self = receive(&f, 1, NULL, "Remember the milk");
  g_assert_null(gh_conversation_get_peers(self)[0]);
  send_stack_select(&f.s, self);
  GhComposer *composer = send_stack_composer(&f.s);
  g_assert_null(gh_composer_get_disabled_reason(composer));
  stack_type(composer, "And the bread");
  g_assert_true(gh_composer_send(composer));
  GhMessage *mine = stack_find(self, "And the bread");
  g_assert_cmpstr(gh_message_get_content(mine), ==, "And the bread");
  wait_status(mine, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(pubs.opens->len, ==, 1);
  g_assert_cmpuint(pubs_to(INBOX_A, stack_hex[1]), ==, 1);
  fixture_clear(&f);
}

static gboolean
reason_contains(gpointer data)
{
  const gpointer *args = data;
  const gchar *reason = gh_composer_get_disabled_reason(args[0]);
  return args[1] ? reason && strstr(reason, args[1]) : reason == NULL;
}

static void
wait_reason(GhComposer *composer, const gchar *fragment)
{
  gconstpointer args[] = { composer, fragment };
  gh_test_spin_until(reason_contains, args);
}

/* §7.7, §7.15: every reason sending is unavailable is said in place. */
static void
test_reasons_account_signer_store(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fake_secret_set_locked(f.s.secret, TRUE);
  send_stack_up(&f.s);
  g_assert_cmpint(gh_account_store_get_state(f.s.store), ==, GH_ACCOUNT_STORE_LOCKED);
  send_stack_window(&f.s, 960, 680);
  GhComposer *composer = send_stack_composer(&f.s);
  wait_reason(composer, "locked");
  g_assert_false(gh_composer_get_can_send(composer));

  fake_secret_set_locked(f.s.secret, FALSE);
  g_assert_true(gh_account_store_retry(f.s.store));
  wait_reason(composer, NULL);
  g_assert_cmpint(gh_account_store_get_state(f.s.store), ==, GH_ACCOUNT_STORE_OPEN);

  g_settings_set_string(f.s.settings, "signer-method", "nip46");
  while (g_main_context_iteration(NULL, FALSE)) {}
  g_assert_null(gh_composer_get_disabled_reason(composer));
  g_settings_set_string(f.s.settings, "signer-method", "auto");

  g_settings_set_string(f.s.settings, "current-npub", "");
  wait_reason(composer, "no Groundhog account");
  g_settings_set_string(f.s.settings, "current-npub", stack_npub[1]);
  wait_reason(composer, NULL);
  fixture_clear(&f);
}

static void
test_reason_no_signer_bus(void)
{
  Fixture f = { 0 };
  fixture_init(&f, NULL);
  send_stack_up(&f.s);
  send_stack_window(&f.s, 960, 680);
  wait_reason(send_stack_composer(&f.s), "no session bus");
  fixture_clear(&f);
}

static gboolean
banner_revealed(gpointer data)
{
  return adw_banner_get_revealed(ADW_BANNER(data));
}

static gboolean
banner_hidden(gpointer data)
{
  return !adw_banner_get_revealed(ADW_BANNER(data));
}

/* A NIP-17 room sends (W17): no reason in its composer; a recipient without
 * a message inbox (§7.15 state 11) gets the banner and the composer's reason
 * with Check Again, which tries the message again. */
static void
test_reasons_group_and_no_inbox(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhComposer *composer = send_stack_composer(&f.s);
  GhConversationView *view = send_stack_view(&f.s);
  GtkWidget *banner = template_child(view, GH_TYPE_CONVERSATION_VIEW, "banner");

  const guint carol[] = { 3, 0 };
  GhConversation *group = receive(&f, 2, carol, "Hike on Saturday?");
  send_stack_select(&f.s, group);
  wait_reason(composer, NULL); /* W17: a room sends */

  GhConversation *dave = receive(&f, 3, NULL, "Hello, it's Carol");
  send_stack_select(&f.s, dave);
  wait_reason(composer, NULL);

  /* G20a: a NIP-29 relay group has no peers, yet it is no note to self: the
   * composer never sends into it through the NIP-17 outbox. */
  g_autofree gchar *group_event = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[[\"h\",\"hikers\"]],\"content\":\"Trail report\"}",
    stack_hex[f.s.key], g_get_real_time() / G_USEC_PER_SEC - 300);
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) relay_message = gh_message_new_from_nip29_event(
    stack_hex[f.s.key], "wss://groups.test.invalid", group_event, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(f.s.model, relay_message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  GhConversation *relay_group =
    gh_conversation_store_lookup(f.s.model, gh_message_get_room_id(relay_message));
  g_assert_nonnull(relay_group);
  g_assert_null(gh_conversation_get_peers(relay_group)[0]);
  send_stack_select(&f.s, relay_group);
  wait_reason(composer, "group"); /* from none: not a stale reason */
  g_assert_cmpuint(pubs.opens->len, ==, 0);
  send_stack_select(&f.s, dave);
  wait_reason(composer, NULL);

  stack_type(composer, "Hi Carol");
  g_assert_true(gh_composer_send(composer));
  GhMessage *mine = stack_find(dave, "Hi Carol");
  wait_status(mine, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  wait_reason(composer, "hasn't set up private messaging");
  g_assert_nonnull(strstr(gh_composer_get_disabled_reason(composer),
                          gh_conversation_get_title(dave)));
  gh_test_spin_until(banner_revealed, banner);
  g_assert_nonnull(strstr(adw_banner_get_title(ADW_BANNER(banner)),
                          gh_conversation_get_title(dave)));
  GtkWidget *check = template_child(composer, GH_TYPE_COMPOSER, "disabled_button");
  g_assert_true(gtk_widget_get_visible(check));
  g_assert_cmpuint(pubs.opens->len, ==, 0); /* nothing was published */

  g_hash_table_insert(f.directory->inboxes, g_strdup(stack_hex[3]),
                      g_strdup("wss://inbox-c.test.invalid"));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f.s.window), "send.check-inbox", NULL));
  wait_status(mine, GH_MESSAGE_STATUS_SENT);
  wait_reason(composer, NULL);
  gh_test_spin_until(banner_hidden, banner);
  g_assert_cmpuint(pubs_to("wss://inbox-c.test.invalid", stack_hex[3]), ==, 1);
  fixture_clear(&f);
}

/* nostrc-8kb2 (W21 review M4): the real gh-send-ui.c routing with two
 * sending engines beside the NIP-17 outbox (G20b relay groups, qp24.13
 * encrypted groups), as gh-group-ui.c and gh-mls-ui.c attach them: each
 * conversation's reason, send, retry and delivery details come from the
 * delegate whose handles() is TRUE and no other; a NIP-17 room still goes
 * through the outbox; no draft is stored for a delegated room; a second
 * add of the same engine replaces it; set_delegate() replaces every one. */
typedef struct {
  const gchar *refusal;   /* the reason while set */
  guint reasons, sends, retries, reports;
  gchar *last_text;
} FakeEngine;

static gboolean
fake_handles_relay(GhConversation *conversation, gpointer data)
{
  (void)data;
  return gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29;
}

static gboolean
fake_handles_mls(GhConversation *conversation, gpointer data)
{
  (void)data;
  return gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_MLS;
}

static gchar *
fake_reason(GhConversation *conversation, gpointer data)
{
  (void)conversation;
  FakeEngine *engine = data;
  engine->reasons++;
  return g_strdup(engine->refusal);
}

static gboolean
fake_send(GhConversation *conversation, const gchar *text, gpointer data, GError **error)
{
  (void)conversation;
  (void)error;
  FakeEngine *engine = data;
  engine->sends++;
  g_free(engine->last_text);
  engine->last_text = g_strdup(text);
  return TRUE;
}

static gboolean
fake_retry(GhMessage *message, gpointer data, GError **error)
{
  (void)message;
  (void)error;
  ((FakeEngine *)data)->retries++;
  return TRUE;
}

static GhDeliveryReport *
fake_report(GhMessage *message, gpointer data)
{
  (void)message;
  ((FakeEngine *)data)->reports++;
  return gh_delivery_report_new();
}

static const GhSendUiDelegate relay_engine = {
  fake_handles_relay, fake_reason, fake_send, fake_retry, fake_report,
};
static const GhSendUiDelegate mls_engine = {
  fake_handles_mls, fake_reason, fake_send, fake_retry, fake_report,
};

static void
test_delegates(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhComposer *composer = send_stack_composer(&f.s);
  GhConversationView *view = send_stack_view(&f.s);
  const gchar *me = stack_hex[f.s.key];
  g_autoptr(GError) error = NULL;

  /* A NIP-17 room, a relay group and an encrypted group, each with an own
   * message. */
  GhConversation *dave = receive(&f, 3, NULL, "Hello, it's Carol");
  g_autofree gchar *dave_room = g_strdup(gh_conversation_get_room_id(dave));
  g_autofree gchar *relay_event = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[[\"h\",\"hikers\"]],\"content\":\"Trail report\"}",
    me, g_get_real_time() / G_USEC_PER_SEC - 300);
  g_autoptr(GhMessage) relay_message = gh_message_new_from_nip29_event(
    me, "wss://groups.test.invalid", relay_event, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(f.s.model, relay_message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  const gchar *relay_room = gh_message_get_room_id(relay_message);
  GhConversation *relay_group = gh_conversation_store_lookup(f.s.model, relay_room);
  g_assert_nonnull(relay_group);
  const gchar *gid = "0123456789abcdef0123456789abcdef";
  g_autofree gchar *mls_room = gh_message_mls_room_id(gid);
  GhConversation *secret = gh_conversation_store_ensure_group(f.s.model, mls_room, "Secret");
  g_assert_nonnull(secret);
  g_autofree gchar *inner = g_strdup_printf(
    "{\"kind\":9,\"pubkey\":\"%s\",\"created_at\":%" G_GINT64_FORMAT ","
    "\"tags\":[],\"content\":\"Sealed\"}", me, g_get_real_time() / G_USEC_PER_SEC - 200);
  g_autoptr(GhMessage) mls_message = gh_message_new_from_mls(me, gid, inner, &error);
  g_assert_no_error(error);
  g_assert_cmpint(gh_conversation_store_add_message(f.s.model, mls_message, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);

  /* Attached as the application does: G20b sets its delegate, then the
   * encrypted-group UI adds its own (twice: the second replaces the first). */
  FakeEngine relay = { 0 }, mls = { 0 }, stale = { 0 };
  gh_send_ui_set_delegate(f.s.window, &relay_engine, &relay);
  gh_send_ui_add_delegate(f.s.window, &mls_engine, &stale);
  gh_send_ui_add_delegate(f.s.window, &mls_engine, &mls);

  /* The encrypted group: its engine's reason and send, nothing else's. */
  send_stack_select(&f.s, secret);
  wait_reason(composer, NULL);
  g_assert_cmpuint(mls.reasons, >, 0);
  stack_type(composer, "to the group");
  g_assert_true(gh_composer_send(composer));
  g_assert_cmpuint(mls.sends, ==, 1);
  g_assert_cmpstr(mls.last_text, ==, "to the group");
  g_assert_cmpuint(relay.sends + stale.sends + stale.reasons, ==, 0);
  g_assert_cmpuint(pubs.opens->len, ==, 0);   /* never through the NIP-17 outbox */
  /* No draft for it (drafts are NIP-17 store rows): a switch saves the
   * shown NIP-17 room's draft at once, so the text would leak into one. */
  stack_type(composer, "unsent group text");
  send_stack_select(&f.s, dave);
  assert_composer_text(composer, "");
  g_autofree gchar *after_mls = send_stack_draft(&f.s, dave_room);
  g_assert_null(after_mls);
  send_stack_select(&f.s, secret);
  assert_composer_text(composer, "");         /* nothing restored */

  /* The relay group: its reason (refusing), no send. */
  relay.refusal = "Relay says no";
  send_stack_select(&f.s, relay_group);
  wait_reason(composer, "Relay says no");
  guint mls_reasons = mls.reasons;
  relay.refusal = NULL;
  gh_send_ui_refresh(f.s.window);
  wait_reason(composer, NULL);
  g_assert_cmpuint(mls.reasons, ==, mls_reasons);
  stack_type(composer, "to the relay group");
  g_assert_true(gh_composer_send(composer));
  g_assert_cmpuint(relay.sends, ==, 1);
  g_assert_cmpstr(relay.last_text, ==, "to the relay group");
  g_assert_cmpuint(mls.sends, ==, 1);
  stack_type(composer, "unsent relay text");
  send_stack_select(&f.s, dave);
  assert_composer_text(composer, "");
  g_autofree gchar *after_relay = send_stack_draft(&f.s, dave_room);
  g_assert_null(after_relay);
  send_stack_select(&f.s, relay_group);
  assert_composer_text(composer, "");

  /* Retry and delivery details go to the message's own engine. */
  g_signal_emit_by_name(view, "retry-requested", mls_message);
  g_assert_cmpuint(mls.retries, ==, 1);
  g_assert_cmpuint(relay.retries, ==, 0);
  g_signal_emit_by_name(view, "retry-requested", relay_message);
  g_assert_cmpuint(relay.retries, ==, 1);
  g_assert_cmpuint(mls.retries, ==, 1);
  g_autoptr(GhDeliveryReport) mls_report = gh_conversation_view_dup_delivery_report(view,
                                                                                    mls_message);
  g_assert_nonnull(mls_report);
  g_assert_cmpuint(mls.reports, ==, 1);
  g_assert_cmpuint(relay.reports, ==, 0);
  g_autoptr(GhDeliveryReport) relay_report =
    gh_conversation_view_dup_delivery_report(view, relay_message);
  g_assert_nonnull(relay_report);
  g_assert_cmpuint(relay.reports, ==, 1);
  g_assert_cmpuint(stale.retries + stale.reports, ==, 0);

  /* A NIP-17 room is still the outbox's: no engine is asked, a draft is kept. */
  send_stack_select(&f.s, dave);
  wait_reason(composer, NULL);
  stack_type(composer, "Draft for Carol");
  send_stack_select(&f.s, secret);
  g_autofree gchar *dave_draft = send_stack_draft(&f.s, dave_room);
  g_assert_cmpstr(dave_draft, ==, "Draft for Carol");
  assert_composer_text(composer, "");
  send_stack_select(&f.s, dave);
  gh_composer_set_text(composer, "");
  stack_type(composer, "Hi Carol");
  g_assert_true(gh_composer_send(composer));
  g_assert_nonnull(stack_find(dave, "Hi Carol"));
  g_assert_cmpuint(relay.sends, ==, 1);
  g_assert_cmpuint(mls.sends, ==, 1);

  /* set_delegate() replaces every delegate: the encrypted group has none. */
  gh_send_ui_set_delegate(f.s.window, &relay_engine, &relay);
  send_stack_select(&f.s, secret);
  wait_reason(composer, "Encrypted groups aren't available in this version yet.");
  g_free(relay.last_text);
  g_free(mls.last_text);
  fixture_clear(&f);
}

typedef struct {
  Fixture *f;
  GhMessage *message;
  GhMessageStatus status;
} AdvanceWait;

static void approve_advancing(Fixture *f, GhMessage *message, GhMessageStatus status);

/* nostrc-lff5: a room where nobody has set up private messaging gets the
 * banner too, speaking of everyone (not only the composer's reason); once
 * one of them has, Check Again sends to them and the banner goes. */
static void
test_room_nobody_has_inbox(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  f.s.clock = gh_clock_new_fake(g_get_real_time());
  fixture_up(&f, 960, 680);
  GhComposer *composer = send_stack_composer(&f.s);
  GhConversationView *view = send_stack_view(&f.s);
  GtkWidget *banner = template_child(view, GH_TYPE_CONVERSATION_VIEW, "banner");
  const guint dave[] = { 4, 0 };
  GhConversation *room = receive(&f, 3, dave, "Anyone around?");
  send_stack_select(&f.s, room);
  wait_reason(composer, NULL);
  stack_type(composer, "Me, but nobody can read this yet");
  g_assert_true(gh_composer_send(composer));
  GhMessage *mine = stack_find(room, "Me, but nobody can read this yet");
  wait_status(mine, GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX);
  wait_reason(composer, "No one in this conversation");
  gh_test_spin_until(banner_revealed, banner);
  g_assert_cmpstr(adw_banner_get_title(ADW_BANNER(banner)), ==,
                  "No one in this conversation has set up private messaging yet");
  g_assert_cmpuint(pubs.opens->len, ==, 0); /* nothing was published */

  g_hash_table_insert(f.directory->inboxes, g_strdup(stack_hex[3]), g_strdup(INBOX_C));
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(f.s.window), "send.check-inbox", NULL));
  approve_advancing(&f, mine, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  wait_reason(composer, NULL);
  gh_test_spin_until(banner_hidden, banner);
  g_assert_cmpuint(pubs_to(INBOX_C, stack_hex[3]), ==, 1);
  fixture_clear(&f);
}

/* Approves the signer and lets the fake clock run (a room's wraps go out
 * U(0, 3) s apart, charter §4.5 S4) until the message has status. */
static gboolean
advance_until_status(gpointer data)
{
  AdvanceWait *wait = data;
  if (signer.held->len)
    gh_test_signer_release_all(&signer);
  if (gh_message_get_status(wait->message) == wait->status)
    return TRUE;
  gh_clock_fake_advance(wait->f->s.clock, G_USEC_PER_SEC);
  return FALSE;
}

static void
approve_advancing(Fixture *f, GhMessage *message, GhMessageStatus status)
{
  AdvanceWait wait = { f, message, status };
  gh_test_spin_until(advance_until_status, &wait);
}

/* W17: a message in a NIP-17 room is one rumor sealed and gift-wrapped for
 * each person, each wrap only to that person's inbox relays, and the
 * self-copy to the account's own. Carol has no message inbox: nothing is
 * sent for her anywhere, her row in the delivery details says so, and the
 * message is "Sent to some people" naming who has it. Once she sets one
 * up, Try Again sends her the same stored wrap (no signer call) and the
 * message is Sent; Bob's wrap is not sent again. */
static void
test_room_send(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  f.s.clock = gh_clock_new_fake(g_get_real_time());
  fixture_up(&f, 960, 680);
  const guint carol[] = { 3, 0 };
  GhConversation *room = receive(&f, 2, carol, "Hike on Saturday?");
  send_stack_select(&f.s, room);
  GhComposer *composer = send_stack_composer(&f.s);
  GhConversationView *view = send_stack_view(&f.s);
  wait_reason(composer, NULL);

  stack_type(composer, "Count me in");
  g_assert_true(gh_composer_send(composer));
  GhMessage *mine = stack_find(room, "Count me in");
  approve_advancing(&f, mine, GH_MESSAGE_STATUS_PARTIALLY_SENT);
  gh_test_run_until_idle();
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, 1);
  g_assert_cmpuint(pubs_to(INBOX_A, stack_hex[1]), ==, 1);
  g_assert_cmpuint(pubs.opens->len, ==, 2); /* nothing for Carol, anywhere */
  {
    g_autoptr(GhDeliveryReport) report = report_of(&f, mine);
    g_assert_nonnull(report);
    g_assert_cmpuint(report->targets->len, ==, 3);
    GhDeliveryTarget *to_bob = g_ptr_array_index(report->targets, 0);
    GhDeliveryTarget *to_carol = g_ptr_array_index(report->targets, 1);
    GhDeliveryTarget *to_self = g_ptr_array_index(report->targets, 2);
    g_assert_cmpstr(to_bob->recipient, ==, stack_hex[2]);
    g_assert_cmpstr(to_bob->relay_url, ==, INBOX_B);
    g_assert_true(to_bob->accepted);
    g_assert_cmpstr(to_carol->recipient, ==, stack_hex[3]);
    g_assert_null(to_carol->relay_url);
    g_assert_false(to_carol->accepted);
    g_assert_nonnull(strstr(to_carol->outcome, "haven't set up private messaging"));
    g_assert_null(to_self->recipient);
    g_assert_cmpstr(to_self->relay_url, ==, INBOX_A);
    g_autofree gchar *bob = gh_message_row_display_name(stack_hex[2]);
    g_autofree gchar *carol_name = gh_message_row_display_name(stack_hex[3]);
    g_autofree gchar *expected = g_strdup_printf("Sent to %s. Not sent to %s.", bob, carol_name);
    g_assert_cmpstr(report->detail, ==, expected);
  }
  /* Someone has it: the composer stays usable, with no banner. */
  g_assert_null(gh_composer_get_disabled_reason(composer));

  g_hash_table_insert(f.directory->inboxes, g_strdup(stack_hex[3]), g_strdup(INBOX_C));
  guint calls = signer.calls;
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(view), "conversation.retry-message", "s",
                                           gh_message_get_rumor_id(mine)));
  approve_advancing(&f, mine, GH_MESSAGE_STATUS_SENT);
  g_assert_cmpuint(pubs_to(INBOX_C, stack_hex[3]), ==, 1);
  g_assert_cmpuint(pubs_to(INBOX_B, stack_hex[2]), ==, 1);
  g_assert_cmpuint(signer.calls, ==, calls); /* the stored wrap, not re-sealed */
  fixture_clear(&f);
}

typedef struct {
  GListModel *model;
  guint count;
} CountWait;

static gboolean
count_reached(gpointer data)
{
  CountWait *wait = data;
  return g_list_model_get_n_items(wait->model) >= wait->count;
}

typedef struct {
  const gchar *url;
  Req *not;
} NewReq;

static gboolean
new_inbox_req(gpointer data)
{
  NewReq *wait = data;
  Req *req = open_req(wait->url, FALSE);
  return req && req != wait->not;
}

/* §7.15 state 12: messages the signer did not unlock are counted in the
 * view, and Unlock offers them to the signer again. */
static void
test_locked_messages(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  send_stack_select(&f.s, bob);
  GhConversationView *view = send_stack_view(&f.s);
  GtkWidget *row = template_child(view, GH_TYPE_CONVERSATION_VIEW, "locked_row");
  GtkWidget *label = template_child(view, GH_TYPE_CONVERSATION_VIEW, "locked_label");
  g_assert_false(gtk_widget_get_visible(row));

  signer.deny = TRUE;
  Req *req = open_req(INBOX_A, FALSE);
  const guint to_me[] = { 1, 0 };
  g_autofree gchar *wrap = stack_craft_wrap(2, 1, to_me, g_get_real_time() / G_USEC_PER_SEC - 30,
                                            "The one the signer declined");
  gh_relay_scope_event(req->scope, INBOX_A, wrap);
  gh_test_spin_until(widget_visible, row);
  g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==,
                  "Waiting for Grotto to unlock 1 message");
  guint before = g_list_model_get_n_items(G_LIST_MODEL(bob));

  signer.deny = FALSE;
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(view), "conversation.unlock-messages",
                                           NULL));
  NewReq again = { INBOX_A, req };
  gh_test_spin_until(new_inbox_req, &again); /* the same REQ, again */
  g_assert_false(gtk_widget_get_visible(row));
  gh_relay_scope_event(open_req(INBOX_A, FALSE)->scope, INBOX_A, wrap);
  CountWait more = { G_LIST_MODEL(bob), before + 1 };
  gh_test_spin_until(count_reached, &more);
  g_assert_true(gh_message_get_sender(stack_find(bob, "The one the signer declined")) != NULL);
  g_assert_false(gtk_widget_get_visible(row));
  fixture_clear(&f);
}

static GtkWidget *
focus_of(GtkWidget *window)
{
  return gtk_root_get_focus(GTK_ROOT(window));
}

typedef struct {
  GhSidebarPage *sidebar;
  const gchar *page;
} PageWait;

static gboolean
sidebar_page_is(gpointer data)
{
  PageWait *wait = data;
  return g_strcmp0(gtk_stack_get_visible_child_name(gh_sidebar_page_get_stack(wait->sidebar)),
                   wait->page) == 0;
}

/* nostrc-qp24.8.1: an account-state transition is announced but never takes
 * keyboard focus from someone typing; a transition that takes the
 * conversation away hands focus to the page's action (when the window is
 * active; otherwise when it is activated again). */
static void
test_focus_guard(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GtkWidget *window = GTK_WIDGET(f.s.window);
  GhSidebarPage *sidebar = gh_window_get_sidebar(f.s.window);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  g_autofree gchar *room = g_strdup(gh_conversation_get_room_id(bob));
  send_stack_select(&f.s, bob);
  GhComposer *composer = send_stack_composer(&f.s);

  /* Typing in the composer: a refresh that changes nothing moves nothing. */
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(composer)));
  stack_type(composer, "typing…");
  guint announced = gh_account_ui_get_announcements(f.s.window);
  gh_account_controller_refresh(f.s.accounts);
  gh_test_run_until_idle();
  g_assert_true(text_view_focused(composer));
  g_assert_cmpuint(gh_account_ui_get_announcements(f.s.window), ==, announced);

  /* Typing in the search field: the account goes, the page changes and is
   * announced, and the search keeps the keyboard. */
  gh_sidebar_page_start_search(sidebar);
  GtkWidget *search = focus_of(window);
  g_assert_true(GTK_IS_EDITABLE(search));
  GhTestActiveSpan span;
  gh_test_active_span_begin(&span, GTK_WINDOW(window));
  g_settings_set_string(f.s.settings, "current-npub", "");
  PageWait unselected = { sidebar, "account-unselected" };
  gh_test_spin_until(sidebar_page_is, &unselected);
  gh_test_run_until_idle();
  g_assert_true(focus_of(window) == search);
  gboolean either = FALSE;
  guint step = gh_test_active_span_expect(&span, 1, &either);
  guint made = gh_account_ui_get_announcements(f.s.window) - announced;
  if (either) /* nostrc-9g6e */
    g_assert_cmpuint(made, <=, 1);
  else
    g_assert_cmpuint(made, ==, step);

  /* Back, and typing in the composer when the account goes: the composer
   * went with the conversation, so focus moves to the page's Choose Account
   * button once the window is active. */
  gtk_window_set_focus(GTK_WINDOW(window), NULL);
  g_settings_set_string(f.s.settings, "current-npub", stack_npub[1]);
  RoomWait back = { f.s.model, room };
  gh_test_spin_until(room_listed, &back);
  send_stack_select(&f.s, gh_conversation_store_lookup(f.s.model, room));
  g_assert_true(gtk_widget_grab_focus(GTK_WIDGET(composer)));
  g_settings_set_string(f.s.settings, "current-npub", "");
  gh_test_spin_until(sidebar_page_is, &unselected);
  gh_test_run_until_idle();
  GtkWidget *focus = focus_of(window);
  if (gtk_window_is_active(GTK_WINDOW(window))) {
    g_assert_nonnull(focus);
    g_assert_true(GTK_IS_BUTTON(focus));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(focus)), ==, "_Choose Account…");
  } else {
    g_assert_false(text_view_focused(composer));
  }
  fixture_clear(&f);
}

/* ---- the disappearing timer before sending (charter §3.7; W14 review B1) --------------- */

/* The indicator's text, tooltip and accessible label. */
static void
assert_timer_shown(GhComposer *composer, const gchar *duration)
{
  GtkWidget *slot = GTK_WIDGET(gh_composer_get_timer_slot(composer));
  GtkWidget *button = template_child(composer, GH_TYPE_COMPOSER, "timer_button");
  GtkLabel *label = GTK_LABEL(template_child(composer, GH_TYPE_COMPOSER, "timer_label"));
  if (!duration) {
    g_assert_false(gtk_widget_get_visible(slot));
    g_assert_cmpint(gh_composer_get_disappearing_timer(composer), ==, 0);
    return;
  }
  g_assert_true(gtk_widget_get_visible(slot));
  g_assert_true(gtk_widget_is_ancestor(button, slot));
  g_assert_cmpstr(gtk_label_get_text(label), ==, duration);
  g_autofree gchar *said = g_strdup_printf("Messages you send disappear after %s", duration);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(button), ==, said);
  /* The accessible label, where there is an AT context (not with the
   * GTK_A11Y=none that stack_gtk_and_bus_up() sets on macOS). */
  g_autoptr(GtkATContext) at = gtk_accessible_get_at_context(GTK_ACCESSIBLE(button));
  if (at)
    gtk_test_accessible_assert_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                        said);
  /* It opens Conversation Info, where the timer is changed. */
  g_assert_cmpstr(gtk_actionable_get_action_name(GTK_ACTIONABLE(button)), ==,
                  "win.conversation-info");
}

/* The widget alone: hidden while off, "1 day" / "1 week" / "4 weeks" with
 * the full sentence for tooltips and assistive technologies. */
static void
test_timer_indicator(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);
  gtk_window_present(window);
  assert_timer_shown(composer, NULL);
  gh_composer_set_disappearing_timer(composer, GH_EXPIRY_TIMER_DAY);
  assert_timer_shown(composer, "1 day");
  g_object_set(composer, "disappearing-timer", GH_EXPIRY_TIMER_WEEK, NULL);
  assert_timer_shown(composer, "1 week");
  gh_composer_set_disappearing_timer(composer, GH_EXPIRY_TIMER_FOUR_WEEKS);
  assert_timer_shown(composer, "4 weeks");
  gh_composer_set_disappearing_timer(composer, 2 * GH_EXPIRY_TIMER_DAY);
  assert_timer_shown(composer, "2 days");
  gh_composer_set_disappearing_timer(composer, 90 * 60); /* never shown shorter */
  assert_timer_shown(composer, "2 hours");
  gh_composer_set_disappearing_timer(composer, GH_EXPIRY_TIMER_OFF);
  assert_timer_shown(composer, NULL);
  gh_composer_set_disappearing_timer(composer, -5);
  assert_timer_shown(composer, NULL);
  gtk_window_destroy(window);
  sink_clear(&sink);
}

static void
on_info_requested(GSimpleAction *action, GVariant *parameter, guint *activations)
{
  (void)action;
  (void)parameter;
  (*activations)++;
}

static gboolean
activated_once(gpointer data)
{
  return *(guint *)data == 1;
}

static GhExpiry *
stack_expiry(Fixture *f)
{
  GhExpiryConfig config = {
    .store = gh_account_store_get_store(f->s.store),
    .conversations = gh_account_store_get_conversations(f->s.store),
  };
  g_assert_nonnull(config.store);
  return gh_expiry_new(&config);
}

/* W14 review B1: a conversation's timer is visible in the composer before
 * sending, live as Conversation Info (gh_expiry_set_timer()) changes it,
 * per conversation, hidden while off, and the message then sent does
 * disappear. The window lets go of a GhExpiry when told (the application
 * does so before disposing it), and its signals stop reaching it. */
static void
test_timer_live(void)
{
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  GhConversation *bob = receive(&f, 2, NULL, "Hi from Bob");
  GhConversation *carol = receive(&f, 3, NULL, "Hi from Carol");
  g_autofree gchar *bob_room = g_strdup(gh_conversation_get_room_id(bob));
  g_autofree gchar *carol_room = g_strdup(gh_conversation_get_room_id(carol));
  GhComposer *composer = send_stack_composer(&f.s);
  guint activations = 0;
  g_autoptr(GSimpleAction) info = g_simple_action_new("conversation-info", NULL);
  g_signal_connect(info, "activate", G_CALLBACK(on_info_requested), &activations);
  g_action_map_add_action(G_ACTION_MAP(f.s.window), G_ACTION(info));

  GhExpiry *expiry = stack_expiry(&f);
  g_assert_true(gh_expiry_set_timer(expiry, carol_room, GH_EXPIRY_TIMER_WEEK, NULL));
  gh_send_ui_set_expiry(f.s.window, expiry);
  send_stack_select(&f.s, bob);
  assert_timer_shown(composer, NULL);

  /* Changed elsewhere (Conversation Info): shown at once. */
  g_assert_true(gh_expiry_set_timer(expiry, bob_room, GH_EXPIRY_TIMER_DAY, NULL));
  assert_timer_shown(composer, "1 day");
  /* Another conversation's change says nothing here. */
  g_assert_true(gh_expiry_set_timer(expiry, carol_room, GH_EXPIRY_TIMER_FOUR_WEEKS, NULL));
  assert_timer_shown(composer, "1 day");
  send_stack_select(&f.s, carol);
  assert_timer_shown(composer, "4 weeks");
  send_stack_select(&f.s, bob);
  assert_timer_shown(composer, "1 day");

  /* Activating it opens Conversation Info. */
  GtkWidget *button = template_child(composer, GH_TYPE_COMPOSER, "timer_button");
  g_assert_true(gtk_widget_is_sensitive(button));
  /* A keyboard activation clicks after the button's press feedback. */
  g_assert_true(gtk_widget_activate(button));
  gh_test_spin_until(activated_once, &activations);
  g_assert_cmpuint(activations, ==, 1);

  /* What it says holds: the message sent now expires a day after it. */
  signer.hold = TRUE;
  stack_type(composer, "Gone tomorrow");
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  GhMessage *mine = stack_find(bob, "Gone tomorrow");
  g_assert_cmpint(gh_message_get_expires_at(mine), ==,
                  gh_message_get_created_at(mine) + GH_EXPIRY_TIMER_DAY);

  /* Off: hidden. */
  g_assert_true(gh_expiry_set_timer(expiry, bob_room, GH_EXPIRY_TIMER_OFF, NULL));
  assert_timer_shown(composer, NULL);
  g_assert_true(gh_expiry_set_timer(expiry, bob_room, GH_EXPIRY_TIMER_WEEK, NULL));
  assert_timer_shown(composer, "1 week");

  /* The store's GhExpiry goes (as at store-closed): nothing is claimed, and
   * the old object's signals no longer reach the window. */
  gh_send_ui_set_expiry(f.s.window, NULL);
  assert_timer_shown(composer, NULL);
  g_assert_true(gh_expiry_set_timer(expiry, bob_room, GH_EXPIRY_TIMER_DAY, NULL));
  assert_timer_shown(composer, NULL);
  /* A new one (the next store open) is followed again. */
  GhExpiry *next = stack_expiry(&f);
  gh_send_ui_set_expiry(f.s.window, next);
  assert_timer_shown(composer, "1 day");
  gh_send_ui_set_expiry(f.s.window, NULL);
  g_object_run_dispose(G_OBJECT(next));
  g_object_unref(next);
  g_object_run_dispose(G_OBJECT(expiry));
  g_object_unref(expiry);
  signer.hold = FALSE;
  fixture_clear(&f);
}

/* ---- the input method's Enter (charter §7.7; W14 review non-blocking #5) --------------- */

/* The text view's own input method context (its key controller's). */
static GtkIMContext *
text_view_im(GhComposer *composer)
{
  GtkWidget *text_view = GTK_WIDGET(gh_composer_get_text_view(composer));
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(text_view);
  for (guint i = 0; i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GtkEventController) controller = g_list_model_get_item(controllers, i);
    if (GTK_IS_EVENT_CONTROLLER_KEY(controller) &&
        gtk_event_controller_key_get_im_context(GTK_EVENT_CONTROLLER_KEY(controller)))
      return gtk_event_controller_key_get_im_context(GTK_EVENT_CONTROLLER_KEY(controller));
  }
  g_error("the text view has no input method context");
  return NULL;
}

/* A key press through the input method as the display would deliver it
 * (real key events, translated by the display's keymap). */
static gboolean
im_press(GtkIMContext *im, GtkWidget *widget, guint keyval, GdkModifierType state)
{
  GdkDisplay *display = gtk_widget_get_display(widget);
  g_autofree GdkKeymapKey *keys = NULL;
  gint n = 0;
  g_assert_true(gdk_display_map_keyval(display, keyval, &keys, &n));
  g_assert_cmpint(n, >, 0);
  GdkSurface *surface = gtk_native_get_surface(gtk_widget_get_native(widget));
  GdkDevice *keyboard = gdk_seat_get_keyboard(gdk_display_get_default_seat(display));
  g_assert_nonnull(surface);
  g_assert_nonnull(keyboard);
  return gtk_im_context_filter_key(im, TRUE, surface, keyboard, GDK_CURRENT_TIME,
                                   keys[0].keycode, state, keys[0].group);
}

/* While an input method composes text, Enter is the input method's: it
 * neither sends the half-typed text nor becomes a newline. Driven through a
 * real GtkIMContextSimple hex sequence (Ctrl+Shift+U e 9 = "é") on the text
 * view's own input method context. Once it has committed, Enter sends. */
static void
test_preedit_enter(void)
{
  Sink sink = { .accept = TRUE };
  GtkWindow *window = NULL;
  GhComposer *composer = lone_composer(&window, &sink);
  gtk_window_present(window);
  gh_test_spin_until(is_mapped, composer);
  GtkWidget *text_view = GTK_WIDGET(gh_composer_get_text_view(composer));
  g_assert_true(gtk_widget_grab_focus(text_view));
  GtkIMContext *im = text_view_im(composer);
  g_assert_true(GTK_IS_IM_MULTICONTEXT(im));
  gtk_im_multicontext_set_context_id(GTK_IM_MULTICONTEXT(im), "gtk-im-context-simple");

  stack_type(composer, "Caf");
  const GdkModifierType hex = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
  g_assert_true(im_press(im, text_view, GDK_KEY_u, hex)); /* starts a preedit */
  g_assert_true(im_press(im, text_view, GDK_KEY_e, hex));
  g_assert_true(im_press(im, text_view, GDK_KEY_9, hex));
  g_autofree gchar *preedit = NULL;
  gtk_im_context_get_preedit_string(im, &preedit, NULL, NULL);
  g_assert_true(preedit && *preedit);
  assert_composer_text(composer, "Caf");

  /* Enter (and Shift+Enter, Ctrl+Enter) during the preedit: taken, nothing
   * sent, no newline. */
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_true(stack_press(composer, GDK_KEY_Return, GDK_SHIFT_MASK));
  g_assert_true(stack_press(composer, GDK_KEY_KP_Enter, GDK_CONTROL_MASK));
  g_assert_cmpuint(sink.sends, ==, 0);
  assert_composer_text(composer, "Caf");

  /* The input method gets that Enter and commits "é"; the preedit ends. */
  g_assert_true(im_press(im, text_view, GDK_KEY_Return, hex));
  assert_composer_text(composer, "Café");
  g_clear_pointer(&preedit, g_free);
  gtk_im_context_get_preedit_string(im, &preedit, NULL, NULL);
  g_assert_cmpstr(preedit, ==, "");
  g_assert_cmpuint(sink.sends, ==, 0);

  /* Now Enter sends the whole word. */
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_cmpuint(sink.sends, ==, 1);
  g_assert_cmpstr(sink.last, ==, "Café");
  assert_composer_text(composer, "");

  /* A preedit the input method drops (a reset, e.g. focus moving away) ends
   * it too: Enter sends again. */
  stack_type(composer, "Bye");
  g_assert_true(im_press(im, text_view, GDK_KEY_u, hex));
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_cmpuint(sink.sends, ==, 1);
  gtk_im_context_reset(im);
  g_assert_true(stack_press(composer, GDK_KEY_Return, 0));
  g_assert_cmpuint(sink.sends, ==, 2);
  g_assert_cmpstr(sink.last, ==, "Bye");

  gtk_window_destroy(window);
  sink_clear(&sink);
}

/* ---- screenshots (opt-in evidence) --------------------------------------------------- */

static gboolean
view_settled(gpointer data)
{
  Fixture *f = data;
  GhConversationView *view = send_stack_view(&f->s);
  return gtk_widget_get_mapped(GTK_WIDGET(view)) && gh_conversation_view_get_at_latest(view);
}

/* <dir>/groundhog-g13-<name>-light.png and -dark.png of the window. */
static void
shoot(Fixture *f, const char *dir, const char *name)
{
  static const struct {
    AdwColorScheme scheme;
    const char *name;
  } schemes[] = {
    { ADW_COLOR_SCHEME_FORCE_LIGHT, "light" },
    { ADW_COLOR_SCHEME_FORCE_DARK, "dark" },
  };
  AdwStyleManager *style = adw_style_manager_get_default();
  for (guint i = 0; i < G_N_ELEMENTS(schemes); i++) {
    adw_style_manager_set_color_scheme(style, schemes[i].scheme);
    gh_test_spin_until(view_settled, f);
    gh_test_run_until_idle();
    gtk_test_widget_wait_for_draw(GTK_WIDGET(f->s.window));
    g_autofree char *path = g_strdup_printf("%s/groundhog-g13-%s-%s.png", dir, name,
                                            schemes[i].name);
    stack_save_png(GTK_WIDGET(f->s.window), path);
  }
  adw_style_manager_set_color_scheme(style, ADW_COLOR_SCHEME_DEFAULT);
}

static void
new_window(Fixture *f, gint width, gint height, GhConversation *conversation)
{
  gtk_window_destroy(GTK_WINDOW(f->s.window));
  gh_test_run_until_idle();
  send_stack_window(&f->s, width, height);
  gh_test_spin_until(is_mapped, f->s.window);
  send_stack_select(&f->s, conversation);
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
  Fixture f = { 0 };
  fixture_init(&f, bus.client);
  fixture_up(&f, 960, 680);
  gtk_window_set_title(GTK_WINDOW(f.s.window), "Groundhog");
  receive(&f, 3, NULL, "Did you get the photos?");
  receive(&f, 2, NULL, "Hey! Are we still on for Saturday?");
  GhConversation *bob = receive(&f, 2, NULL, "I found a trail near the lake");
  send_stack_select(&f.s, bob);
  GhComposer *composer = send_stack_composer(&f.s);

  /* ASCII: every text that goes through a signer here is (nostrc-30gt on
   * macOS: libnostr's escaping of UTF-8 depends on the locale). */
  stack_type(composer, "Yes! Trailhead at 8? I'll bring coffee.");
  shoot(&f, dir, "compose");
  new_window(&f, 360, 640, bob);
  composer = send_stack_composer(&f.s);
  shoot(&f, dir, "compose-narrow");
  new_window(&f, 960, 680, bob);
  composer = send_stack_composer(&f.s);

  signer.hold = TRUE;
  pubs.mode = PUB_HOLD;
  g_assert_true(gh_composer_send(composer));
  GhMessage *mine = stack_find(bob, "Yes! Trailhead at 8? I'll bring coffee.");
  gh_test_spin_until(signer_waiting, NULL);
  shoot(&f, dir, "waiting");
  approve_until(mine, GH_MESSAGE_STATUS_SENDING);
  signer.hold = FALSE;
  shoot(&f, dir, "sending");
  answer_held(TRUE, "");
  wait_status(mine, GH_MESSAGE_STATUS_SENT);
  shoot(&f, dir, "sent");

  pubs.refuse_url = INBOX_B;
  pubs.refuse_message = "blocked: not accepting messages from you";
  stack_type(composer, "Can you see this one?");
  g_assert_true(gh_composer_send(composer));
  GhMessage *failed = stack_find(bob, "Can you see this one?");
  wait_status(failed, GH_MESSAGE_STATUS_NOT_SENT);
  shoot(&f, dir, "failed");
  new_window(&f, 360, 640, bob);
  shoot(&f, dir, "failed-narrow");

  fixture_clear(&f);
  g_log_set_always_fatal(fatal);
}

/* Private XDG homes, then GTK and the private bus with the mock signer
 * (GTK's theme may warn once at start, before GTest makes warnings fatal). */
static gchar *
test_env_up(void)
{
  gchar *root = g_dir_make_tmp("groundhog-composer-xdg-XXXXXX", NULL);
  g_assert_nonnull(root);
  static const gchar *const vars[] = { "XDG_DATA_HOME", "XDG_STATE_HOME", "XDG_CACHE_HOME",
                                       "XDG_CONFIG_HOME" };
  for (guint i = 0; i < G_N_ELEMENTS(vars); i++) {
    g_autofree gchar *dir = g_build_filename(root, vars[i], NULL);
    g_assert_cmpint(g_mkdir(dir, 0700), ==, 0);
    g_setenv(vars[i], dir, TRUE);
  }
  return root;
}

int
main(int argc, char **argv)
{
  if (!nostrc_test_bus_available()) {
    g_printerr("groundhog-composer test skipped: dbus-daemon is not installed\n");
    return 77;
  }
  g_autofree gchar *xdg = test_env_up();
  if (!stack_gtk_and_bus_up(&bus)) {
    g_printerr("groundhog-composer test skipped: no graphical display\n");
    gh_test_remove_tree(xdg);
    return 77;
  }
  gh_test_signer_up(&bus, &signer);
  groundhog_register_resource();
  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(css, "/org/nostr/Groundhog/style.css");
  gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  /* 96 dpi makes 1sp one pixel everywhere; no animations. */
  g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024,
               "gtk-enable-animations", FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
  g_test_init(&argc, &argv, NULL);
  nostrc_test_tolerate_gdk_frame_warning();
  stack_keys_init();

#define ADD(path, func) nostrc_test_bus_add_func("/groundhog/composer/" path, func)
  ADD("keys", test_keys);
  ADD("bounds", test_bounds);
  ADD("disabled", test_disabled);
  ADD("compact-and-lines", test_compact_and_lines);
  ADD("draft-timer-and-emoji", test_draft_timer_and_emoji);
  ADD("focus-after-send", test_focus_after_send);
  ADD("breakpoints", test_breakpoints);
  ADD("minimum-size", test_minimum_size);
  ADD("send-echo-status", test_send_echo_status);
  ADD("failed-retry", test_failed_retry);
  ADD("drafts", test_drafts);
  ADD("restart-queued", test_restart_queued);
  ADD("note-to-self", test_note_to_self);
  ADD("reasons-account-signer-store", test_reasons_account_signer_store);
  ADD("reason-no-signer-bus", test_reason_no_signer_bus);
  ADD("reasons-group-and-no-inbox", test_reasons_group_and_no_inbox);
  ADD("delegates", test_delegates);
  ADD("room-send", test_room_send);
  ADD("room-nobody-has-inbox", test_room_nobody_has_inbox);
  ADD("locked-messages", test_locked_messages);
  ADD("focus-guard", test_focus_guard);
  ADD("timer-indicator", test_timer_indicator);
  ADD("timer-live", test_timer_live);
  ADD("preedit-enter", test_preedit_enter);
  ADD("screenshots", test_screenshots);
#undef ADD
  int status = g_test_run();
  stack_keys_clear();
  gh_test_signer_down(&bus, &signer);
  gh_test_bus_down(&bus);
  gh_test_remove_tree(xdg);
  return status;
}
