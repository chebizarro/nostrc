#include "gh-mls-ui.h"

#include "gh-mls-attachments.h"
#include "gh-composer.h"
#include "gh-create-poll-dialog.h"
#include "gh-message-row.h"
#include "gh-poll-card.h"
#include "gh-shell.h"

#include "gh-conversation-list.h"
#include "gh-conversation-view.h"
#include "gh-features.h"
#include "gh-mls-copy.h"
#include "gh-mls-group-info-dialog.h"
#include "gh-mls-invites-dialog.h"
#include "gh-mls-new-group-page.h"
#include "gh-recipient.h"
#include "gh-send-ui.h"
#include "gh-store.h"

#include <glib/gi18n.h>

#define MLS_UI_DATA "groundhog-mls-ui"

typedef struct {
  GhWindow *window; /* not owned: the struct is its data */
  GhConversationStore *model;
  GhAccountController *accounts;
  GSettings *settings;
  GhMlsUiServiceFunc service_func;
  gpointer service_data;
  GhMlsNameFunc display_name;
  gpointer names_data;
  GhAccountRelays *account_relays;
  guint lookup_deadline;
  GhMlsAttachments *files;  /* nullable (W25): the group picture */
  GSimpleAction *invites_action;
  GhMlsService *watched;   /* weak */
  GhMlsGroup *shown;       /* the shown conversation's group (ref), or NULL */
  guint invitations;
  GhConversationView *view; /* a reference: the list may keep it past the window */
  gulong view_handler;
  gulong poll_requested_handler;
} MlsUi;

/* Forward declarations for poll wiring (W26). */
static void rebuild_polls(MlsUi *ui);
static void sync_poll_button(MlsUi *ui);

static MlsUi *
ui_of(GhWindow *window)
{
  return window ? g_object_get_data(G_OBJECT(window), MLS_UI_DATA) : NULL;
}

static GhMlsService *
current(MlsUi *ui)
{
  return ui->service_func(ui->service_data);
}

static GhMlsGroup *
group_of(MlsUi *ui, GhConversation *conversation)
{
  GhMlsService *service = current(ui);
  if (!service || !conversation ||
      gh_conversation_get_backend(conversation) != GH_CONVERSATION_BACKEND_MLS)
    return NULL;
  return gh_mls_service_lookup(service, gh_conversation_get_room_id(conversation));
}

static void
context_of(MlsUi *ui, GhMlsUiContext *context)
{
  memset(context, 0, sizeof *context);
  context->service = current(ui);
  context->accounts = ui->accounts;
  context->model = ui->model;
  context->settings = ui->settings;
  context->display_name = ui->display_name;
  context->names_data = ui->names_data;
  context->lookup_deadline = ui->lookup_deadline;
  context->files = ui->files;
  context->default_relays = ui->account_relays
    ? gh_account_relays_get_write_relays(ui->account_relays) : NULL;
}

static void
open_group(GhWindow *window, GhMlsGroup *group)
{
  MlsUi *ui = ui_of(window);
  GhConversation *conversation = ui ? gh_conversation_store_lookup(
                                        ui->model, gh_mls_group_get_room_id(group))
                                    : NULL;
  if (!conversation || !gh_window_open_item(window, conversation))
    adw_toast_overlay_add_toast(gh_window_get_toasts(window),
                                adw_toast_new(_("The group is listed in a moment")));
}

/* ---- the shown group: its header count and what waits for a Commit ------------------- */

static GhConversationView *
view_of(MlsUi *ui)
{
  return ui->view;
}

static void
sync_unreadable(MlsUi *ui)
{
  GhConversationView *view = view_of(ui);
  if (!view)
    return;
  gh_conversation_view_set_decrypt_pending(
    view, ui->shown && gh_mls_group_get_decrypt_pending(ui->shown));
  /* A change refused for good is said as such, by its recorded cause,
   * never as a wait (nostrc-prrl, W24 review L4). */
  gboolean refused = ui->shown && gh_mls_group_get_active(ui->shown) &&
                     gh_mls_group_get_change_refused(ui->shown);
  gh_conversation_view_set_unreadable_reason(
    view, refused ? gh_mls_refused_copy(gh_mls_group_get_refusal(ui->shown)) : NULL);
}

