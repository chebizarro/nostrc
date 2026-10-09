#include "gh-group-ui.h"

#include "gh-composer.h"
#include "gh-conversation-view.h"
#include "gh-group-copy.h"
#include "gh-group-join-dialog.h"
#include "gh-new-group-dialog.h"
#include "gh-send-ui.h"
#include "gh-store.h"
#include "gh-store-nip29.h"

#include <glib/gi18n.h>

#define GROUP_UI_DATA "groundhog-group-ui"

typedef struct {
  GhWindow *window; /* not owned: the struct is its data */
  GhConversationStore *model;
  GhGroupUiServiceFunc service_func;
  gpointer service_data;
  GhGroupNameFunc display_name;
  gpointer names_data;
  GhConversationListLoadOlder load_older;
  GhConversationListLoadOlder load_newer;
  GhConversationListLoadOlder reset_latest;
  gpointer load_older_data;
  GSimpleAction *join_action;
  GSimpleAction *new_action;
  GhNip29Service *watched; /* weak: whose rooms refresh the composer */
  GhGroupUiNewGroupFunc extend_new; /* qp24.13 part 2: the encrypted page */
  gpointer extend_new_data;
} GroupUi;

static GhNip29Service *
current(GroupUi *ui)
{
  return ui->service_func(ui->service_data);
}

static GroupUi *
ui_of(GhWindow *window)
{
  return g_object_get_data(G_OBJECT(window), GROUP_UI_DATA);
}

/* ---- the composer's reason and member suggestions follow the rooms ------------------- */

static void
sync_mention_members(GhWindow *window)
{
  GroupUi *ui = ui_of(window);
  GhContentPage *content = gh_window_get_content(window);
  GhComposer *composer = content ? gh_content_page_get_composer(content) : NULL;
  GhConversationView *view = content ? GH_CONVERSATION_VIEW(gh_content_page_get_view(content))
                                     : NULL;
  GhConversation *conversation = view ? gh_conversation_view_get_conversation(view) : NULL;
  GhNip29Service *service = ui ? current(ui) : NULL;
  if (!composer || !conversation || !service ||
      gh_conversation_get_backend(conversation) != GH_CONVERSATION_BACKEND_NIP29)
    return;
  g_autoptr(GhNip29Room) room = gh_nip29_service_lookup_room(
    service, gh_conversation_get_room_id(conversation));
  const GhNip29Group *group = room ? gh_nip29_room_get_group(room) : NULL;
  g_auto(GStrv) members = NULL;
  if (group)
    gh_nip29_group_dup_members(group, &members);
  gh_composer_set_mention_members(composer, gh_conversation_get_room_id(conversation),
                                   (const gchar *const *) members);
}


static void
refresh_composer(GhWindow *window)
{
  gh_send_ui_refresh(window);
}

static void
watch_rooms(GroupUi *ui)
{
  if (!ui->watched)
    return;
  GListModel *rooms = G_LIST_MODEL(ui->watched);
  for (guint i = 0; i < g_list_model_get_n_items(rooms); i++) {
    g_autoptr(GhNip29Room) room = g_list_model_get_item(rooms, i);
    g_signal_handlers_disconnect_by_func(room, refresh_composer, ui->window);
    g_signal_connect_object(room, "notify::join-state", G_CALLBACK(refresh_composer),
                            ui->window, G_CONNECT_SWAPPED);
    g_signal_connect_object(room, "notify::is-restricted", G_CALLBACK(refresh_composer),
                            ui->window, G_CONNECT_SWAPPED);
    g_signal_handlers_disconnect_by_func(room, sync_mention_members, ui->window);
    g_signal_connect_object(room, "group-changed", G_CALLBACK(sync_mention_members),
                            ui->window, G_CONNECT_SWAPPED);
  }
}

static void
on_rooms_changed(GhWindow *window)
{
  GroupUi *ui = ui_of(window);
  if (ui) {
    watch_rooms(ui);
    gh_send_ui_refresh(window);
    sync_mention_members(window);
  }
}

static void sync_service(GroupUi *ui);

static void
on_watched_gone(gpointer data, GObject *where)
{
  GroupUi *ui = data;
  (void)where;
  ui->watched = NULL;
  g_simple_action_set_enabled(ui->join_action, FALSE);
  g_simple_action_set_enabled(ui->new_action, FALSE);
}

static void
sync_service(GroupUi *ui)
{
  GhNip29Service *service = current(ui);
  g_simple_action_set_enabled(ui->join_action, service != NULL);
  g_simple_action_set_enabled(ui->new_action, service != NULL);
  if (service != ui->watched) {
    if (ui->watched) {
      g_signal_handlers_disconnect_by_func(ui->watched, on_rooms_changed, ui->window);
      g_object_weak_unref(G_OBJECT(ui->watched), on_watched_gone, ui);
    }
    ui->watched = service;
    if (service) {
      g_object_weak_ref(G_OBJECT(service), on_watched_gone, ui);
      g_signal_connect_object(service, "items-changed", G_CALLBACK(on_rooms_changed),
                              ui->window, G_CONNECT_SWAPPED);
      watch_rooms(ui);
    }
  }
  gh_send_ui_refresh(ui->window);
  sync_mention_members(ui->window);
}

