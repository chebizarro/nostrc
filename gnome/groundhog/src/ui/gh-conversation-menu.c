#include "gh-conversation-menu.h"
#include "gh-conversation-actions.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-shell.h"

#include <glib/gi18n.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

#define ATTACH_DATA "groundhog-conversation-menu"
#define MUTE_HOUR   G_GINT64_CONSTANT(3600)

typedef struct {
  GhWindow *window; /* not owned: the struct is its data */
  GhConversationInfoServicesFunc services_func;
  gpointer user_data;
  GDestroyNotify destroy;
  gchar *last_toast;
  GhConversation *shown; /* the header menu's conversation (a reference), or NULL */
} MenuAttach;

static void sync_header(MenuAttach *attach);

static void
watch_shown(MenuAttach *attach, GhConversation *conversation)
{
  if (attach->shown == conversation)
    return;
  if (attach->shown)
    g_signal_handlers_disconnect_by_data(attach->shown, attach);
  g_set_object(&attach->shown, conversation);
  if (conversation) {
    g_signal_connect_swapped(conversation, "notify::pinned", G_CALLBACK(sync_header), attach);
    g_signal_connect_swapped(conversation, "notify::is-request", G_CALLBACK(sync_header),
                             attach);
  }
}

static void
attach_free(gpointer data)
{
  MenuAttach *attach = data;
  watch_shown(attach, NULL);
  if (attach->destroy)
    attach->destroy(attach->user_data);
  g_free(attach->last_toast);
  g_free(attach);
}

/* Fresh services, and room_id's conversation when it is a listed private
 * conversation; else NULL. */
static GhConversation *
lookup(MenuAttach *attach, const gchar *room_id, GhConversationInfoServices *services)
{
  *services = (GhConversationInfoServices){ 0 };
  if (!room_id || !attach->services_func(services, attach->user_data) || !services->model)
    return NULL;
  GhConversation *conversation = gh_conversation_store_lookup(services->model, room_id);
  return conversation &&
         gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP17
    ? conversation : NULL;
}

static void
toast(MenuAttach *attach, const gchar *text)
{
  g_free(attach->last_toast);
  attach->last_toast = g_strdup(text);
  AdwToast *toast = adw_toast_new(text);
  adw_toast_set_use_markup(toast, FALSE);
  adw_toast_overlay_add_toast(gh_window_get_toasts(attach->window), toast);
}

static void
report_failure(MenuAttach *attach, const gchar *what, GError *error)
{
  g_message("Groundhog could not %s: %s", what, error ? error->message : "no message store");
  toast(attach, _("Groundhog couldn’t save this change"));
}

static gint64
now_unix(const GhConversationInfoServices *services)
{
  return services->store ? gh_clock_get_unix(gh_store_get_clock(services->store))
                         : g_get_real_time() / G_USEC_PER_SEC;
}

/* The one other person's name as Conversation Info shows it: their cached
 * display name (never looked up), else their short npub; NULL when the
 * conversation has more people (or none). Never the subject. */
