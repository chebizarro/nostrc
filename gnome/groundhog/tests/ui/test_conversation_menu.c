/* A conversation row's context menu and Preferences' Blocked Conversations
 * (privacy charter §7.4, §7.9, §7.13; nostrc-qp24.74, nostrc-qp24.72), and
 * conversation titles following the contact names in the UI
 * (nostrc-qp24.66): the entry points to GhConversationActions besides
 * Conversation Info.
 *
 * Default mode: gh_blocked_conversations_list()/_unblock() over a real
 * (ephemeral) encrypted store: the blocked rooms with the other people's
 * npubs, whether their history was kept, and unblocking back to the list as
 * a message request. No display needed.
 *
 * --gui: on a real store and conversation model (no notifier), the row
 * menu's Mute… (for how long, then Unmute), Conversation Info and Delete…
 * (ST-9) through their confirmations, Shift+F10/Menu on the focused row, no
 * menu for a group or a request's Mute; the Blocked Conversations page
 * pushed from Preferences › Privacy, its rows, Unblock and its empty and
 * error states; and a row and the conversation header taking a contact name
 * as the title when it is set. It exits 77 without a display. */
#include "gh-blocked-conversations.h"
#include "gh-conversation-actions.h"
#include "gh-conversation-info-dialog.h"
#include "gh-conversation-list.h"
#include "gh-conversation-menu.h"
#include "gh-conversation-private.h"
#include "gh-conversation-row.h"

#include "nostrc-test-gdk-frame.h"
#include "gh-conversation-view.h"
#include "gh-expiry.h"
#include "gh-shell.h"
#include "gh-store-conversations.h"
#include "gh-test-dialog.h"

#include "nostr-event.h"
#include "nostr-tag.h"
#include "nostr-utils.h"
#include "nostr/nip19/nip19.h"

#include <adwaita.h>
#include <stdlib.h>
#include <string.h>

#define T0 ((gint64)1790000000)

void groundhog_register_resource(void);

static gchar *ACCOUNT, *PEER[3];

static gchar *
hex_of(const gchar *seed)
{
  return g_compute_checksum_for_string(G_CHECKSUM_SHA256, seed, -1);
}

static gchar *
npub_of(const gchar *hex)
{
  guint8 bytes[32];
  char *npub = NULL;
  g_assert_true(nostr_hex2bin(bytes, hex, sizeof bytes));
  g_assert_cmpint(nostr_nip19_encode_npub(bytes, &npub), ==, 0);
  gchar *out = g_strdup(npub);
  free(npub);
  return out;
}

static gchar *
random_hex(void)
{
  GString *hex = g_string_new(NULL);
  for (guint i = 0; i < 32; i++)
    g_string_append_printf(hex, "%02x", (guint)g_random_int_range(0, 256));
  return g_string_free(hex, FALSE);
}

/* A kind-14 rumor from author to the account (or from the account to to). */
static GhMessage *
message_new(const gchar *author, const gchar *to, gint64 created_at, const gchar *content)
{
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, 14);
  nostr_event_set_pubkey(event, author);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, content);
  nostr_event_set_tags(event, nostr_tags_new(1, nostr_tag_new("p", to, NULL)));
  event->id = nostr_event_get_id(event);
  char *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  g_autoptr(GError) error = NULL;
  GhMessage *message = gh_message_new_from_rumor(ACCOUNT, json, &error);
  free(json);
  g_assert_no_error(error);
  return message;
}

/* ---- an account's store (in memory, "Continue Without Saving Messages") ---------- */

typedef struct {
  GhClock *clock;
  GhStore *store;
  GhConversationStore *model;
  GhStoreConversations *conversations;
  GhExpiry *expiry; /* the timer row's (nostrc-qp24.86 Disappearing Messages…) */
} Account;

static void
account_up(Account *a)
{
  memset(a, 0, sizeof *a);
  g_autoptr(GError) error = NULL;
  a->clock = gh_clock_new_fake(T0 * G_USEC_PER_SEC);
  a->store = gh_store_open_ephemeral(ACCOUNT, a->clock, &error);
  g_assert_no_error(error);
  a->model = gh_conversation_store_new();
  a->conversations = gh_store_conversations_new(a->store);
  g_assert_true(gh_store_conversations_attach(a->conversations, a->model, 0, &error));
  g_assert_no_error(error);
  GhExpiryConfig expiry = { .store = a->store, .conversations = a->conversations };
  a->expiry = gh_expiry_new(&expiry);
}

static void
account_down(Account *a)
{
  g_object_run_dispose(G_OBJECT(a->expiry));
  g_clear_object(&a->expiry);
  gh_store_conversations_close(a->conversations);
  g_clear_object(&a->conversations);
  gh_conversation_store_set_account(a->model, NULL, NULL, NULL, NULL);
  g_clear_object(&a->model);
  g_clear_pointer(&a->store, gh_store_close);
  gh_clock_unref(a->clock);
}

/* A message from peer, in a fresh wrap: its room (a message request). */
static GhConversation *
receive(Account *a, const gchar *peer, gint64 created_at, const gchar *text)
{
  g_autoptr(GhMessage) message = message_new(peer, ACCOUNT, created_at, text);
  g_autofree gchar *wrap = random_hex();
  g_autoptr(GError) error = NULL;
  g_assert_cmpint(gh_conversation_store_admit(a->model, message, wrap, &error), ==,
                  GH_CONVERSATION_ADD_NEW);
  g_assert_no_error(error);
  GhConversation *conversation =
    gh_conversation_store_lookup(a->model, gh_message_get_room_id(message));
  g_assert_nonnull(conversation);
  return conversation;
}