static void
on_shown_members(MlsUi *ui)
{
  gh_conversation_list_refresh_title(ui->window);
}

static void
on_shown_state(MlsUi *ui)
{
  sync_unreadable(ui);
  gh_send_ui_refresh(ui->window);
}

/* nostrc-2um6: a member of the shown group who asked to leave is out. */
static void
on_shown_member_left(GhMlsGroup *group, const gchar *pubkey, gpointer data)
{
  (void)group;
  MlsUi *ui = data;
  const gchar *name = ui->display_name ? ui->display_name(pubkey, ui->names_data) : NULL;
  g_autofree gchar *npub = gh_recipient_npub_short(pubkey);
  g_autofree gchar *words = gh_mls_member_left_copy(name && *name ? name : npub);
  adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window), adw_toast_new(words));
}

/* nostrc-xrza: the shown group followed another branch. */
static void
on_shown_conflict(GhMlsGroup *group, guint withdrawn, guint undone, gpointer data)
{
  (void)group;
  MlsUi *ui = data;
  g_autofree gchar *words = gh_mls_conflict_copy(withdrawn, undone);
  AdwToast *toast = adw_toast_new(words);
  adw_toast_set_timeout(toast, 0);   /* read it: it says what changed */
  adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window), toast);
  gh_conversation_list_refresh_title(ui->window);
}

static void
set_shown(MlsUi *ui, GhMlsGroup *group)
{
  if (ui->shown == group)
    return;
  if (ui->shown)
    g_signal_handlers_disconnect_by_data(ui->shown, ui);
  g_set_object(&ui->shown, group);
  if (group) {
    g_signal_connect_swapped(group, "members-changed", G_CALLBACK(on_shown_members), ui);
    g_signal_connect_swapped(group, "notify::decrypt-pending", G_CALLBACK(on_shown_state), ui);
    g_signal_connect_swapped(group, "notify::change-refused", G_CALLBACK(on_shown_state), ui);
    g_signal_connect_swapped(group, "notify::active", G_CALLBACK(on_shown_state), ui);
    g_signal_connect_swapped(group, "notify::end", G_CALLBACK(on_shown_state), ui);
    g_signal_connect_swapped(group, "notify::read-state", G_CALLBACK(on_shown_state), ui);
    g_signal_connect_swapped(group, "notify::leaving", G_CALLBACK(on_shown_state), ui);
    g_signal_connect(group, "member-left", G_CALLBACK(on_shown_member_left), ui);
    g_signal_connect(group, "conflict-resolved", G_CALLBACK(on_shown_conflict), ui);
    g_signal_connect_swapped(group, "notify::name", G_CALLBACK(on_shown_members), ui);
  }
  sync_unreadable(ui);
  gh_conversation_list_refresh_title(ui->window);
  gh_send_ui_refresh(ui->window);
  rebuild_polls(ui);
  sync_poll_button(ui);
}

static void
sync_shown(MlsUi *ui)
{
  GhConversationView *view = view_of(ui);
  set_shown(ui, group_of(ui, view ? gh_conversation_view_get_conversation(view) : NULL));
}

static guint
member_count(GhConversation *conversation, gpointer data)
{
  GhMlsGroup *group = group_of(data, conversation);
  g_auto(GStrv) members = group ? gh_mls_group_dup_members(group) : NULL;
  return members ? g_strv_length(members) : 0;
}

/* ---- invitations ---------------------------------------------------------------------- */

static void
sync_invitations(MlsUi *ui)
{
  GhMlsService *service = current(ui);
  g_autoptr(GPtrArray) invites = service ? gh_mls_service_list_invites(service, NULL) : NULL;
  ui->invitations = invites ? invites->len : 0;
  gh_sidebar_page_set_invitations(gh_window_get_sidebar(ui->window), ui->invitations);
}

static void
on_invite_received(MlsUi *ui)
{
  sync_invitations(ui);
  /* PD-8: no name, whoever sent it; the list shows only a stranger's npub. */
  AdwToast *toast = adw_toast_new(_("New invitation to an encrypted group"));
  adw_toast_set_button_label(toast, _("_View"));
  adw_toast_set_action_name(toast, "win.group-invitations");
  adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window), toast);
}