static gchar *
only_person(GhConversation *conversation, const GhConversationInfoServices *services)
{
  const gchar *const *peers = gh_conversation_get_peers(conversation);
  if (!peers || !peers[0] || peers[1])
    return NULL;
  GhConversationInfoProfile profile = { 0 };
  if (services->profile)
    services->profile(peers[0], &profile, services->profile_data);
  if (profile.name)
    return g_strdup(profile.name);
  guint8 bytes[32];
  char *npub = NULL;
  if (!nostr_hex2bin(bytes, peers[0], sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(peers[0]);
  gsize length = strlen(npub);
  gchar *out = length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                           : g_strdup(npub);
  free(npub);
  return out;
}

/* ---- confirmations --------------------------------------------------------------- */

typedef struct {
  GWeakRef window;
  gchar *room_id;
} Pending;

static void
pending_free(gpointer data, GClosure *closure)
{
  Pending *pending = data;
  (void)closure;
  g_weak_ref_clear(&pending->window);
  g_free(pending->room_id);
  g_free(pending);
}

/* The attach and fresh services for an answered confirmation; NULL when the
 * window or the conversation went meanwhile. */
static GhConversation *
pending_lookup(Pending *pending, MenuAttach **out_attach, GhConversationInfoServices *services)
{
  g_autoptr(GObject) window = g_weak_ref_get(&pending->window);
  *out_attach = window ? g_object_get_data(window, ATTACH_DATA) : NULL;
  return *out_attach ? lookup(*out_attach, pending->room_id, services) : NULL;
}

/* A new confirmation (a reference) whose response runs on_response. */
static AdwAlertDialog *
new_alert(MenuAttach *attach, const gchar *name, const gchar *room_id, GCallback on_response)
{
  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-conversation-menu.ui");
  AdwAlertDialog *dialog =
    ADW_ALERT_DIALOG(g_object_ref_sink(gtk_builder_get_object(builder, name)));
  Pending *pending = g_new0(Pending, 1);
  g_weak_ref_init(&pending->window, attach->window);
  pending->room_id = g_strdup(room_id);
  g_signal_connect_data(dialog, "response", on_response, pending, pending_free, 0);
  return dialog;
}

/* ---- mute --------------------------------------------------------------------------- */

static gchar *
format_until(gint64 until, gint64 now)
{
  g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(until);
  g_autoptr(GDateTime) today = g_date_time_new_from_unix_local(now);
  if (!when || !today)
    return g_strdup("");
  if (g_date_time_get_year(when) == g_date_time_get_year(today) &&
      g_date_time_get_day_of_year(when) == g_date_time_get_day_of_year(today))
    return g_date_time_format(when, "%H:%M");
  if (until - now < 6 * 24 * 3600)
    /* TRANSLATORS: g_date_time_format() format, e.g. "Tuesday 14:30". */
    return g_date_time_format(when, _("%A %H:%M"));
  return g_date_time_format(when, "%x");
}

static void
on_mute_response(AdwAlertDialog *dialog, const gchar *response, Pending *pending)
{
  (void)dialog;
  static const struct {
    const gchar *response;
    gint64 seconds; /* 0: unmute; -1: until turned back on */
  } choices[] = {
    { "mute-hour", MUTE_HOUR },
    { "mute-hours", 8 * MUTE_HOUR },
    { "mute-week", 7 * 24 * MUTE_HOUR },
    { "mute-always", -1 },
    { "mute-off", 0 },
  };
  guint i = 0;
  while (i < G_N_ELEMENTS(choices) && g_strcmp0(choices[i].response, response) != 0)
    i++;
  if (i == G_N_ELEMENTS(choices))
    return;
  MenuAttach *attach = NULL;
  GhConversationInfoServices services;
  if (!pending_lookup(pending, &attach, &services))
    return;
  gint64 seconds = choices[i].seconds;
  gint64 until = seconds < 0 ? GH_STORE_CONVERSATIONS_MUTED_ALWAYS
                 : seconds ? now_unix(&services) + seconds : 0;
  g_autoptr(GError) error = NULL;
  /* N3: what shows for a muted conversation goes now. */
  if (!services.conversations ||
      !gh_conversation_actions_mute(services.conversations, services.notifier, pending->room_id,
                                    until, &error)) {
    report_failure(attach, "change a mute", error);
    return;
  }
  toast(attach, until ? _("Conversation muted") : _("Conversation unmuted"));
}

static void
on_mute(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  MenuAttach *attach = data;
  (void)action;
  const gchar *room_id = g_variant_get_string(parameter, NULL);
  GhConversationInfoServices services;
  GhConversation *conversation = lookup(attach, room_id, &services);
  if (!conversation)
    return;
  if (!services.conversations) {
    toast(attach, _("Unavailable while Groundhog isn’t saving messages on this device"));
    return;
  }
  GhStoreNotifyState state = { 0 };
  g_autoptr(GError) error = NULL;
  if (!gh_store_conversations_get_notify_state(services.conversations, room_id, &state, &error))
    g_message("Groundhog could not read a mute: %s", error->message);
  gint64 now = now_unix(&services);
  gboolean muted = state.muted_until == GH_STORE_CONVERSATIONS_MUTED_ALWAYS ||
                   state.muted_until > now;

  g_autoptr(AdwAlertDialog) dialog = new_alert(attach, "mute_dialog", room_id,
                                                G_CALLBACK(on_mute_response));
  g_autofree gchar *name = only_person(conversation, &services);
  g_autofree gchar *heading = name ? g_strdup_printf(_("Mute %s?"), name)
                                   : g_strdup(_("Mute This Conversation?"));
  adw_alert_dialog_set_heading(dialog, heading);
  if (muted) {
    g_autofree gchar *until = format_until(state.muted_until, now);
    g_autofree gchar *status =
      state.muted_until == GH_STORE_CONVERSATIONS_MUTED_ALWAYS
        ? g_strdup(_("Muted until you turn notifications back on"))
        : g_strdup_printf(_("Muted until %s"), until);
    g_autofree gchar *body = g_strconcat(status, "\n\n", adw_alert_dialog_get_body(dialog), NULL);
    adw_alert_dialog_set_body(dialog, body);
  } else {
    adw_alert_dialog_remove_response(dialog, "mute-off");
  }
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(attach->window));
}

/* ---- delete ------------------------------------------------------------------------- */

static void
on_delete_response(AdwAlertDialog *dialog, const gchar *response, Pending *pending)
{
  (void)dialog;
  if (g_strcmp0(response, "delete-confirm") != 0)
    return;
  MenuAttach *attach = NULL;
  GhConversationInfoServices services;
  if (!pending_lookup(pending, &attach, &services))
    return;
  g_autoptr(GError) error = NULL;
  /* ST-9: the rows go, the tombstone refuses older backfill, and the model
   * drops the room; what shows for it is withdrawn. */
  if (!services.conversations ||
      !gh_conversation_actions_forget(services.conversations, services.notifier,
                                      pending->room_id, &error)) {
    report_failure(attach, "delete a conversation", error);
    return;
  }
  toast(attach, _("Conversation deleted from this device"));
}

static void
on_delete(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  MenuAttach *attach = data;
  (void)action;
  const gchar *room_id = g_variant_get_string(parameter, NULL);
  GhConversationInfoServices services;
  GhConversation *conversation = lookup(attach, room_id, &services);
  if (!conversation)
    return;
  if (!services.conversations) {
    toast(attach, _("Unavailable while Groundhog isn’t saving messages on this device"));
    return;
  }
  g_autoptr(AdwAlertDialog) dialog = new_alert(attach, "delete_dialog", room_id,
                                                G_CALLBACK(on_delete_response));
  g_autofree gchar *name = only_person(conversation, &services);
  g_autofree gchar *heading = name ? g_strdup_printf(_("Delete Conversation with %s?"), name)
                                   : g_strdup(_("Delete This Conversation?"));
  adw_alert_dialog_set_heading(dialog, heading);
  /* The same words as Conversation Info's Forget on This Device. */
  g_autofree gchar *body = name
    ? g_strdup_printf(_("Its messages and draft are deleted from this device, and older messages "
                        "won't come back from relays. A new message starts it again.\n\n%s still "
                        "has their copy, and encrypted copies may remain on relays."), name)
    : g_strdup(_("Its messages and draft are deleted from this device, and older messages won't "
                 "come back from relays. A new message starts it again.\n\nThe others still "
                 "have their copies, and encrypted copies may remain on relays."));
  adw_alert_dialog_set_body(dialog, body);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(attach->window));
}

