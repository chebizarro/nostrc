#include "gh-send-ui.h"

#include "gh-composer.h"
#include "gh-conversation-view.h"
#include "gh-delivery-indicator.h"
#include "gh-expiry.h"
#include "gh-outbox.h"

#include <glib/gi18n.h>
#include <string.h>

#define SEND_UI_DATA "groundhog-send-ui"
/* Set on a GhMessage bound to its GhOutboxItem (holds a reference). */
#define BOUND_ITEM_DATA "groundhog-outbox-item"

typedef struct {
  GtkWidget *window;          /* not owned: the struct is the window's data */
  GhComposer *composer;       /* template children of the window */
  GhConversationView *view;
  GhAccountController *accounts;
  GhConversationStore *model;
  GhAccountStore *store;
  GhDmInbox *inbox;
  GSettings *settings;
  GhOutbox *outbox;           /* the account store's current outbox */
  GhExpiry *expiry;           /* the open store's timers, or NULL */
  GHashTable *items;          /* rumor id -> GhOutboxItem (ref) of outbox */
  GHashTable *unknown;        /* rumor ids outbox has no entry for */
  GhConversation *shown;
  GhStoreConversations *drafts; /* where the shown conversation's draft lives */
  gchar *draft_room;
  gchar *draft_saved;         /* the stored draft, as far as this knows */
  gchar *no_inbox_rumor;      /* the shown room's newest own message if "Can't send" */
  GSimpleAction *check_inbox;
  gboolean updating;          /* in update_reason() (a lookup can re-enter it) */
  GhSendUiDelegate delegate;  /* G20b: relay groups; handles NULL when none */
  gpointer delegate_data;
} GhSendUi;

static void update_reason(GhSendUi *ui);

/* Handlers on objects that outlive the window are tied to the composer, so
 * they stop when the window's widgets go; they find the state here. */
static GhSendUi *
ui_of(GhComposer *composer)
{
  return g_object_get_data(G_OBJECT(composer), SEND_UI_DATA);
}

static void
send_ui_free(gpointer data)
{
  GhSendUi *ui = data;
  g_clear_pointer(&ui->items, g_hash_table_unref);
  g_clear_pointer(&ui->unknown, g_hash_table_unref);
  g_clear_object(&ui->outbox);
  g_clear_object(&ui->expiry);
  g_clear_object(&ui->shown);
  g_clear_object(&ui->drafts);
  g_clear_object(&ui->accounts);
  g_clear_object(&ui->model);
  g_clear_object(&ui->store);
  g_clear_object(&ui->inbox);
  g_clear_object(&ui->settings);
  g_clear_object(&ui->check_inbox);
  g_free(ui->draft_room);
  g_free(ui->draft_saved);
  g_free(ui->no_inbox_rumor);
  g_free(ui);
}

static void
toast(GhSendUi *ui, const gchar *text)
{
  adw_toast_overlay_add_toast(gh_window_get_toasts(GH_WINDOW(ui->window)), adw_toast_new(text));
}

/* Whether conversation is sent to by the delegate (a relay group). */
static gboolean
delegated(GhSendUi *ui, GhConversation *conversation)
{
  return conversation && ui->delegate.handles &&
         ui->delegate.handles(conversation, ui->delegate_data);
}

static gboolean
message_delegated(GhSendUi *ui, GhMessage *message)
{
  return message &&
         delegated(ui, gh_conversation_store_lookup(ui->model, gh_message_get_room_id(message)));
}

/* The one other participant (the account itself in a note to self); NULL
 * in a group conversation, which only receives for now. A NIP-29 relay group
 * has no peers but is not a note to self: its messages go to its relay
 * through GhNip29Service, never through the NIP-17 outbox (G20b). */
static const gchar *
recipient_of(GhSendUi *ui, GhConversation *conversation)
{
  if (gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP29)
    return NULL;
  const gchar *const *peers = gh_conversation_get_peers(conversation);
  if (!peers || !peers[0])
    return gh_conversation_store_get_account(ui->model);
  return peers[1] ? NULL : peers[0];
}

/* ---- outbox items and message status -------------------------------------------- */

static void
on_item_status(GhOutboxItem *item, GParamSpec *pspec, GhMessage *message)
{
  (void)pspec;
  gh_message_set_status(message, gh_outbox_item_get_status(item));
}