static void
on_invites(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  MlsUi *ui = data;
  GhMlsUiContext context;
  context_of(ui, &context);
  if (!context.service)
    return;
  GhMlsInvitesDialog *dialog = gh_mls_invites_dialog_new(&context);
  g_signal_connect_object(dialog, "open-group", G_CALLBACK(open_group), ui->window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_swapped(dialog, "closed", G_CALLBACK(sync_invitations), ui);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ui->window));
}

/* ---- the service ---------------------------------------------------------------------- */

static void
on_groups_changed(MlsUi *ui)
{
  sync_shown(ui);
  sync_invitations(ui);   /* an accepted invitation became a group */
  gh_send_ui_refresh(ui->window);
}

static void
on_watched_gone(gpointer data, GObject *where)
{
  MlsUi *ui = data;
  (void)where;
  ui->watched = NULL;
  set_shown(ui, NULL);
  g_simple_action_set_enabled(ui->invites_action, FALSE);
}

static void
sync_service(MlsUi *ui)
{
  GhMlsService *service = current(ui);
  g_simple_action_set_enabled(ui->invites_action, service != NULL);
  if (service != ui->watched) {
    if (ui->watched) {
      g_signal_handlers_disconnect_by_data(ui->watched, ui);
      g_object_weak_unref(G_OBJECT(ui->watched), on_watched_gone, ui);
    }
    ui->watched = service;
    if (service) {
      g_object_weak_ref(G_OBJECT(service), on_watched_gone, ui);
      g_signal_connect_swapped(service, "invite-received", G_CALLBACK(on_invite_received), ui);
      g_signal_connect_swapped(service, "items-changed", G_CALLBACK(on_groups_changed), ui);
    }
  }
  sync_invitations(ui);
  sync_shown(ui);
  gh_send_ui_refresh(ui->window);
}

static void
on_state_changed(GhWindow *window)
{
  MlsUi *ui = ui_of(window);
  if (ui)
    sync_service(ui);
}

/* ---- the composer delegate -------------------------------------------------------------- */

static gboolean
delegate_handles(GhConversation *conversation, gpointer data)
{
  (void)data;
  return gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_MLS;
}

/* Who removed the account from group: a contact's cached name, else the
 * short npub; NULL when not removed or not known (transfer full). */
static gchar *
remover_label(MlsUi *ui, GhMlsGroup *group)
{
  const gchar *by = group ? gh_mls_group_get_removed_by(group) : NULL;
  if (!by)
    return NULL;
  const gchar *name = ui->display_name ? ui->display_name(by, ui->names_data) : NULL;
  return name && *name ? g_strdup(name) : gh_recipient_npub_short(by);
}

static gchar *
delegate_reason(GhConversation *conversation, gpointer data)
{
  MlsUi *ui = data;
  GhMlsService *service = current(ui);
  GhMlsGroup *group = group_of(ui, conversation);
  g_autofree gchar *remover = remover_label(ui, group);
  return gh_mls_send_reason(service, group, remover);
}

static gboolean
delegate_send(GhConversation *conversation, const gchar *text, gpointer data, GError **error)
{
  MlsUi *ui = data;
  GhMlsService *service = current(ui);
  GhMlsGroup *group = group_of(ui, conversation);
  if (!group) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        _("This group isn’t available, so the message was not sent. It is kept "
                          "here."));
    return FALSE;
  }
  g_autoptr(GError) local = NULL;
  /* One transaction (the message row, the ratchet step, the sealed 445)
   * before anything is published; the service lists the message at once. */
  g_autoptr(GhMessage) message = gh_mls_service_send(service, group, text, &local);
  if (message)
    return TRUE;
  g_message("Groundhog could not send to an encrypted group: %s", local->message);
  if (g_error_matches(local, GH_STORE_ERROR, GH_STORE_ERROR_FULL))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                        _("Storage is full, so this message was not sent. It is kept here."));
  else if (g_error_matches(local, GH_MLS_SERVICE_ERROR, GH_MLS_SERVICE_ERROR_INACTIVE))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        _("Encrypted groups send only while you’re online. The message is kept "
                          "here."));
  else if (gh_mls_group_get_end(group) == GH_MLS_GROUP_END_REMOVED)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        _("You were removed from this group, so the message was not sent. It "
                          "is kept here."));
  else if (gh_mls_group_get_end(group) == GH_MLS_GROUP_END_LEFT ||
           gh_mls_group_get_end(group) == GH_MLS_GROUP_END_LEFT_DEVICE)
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        _("You left this group, so the message was not sent. It is kept here."));
  else if (gh_mls_group_get_leaving(group))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        _("You’re leaving this group, so the message was not sent. It is kept "
                          "here."));
  else if (!gh_mls_group_get_active(group))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
                        _("This group has ended, so the message was not sent. It is kept "
                          "here."));
  else
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        _("This message couldn’t be sent. It is kept here."));
  return FALSE;
}