static gint64
muted_until(Account *a, const gchar *room_id)
{
  GhStoreNotifyState state = { 0 };
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_store_conversations_get_notify_state(a->conversations, room_id, &state,
                                                        &error));
  g_assert_no_error(error);
  return state.muted_until;
}

/* ==== Default mode: the blocked list over the store ===================================== */

static GPtrArray *
list_blocked(Account *a)
{
  g_autoptr(GError) error = NULL;
  GPtrArray *blocked = gh_blocked_conversations_list(a->conversations, ACCOUNT, &error);
  g_assert_no_error(error);
  g_assert_nonnull(blocked);
  return blocked;
}

static void
test_blocked_list(void)
{
  Account a;
  account_up(&a);
  GhConversation *kept = receive(&a, PEER[0], T0 - 20, "blocked, history kept");
  g_autofree gchar *kept_room = g_strdup(gh_conversation_get_room_id(kept));
  GhConversation *forgotten = receive(&a, PEER[1], T0 - 10, "blocked from Message Requests");
  g_autofree gchar *forgotten_room = g_strdup(gh_conversation_get_room_id(forgotten));
  receive(&a, PEER[2], T0, "not blocked");
  g_autoptr(GPtrArray) none = list_blocked(&a);
  g_assert_cmpuint(none->len, ==, 0);

  g_autoptr(GError) error = NULL;
  g_assert_true(gh_conversation_actions_block(a.conversations, NULL, kept_room, TRUE, &error));
  g_assert_true(gh_store_conversations_block_and_forget(a.conversations, forgotten_room, &error));
  g_assert_no_error(error);
  g_autoptr(GPtrArray) blocked = list_blocked(&a);
  g_assert_cmpuint(blocked->len, ==, 2);
  /* Newest first; the other person only, as an npub. */
  GhBlockedConversation *first = g_ptr_array_index(blocked, 0);
  GhBlockedConversation *second = g_ptr_array_index(blocked, 1);
  g_autofree gchar *npub0 = npub_of(PEER[0]);
  g_autofree gchar *npub1 = npub_of(PEER[1]);
  g_assert_cmpstr(first->room_id, ==, forgotten_room);
  g_assert_cmpuint(g_strv_length(first->npubs), ==, 1);
  g_assert_cmpstr(first->npubs[0], ==, npub1);
  g_assert_false(first->has_messages);
  g_assert_cmpstr(second->room_id, ==, kept_room);
  g_assert_cmpstr(second->npubs[0], ==, npub0);
  g_assert_true(second->has_messages);

  /* Unblocking lists its kept history again, as a message request: nobody
   * here wrote in it, and unblocking is not accepting (PT-8). */
  g_assert_true(gh_blocked_conversations_unblock(a.conversations, kept_room, &error));
  g_assert_no_error(error);
  GhConversation *back = gh_conversation_store_lookup(a.model, kept_room);
  g_assert_nonnull(back);
  g_assert_true(gh_conversation_get_is_request(back));
  g_assert_cmpuint(g_list_model_get_n_items(G_LIST_MODEL(back)), ==, 1);
  g_autoptr(GPtrArray) left = list_blocked(&a);
  g_assert_cmpuint(left->len, ==, 1);
  /* A room that was never stored has no block to lift. */
  g_autofree gchar *stranger = hex_of("stranger");
  g_autofree gchar *unknown = strcmp(ACCOUNT, stranger) < 0
                                ? g_strconcat(ACCOUNT, ",", stranger, NULL)
                                : g_strconcat(stranger, ",", ACCOUNT, NULL);
  g_assert_false(gh_blocked_conversations_unblock(a.conversations, unknown, &error));
  g_assert_error(error, GH_STORE_ERROR, GH_STORE_ERROR_NOT_FOUND);
  account_down(&a);
}

/* ==== --gui ================================================================================ */

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
is_mapped(gpointer widget)
{
  return gtk_widget_get_mapped(GTK_WIDGET(widget));
}

static GtkWidget *
find_type(GtkWidget *widget, GType type)
{
  if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type))
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_type(c, type);
    if (found)
      return found;
  }
  return NULL;
}

static GtkWidget *
find_button(GtkWidget *widget, const char *label)
{
  if (GTK_IS_BUTTON(widget) && gtk_widget_get_mapped(widget) &&
      g_strcmp0(gtk_button_get_label(GTK_BUTTON(widget)), label) == 0)
    return widget;
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GtkWidget *found = find_button(c, label);
    if (found)
      return found;
  }
  return NULL;
}

/* The list's row for conversation, once the list has made it. */
typedef struct {
  GtkWidget *root;
  GhConversation *conversation;
  GhConversationRow *row;
} RowWait;

static GhConversationRow *
find_row(GtkWidget *widget, GhConversation *conversation)
{
  if (GH_IS_CONVERSATION_ROW(widget) &&
      gh_conversation_row_get_conversation(GH_CONVERSATION_ROW(widget)) == conversation)
    return GH_CONVERSATION_ROW(widget);
  for (GtkWidget *c = gtk_widget_get_first_child(widget); c; c = gtk_widget_get_next_sibling(c)) {
    GhConversationRow *found = find_row(c, conversation);
    if (found)
      return found;
  }
  return NULL;
}

static gboolean
row_shown(gpointer data)
{
  RowWait *wait = data;
  wait->row = find_row(wait->root, wait->conversation);
  return wait->row && gtk_widget_get_mapped(GTK_WIDGET(wait->row));
}

typedef struct {
  Account account;
  GhWindow *window;
  gboolean with_store; /* services give the open store (else: none open) */
} Gui;

