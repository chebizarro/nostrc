#include "gh-poll-ui.h"

#include "gh-composer.h"
#include "gh-conversation-view.h"
#include "gh-create-poll-dialog.h"
#include "gh-message-row.h"
#include "gh-outbox.h"
#include "gh-poll-card.h"
#include "gh-shell.h"

#include <glib/gi18n.h>

/* Polls and responses are messages in each room's authenticated transport.
 * The card is a projection of that room's messages, never a relay query: a
 * private vote must not be fetched from (or leaked to) a public relay. */
typedef struct {
  GhWindow *window;                 /* window owns this state */
  GhConversationView *view;         /* template child */
  GhComposer *composer;             /* template child */
  GhConversation *shown;            /* ref */
  GhAccountStore *account_store;    /* borrowed from app services */
  GhMlsService *(*mls_service)(gpointer);
  GhNip29Service *(*nip29_service)(gpointer);
  gpointer service_data;
  GHashTable *polls;                /* event id -> GhMlsPoll */
} PollUi;

static PollUi *
ui_of(GhWindow *window)
{
  return g_object_get_data(G_OBJECT(window), GH_POLL_UI_DATA);
}

static void
apply_vote(PollUi *ui, GhMessage *message)
{
  g_autofree gchar *target = NULL;
  g_auto(GStrv) choices = NULL;
  if (gh_message_get_withdrawn(message) ||
      !gh_mls_poll_parse_vote(gh_message_get_rumor_json(message), &target,
                              &choices, NULL, NULL))
    return;
  GhMlsPoll *poll = g_hash_table_lookup(ui->polls, target);
  if (!poll)
    return;
  gint64 when = gh_message_get_created_at(message);
  if (when >= gh_mls_poll_get_created_at(poll) && gh_mls_poll_is_open(poll, when))
    gh_mls_poll_apply_vote(poll, gh_message_get_sender(message),
                            (const gchar **) choices, when,
                            gh_message_get_rumor_id(message));
}

static void
rebuild(PollUi *ui)
{
  GHashTable *old = ui->polls;
  ui->polls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  if (ui->shown) {
    GListModel *messages = G_LIST_MODEL(ui->shown);
    guint count = g_list_model_get_n_items(messages);
    /* Reuse live poll objects so on-screen cards keep receiving tally changes. */
    for (guint i = 0; i < count; i++) {
      g_autoptr(GhMessage) message = g_list_model_get_item(messages, i);
      if (gh_message_get_kind(message) != GH_MESSAGE_MLS_POLL_KIND ||
          gh_message_get_withdrawn(message))
        continue;
      const gchar *id = gh_message_get_rumor_id(message);
      GhMlsPoll *poll = old ? g_hash_table_lookup(old, id) : NULL;
      if (poll) {
        g_object_ref(poll);
        gh_mls_poll_reset_votes(poll);
      } else {
        poll = gh_mls_poll_new_from_event(id, gh_message_get_sender(message),
                                          gh_message_get_created_at(message),
                                          gh_message_get_rumor_json(message), NULL);
      }
      if (!poll)
        continue;
      gh_mls_poll_set_local_account(poll, gh_conversation_get_account(ui->shown));
      g_hash_table_insert(ui->polls, g_strdup(id), poll);
    }
    /* Votes may arrive before definitions; two passes also make history
     * paging independent of arrival order. The model picks the newest vote
     * from each pubkey, as NIP-88 requires. */
    for (guint i = 0; i < count; i++) {
      g_autoptr(GhMessage) message = g_list_model_get_item(messages, i);
      if (gh_message_get_kind(message) == GH_MESSAGE_MLS_POLL_VOTE_KIND)
        apply_vote(ui, message);
    }
  }
  if (old)
    g_hash_table_unref(old);
}

static void
on_messages_changed(GhWindow *window)
{
  PollUi *ui = ui_of(window);
  if (ui)
    rebuild(ui);
}

static void
sync_button(PollUi *ui)
{
  gboolean allowed = ui->shown != NULL;
  if (allowed && gh_conversation_get_backend(ui->shown) == GH_CONVERSATION_BACKEND_MLS) {
    GhMlsService *service = ui->mls_service ? ui->mls_service(ui->service_data) : NULL;
    GhMlsGroup *group = service ? gh_mls_service_lookup(
      service, gh_conversation_get_room_id(ui->shown)) : NULL;
    allowed = group && gh_mls_group_get_active(group) && !gh_mls_group_get_leaving(group);
  }
  gh_composer_set_can_create_poll(ui->composer, allowed);
}