static const GhSendUiDelegate mls_delegate = {
  .handles = delegate_handles,
  .reason = delegate_reason,
  .send = delegate_send,
  /* A sent message is republished by the service until a relay takes it. */
  .retry = NULL,
  .report = NULL,
};


/* ---- polls (W26 slice C, nostrc-skc7) ------------------------------------------------- */

/* Rebuild poll state from stored messages when switching to a group, so polls
 * survive a restart without a schema change: poll definitions and votes are
 * already in the messages table (kind 1068 / 1018). */
static void
rebuild_polls(MlsUi *ui)
{
  GhMlsService *service = current(ui);
  if (!service || !ui->shown || !ui->view) return;
  GListModel *timeline = gh_conversation_view_get_timeline(ui->view);
  if (!timeline) return;
  const gchar *gid = gh_mls_group_get_group_id(ui->shown);
  guint n = g_list_model_get_n_items(timeline);
  /* Two passes: polls first (kind 1068), then votes (kind 1018), so every
   * poll exists before its votes are applied. */
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) obj = g_list_model_get_item(timeline, i);
    if (!GH_IS_TIMELINE_ITEM(obj)) continue;
    GhMessage *msg = gh_timeline_item_get_message(GH_TIMELINE_ITEM(obj));
    if (msg && gh_message_get_kind(msg) == GH_MLS_POLL_KIND)
      gh_mls_service_rebuild_poll_from_stored(service, gid, msg);
  }
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) obj = g_list_model_get_item(timeline, i);
    if (!GH_IS_TIMELINE_ITEM(obj)) continue;
    GhMessage *msg = gh_timeline_item_get_message(GH_TIMELINE_ITEM(obj));
    if (msg && gh_message_get_kind(msg) == GH_MLS_POLL_VOTE_KIND)
      gh_mls_service_rebuild_poll_from_stored(service, gid, msg);
  }
}

/* Row enricher: called by GhMessageRow when its message changes. For kind-1068
 * messages, injects a GhPollCard bound to the GhMlsPoll; for any other kind,
 * removes the poll card. */
static void
on_poll_vote_cast(GhPollCard *card, const gchar **option_ids, guint n_options, gpointer data)
{
  MlsUi *ui = data;
  GhMlsService *service = current(ui);
  GhMlsPoll *poll = gh_poll_card_get_poll(card);
  if (!service || !ui->shown || !poll) return;
  const gchar *poll_event_id = gh_mls_poll_get_event_id(poll);
  g_autoptr(GError) error = NULL;
  gh_mls_service_cast_vote(service, ui->shown, poll_event_id,
                            option_ids, n_options, &error);
  if (error)
    g_message("Groundhog: could not cast vote: %s", error->message);
}