/* What the contact directory has cached: a name for PEER[0] only. */
static void
profile(const gchar *pubkey, GhConversationInfoProfile *out, gpointer data)
{
  (void)data;
  if (g_strcmp0(pubkey, PEER[0]) == 0)
    out->name = "Alice";
}

static gboolean
services_func(GhConversationInfoServices *services, gpointer data)
{
  Gui *g = data;
  *services = (GhConversationInfoServices){
    .model = g->account.model,
    .conversations = g->with_store ? g->account.conversations : NULL,
    .store = g->with_store ? g->account.store : NULL,
    .expiry = g->with_store ? g->account.expiry : NULL,
    .profile = profile,
  };
  return TRUE;
}

static void
gui_up(Gui *g)
{
  memset(g, 0, sizeof *g);
  account_up(&g->account);
  g->with_store = TRUE;
  g->window = gh_window_new(NULL);
  gh_conversation_list_attach(g->window, g->account.model, NULL);
  gh_conversation_menu_attach(g->window, services_func, g, NULL);
  gtk_window_set_default_size(GTK_WINDOW(g->window), 900, 600);
  gtk_window_present(GTK_WINDOW(g->window));
  spin_until(is_mapped, g->window);
  drain_idle();
}

static void
gui_down(Gui *g)
{
  gtk_window_destroy(GTK_WINDOW(g->window));
  drain_idle();
  account_down(&g->account);
}

static GhConversationRow *
row_of(Gui *g, GhConversation *conversation)
{
  RowWait wait = { GTK_WIDGET(g->window), conversation, NULL };
  spin_until(row_shown, &wait);
  return wait.row;
}

static AdwDialog *
visible_dialog(Gui *g)
{
  return adw_application_window_get_visible_dialog(ADW_APPLICATION_WINDOW(g->window));
}

/* The visible dialog, shown (see gh-test-dialog.h). */
static gboolean
dialog_shown(gpointer data)
{
  AdwDialog *dialog = visible_dialog(data);
  return dialog && gh_test_dialog_shown(dialog);
}

static gboolean
alert_shown(gpointer data)
{
  return ADW_IS_ALERT_DIALOG(visible_dialog(data)) && dialog_shown(data);
}

static gboolean
no_dialog(gpointer data)
{
  return visible_dialog(data) == NULL;
}

/* Runs the row's action (as its menu item does) and returns the
 * confirmation it shows. */
static AdwAlertDialog *
row_alert(Gui *g, GhConversationRow *row, const char *action)
{
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(row), action, NULL));
  spin_until(alert_shown, g);
  return ADW_ALERT_DIALOG(visible_dialog(g));
}

typedef struct {
  GtkWidget *root;
  const char *label;
  GtkWidget *button;
} ButtonWait;

static gboolean
button_shown(gpointer data)
{
  ButtonWait *wait = data;
  wait->button = find_button(wait->root, wait->label);
  return wait->button != NULL;
}

/* Answers the shown confirmation with its button labelled label, as a user
 * does. */
static void
answer(Gui *g, AdwAlertDialog *alert, const char *label)
{
  ButtonWait wait = { GTK_WIDGET(alert), label, NULL };
  spin_until(button_shown, &wait);
  g_signal_emit_by_name(wait.button, "clicked");
  spin_until(no_dialog, g);
  drain_idle();
}

/* The accepted conversation with PEER[0] and the request from PEER[1]. */
static GhConversation *
accepted_room(Gui *g)
{
  GhConversation *conversation = receive(&g->account, PEER[0], T0 - 60, "hello");
  gh_conversation_accept(conversation);
  return conversation;
}

static void
test_gui_menu(void)
{
  Gui g;
  gui_up(&g);
  GhConversation *conversation = accepted_room(&g);
  g_autofree gchar *room = g_strdup(gh_conversation_get_room_id(conversation));
  GhConversationRow *row = row_of(&g, conversation);

  /* The menu, for this row's room: Pin or Unpin and Mark as Read or Unread
   * (nostrc-qp24.86); Mute…, Conversation Info; Delete…. */
  GtkPopover *menu = gh_conversation_row_get_menu(row);
  g_assert_true(GTK_IS_POPOVER_MENU(menu));
  GMenuModel *model = gtk_popover_menu_get_menu_model(GTK_POPOVER_MENU(menu));
  g_assert_cmpint(g_menu_model_get_n_items(model), ==, 3);
  AdwAlertDialog *alert = row_alert(&g, row, "row.mute");
  /* The person as Conversation Info names them (their cached name). */
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Mute Alice?");
  /* Not muted: no Unmute. */
  g_assert_false(adw_alert_dialog_has_response(alert, "mute-off"));
  answer(&g, alert, "For _8 Hours");
  g_assert_cmpint(muted_until(&g.account, room), ==, T0 + 8 * 3600);
  g_assert_cmpstr(gh_conversation_menu_get_last_toast(g.window), ==, "Conversation muted");

  /* Muted: it says until when and offers Unmute. */
  alert = row_alert(&g, row, "row.mute");
  g_assert_true(adw_alert_dialog_has_response(alert, "mute-off"));
  g_assert_true(g_str_has_prefix(adw_alert_dialog_get_body(alert), "Muted until "));
  answer(&g, alert, "_Unmute");
  g_assert_cmpint(muted_until(&g.account, room), ==, 0);
  g_assert_cmpstr(gh_conversation_menu_get_last_toast(g.window), ==, "Conversation unmuted");
  alert = row_alert(&g, row, "row.mute");
  answer(&g, alert, "_Until I Turn It Back On");
  g_assert_cmpint(muted_until(&g.account, room), ==, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);
  alert = row_alert(&g, row, "row.mute");
  answer(&g, alert, "_Cancel");
  g_assert_cmpint(muted_until(&g.account, room), ==, GH_STORE_CONVERSATIONS_MUTED_ALWAYS);

  /* Conversation Info for this row, whichever is shown. */
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(row), "row.info", NULL));
  AdwDialog *info = visible_dialog(&g);
  g_assert_true(GH_IS_CONVERSATION_INFO_DIALOG(info));
  g_assert_true(gh_conversation_info_dialog_get_conversation(GH_CONVERSATION_INFO_DIALOG(info)) ==
                conversation);
  spin_until(dialog_shown, &g);
  adw_dialog_force_close(info);
  spin_until(no_dialog, &g);

  /* Delete…: Cancel keeps it; Delete forgets it on this device (ST-9). */
  alert = row_alert(&g, row, "row.delete");
  g_assert_cmpstr(adw_alert_dialog_get_heading(alert), ==, "Delete Conversation with Alice?");
  g_assert_cmpint(adw_alert_dialog_get_response_appearance(alert, "delete-confirm"), ==,
                  ADW_RESPONSE_DESTRUCTIVE);
  g_assert_nonnull(strstr(adw_alert_dialog_get_body(alert), "Alice still has their copy"));
  answer(&g, alert, "_Cancel");
  g_assert_nonnull(gh_conversation_store_lookup(g.account.model, room));
  alert = row_alert(&g, row, "row.delete");
  answer(&g, alert, "_Delete");
  g_assert_null(gh_conversation_store_lookup(g.account.model, room));
  g_assert_cmpstr(gh_conversation_menu_get_last_toast(g.window), ==,
                  "Conversation deleted from this device");
  gui_down(&g);
}