static void
on_state_changed(GhWindow *window)
{
  GroupUi *ui = ui_of(window);
  if (ui)
    sync_service(ui);
}

/* ---- the composer delegate ------------------------------------------------------------- */

static gboolean
delegate_handles(GhConversation *conversation, gpointer data)
{
  (void)data;
  return gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29;
}

static gchar *
delegate_reason(GhConversation *conversation, gpointer data)
{
  return gh_group_room_send_reason(current(data), gh_conversation_get_room_id(conversation));
}

static gboolean
delegate_send(GhConversation *conversation, const gchar *text, gpointer data, GError **error)
{
  GhNip29Service *service = current(data);
  g_autoptr(GhNip29Room) room = service ? gh_nip29_service_lookup_room(
                                            service, gh_conversation_get_room_id(conversation))
                                        : NULL;
  if (!room) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        _("This group isn’t available, so the message was not sent. It is kept "
                          "here."));
    return FALSE;
  }
  g_autoptr(GError) local = NULL;
  /* T-enqueue before any signer call; the service lists the message at once. */
  g_autoptr(GhNip29Op) op = gh_nip29_service_send(service, room, text, &local);
  if (op)
    return TRUE;
  g_message("Groundhog could not queue a group message: %s", local->message);
  if (g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_FULL))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        _("Storage is full, so this message was not sent. It is kept here."));
  else if (g_error_matches(local, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        _("Join this group to write in it. The message is kept here."));
  else
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("This message couldn't be queued for sending. It is kept here."));
  return FALSE;
}

static gboolean
delegate_retry(GhMessage *message, gpointer data, GError **error)
{
  GhNip29Service *service = current(data);
  g_autoptr(GhNip29Op) op = gh_group_find_message_op(service,
                                                     gh_message_get_rumor_id(message));
  if (!op) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "No queued group message to retry");
    return FALSE;
  }
  return gh_nip29_outbox_retry(gh_nip29_service_get_outbox(service), op, error);
}

static GhDeliveryReport *
delegate_report(GhMessage *message, gpointer data)
{
  g_autoptr(GhNip29Op) op = gh_group_find_message_op(current(data),
                                                     gh_message_get_rumor_id(message));
  if (!op)
    return NULL;
  GhDeliveryReport *report = gh_delivery_report_new();
  GhNip29OpResult result = gh_nip29_op_get_result(op);
  g_autofree gchar *outcome = gh_group_op_outcome(op);
  gh_delivery_report_add(report, NULL, gh_nip29_op_get_relay_url(op),
                         result == GH_NIP29_OP_ACCEPTED || result == GH_NIP29_OP_DUPLICATE,
                         outcome);
  report->next_attempt_at = gh_nip29_op_get_next_attempt_at(op);
  return report;
}

static const GhSendUiDelegate group_delegate = {
  .handles = delegate_handles,
  .reason = delegate_reason,
  .send = delegate_send,
  .retry = delegate_retry,
  .report = delegate_report,
};

/* ---- older history ----------------------------------------------------------------------- */

static gboolean
load_older(GhConversation *conversation, GError **error, gpointer data)
{
  GroupUi *ui = data;
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29) {
    GhNip29Service *service = current(ui);
    if (!service) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "No message storage is open");
      return FALSE;
    }
    return gh_nip29_service_load_older(service, conversation, GH_CONVERSATION_WINDOW_PAGE, NULL,
                                       error);
  }
  if (!ui->load_older) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "Older messages aren't stored");
    return FALSE;
  }
  return ui->load_older(conversation, error, ui->load_older_data);
}

static gboolean
load_newer(GhConversation *conversation, GError **error, gpointer data)
{
  GroupUi *ui = data;
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29) {
    GhNip29Service *service = current(ui);
    if (!service) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "No message storage is open");
      return FALSE;
    }
    return gh_nip29_service_load_newer(service, conversation, GH_CONVERSATION_WINDOW_PAGE,
                                       NULL, error);
  }
  return ui->load_newer && ui->load_newer(conversation, error, ui->load_older_data);
}

static gboolean
reset_latest(GhConversation *conversation, GError **error, gpointer data)
{
  GroupUi *ui = data;
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29) {
    GhNip29Service *service = current(ui);
    if (!service) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                          "No message storage is open");
      return FALSE;
    }
    return gh_nip29_service_reset_latest(service, conversation, error);
  }
  return ui->reset_latest && ui->reset_latest(conversation, error, ui->load_older_data);
}

/* ---- dialogs ------------------------------------------------------------------------------ */