/* The item's status is the message's (the same enum), from now on, for as
 * long as the message lives. */
static void
bind_status(GhOutboxItem *item, GhMessage *message)
{
  if (g_object_get_data(G_OBJECT(message), BOUND_ITEM_DATA) == item)
    return;
  g_object_set_data_full(G_OBJECT(message), BOUND_ITEM_DATA, g_object_ref(item),
                         g_object_unref);
  g_signal_connect_object(item, "notify::status", G_CALLBACK(on_item_status), message, 0);
  on_item_status(item, NULL, message);
}

/* The local echo of a queued message: the message the queued rumor is. The
 * store already holds it (T-enqueue), so the model lists it. */
static GhMessage *
echo(GhSendUi *ui, GhOutboxItem *item)
{
  const gchar *account = gh_conversation_store_get_account(ui->model);
  const gchar *rumor = gh_outbox_item_get_rumor_json(item);
  if (!account || !rumor)
    return NULL;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhMessage) message = gh_message_new_from_rumor(account, rumor, &error);
  if (!message) {
    g_message("Groundhog could not show a queued message: %s", error->message);
    return NULL;
  }
  GhConversationAddResult result = gh_conversation_store_add_message(ui->model, message, &error);
  if (result != GH_CONVERSATION_ADD_NEW && result != GH_CONVERSATION_ADD_DUPLICATE) {
    g_message("Groundhog could not show a queued message: %s",
              error ? error->message : "not listed");
    return NULL;
  }
  /* NULL when it went into older history that is not listed yet. */
  return gh_conversation_store_lookup_message(ui->model, gh_message_get_rumor_id(message));
}

static void
track_item(GhSendUi *ui, GhOutboxItem *item, gboolean show)
{
  const gchar *rumor_id = gh_outbox_item_get_rumor_id(item);
  if (!rumor_id)
    return;
  g_hash_table_replace(ui->items, g_strdup(rumor_id), g_object_ref(item));
  g_hash_table_remove(ui->unknown, rumor_id);
  GhMessage *message = gh_conversation_store_lookup_message(ui->model, rumor_id);
  if (!message && show)
    message = echo(ui, item);
  if (message)
    bind_status(item, message);
}

/* A message sent (or first looked up): shown and bound at once. */
static void
on_item_added(GhComposer *composer, GhOutboxItem *item, GhOutbox *outbox)
{
  GhSendUi *ui = ui_of(composer);
  if (outbox != ui->outbox)
    return;
  track_item(ui, item, TRUE);
  update_reason(ui);
}

static void
on_item_removed(GhComposer *composer, GhOutboxItem *item, GhOutbox *outbox)
{
  GhSendUi *ui = ui_of(composer);
  const gchar *rumor_id = gh_outbox_item_get_rumor_id(item);
  if (outbox == ui->outbox && rumor_id && g_hash_table_lookup(ui->items, rumor_id) == item)
    g_hash_table_remove(ui->items, rumor_id);
}

/* The outbox item of an own message: sent from here, or looked up in the
 * store (a settled one too, after a restart), and bound to it. NULL for a
 * message this device did not send and incoming ones. */
static GhOutboxItem *
find_item(GhSendUi *ui, GhMessage *message)
{
  if (!ui->outbox || !gh_message_is_self(message) || message_delegated(ui, message))
    return NULL;
  const gchar *rumor_id = gh_message_get_rumor_id(message);
  GhOutboxItem *item = g_hash_table_lookup(ui->items, rumor_id);
  if (!item && !g_hash_table_contains(ui->unknown, rumor_id)) {
    /* Loading it emits "item-added", which tracks it. */
    g_autoptr(GhOutboxItem) found = gh_outbox_lookup_rumor(ui->outbox,
                                                           gh_message_get_room_id(message),
                                                           rumor_id);
    if (found) {
      track_item(ui, found, FALSE);
      item = g_hash_table_lookup(ui->items, rumor_id);
    } else {
      g_hash_table_add(ui->unknown, g_strdup(rumor_id));
    }
  }
  if (item)
    bind_status(item, message);
  return item;
}