/* The sidebar list's section header texts, in list order. */
static GPtrArray *
section_texts(Gui *g)
{
  GPtrArray *texts = g_ptr_array_new();
  GtkWidget *list = GTK_WIDGET(gh_sidebar_page_get_list(gh_window_get_sidebar(g->window)));
  for (GtkWidget *c = gtk_widget_get_first_child(list); c; c = gtk_widget_get_next_sibling(c))
    if (g_str_equal(gtk_widget_get_css_name(c), "header") &&
        GTK_IS_LABEL(gtk_widget_get_first_child(c)))
      g_ptr_array_add(texts, (gpointer)gtk_label_get_text(GTK_LABEL(gtk_widget_get_first_child(c))));
  return texts;
}

typedef struct {
  Gui *g;
  const gchar *expected; /* "|"-joined */
} SectionsWait;

static gboolean
sections_are(gpointer data)
{
  SectionsWait *wait = data;
  g_autoptr(GPtrArray) texts = section_texts(wait->g);
  g_ptr_array_add(texts, NULL);
  g_autofree gchar *joined = g_strjoinv("|", (gchar **)texts->pdata);
  return g_strcmp0(joined, wait->expected) == 0;
}

static gpointer
listed_first(Gui *g)
{
  GListModel *model = G_LIST_MODEL(gtk_list_view_get_model(
    gh_sidebar_page_get_list(gh_window_get_sidebar(g->window))));
  return g_list_model_get_n_items(model) ? g_list_model_get_item(model, 0) : NULL;
}

/* Runs an enabled row action, as its (shown) menu item does. */
static gboolean
action_enabled(GhConversationRow *row, const char *action)
{
  return gtk_widget_activate_action(GTK_WIDGET(row), action, NULL);
}

/* nostrc-qp24.86: Pin and Mark as Read/Unread from a row's menu. A pinned
 * conversation is listed first, under "Pinned" (the others under "Recent"),
 * whatever arrives later; there are no headers while nothing is pinned. The
 * pin lives in the encrypted store's conversations row (PD-11), survives a
 * restart of the model and is never published. Mark as Unread makes the
 * newest message unread again (a badge, a count); Mark as Read reads it. Of
 * each pair only the one that applies runs. */