static void
on_view_changed(GhWindow *window)
{
  PollUi *ui = ui_of(window);
  if (!ui)
    return;
  if (ui->shown)
    g_signal_handlers_disconnect_by_func(ui->shown, on_messages_changed, window);
  g_set_object(&ui->shown, gh_conversation_view_get_conversation(ui->view));
  if (ui->shown)
    g_signal_connect_object(ui->shown, "items-changed", G_CALLBACK(on_messages_changed),
                            window, G_CONNECT_SWAPPED);
  rebuild(ui);
  sync_button(ui);
}

static GhOutbox *
current_outbox(PollUi *ui)
{
  GObject *object = ui->account_store ? gh_account_store_get_outbox(ui->account_store) : NULL;
  return GH_IS_OUTBOX(object) ? GH_OUTBOX(object) : NULL;
}

static const gchar *const *
private_recipients(GhConversation *conversation, const gchar **self_recipient)
{
  const gchar *const *peers = gh_conversation_get_peers(conversation);
  if (peers && peers[0])
    return peers;
  self_recipient[0] = gh_conversation_get_account(conversation);
  self_recipient[1] = NULL;
  return self_recipient;
}

static gboolean
cast_vote(PollUi *ui, GhMessage *message, const gchar **options,
          guint n_options, GError **error)
{
  GhConversationBackend backend = gh_conversation_get_backend(ui->shown);
  const gchar *id = gh_message_get_rumor_id(message);
  if (backend == GH_CONVERSATION_BACKEND_MLS) {
    GhMlsService *service = ui->mls_service ? ui->mls_service(ui->service_data) : NULL;
    GhMlsGroup *group = service ? gh_mls_service_lookup(
      service, gh_conversation_get_room_id(ui->shown)) : NULL;
    g_autoptr(GhMessage) vote = group ? gh_mls_service_cast_vote(
      service, group, id, options, n_options, error) : NULL;
    return vote != NULL;
  }
  if (backend == GH_CONVERSATION_BACKEND_NIP29) {
    GhNip29Service *service = ui->nip29_service ? ui->nip29_service(ui->service_data) : NULL;
    g_autoptr(GhNip29Room) room = service ? gh_nip29_service_lookup_room(
      service, gh_conversation_get_room_id(ui->shown)) : NULL;
    g_autoptr(GhNip29Op) op = room ? gh_nip29_service_cast_poll_vote(
      service, room, id, options, n_options, error) : NULL;
    return op != NULL;
  }
  GhOutbox *outbox = current_outbox(ui);
  if (outbox) {
    g_autofree gchar *event = gh_mls_poll_build_vote_event(
      gh_conversation_get_account(ui->shown), NULL, g_get_real_time() / G_USEC_PER_SEC,
      id, options, n_options, error);
    const gchar *self_recipient[2];
    const gchar *const *recipients = private_recipients(ui->shown, self_recipient);
    g_autoptr(GhOutboxItem) item = event ? gh_outbox_send_poll_event_room(
      outbox, recipients, event, error) : NULL;
    return item != NULL;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                      "Private message storage is unavailable");
  return FALSE;
}

static void
on_vote_cast(GhPollCard *card, const gchar **options, guint n_options, PollUi *ui)
{
  GhMessage *message = g_object_get_data(G_OBJECT(card), "poll-message");
  if (!message || !ui->shown)
    return;
  g_autoptr(GError) error = NULL;
  if (!cast_vote(ui, message, options, n_options, &error)) {
    g_message("Groundhog: could not cast poll vote: %s", error ? error->message : "unknown");
    adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window),
                                adw_toast_new(_("The vote couldn’t be sent")));
  }
}

static void
poll_enricher(GhMessageRow *row, GhMessage *message, gpointer data)
{
  PollUi *ui = data;
  if (!message || gh_message_get_kind(message) != GH_MESSAGE_MLS_POLL_KIND) {
    gh_message_row_set_poll_widget(row, NULL);
    return;
  }
  const gchar *id = gh_message_get_rumor_id(message);
  GhMlsPoll *poll = g_hash_table_lookup(ui->polls, id);
  if (!poll) {
    /* A mapped list may bind its first rows before notify::conversation has
     * switched shown. Project from the row's authenticated message itself;
     * the subsequent room rebuild reuses this object and adds its votes. */
    poll = gh_mls_poll_new_from_event(id, gh_message_get_sender(message),
                                      gh_message_get_created_at(message),
                                      gh_message_get_rumor_json(message), NULL);
    if (poll) {
      gh_mls_poll_set_local_account(poll, gh_message_get_account(message));
      g_hash_table_insert(ui->polls, g_strdup(id), poll);
    }
  }
  if (!poll) {
    gh_message_row_set_poll_widget(row, NULL);
    return;
  }
  GtkWidget *card = gh_poll_card_new();
  gh_poll_card_set_poll(GH_POLL_CARD(card), poll);
  g_object_set_data_full(G_OBJECT(card), "poll-message", g_object_ref(message), g_object_unref);
  g_signal_connect(card, "vote-cast", G_CALLBACK(on_vote_cast), ui);
  gh_message_row_set_poll_widget(row, card);
}