static void
sync_outbox(GhSendUi *ui)
{
  GhAccountStoreState state = gh_account_store_get_state(ui->store);
  GObject *object = state == GH_ACCOUNT_STORE_OPEN || state == GH_ACCOUNT_STORE_EPHEMERAL
                      ? gh_account_store_get_outbox(ui->store) : NULL;
  GhOutbox *outbox = GH_IS_OUTBOX(object) ? GH_OUTBOX(object) : NULL;
  if (outbox == ui->outbox)
    return;
  if (ui->outbox)
    g_signal_handlers_disconnect_by_data(ui->outbox, ui->composer);
  g_clear_object(&ui->outbox);
  g_hash_table_remove_all(ui->items);
  g_hash_table_remove_all(ui->unknown);
  if (!outbox)
    return;
  ui->outbox = g_object_ref(outbox);
  g_signal_connect_object(outbox, "item-added", G_CALLBACK(on_item_added), ui->composer,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(outbox, "item-removed", G_CALLBACK(on_item_removed), ui->composer,
                          G_CONNECT_SWAPPED);
  /* The messages it resumed at start (their rooms are restored already). */
  g_autoptr(GPtrArray) items = gh_outbox_dup_items(outbox);
  for (guint i = 0; i < items->len; i++)
    track_item(ui, g_ptr_array_index(items, i), FALSE);
}

/* ---- the shown conversation ---------------------------------------------------------- */

static void
on_shown_status(GhComposer *composer)
{
  update_reason(ui_of(composer));
}

static void
watch_message(GhSendUi *ui, GhMessage *message)
{
  if (!gh_message_is_self(message))
    return;
  g_signal_handlers_disconnect_by_func(message, on_shown_status, ui->composer);
  g_signal_connect_object(message, "notify::status", G_CALLBACK(on_shown_status),
                          ui->composer, G_CONNECT_SWAPPED);
  find_item(ui, message);
}

static void
on_shown_items(GhComposer *composer, guint position, guint removed, guint added,
               GListModel *conversation)
{
  GhSendUi *ui = ui_of(composer);
  (void)removed;
  for (guint i = position; i < position + added; i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(conversation, i);
    watch_message(ui, message);
  }
  update_reason(ui);
}

static void
unwatch_shown(GhSendUi *ui)
{
  if (!ui->shown)
    return;
  g_signal_handlers_disconnect_by_func(ui->shown, on_shown_items, ui->composer);
  GListModel *model = G_LIST_MODEL(ui->shown);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    g_signal_handlers_disconnect_by_func(message, on_shown_status, ui->composer);
  }
}

static void
watch_shown(GhSendUi *ui)
{
  if (!ui->shown)
    return;
  g_signal_connect_object(ui->shown, "items-changed", G_CALLBACK(on_shown_items),
                          ui->composer, G_CONNECT_SWAPPED);
  GListModel *model = G_LIST_MODEL(ui->shown);
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i);
    watch_message(ui, message);
  }
}

/* ---- the disappearing timer ---------------------------------------------------------- */

/* The shown conversation's timer, 0 (hidden) when there is nothing to say. */
static void
update_timer(GhSendUi *ui)
{
  gint64 seconds = 0;
  if (ui->expiry && ui->shown) {
    g_autoptr(GError) error = NULL;
    if (!gh_expiry_get_timer(ui->expiry, gh_conversation_get_room_id(ui->shown), &seconds,
                             &error)) {
      /* Not a NIP-17 room of the account (nothing to show), or unreadable. */
      if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
        g_message("Groundhog could not read a disappearing timer: %s", error->message);
      seconds = 0;
    }
  }
  gh_composer_set_disappearing_timer(ui->composer, seconds);
}

static void
on_timer_changed(GhExpiry *expiry, const gchar *room_id, gint64 seconds, GhComposer *composer)
{
  GhSendUi *ui = ui_of(composer);
  if (expiry == ui->expiry && ui->shown &&
      g_strcmp0(room_id, gh_conversation_get_room_id(ui->shown)) == 0)
    gh_composer_set_disappearing_timer(ui->composer, seconds);
}

static void
use_expiry(GhSendUi *ui, GhExpiry *expiry)
{
  if (expiry != ui->expiry) {
    if (ui->expiry)
      g_signal_handlers_disconnect_by_data(ui->expiry, ui->composer);
    g_set_object(&ui->expiry, expiry);
    /* Tied to the composer: it stops with the window's widgets. */
    if (expiry)
      g_signal_connect_object(expiry, "timer-changed", G_CALLBACK(on_timer_changed),
                              ui->composer, 0);
  }
  update_timer(ui);
}