static void
test_gui_pin_and_read(void)
{
  Gui g;
  gui_up(&g);
  GhConversation *alice = accepted_room(&g); /* T0 - 60 */
  GhConversation *bob = receive(&g.account, PEER[1], T0 - 30, "newer");
  gh_conversation_accept(bob);
  GhConversationRow *alice_row = row_of(&g, alice);
  GtkListView *list = gh_sidebar_page_get_list(gh_window_get_sidebar(g.window));
  g_assert_null(gtk_list_view_get_header_factory(list));
  g_autoptr(GObject) first = listed_first(&g);
  g_assert_true(first == G_OBJECT(bob));
  g_clear_object(&first);

  /* Unpin does nothing while it is not pinned; Pin pins it. */
  gtk_widget_activate_action(GTK_WIDGET(alice_row), "row.unpin", NULL);
  drain_idle();
  g_assert_false(gh_conversation_get_pinned(alice));
  g_assert_true(action_enabled(alice_row, "row.pin"));
  drain_idle();
  g_assert_true(gh_conversation_get_pinned(alice));
  first = listed_first(&g);
  g_assert_true(first == G_OBJECT(alice));
  g_clear_object(&first);
  g_assert_nonnull(gtk_list_view_get_header_factory(list));
  SectionsWait sections = { &g, "Pinned|Recent" };
  spin_until(sections_are, &sections);
  alice_row = row_of(&g, alice);
  GtkWidget *pin = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(alice_row),
                                                            GH_TYPE_CONVERSATION_ROW,
                                                            "pinned_icon"));
  g_assert_true(gtk_widget_get_visible(pin));
  g_assert_nonnull(strstr(gh_conversation_row_get_summary(alice_row), ". Pinned."));
  /* A newer message elsewhere does not move it. */
  receive(&g.account, PEER[1], T0 - 10, "newest");
  first = listed_first(&g);
  g_assert_true(first == G_OBJECT(alice));
  g_clear_object(&first);

  /* In the store, and back after the model is restored from it. */
  g_autoptr(GError) error = NULL;
  g_autoptr(GhConversationStore) restored = gh_conversation_store_new();
  g_autoptr(GhStoreConversations) again = gh_store_conversations_new(g.account.store);
  g_assert_true(gh_store_conversations_attach(again, restored, 0, &error));
  g_assert_no_error(error);
  GhConversation *alice_again =
    gh_conversation_store_lookup(restored, gh_conversation_get_room_id(alice));
  g_assert_nonnull(alice_again);
  g_assert_true(gh_conversation_get_pinned(alice_again));
  g_autoptr(GhConversation) top = g_list_model_get_item(G_LIST_MODEL(restored), 0);
  g_assert_true(top == alice_again);
  gh_store_conversations_close(again);

  /* Unpinned: back in activity order, and no headers. */
  alice_row = row_of(&g, alice);
  g_assert_true(action_enabled(alice_row, "row.unpin"));
  drain_idle();
  g_assert_false(gh_conversation_get_pinned(alice));
  first = listed_first(&g);
  g_assert_true(first == G_OBJECT(bob));
  g_clear_object(&first);
  g_assert_null(gtk_list_view_get_header_factory(list));

  /* Mark as Unread, then as Read (local only: nothing is published). */
  alice_row = row_of(&g, alice);
  gh_conversation_mark_read(alice);
  gtk_widget_activate_action(GTK_WIDGET(alice_row), "row.mark-read", NULL); /* nothing unread */
  g_assert_true(action_enabled(alice_row, "row.mark-unread"));
  drain_idle();
  g_assert_cmpuint(gh_conversation_get_unread_count(alice), ==, 1);
  GtkWidget *badge = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(alice_row),
                                                              GH_TYPE_CONVERSATION_ROW,
                                                              "unread_badge"));
  g_assert_true(gtk_widget_get_visible(badge));
  gtk_widget_activate_action(GTK_WIDGET(alice_row), "row.mark-unread", NULL); /* already */
  g_assert_cmpuint(gh_conversation_get_unread_count(alice), ==, 1);
  g_assert_true(action_enabled(alice_row, "row.mark-read"));
  drain_idle();
  g_assert_cmpuint(gh_conversation_get_unread_count(alice), ==, 0);
  g_assert_false(gtk_widget_get_visible(badge));

  /* A message request has none of them. */
  GhConversation *request = receive(&g.account, PEER[2], T0 - 5, "a request");
  gh_sidebar_page_set_show_requests(gh_window_get_sidebar(g.window), TRUE);
  GhConversationRow *request_row = row_of(&g, request);
  gtk_widget_activate_action(GTK_WIDGET(request_row), "row.pin", NULL);
  gtk_widget_activate_action(GTK_WIDGET(request_row), "row.mark-read", NULL);
  drain_idle();
  g_assert_false(gh_conversation_get_pinned(request));
  g_assert_cmpuint(gh_conversation_get_unread_count(request), ==, 1);
  gh_sidebar_page_set_show_requests(gh_window_get_sidebar(g.window), FALSE);
  gui_down(&g);
}

typedef struct {
  GActionGroup *actions;
  const gchar *name;
  gboolean enabled;
} ActionStateWait;

static gboolean
action_state_is(gpointer data)
{
  ActionStateWait *wait = data;
  return g_action_group_get_action_enabled(wait->actions, wait->name) == wait->enabled;
}

static gboolean
widget_visible(gpointer data)
{
  return gtk_widget_get_visible(GTK_WIDGET(data));
}

/* nostrc-qp24.86: the conversation header's menu (charter §7.4
 * conversation_menu): shown with a private conversation; Conversation Info,
 * Pin or Unpin, Mute…, Disappearing Messages… (Conversation Info at its
 * timer) and Delete Conversation…, each for the shown conversation. */