static void
open_room(GhWindow *window, GhNip29Room *room)
{
  GroupUi *ui = ui_of(window);
  GhConversation *conversation = ui ? gh_conversation_store_lookup(
                                        ui->model, gh_nip29_room_get_room_id(room))
                                    : NULL;
  if (!conversation || !gh_window_open_item(window, conversation))
    adw_toast_overlay_add_toast(gh_window_get_toasts(window),
                                adw_toast_new(_("The group is listed once the relay has "
                                                "answered")));
}

static void
on_join(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GroupUi *ui = data;
  (void)action;
  (void)parameter;
  GhNip29Service *service = current(ui);
  if (!service)
    return;
  GhGroupJoinDialog *dialog = gh_group_join_dialog_new(service);
  g_signal_connect_object(dialog, "open-group", G_CALLBACK(open_room), ui->window,
                          G_CONNECT_SWAPPED);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ui->window));
}

static void
on_new(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GroupUi *ui = data;
  (void)action;
  (void)parameter;
  GhNip29Service *service = current(ui);
  if (!service)
    return;
  GhNewGroupDialog *dialog = gh_new_group_dialog_new(service);
  g_signal_connect_object(dialog, "open-group", G_CALLBACK(open_room), ui->window,
                          G_CONNECT_SWAPPED);
  if (ui->extend_new)
    ui->extend_new(ui->window, dialog, ui->extend_new_data);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ui->window));
}

gboolean
gh_group_ui_show_info(GhWindow *window, GhConversation *conversation, gpointer user_data)
{
  (void)user_data;
  g_return_val_if_fail(GH_IS_WINDOW(window), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  GroupUi *ui = ui_of(window);
  GhNip29Service *service = ui ? current(ui) : NULL;
  g_autoptr(GhNip29Room) room = service ? gh_nip29_service_lookup_room(
                                            service, gh_conversation_get_room_id(conversation))
                                        : NULL;
  if (!room)
    return FALSE;
  GhGroupInfoConfig config = {
    .service = service,
    .display_name = ui->display_name,
    .names_data = ui->names_data,
  };
  GhGroupInfoDialog *dialog = gh_group_info_dialog_new(room, &config);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  return TRUE;
}

void
gh_group_ui_set_new_group_extension(GhWindow *window, GhGroupUiNewGroupFunc func,
                                    gpointer user_data)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  GroupUi *ui = ui_of(window);
  g_return_if_fail(ui != NULL);
  ui->extend_new = func;
  ui->extend_new_data = func ? user_data : NULL;
}

/* ---- attach -------------------------------------------------------------------------------- */

static void
group_ui_free(gpointer data)
{
  GroupUi *ui = data;
  if (ui->watched)
    g_object_weak_unref(G_OBJECT(ui->watched), on_watched_gone, ui);
  g_clear_object(&ui->join_action);
  g_clear_object(&ui->new_action);
  g_clear_object(&ui->model);
  g_free(ui);
}

void
gh_group_ui_attach(GhWindow *window, const GhGroupUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL && config->service != NULL);
  g_return_if_fail(GH_IS_CONVERSATION_STORE(config->conversations));
  g_return_if_fail(ui_of(window) == NULL);
  GroupUi *ui = g_new0(GroupUi, 1);
  ui->window = window;
  ui->model = g_object_ref(config->conversations);
  ui->service_func = config->service;
  ui->service_data = config->service_data;
  ui->display_name = config->display_name;
  ui->names_data = config->names_data;
  ui->load_older = config->load_older;
  ui->load_newer = config->load_newer;
  ui->reset_latest = config->reset_latest;
  ui->load_older_data = config->load_older_data;
  g_object_set_data_full(G_OBJECT(window), GROUP_UI_DATA, ui, group_ui_free);

  ui->join_action = g_simple_action_new("join-group", NULL);
  g_signal_connect(ui->join_action, "activate", G_CALLBACK(on_join), ui);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(ui->join_action));
  ui->new_action = g_simple_action_new("new-group", NULL);
  g_signal_connect(ui->new_action, "activate", G_CALLBACK(on_new), ui);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(ui->new_action));

  if (config->state_source)
    g_signal_connect_object(config->state_source, "changed", G_CALLBACK(on_state_changed),
                            window, G_CONNECT_SWAPPED);
  gh_conversation_list_set_history_source(window, load_older, ui, NULL);
  gh_conversation_list_set_window_source(window, load_newer, reset_latest, ui, NULL);
  gh_send_ui_set_delegate(window, &group_delegate, ui);
  GhContentPage *content = gh_window_get_content(window);
  GtkWidget *view = content ? gh_content_page_get_view(content) : NULL;
  if (GH_IS_CONVERSATION_VIEW(view))
    g_signal_connect_object(view, "notify::conversation", G_CALLBACK(sync_mention_members),
                            window, G_CONNECT_SWAPPED);
  sync_service(ui);
}