/* ---- drafts -------------------------------------------------------------------------- */

static void
on_draft_changed(GhComposer *composer, const gchar *text, gpointer data)
{
  GhSendUi *ui = data;
  (void)composer;
  const gchar *draft = text && *text ? text : NULL;
  if (!ui->drafts || !ui->draft_room || g_strcmp0(draft, ui->draft_saved) == 0)
    return;
  g_autoptr(GError) error = NULL;
  if (!gh_store_conversations_set_draft(ui->drafts, ui->draft_room, draft, &error)) {
    g_message("Groundhog could not save a draft: %s", error->message);
    return;
  }
  g_free(ui->draft_saved);
  ui->draft_saved = g_strdup(draft);
}

/* The shown conversation's draft, from the account's store. */
static void
load_draft(GhSendUi *ui)
{
  g_clear_object(&ui->drafts);
  g_clear_pointer(&ui->draft_room, g_free);
  g_clear_pointer(&ui->draft_saved, g_free);
  GhStoreConversations *drafts = ui->shown && !delegated(ui, ui->shown)
                                   ? gh_account_store_get_conversations(ui->store) : NULL;
  if (drafts) {
    g_autoptr(GError) error = NULL;
    ui->drafts = g_object_ref(drafts);
    ui->draft_room = g_strdup(gh_conversation_get_room_id(ui->shown));
    if (!gh_store_conversations_get_draft(drafts, ui->draft_room, &ui->draft_saved, &error))
      g_message("Groundhog could not read a draft: %s", error->message);
  }
  gh_composer_set_text(ui->composer, ui->draft_saved);
}

static void
on_view_conversation(GhComposer *composer)
{
  GhSendUi *ui = ui_of(composer);
  GhConversation *conversation = gh_conversation_view_get_conversation(ui->view);
  if (conversation == ui->shown)
    return;
  /* The draft of the conversation being left is saved first. */
  gh_composer_flush_draft(ui->composer);
  gh_composer_set_error(ui->composer, NULL);
  unwatch_shown(ui);
  g_set_object(&ui->shown, conversation);
  load_draft(ui);
  watch_shown(ui);
  update_timer(ui);
  update_reason(ui);
  gh_composer_revalidate(ui->composer);
}

/* ---- why sending is unavailable ---------------------------------------------------- */

static gchar *
store_reason(GhSendUi *ui)
{
  switch (gh_account_store_get_state(ui->store)) {
  case GH_ACCOUNT_STORE_INACTIVE:
  case GH_ACCOUNT_STORE_OPENING:
    return g_strdup(_("Opening message storage…"));
  case GH_ACCOUNT_STORE_LOCKED:
    return g_strdup(_("Message storage is locked. Unlock it to send messages."));
  case GH_ACCOUNT_STORE_UNAVAILABLE:
    return g_strdup(_("Messages can't be sent without private storage on this device."));
  case GH_ACCOUNT_STORE_KEY_MISSING:
    return g_strdup(_("Message storage can't be opened without its key, so messages "
                      "can't be sent."));
  case GH_ACCOUNT_STORE_CORRUPT:
    return g_strdup(_("Message storage is damaged, so messages can't be sent."));
  case GH_ACCOUNT_STORE_ERROR:
    return g_strdup(_("Message storage couldn't be opened, so messages can't be sent."));
  case GH_ACCOUNT_STORE_OPEN:
  case GH_ACCOUNT_STORE_EPHEMERAL:
  default:
    return ui->outbox ? NULL
                      : g_strdup(_("Sending couldn't start on this device, so messages "
                                   "can't be sent."));
  }
}

/* The shown room's newest own message, if the recipient has no message
 * inbox (charter §7.15 state 11). */
static GhMessage *
newest_without_inbox(GhSendUi *ui)
{
  GListModel *model = G_LIST_MODEL(ui->shown);
  for (guint i = g_list_model_get_n_items(model); i > 0; i--) {
    g_autoptr(GhMessage) message = g_list_model_get_item(model, i - 1);
    if (gh_message_is_self(message))
      return gh_message_get_status(message) == GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX
               ? message : NULL;
  }
  return NULL;
}