/* ---- info and the disappearing timer ------------------------------------------------ */

/* Conversation Info for room_id, at its disappearing timer when timer. */
static void
show_info(MenuAttach *attach, const gchar *room_id, gboolean timer)
{
  GhConversationInfoServices services;
  GhConversation *conversation = lookup(attach, room_id, &services);
  if (!conversation)
    return;
  GhConversationInfoDialog *dialog = gh_conversation_info_dialog_new(conversation, &services);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(attach->window));
  if (timer)
    gh_conversation_info_dialog_focus_timer(dialog);
}

static void
on_show_info(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  show_info(data, g_variant_get_string(parameter, NULL), FALSE);
}

static void
on_disappearing(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  show_info(data, g_variant_get_string(parameter, NULL), TRUE);
}

/* ---- pin, read and unread (charter §7.4, §7.5; nostrc-qp24.86) ------------------- */

/* Local only (P8): a pin is conversations.pinned_rank in the encrypted store
 * (PD-11), never GSettings; nothing is published. */
static void
set_pinned(MenuAttach *attach, const gchar *room_id, gboolean pinned)
{
  GhConversationInfoServices services;
  if (!lookup(attach, room_id, &services))
    return;
  if (!services.conversations) {
    toast(attach, _("Unavailable while Groundhog isn’t saving messages on this device"));
    return;
  }
  g_autoptr(GError) error = NULL;
  if (!gh_store_conversations_set_pinned(services.conversations, room_id, pinned, &error))
    report_failure(attach, pinned ? "pin a conversation" : "unpin a conversation", error);
}

static void
on_pin(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  set_pinned(data, g_variant_get_string(parameter, NULL), TRUE);
}

static void
on_unpin(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  set_pinned(data, g_variant_get_string(parameter, NULL), FALSE);
}