static void
test_gui_header_menu(void)
{
  Gui g;
  gui_up(&g);
  gh_conversation_info_attach(g.window, services_func, &g, NULL);
  GhConversation *alice = accepted_room(&g);
  GhContentPage *content = gh_window_get_content(g.window);
  GtkWidget *button = gh_content_page_get_menu_button(content);
  g_assert_false(gtk_widget_get_visible(button));
  g_assert_true(gh_window_open_item(g.window, alice));
  spin_until(widget_visible, button);
  g_assert_cmpstr(gtk_widget_get_tooltip_text(button), ==, "Conversation Menu");
  GMenuModel *model = gtk_menu_button_get_menu_model(GTK_MENU_BUTTON(button));
  g_assert_cmpint(g_menu_model_get_n_items(model), ==, 3);
  GActionGroup *actions = G_ACTION_GROUP(g.window);
  g_assert_true(g_action_group_get_action_enabled(actions, "pin-shown-conversation"));
  g_assert_false(g_action_group_get_action_enabled(actions, "unpin-shown-conversation"));

  g_action_group_activate_action(actions, "pin-shown-conversation", NULL);
  drain_idle();
  g_assert_true(gh_conversation_get_pinned(alice));
  ActionStateWait pin_disabled = { actions, "pin-shown-conversation", FALSE };
  spin_until(action_state_is, &pin_disabled);
  g_assert_false(g_action_group_get_action_enabled(actions, "pin-shown-conversation"));
  g_assert_true(g_action_group_get_action_enabled(actions, "unpin-shown-conversation"));
  g_action_group_activate_action(actions, "unpin-shown-conversation", NULL);
  drain_idle();
  g_assert_false(gh_conversation_get_pinned(alice));

  /* Disappearing Messages…: Conversation Info, at its timer. */
  g_action_group_activate_action(actions, "disappearing-shown-conversation", NULL);
  spin_until(dialog_shown, &g);
  AdwDialog *info = visible_dialog(&g);
  g_assert_true(GH_IS_CONVERSATION_INFO_DIALOG(info));
  GtkWidget *timer = GTK_WIDGET(gtk_widget_get_template_child(GTK_WIDGET(info),
                                                              GH_TYPE_CONVERSATION_INFO_DIALOG,
                                                              "timer_row"));
  g_assert_true(adw_dialog_get_focus(info) == timer);
  adw_dialog_force_close(info);
  spin_until(no_dialog, &g);

  /* Mute… and Delete Conversation… ask first, about the shown one. */
  g_action_group_activate_action(actions, "mute-shown-conversation", NULL);
  spin_until(alert_shown, &g);
  g_assert_cmpstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(visible_dialog(&g))), ==,
                  "Mute Alice?");
  answer(&g, ADW_ALERT_DIALOG(visible_dialog(&g)), "_Cancel");
  g_action_group_activate_action(actions, "delete-shown-conversation", NULL);
  spin_until(alert_shown, &g);
  g_assert_cmpstr(adw_alert_dialog_get_heading(ADW_ALERT_DIALOG(visible_dialog(&g))), ==,
                  "Delete Conversation with Alice?");
  answer(&g, ADW_ALERT_DIALOG(visible_dialog(&g)), "_Cancel");
  g_assert_nonnull(gh_conversation_store_lookup(g.account.model,
                                                gh_conversation_get_room_id(alice)));

  /* A relay group shows the menu button with Rename Group… enabled, but DM
   * actions (mute, pin, delete, disappearing) are disabled (nostrc-0srb). */
  g_autofree gchar *group_room = gh_message_nip29_room_id("wss://groups.test.invalid", "pies");
  GhConversation *group = gh_conversation_store_ensure_group(g.account.model, group_room, "Pies");
  g_assert_true(gh_window_open_item(g.window, group));
  drain_idle();
  g_assert_true(gtk_widget_get_visible(button));
  ActionStateWait rename_enabled = { actions, "rename-shown-group", TRUE };
  spin_until(action_state_is, &rename_enabled);
  g_assert_true(g_action_group_get_action_enabled(actions, "rename-shown-group"));
  g_assert_false(g_action_group_get_action_enabled(actions, "mute-shown-conversation"));
  g_assert_false(g_action_group_get_action_enabled(actions, "pin-shown-conversation"));
  g_assert_false(g_action_group_get_action_enabled(actions, "delete-shown-conversation"));
  gui_down(&g);
}

/* The menu's signal callbacks must not retain their attachment after the
 * window is destroyed, even if a child is kept alive by another owner. */
static void
test_gui_header_menu_lifetime(void)
{
  Gui g;
  gui_up(&g);
  GhContentPage *content = gh_window_get_content(g.window);
  g_autoptr(GtkStack) stack = g_object_ref(gh_content_page_get_stack(content));
  g_autoptr(GtkWidget) view = g_object_ref(gh_content_page_get_view(content));
  gtk_window_destroy(GTK_WINDOW(g.window));
  drain_idle();
  g_object_notify(G_OBJECT(stack), "visible-child-name");
  if (GH_IS_CONVERSATION_VIEW(view))
    g_object_notify(G_OBJECT(view), "conversation");
  account_down(&g.account);
}

/* Shift+F10 / Menu on the focused row opens its menu; a request has no
 * Mute (it is never notified); a group has no menu here; without an open
 * store mute and delete say so. */
static void
test_gui_menu_states(void)
{
  Gui g;
  gui_up(&g);
  GhConversation *conversation = accepted_room(&g);
  GhConversationRow *row = row_of(&g, conversation);

  /* The list's shortcut, as the keyboard triggers it on the focused item. */
  GtkWidget *list = GTK_WIDGET(gh_sidebar_page_get_list(gh_window_get_sidebar(g.window)));
  g_assert_true(gtk_widget_grab_focus(gtk_widget_get_parent(GTK_WIDGET(row))));
  GtkShortcut *shortcut = NULL;
  g_autoptr(GListModel) controllers = gtk_widget_observe_controllers(list);
  for (guint i = 0; !shortcut && i < g_list_model_get_n_items(controllers); i++) {
    g_autoptr(GObject) controller = g_list_model_get_item(controllers, i);
    if (!GTK_IS_SHORTCUT_CONTROLLER(controller))
      continue;
    g_autoptr(GtkShortcut) candidate = g_list_model_get_item(G_LIST_MODEL(controller), 0);
    g_autofree char *trigger = gtk_shortcut_trigger_to_string(gtk_shortcut_get_trigger(candidate));
    if (strstr(trigger, "F10") && strstr(trigger, "Menu"))
      shortcut = g_steal_pointer(&candidate);
  }
  g_assert_nonnull(shortcut);
  GtkPopover *menu = gh_conversation_row_get_menu(row);
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(menu)));
  g_assert_true(gtk_shortcut_action_activate(gtk_shortcut_get_action(shortcut), 0, list, NULL));
  g_assert_true(gtk_widget_get_visible(GTK_WIDGET(menu)));
  gtk_popover_popdown(menu);
  g_object_unref(shortcut);

  /* A message request: Info and Delete…, no Mute…. */
  GhConversation *request = receive(&g.account, PEER[1], T0 - 30, "request");
  gh_sidebar_page_set_show_requests(gh_window_get_sidebar(g.window), TRUE);
  GhConversationRow *request_row = row_of(&g, request);
  /* A disabled action does nothing (and its menu item is hidden). */
  gtk_widget_activate_action(GTK_WIDGET(request_row), "row.mute", NULL);
  drain_idle();
  g_assert_null(visible_dialog(&g));
  g_assert_true(gh_conversation_row_popup_menu(request_row));
  gtk_popover_popdown(gh_conversation_row_get_menu(request_row));
  gh_sidebar_page_set_show_requests(gh_window_get_sidebar(g.window), FALSE);

  /* A relay group: its actions are the group dialogs', not these. */
  g_autofree gchar *group_room = gh_message_nip29_room_id("wss://groups.test.invalid", "pies");
  GhConversation *group = gh_conversation_store_ensure_group(g.account.model, group_room, "Pies");
  g_assert_nonnull(group);
  GhConversationRow *group_row = row_of(&g, group);
  g_assert_false(gh_conversation_row_popup_menu(group_row));
  g_assert_false(gtk_widget_get_visible(GTK_WIDGET(gh_conversation_row_get_menu(group_row))));
  gtk_widget_activate_action(GTK_WIDGET(group_row), "row.delete", NULL);
  drain_idle();
  g_assert_null(visible_dialog(&g));
  g_assert_nonnull(gh_conversation_store_lookup(g.account.model, group_room));

  /* No open store: nothing to mute or delete, and it says so. (The list
   * made its rows again when it switched models.) */
  g.with_store = FALSE;
  row = row_of(&g, conversation);
  g_assert_true(gtk_widget_activate_action(GTK_WIDGET(row), "row.delete", NULL));
  drain_idle();
  g_assert_null(visible_dialog(&g));
  g_assert_cmpstr(gh_conversation_menu_get_last_toast(g.window), ==,
                  "Unavailable while Groundhog isn’t saving messages on this device");
  g_assert_nonnull(gh_conversation_store_lookup(g.account.model,
                                                gh_conversation_get_room_id(conversation)));
  gui_down(&g);
}