static void
update_reason(GhSendUi *ui)
{
  if (ui->updating)
    return;
  ui->updating = TRUE;
  GhAccountState state = gh_account_controller_get_state(ui->accounts);
  GhSignerAvailability availability = gh_account_controller_get_signer_availability(ui->accounts);
  g_autofree gchar *method = ui->settings ? g_settings_get_string(ui->settings, "signer-method")
                                          : g_strdup("auto");
  /* Offline is not a reason: the outbox waits for the connection. */
  g_autofree gchar *reason = gh_account_describe_limits(state, availability, method, TRUE);
  if (!reason)
    reason = store_reason(ui);
  GhMessage *no_inbox = NULL;
  if (!reason && ui->shown) {
    if (delegated(ui, ui->shown))
      reason = ui->delegate.reason(ui->shown, ui->delegate_data);
    else if (!recipient_of(ui, ui->shown))
      reason = g_strdup(_("Replying in group conversations isn't possible yet."));
    else if ((no_inbox = newest_without_inbox(ui)))
      reason = g_strdup_printf(_("%s hasn't set up private messaging yet, so messages "
                                 "can't be sent to them."),
                               gh_conversation_get_title(ui->shown));
  }

  g_free(ui->no_inbox_rumor);
  ui->no_inbox_rumor = no_inbox ? g_strdup(gh_message_get_rumor_id(no_inbox)) : NULL;
  GhOutboxItem *item = no_inbox ? find_item(ui, no_inbox) : NULL;
  g_simple_action_set_enabled(ui->check_inbox, item && gh_outbox_item_get_can_retry(item));
  gh_composer_set_disabled_reason(ui->composer, reason);
  gh_composer_set_disabled_action(ui->composer, no_inbox ? _("_Check Again") : NULL,
                                  "send.check-inbox");
  gh_conversation_view_set_recipient_without_inbox(ui->view, no_inbox
                                                     ? gh_conversation_get_title(ui->shown)
                                                     : NULL);
  ui->updating = FALSE;
}

static void
on_state_source(GhComposer *composer)
{
  GhSendUi *ui = ui_of(composer);
  sync_outbox(ui);
  update_reason(ui);
  gh_composer_revalidate(ui->composer);
}

/* ---- sending ------------------------------------------------------------------------- */

static const gchar *
send_error(const GError *error)
{
  if (g_error_matches(error, GH_STORE_ERROR, GH_STORE_ERROR_FULL))
    return _("Storage is full, so this message was not sent. It is kept here.");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
    return _("This message can't be sent privately. It is kept here.");
  return _("This message couldn't be queued for sending. It is kept here.");
}

static gboolean
on_send(GhComposer *composer, const gchar *text, gpointer data)
{
  GhSendUi *ui = data;
  if (delegated(ui, ui->shown)) {
    g_autoptr(GError) error = NULL;
    if (!ui->delegate.send(ui->shown, text, ui->delegate_data, &error)) {
      gh_composer_set_error(composer, error ? error->message
                                            : _("This message can't be sent right now. It is "
                                                "kept here."));
      return FALSE;
    }
    return TRUE;
  }
  const gchar *recipient = ui->shown ? recipient_of(ui, ui->shown) : NULL;
  if (!ui->outbox || !recipient) {
    gh_composer_set_error(composer, _("This message can't be sent right now. It is kept here."));
    return FALSE;
  }
  g_autoptr(GError) error = NULL;
  /* T-enqueue: stored with the draft cleared before any signer call. Its
   * "item-added" already showed and bound the message. */
  g_autoptr(GhOutboxItem) item = gh_outbox_send(ui->outbox, recipient, text, &error);
  if (!item) {
    g_message("Groundhog could not queue a message: %s", error->message);
    gh_composer_set_error(composer, send_error(error));
    return FALSE;
  }
  g_clear_pointer(&ui->draft_saved, g_free);
  track_item(ui, item, TRUE);
  return TRUE;
}

static gboolean
text_fits(const gchar *text, gpointer data)
{
  GhSendUi *ui = data;
  const gchar *recipient = ui->shown ? recipient_of(ui, ui->shown) : NULL;
  if (!ui->outbox || !recipient)
    return strlen(text) <= GH_COMPOSER_DEFAULT_MAX_BYTES;
  return gh_outbox_text_fits(ui->outbox, recipient, text);
}