/* Read state is local only too (PD-1, P8), kept by the model's store. */
static void
on_mark_read(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  GhConversationInfoServices services;
  GhConversation *conversation = lookup(data, g_variant_get_string(parameter, NULL), &services);
  if (conversation && !gh_conversation_get_is_request(conversation))
    gh_conversation_mark_read(conversation);
}

static void
on_mark_unread(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  GhConversationInfoServices services;
  GhConversation *conversation = lookup(data, g_variant_get_string(parameter, NULL), &services);
  if (conversation && !gh_conversation_get_is_request(conversation))
    gh_conversation_mark_unread(conversation);
}

/* ---- rename (nostrc-0srb) ---------------------------------------------------- */

/* The shown group conversation, or NULL for DMs. */
static GhConversation *
shown_group(MenuAttach *attach)
{
  GhContentPage *content = gh_window_get_content(attach->window);
  GtkWidget *view = gh_content_page_get_view(content);
  if (!gh_content_page_get_conversation_shown(content) || !GH_IS_CONVERSATION_VIEW(view))
    return NULL;
  GhConversation *conversation = gh_conversation_view_get_conversation(GH_CONVERSATION_VIEW(view));
  if (!conversation)
    return NULL;
  GhConversationBackend backend = gh_conversation_get_backend(conversation);
  return (backend == GH_CONVERSATION_BACKEND_MLS || backend == GH_CONVERSATION_BACKEND_NIP29)
           ? conversation : NULL;
}

/* "Rename Group…": opens the group's own info dialog, which already has the
 * rename page and admin checks (nostrc-0srb). */
static void
on_rename_group(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  (void)action;
  (void)parameter;
  MenuAttach *attach = data;
  if (!shown_group(attach))
    return;
  /* Activate win.conversation-info, which dispatches to the MLS or NIP-29
   * group info dialog. The dialog's rename page handles admin checks. */
  g_action_group_activate_action(G_ACTION_GROUP(attach->window),
                                 "conversation-info", NULL);
}

/* ---- the conversation header menu (charter §7.4 conversation_menu) ------------------ */

/* The private conversation the content page shows, or NULL. */
static GhConversation *
shown_private(MenuAttach *attach)
{
  GhContentPage *content = gh_window_get_content(attach->window);
  GtkWidget *view = gh_content_page_get_view(content);
  if (!gh_content_page_get_conversation_shown(content) || !GH_IS_CONVERSATION_VIEW(view))
    return NULL;
  GhConversation *conversation = gh_conversation_view_get_conversation(GH_CONVERSATION_VIEW(view));
  return conversation &&
         gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP17 &&
         !gh_conversation_get_is_request(conversation)
    ? conversation : NULL;
}

static void
set_enabled(MenuAttach *attach, const gchar *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(attach->window), name);
  if (G_IS_SIMPLE_ACTION(action))
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

/* The header's menu button shows for private conversations and groups; of
 * Pin and Unpin only the one that applies. Rename shows for groups only
 * (nostrc-0srb). */
static void
sync_header(MenuAttach *attach)
{
  GhConversation *conversation = shown_private(attach);
  GhConversation *group = shown_group(attach);
  watch_shown(attach, conversation ? conversation : group);
  gboolean pinned = conversation && gh_conversation_get_pinned(conversation);
  set_enabled(attach, "pin-shown-conversation", conversation && !pinned);
  set_enabled(attach, "unpin-shown-conversation", pinned);
  set_enabled(attach, "mute-shown-conversation", conversation != NULL);
  set_enabled(attach, "disappearing-shown-conversation", conversation != NULL);
  set_enabled(attach, "delete-shown-conversation", conversation != NULL);
  set_enabled(attach, "rename-shown-group", group != NULL);
  gtk_widget_set_visible(gh_content_page_get_menu_button(gh_window_get_content(attach->window)),
                         conversation != NULL || group != NULL);
}

/* Each header item runs the row action of the same name for the shown
 * conversation. */
static void
on_shown(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  MenuAttach *attach = data;
  (void)parameter;
  GhConversation *conversation = shown_private(attach);
  if (!conversation)
    return;
  typedef void (*RoomAction)(GSimpleAction *action, GVariant *parameter, gpointer data);
  static const struct {
    const gchar *name;
    RoomAction func;
  } forward[] = {
    { "pin-shown-conversation", on_pin },
    { "unpin-shown-conversation", on_unpin },
    { "mute-shown-conversation", on_mute },
    { "disappearing-shown-conversation", on_disappearing },
    { "delete-shown-conversation", on_delete },
  };
  const gchar *name = g_action_get_name(G_ACTION(action));
  g_autoptr(GVariant) room =
    g_variant_ref_sink(g_variant_new_string(gh_conversation_get_room_id(conversation)));
  for (guint i = 0; i < G_N_ELEMENTS(forward); i++)
    if (g_str_equal(name, forward[i].name))
      forward[i].func(action, room, attach);
}