static gboolean
create_poll(PollUi *ui, const gchar *question, const gchar **options, guint n_options,
            GhMlsPollType type, gint64 ends_at, GError **error)
{
  GhConversationBackend backend = gh_conversation_get_backend(ui->shown);
  if (backend == GH_CONVERSATION_BACKEND_MLS) {
    GhMlsService *service = ui->mls_service ? ui->mls_service(ui->service_data) : NULL;
    GhMlsGroup *group = service ? gh_mls_service_lookup(
      service, gh_conversation_get_room_id(ui->shown)) : NULL;
    g_autoptr(GhMessage) message = group ? gh_mls_service_create_poll(
      service, group, question, options, n_options, type, ends_at, error) : NULL;
    return message != NULL;
  }
  if (backend == GH_CONVERSATION_BACKEND_NIP29) {
    GhNip29Service *service = ui->nip29_service ? ui->nip29_service(ui->service_data) : NULL;
    g_autoptr(GhNip29Room) room = service ? gh_nip29_service_lookup_room(
      service, gh_conversation_get_room_id(ui->shown)) : NULL;
    g_autoptr(GhNip29Op) op = room ? gh_nip29_service_create_poll(
      service, room, question, options, n_options, type == GH_MLS_POLL_MULTIPLE_CHOICE,
      ends_at, error) : NULL;
    return op != NULL;
  }
  GhOutbox *outbox = current_outbox(ui);
  if (outbox) {
    g_autofree gchar *event = gh_mls_poll_build_event(
      gh_conversation_get_account(ui->shown), NULL, g_get_real_time() / G_USEC_PER_SEC,
      question, options, n_options, type, ends_at, error);
    const gchar *self_recipient[2];
    const gchar *const *recipients = private_recipients(ui->shown, self_recipient);
    g_autoptr(GhOutboxItem) item = event ? gh_outbox_send_poll_event_room(
      outbox, recipients, event, error) : NULL;
    return item != NULL;
  }
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                      "Private message storage is unavailable");
  return FALSE;
}

static void
on_poll_created(GhCreatePollDialog *dialog, const gchar *question,
                const gchar **options, guint n_options, gint type,
                gint64 ends_at, PollUi *ui)
{
  (void)dialog;
  if (!ui->shown)
    return;
  g_autoptr(GError) error = NULL;
  if (!create_poll(ui, question, options, n_options, (GhMlsPollType)type, ends_at, &error)) {
    g_message("Groundhog: could not create poll: %s", error ? error->message : "unknown");
    adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window),
                                adw_toast_new(_("The poll couldn’t be created")));
  }
}

static void
on_poll_requested(GhWindow *window)
{
  PollUi *ui = ui_of(window);
  if (!ui || !ui->shown)
    return;
  GtkWidget *dialog = gh_create_poll_dialog_new();
  g_signal_connect(dialog, "poll-created", G_CALLBACK(on_poll_created), ui);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ui->window));
}

static void
poll_ui_free(gpointer data)
{
  PollUi *ui = data;
  if (ui->shown)
    g_signal_handlers_disconnect_by_func(ui->shown, on_messages_changed, ui->window);
  g_clear_object(&ui->shown);
  g_clear_pointer(&ui->polls, g_hash_table_unref);
  g_free(ui);
}

void
gh_poll_ui_attach(GhWindow *window, const GhPollUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window) && config && !ui_of(window));
  GhContentPage *content = gh_window_get_content(window);
  GtkWidget *view = content ? gh_content_page_get_view(content) : NULL;
  GhComposer *composer = content ? gh_content_page_get_composer(content) : NULL;
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(view) && GH_IS_COMPOSER(composer));
  PollUi *ui = g_new0(PollUi, 1);
  ui->window = window;
  ui->view = GH_CONVERSATION_VIEW(view);
  ui->composer = composer;
  ui->account_store = config->account_store;
  ui->mls_service = config->mls_service;
  ui->nip29_service = config->nip29_service;
  ui->service_data = config->service_data;
  ui->polls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  g_object_set_data_full(G_OBJECT(window), GH_POLL_UI_DATA, ui, poll_ui_free);
  gh_conversation_view_set_row_enricher(ui->view, poll_enricher, ui);
  g_signal_connect_object(view, "notify::conversation", G_CALLBACK(on_view_changed),
                          window, G_CONNECT_SWAPPED);
  g_signal_connect_object(composer, "poll-requested", G_CALLBACK(on_poll_requested),
                          window, G_CONNECT_SWAPPED);
  on_view_changed(window);
}