static void
retry(GhSendUi *ui, GhMessage *message)
{
  g_autoptr(GError) error = NULL;
  if (message_delegated(ui, message)) {
    if (!ui->delegate.retry || !ui->delegate.retry(message, ui->delegate_data, &error)) {
      g_message("Groundhog could not retry a group message: %s",
                error ? error->message : "no retry");
      toast(ui, _("This message can't be sent again right now"));
    }
    return;
  }
  GhOutboxItem *item = message ? find_item(ui, message) : NULL;
  if (!item) {
    toast(ui, _("This message can't be sent again from this device"));
    return;
  }
  if (!gh_outbox_retry(ui->outbox, gh_outbox_item_get_outbox_id(item), &error)) {
    g_message("Groundhog could not retry a message: %s", error->message);
    toast(ui, _("This message can't be sent again right now"));
  }
}

static void
on_retry_requested(GhComposer *composer, GhMessage *message)
{
  retry(ui_of(composer), message);
}

static void
on_check_inbox(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  GhSendUi *ui = data;
  (void)action;
  (void)parameter;
  GhMessage *message = ui->shown && ui->no_inbox_rumor
                         ? gh_conversation_lookup_message(ui->shown, ui->no_inbox_rumor)
                         : NULL;
  retry(ui, message);
}

/* Delivery details (charter §3.6 "details on demand"). */
static GhDeliveryReport *
delivery_report(GhMessage *message, gpointer data)
{
  GhSendUi *ui = data;
  if (message_delegated(ui, message))
    return ui->delegate.report ? ui->delegate.report(message, ui->delegate_data) : NULL;
  GhOutboxItem *item = find_item(ui, message);
  if (!item)
    return NULL;
  GhDeliveryReport *report = gh_delivery_report_new();
  g_autoptr(GPtrArray) targets = gh_outbox_item_dup_targets(item);
  for (guint i = 0; i < targets->len; i++) {
    const GhOutboxTarget *target = g_ptr_array_index(targets, i);
    gh_delivery_report_add(report,
                           target->role == GH_STORE_OUTBOX_ROLE_SELF_WRAP ? NULL
                                                                          : target->pubkey,
                           target->url, target->target_class == GH_TARGET_CLASS_ACCEPTED,
                           target->description);
  }
  report->detail = g_strdup(gh_outbox_item_get_detail(item));
  report->next_attempt_at = gh_outbox_item_get_next_attempt_at(item);
  report->self_copy_missing = gh_outbox_item_get_self_copy_missing(item);
  return report;
}

/* ---- messages the signer has not unlocked (charter §7.15 state 12) ---------------------- */

static void
on_inbox_changed(GhComposer *composer)
{
  GhSendUi *ui = ui_of(composer);
  gh_conversation_view_set_locked_messages(ui->view, gh_dm_inbox_get_locked(ui->inbox));
}

static void
on_unlock_requested(GhComposer *composer)
{
  GhSendUi *ui = ui_of(composer);
  gh_dm_inbox_unlock(ui->inbox);
}

/* ---- public -------------------------------------------------------------------------- */