/* nostrc-qp24.66 in the UI: the row and, while it is shown, the
 * conversation header follow the title as a contact name arrives. */
static void
test_gui_contact_title(void)
{
  Gui g;
  gui_up(&g);
  GhConversation *conversation = accepted_room(&g);
  GhConversationRow *row = row_of(&g, conversation);
  g_assert_true(gh_window_open_item(g.window, conversation));
  drain_idle();
  AdwWindowTitle *header = gh_content_page_get_window_title(gh_window_get_content(g.window));
  g_autofree gchar *npubs = g_strdup(gh_conversation_get_title(conversation));
  g_assert_cmpstr(adw_window_title_get_title(header), ==, npubs);
  gh_conversation_set_contact_title(conversation, "Alice");
  g_assert_cmpstr(adw_window_title_get_title(header), ==, "Alice");
  g_assert_true(g_str_has_prefix(gh_conversation_row_get_summary(row), "Alice. "));
  gh_conversation_set_contact_title(conversation, NULL);
  g_assert_cmpstr(adw_window_title_get_title(header), ==, npubs);
  gui_down(&g);
}

/* ---- Blocked Conversations ------------------------------------------------------------ */

typedef struct {
  Account *account;
  gboolean fail;
} Blocked;

static GPtrArray *
blocked_list(gpointer data, GError **error)
{
  Blocked *blocked = data;
  if (blocked->fail) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED, "Storage is locked");
    return NULL;
  }
  return gh_blocked_conversations_list(blocked->account->conversations, ACCOUNT, error);
}

static gboolean
blocked_unblock(gpointer data, const gchar *room_id, GError **error)
{
  Blocked *blocked = data;
  return gh_blocked_conversations_unblock(blocked->account->conversations, room_id, error);
}

static const GhBlockedBackend blocked_backend = { blocked_list, blocked_unblock };

static gboolean
page_shown(gpointer data)
{
  GtkWidget *page = find_type(GTK_WIDGET(data), GH_TYPE_BLOCKED_PAGE);
  return page && gtk_widget_get_mapped(page);
}