/* ---- keyboard ----------------------------------------------------------------------- */

/* Shift+F10 or Menu in the list: the focused row's menu. A list item holds
 * the row as its child; focus is on the item (or inside it). */
static gboolean
popup_focused_row(GtkWidget *list, GVariant *args, gpointer data)
{
  (void)args;
  (void)data;
  GtkRoot *root = gtk_widget_get_root(list);
  GtkWidget *focus = root ? gtk_root_get_focus(root) : NULL;
  for (GtkWidget *w = focus; w && w != list; w = gtk_widget_get_parent(w)) {
    if (GH_IS_CONVERSATION_ROW(w))
      return gh_conversation_row_popup_menu(GH_CONVERSATION_ROW(w));
    GtkWidget *child = gtk_widget_get_first_child(w);
    if (GH_IS_CONVERSATION_ROW(child))
      return gh_conversation_row_popup_menu(GH_CONVERSATION_ROW(child));
  }
  return FALSE;
}

void
gh_conversation_menu_attach(GhWindow *window, GhConversationInfoServicesFunc services_func,
                            gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(services_func != NULL);
  g_return_if_fail(g_object_get_data(G_OBJECT(window), ATTACH_DATA) == NULL);
  MenuAttach *attach = g_new0(MenuAttach, 1);
  attach->window = window;
  attach->services_func = services_func;
  attach->user_data = user_data;
  attach->destroy = destroy;
  g_object_set_data_full(G_OBJECT(window), ATTACH_DATA, attach, attach_free);
  const GActionEntry entries[] = {
    { "show-conversation-info", on_show_info, "s", NULL, NULL, { 0 } },
    { "mute-conversation", on_mute, "s", NULL, NULL, { 0 } },
    { "delete-conversation", on_delete, "s", NULL, NULL, { 0 } },
    { "pin-conversation", on_pin, "s", NULL, NULL, { 0 } },
    { "unpin-conversation", on_unpin, "s", NULL, NULL, { 0 } },
    { "mark-conversation-read", on_mark_read, "s", NULL, NULL, { 0 } },
    { "mark-conversation-unread", on_mark_unread, "s", NULL, NULL, { 0 } },
    { "disappearing-messages", on_disappearing, "s", NULL, NULL, { 0 } },
    { "pin-shown-conversation", on_shown, NULL, NULL, NULL, { 0 } },
    { "unpin-shown-conversation", on_shown, NULL, NULL, NULL, { 0 } },
    { "mute-shown-conversation", on_shown, NULL, NULL, NULL, { 0 } },
    { "disappearing-shown-conversation", on_shown, NULL, NULL, NULL, { 0 } },
    { "delete-shown-conversation", on_shown, NULL, NULL, NULL, { 0 } },
    { "rename-shown-group", on_rename_group, NULL, NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(window), entries, G_N_ELEMENTS(entries), attach);
  /* The header menu follows the shown conversation. */
  GhContentPage *content = gh_window_get_content(window);
  g_signal_connect_swapped(gh_content_page_get_stack(content), "notify::visible-child-name",
                           G_CALLBACK(sync_header), attach);
  GtkWidget *view = gh_content_page_get_view(content);
  if (GH_IS_CONVERSATION_VIEW(view))
    g_signal_connect_swapped(view, "notify::conversation", G_CALLBACK(sync_header), attach);
  sync_header(attach);

  GtkEventController *keys = gtk_shortcut_controller_new();
  gtk_shortcut_controller_add_shortcut(
    GTK_SHORTCUT_CONTROLLER(keys),
    gtk_shortcut_new(gtk_shortcut_trigger_parse_string("<Shift>F10|Menu"),
                     gtk_callback_action_new(popup_focused_row, NULL, NULL)));
  gtk_widget_add_controller(GTK_WIDGET(gh_sidebar_page_get_list(gh_window_get_sidebar(window))),
                            keys);
}

const gchar *
gh_conversation_menu_get_last_toast(GhWindow *window)
{
  g_return_val_if_fail(GH_IS_WINDOW(window), NULL);
  MenuAttach *attach = g_object_get_data(G_OBJECT(window), ATTACH_DATA);
  return attach ? attach->last_toast : NULL;
}