void
gh_send_ui_attach(GhWindow *window, const GhSendUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL);
  g_return_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts));
  g_return_if_fail(GH_IS_CONVERSATION_STORE(config->conversations));
  g_return_if_fail(GH_IS_ACCOUNT_STORE(config->account_store));
  g_return_if_fail(!config->inbox || GH_IS_DM_INBOX(config->inbox));
  g_return_if_fail(!config->settings || G_IS_SETTINGS(config->settings));
  g_return_if_fail(!config->expiry || GH_IS_EXPIRY(config->expiry));
  g_return_if_fail(g_object_get_data(G_OBJECT(window), SEND_UI_DATA) == NULL);
  GhContentPage *content = gh_window_get_content(window);
  GtkWidget *view = gh_content_page_get_view(content);
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(view));

  GhSendUi *ui = g_new0(GhSendUi, 1);
  ui->window = GTK_WIDGET(window);
  ui->composer = gh_content_page_get_composer(content);
  ui->view = GH_CONVERSATION_VIEW(view);
  ui->accounts = g_object_ref(config->accounts);
  ui->model = g_object_ref(config->conversations);
  ui->store = g_object_ref(config->account_store);
  ui->inbox = config->inbox ? g_object_ref(config->inbox) : NULL;
  ui->settings = config->settings ? g_object_ref(config->settings) : NULL;
  ui->items = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
  ui->unknown = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_object_set_data_full(G_OBJECT(window), SEND_UI_DATA, ui, send_ui_free);
  g_object_set_data(G_OBJECT(ui->composer), SEND_UI_DATA, ui);

  g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
  ui->check_inbox = g_simple_action_new("check-inbox", NULL);
  g_simple_action_set_enabled(ui->check_inbox, FALSE);
  g_signal_connect(ui->check_inbox, "activate", G_CALLBACK(on_check_inbox), ui);
  g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(ui->check_inbox));
  gtk_widget_insert_action_group(GTK_WIDGET(window), "send", G_ACTION_GROUP(group));

  if (ui->settings) {
    g_autoptr(GSettingsSchema) schema = NULL;
    g_object_get(ui->settings, "settings-schema", &schema, NULL);
    if (schema && g_settings_schema_has_key(schema, "enter-sends"))
      g_settings_bind(ui->settings, "enter-sends", ui->composer, "enter-sends",
                      G_SETTINGS_BIND_GET);
    g_signal_connect_object(ui->settings, "changed::signer-method",
                            G_CALLBACK(on_state_source), ui->composer, G_CONNECT_SWAPPED);
  }
  gh_composer_set_length_func(ui->composer, text_fits, ui, NULL);
  g_signal_connect(ui->composer, "send", G_CALLBACK(on_send), ui);
  g_signal_connect(ui->composer, "draft-changed", G_CALLBACK(on_draft_changed), ui);

  gh_conversation_view_set_delivery_report_func(ui->view, delivery_report, ui, NULL);
  g_signal_connect_object(ui->view, "retry-requested", G_CALLBACK(on_retry_requested),
                          ui->composer, G_CONNECT_SWAPPED);
  g_signal_connect_object(ui->view, "notify::conversation", G_CALLBACK(on_view_conversation),
                          ui->composer, G_CONNECT_SWAPPED);
  if (ui->inbox) {
    g_signal_connect_object(ui->view, "unlock-requested", G_CALLBACK(on_unlock_requested),
                            ui->composer, G_CONNECT_SWAPPED);
    g_signal_connect_object(ui->inbox, "changed", G_CALLBACK(on_inbox_changed), ui->composer,
                            G_CONNECT_SWAPPED);
    on_inbox_changed(ui->composer);
  }
  g_signal_connect_object(ui->accounts, "changed", G_CALLBACK(on_state_source), ui->composer,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(ui->store, "changed", G_CALLBACK(on_state_source), ui->composer,
                          G_CONNECT_SWAPPED);
  sync_outbox(ui);
  use_expiry(ui, config->expiry);
  on_view_conversation(ui->composer);
  update_reason(ui);
}

void
gh_send_ui_set_expiry(GhWindow *window, GhExpiry *expiry)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(!expiry || GH_IS_EXPIRY(expiry));
  GhSendUi *ui = g_object_get_data(G_OBJECT(window), SEND_UI_DATA);
  if (ui)
    use_expiry(ui, expiry);
}

void
gh_send_ui_set_delegate(GhWindow *window, const GhSendUiDelegate *delegate, gpointer data)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(!delegate || (delegate->handles && delegate->reason && delegate->send));
  GhSendUi *ui = g_object_get_data(G_OBJECT(window), SEND_UI_DATA);
  if (!ui)
    return;
  if (delegate)
    ui->delegate = *delegate;
  else
    memset(&ui->delegate, 0, sizeof ui->delegate);
  ui->delegate_data = delegate ? data : NULL;
  /* The shown conversation may change hands: its draft and reason too. */
  load_draft(ui);
  update_reason(ui);
  gh_composer_revalidate(ui->composer);
}

void
gh_send_ui_refresh(GhWindow *window)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  GhSendUi *ui = g_object_get_data(G_OBJECT(window), SEND_UI_DATA);
  if (ui)
    update_reason(ui);
}