static void
test_gui_blocked_page(void)
{
  Gui g;
  gui_up(&g);
  Account *a = &g.account;
  GhConversation *kept = receive(a, PEER[0], T0 - 20, "kept");
  g_autofree gchar *kept_room = g_strdup(gh_conversation_get_room_id(kept));
  GhConversation *gone = receive(a, PEER[1], T0 - 10, "gone");
  g_autofree gchar *gone_room = g_strdup(gh_conversation_get_room_id(gone));
  g_autoptr(GError) error = NULL;
  g_assert_true(gh_conversation_actions_block(a->conversations, NULL, kept_room, TRUE, &error));
  g_assert_true(gh_store_conversations_block_and_forget(a->conversations, gone_room, &error));

  /* Hidden until something can list the blocks. */
  g_autoptr(GSettings) settings = g_settings_new("org.nostr.Groundhog");
  GhPreferencesDialog *dialog = gh_preferences_dialog_new(settings, GH_PREFERENCES_FEATURES_ALL);
  GtkWidget *group = GTK_WIDGET(gtk_widget_get_template_child(
    GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG, "blocked_group"));
  AdwActionRow *entry = ADW_ACTION_ROW(gtk_widget_get_template_child(
    GTK_WIDGET(dialog), GH_TYPE_PREFERENCES_DIALOG, "blocked_row"));
  g_assert_false(gtk_widget_get_visible(group));
  Blocked blocked = { a, FALSE };
  gh_blocked_page_attach(dialog, &blocked_backend, &blocked, NULL);
  g_assert_true(gtk_widget_get_visible(group));
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(entry)), ==,
                  "Blocked Conversations");
  g_assert_true(gtk_list_box_row_get_activatable(GTK_LIST_BOX_ROW(entry)));
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(g.window));
  spin_until(gh_test_dialog_shown, dialog);
  g_signal_emit_by_name(entry, "activated");
  spin_until(page_shown, dialog);
  GhBlockedPage *page = GH_BLOCKED_PAGE(find_type(GTK_WIDGET(dialog), GH_TYPE_BLOCKED_PAGE));
  g_assert_cmpstr(gh_blocked_page_get_state(page), ==, "list");

  /* Newest first; npubs only; the one blocked from Message Requests says
   * its messages were deleted. */
  GtkListBox *list = gh_blocked_page_get_list(page);
  g_autofree gchar *npub0 = npub_of(PEER[0]);
  g_autofree gchar *npub1 = npub_of(PEER[1]);
  AdwActionRow *first = ADW_ACTION_ROW(gtk_list_box_get_row_at_index(list, 0));
  AdwActionRow *second = ADW_ACTION_ROW(gtk_list_box_get_row_at_index(list, 1));
  g_assert_null(gtk_list_box_get_row_at_index(list, 2));
  g_autofree gchar *short1 = g_strdup_printf("%.10s…%s", npub1, npub1 + strlen(npub1) - 4);
  g_assert_cmpstr(adw_preferences_row_get_title(ADW_PREFERENCES_ROW(first)), ==, short1);
  g_assert_true(g_str_has_prefix(adw_action_row_get_subtitle(first), npub1));
  g_assert_nonnull(strstr(adw_action_row_get_subtitle(first), "messages were deleted"));
  g_assert_cmpstr(adw_action_row_get_subtitle(second), ==, npub0);
  g_assert_false(adw_preferences_row_get_use_markup(ADW_PREFERENCES_ROW(first)));
  GtkWidget *unblock = find_button(GTK_WIDGET(second), "_Unblock");
  g_assert_nonnull(unblock);
  g_autofree gchar *short0 = g_strdup_printf("%.10s…%s", npub0, npub0 + strlen(npub0) - 4);
  g_autofree gchar *label = g_strdup_printf("Unblock %s", short0);
  gtk_test_accessible_assert_property(GTK_ACCESSIBLE(unblock), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                      label);

  /* Unblock: back in the model (a request: unblocking is not accepting),
   * off the list. */
  g_signal_emit_by_name(unblock, "clicked");
  drain_idle();
  GhConversation *back = gh_conversation_store_lookup(a->model, kept_room);
  g_assert_nonnull(back);
  g_assert_true(gh_conversation_get_is_request(back));
  g_assert_null(gtk_list_box_get_row_at_index(list, 1));
  unblock = find_button(GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)), "_Unblock");
  g_signal_emit_by_name(unblock, "clicked");
  drain_idle();
  g_assert_cmpstr(gh_blocked_page_get_state(page), ==, "empty");
  /* Nothing of it was stored: it starts again with the next message. */
  g_assert_null(gh_conversation_store_lookup(a->model, gone_room));

  /* The list can't be read: it says why. */
  blocked.fail = TRUE;
  gh_blocked_page_refresh(page);
  g_assert_cmpstr(gh_blocked_page_get_state(page), ==, "error");
  adw_dialog_force_close(ADW_DIALOG(dialog));
  drain_idle();
  gui_down(&g);
}

/* ---- main ------------------------------------------------------------------------------- */

int
main(int argc, char **argv)
{
  gboolean gui_mode = argc > 1 && g_str_equal(argv[1], "--gui");
  if (gui_mode) {
    argv[1] = argv[0];
    argv++;
    argc--;
  }
  /* GTK's in-process accessibility backend: accessible labels are
   * checked, and no AT-SPI bus is needed. */
  g_setenv("GTK_A11Y", "test", TRUE);
  ACCOUNT = hex_of("account-a");
  for (guint i = 0; i < G_N_ELEMENTS(PEER); i++) {
    g_autofree gchar *seed = g_strdup_printf("peer-%u", i);
    PEER[i] = hex_of(seed);
  }
  int status;
  if (gui_mode) {
    if (!gtk_init_check()) {
      g_printerr("groundhog-conversation-menu GUI test skipped: no graphical display\n");
      status = 77;
      goto out;
    }
    adw_init();
    groundhog_register_resource();
    g_object_set(gtk_settings_get_default(), "gtk-xft-dpi", 96 * 1024, "gtk-enable-animations",
                 FALSE, "gtk-decoration-layout", "appmenu:close", NULL);
    g_test_init(&argc, &argv, NULL);
    nostrc_test_tolerate_gdk_frame_warning();
    g_test_add_func("/groundhog/conversation-menu-gui/menu", test_gui_menu);
    g_test_add_func("/groundhog/conversation-menu-gui/menu-states", test_gui_menu_states);
    g_test_add_func("/groundhog/conversation-menu-gui/contact-title", test_gui_contact_title);
    g_test_add_func("/groundhog/conversation-menu-gui/blocked-page", test_gui_blocked_page);
    g_test_add_func("/groundhog/conversation-menu-gui/pin-and-read", test_gui_pin_and_read);
    g_test_add_func("/groundhog/conversation-menu-gui/header-menu", test_gui_header_menu);
    g_test_add_func("/groundhog/conversation-menu-gui/header-menu-lifetime",
                    test_gui_header_menu_lifetime);
    status = g_test_run();
    goto out;
  }
  g_test_init(&argc, &argv, NULL);
  g_test_add_func("/groundhog/conversation-menu/blocked-list", test_blocked_list);
  status = g_test_run();
out:
  g_free(ACCOUNT);
  for (guint i = 0; i < G_N_ELEMENTS(PEER); i++)
    g_free(PEER[i]);
  return status;
}