static void
poll_enricher(GhMessageRow *row, GhMessage *message, gpointer data)
{
  MlsUi *ui = data;
  if (!message || !ui->shown) {
    gh_message_row_set_poll_widget(row, NULL);
    return;
  }
  gint kind = gh_message_get_kind(message);
  if (kind != GH_MLS_POLL_KIND) {
    gh_message_row_set_poll_widget(row, NULL);
    return;
  }
  GhMlsService *service = current(ui);
  if (!service) {
    gh_message_row_set_poll_widget(row, NULL);
    return;
  }
  const gchar *rumor_id = gh_message_get_rumor_id(message);
  const gchar *group_id = gh_mls_group_get_group_id(ui->shown);
  GhMlsPoll *poll = gh_mls_service_lookup_poll(service, group_id, rumor_id);
  /* Lazy rebuild: the message was stored before this session. */
  if (!poll) {
    gh_mls_service_rebuild_poll_from_stored(service, group_id, message);
    poll = gh_mls_service_lookup_poll(service, group_id, rumor_id);
  }
  if (!poll) {
    gh_message_row_set_poll_widget(row, NULL);
    return;
  }
  GtkWidget *card = gh_poll_card_new();
  gh_poll_card_set_poll(GH_POLL_CARD(card), poll);
  g_signal_connect(card, "vote-cast", G_CALLBACK(on_poll_vote_cast), ui);
  gh_message_row_set_poll_widget(row, card);
}

static void
on_poll_created(GhCreatePollDialog *dialog, const gchar *question,
                const gchar **option_labels, guint n_options,
                gint poll_type, gint64 ends_at, gpointer data)
{
  (void) dialog;
  MlsUi *ui = data;
  GhMlsService *service = current(ui);
  if (!service || !ui->shown) return;
  g_autoptr(GError) error = NULL;
  gh_mls_service_create_poll(service, ui->shown, question, option_labels,
                              n_options, (GhMlsPollType) poll_type, ends_at,
                              &error);
  if (error) {
    g_message("Groundhog: could not create poll: %s", error->message);
    adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window),
                                adw_toast_new(_("The poll couldn’t be created")));
  }
}

static void
on_poll_requested(GhComposer *composer, gpointer data)
{
  (void) composer;
  MlsUi *ui = data;
  GhMlsService *service = current(ui);
  if (!service || !ui->shown) return;
  GtkWidget *dialog = gh_create_poll_dialog_new();
  g_signal_connect(dialog, "poll-created", G_CALLBACK(on_poll_created), ui);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(ui->window));
}

static void
sync_poll_button(MlsUi *ui)
{
  GhContentPage *content = gh_window_get_content(ui->window);
  if (!content) return;
  GhComposer *composer = gh_content_page_get_composer(content);
  if (!composer) return;
  gboolean can_poll = ui->shown != NULL &&
                      gh_mls_group_get_active(ui->shown) &&
                      !gh_mls_group_get_leaving(ui->shown);
  gh_composer_set_can_create_poll(composer, can_poll);
}

/* ---- dialogs ------------------------------------------------------------------------------ */

void
gh_mls_ui_extend_new_group(GhWindow *window, GhNewGroupDialog *dialog, gpointer user_data)
{
  (void)user_data;
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(GH_IS_NEW_GROUP_DIALOG(dialog));
  MlsUi *ui = ui_of(window);
  GhMlsUiContext context;
  if (!ui)
    return;
  context_of(ui, &context);
  if (!context.service)
    return;
  GhMlsNewGroupPage *page = gh_mls_new_group_page_new(&context);
  g_signal_connect_object(page, "open-group", G_CALLBACK(open_group), window,
                          G_CONNECT_SWAPPED);
  gh_new_group_dialog_add_encrypted_page(dialog, ADW_NAVIGATION_PAGE(page));
}

gboolean
gh_mls_ui_show_info(GhWindow *window, GhConversation *conversation, gpointer user_data)
{
  (void)user_data;
  g_return_val_if_fail(GH_IS_WINDOW(window), FALSE);
  g_return_val_if_fail(GH_IS_CONVERSATION(conversation), FALSE);
  MlsUi *ui = ui_of(window);
  GhMlsGroup *group = ui ? group_of(ui, conversation) : NULL;
  if (!group)
    return FALSE;
  GhMlsUiContext context;
  context_of(ui, &context);
  GhMlsGroupInfoDialog *dialog = gh_mls_group_info_dialog_new(group, &context);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
  return TRUE;
}

GhMlsService *
gh_mls_ui_get_service(GhWindow *window)
{
  MlsUi *ui = ui_of(window);
  return ui ? current(ui) : NULL;
}

/* ---- attach ------------------------------------------------------------------------------ */

gboolean
gh_mls_ui_enabled(void)
{
  return GH_FEATURE_ENCRYPTED_GROUPS != 0;
}

gboolean
gh_mls_ui_attach_if_enabled(GhWindow *window, const GhMlsUiConfig *config)
{
  if (!gh_mls_ui_enabled())
    return FALSE;
  gh_mls_ui_attach(window, config);
  return TRUE;
}

static void
mls_ui_free(gpointer data)
{
  MlsUi *ui = data;
  if (ui->watched) {
    g_signal_handlers_disconnect_by_data(ui->watched, ui);
    g_object_weak_unref(G_OBJECT(ui->watched), on_watched_gone, ui);
  }
  if (ui->shown)
    g_signal_handlers_disconnect_by_data(ui->shown, ui);
  g_clear_object(&ui->shown);
  if (ui->poll_requested_handler) {
    GhContentPage *content = gh_window_get_content(ui->window);
    GhComposer *composer = content ? gh_content_page_get_composer(content) : NULL;
    if (composer)
      g_clear_signal_handler(&ui->poll_requested_handler, composer);
    ui->poll_requested_handler = 0;
  }
  if (ui->view) {
    gh_conversation_view_set_row_enricher(ui->view, NULL, NULL);
    g_clear_signal_handler(&ui->view_handler, ui->view);
    g_clear_object(&ui->view);
  }
  g_clear_object(&ui->invites_action);
  g_clear_object(&ui->model);
  g_clear_object(&ui->accounts);
  g_clear_object(&ui->settings);
  g_clear_object(&ui->account_relays);
  g_clear_object(&ui->files);
  g_free(ui);
}

void
gh_mls_ui_attach(GhWindow *window, const GhMlsUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL && config->service != NULL);
  g_return_if_fail(GH_IS_CONVERSATION_STORE(config->conversations));
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts));
  g_return_if_fail(ui_of(window) == NULL);
  MlsUi *ui = g_new0(MlsUi, 1);
  ui->window = window;
  ui->model = g_object_ref(config->conversations);
  ui->accounts = g_object_ref(config->accounts);
  ui->settings = config->settings ? g_object_ref(config->settings) : NULL;
  ui->service_func = config->service;
  ui->service_data = config->service_data;
  ui->display_name = config->display_name;
  ui->names_data = config->names_data;
  ui->account_relays = config->account_relays ? g_object_ref(config->account_relays) : NULL;
  ui->lookup_deadline = config->lookup_deadline;
  ui->files = config->files ? g_object_ref(config->files) : NULL;
  g_object_set_data_full(G_OBJECT(window), MLS_UI_DATA, ui, mls_ui_free);

  ui->invites_action = g_simple_action_new("group-invitations", NULL);
  g_signal_connect(ui->invites_action, "activate", G_CALLBACK(on_invites), ui);
  g_action_map_add_action(G_ACTION_MAP(window), G_ACTION(ui->invites_action));

  if (config->state_source)
    g_signal_connect_object(config->state_source, "changed", G_CALLBACK(on_state_changed),
                            window, G_CONNECT_SWAPPED);
  GtkWidget *view = gh_content_page_get_view(gh_window_get_content(window));
  if (GH_IS_CONVERSATION_VIEW(view)) {
    ui->view = g_object_ref(GH_CONVERSATION_VIEW(view));
    ui->view_handler = g_signal_connect_swapped(view, "notify::conversation",
                                                G_CALLBACK(sync_shown), ui);
  }
  gh_conversation_list_set_member_count_func(window, member_count, ui, NULL);
  gh_send_ui_add_delegate(window, &mls_delegate, ui);
  gh_group_ui_set_new_group_extension(window, gh_mls_ui_extend_new_group, NULL);
  if (ui->view)
    gh_conversation_view_set_row_enricher(ui->view, poll_enricher, ui);
  {
    GhContentPage *content = gh_window_get_content(window);
    GhComposer *composer = content ? gh_content_page_get_composer(content) : NULL;
    if (composer)
      ui->poll_requested_handler = g_signal_connect(composer, "poll-requested",
                                                     G_CALLBACK(on_poll_requested), ui);
  }
  sync_service(ui);
}
