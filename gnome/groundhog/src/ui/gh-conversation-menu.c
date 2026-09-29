#include "gh-conversation-menu.h"
#include "gh-conversation-actions.h"
#include "gh-conversation-row.h"

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
} MenuAttach;

static void
attach_free(gpointer data)
{
  MenuAttach *attach = data;
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

/* ---- info --------------------------------------------------------------------------- */

static void
on_show_info(GSimpleAction *action, GVariant *parameter, gpointer data)
{
  MenuAttach *attach = data;
  (void)action;
  GhConversationInfoServices services;
  GhConversation *conversation =
    lookup(attach, g_variant_get_string(parameter, NULL), &services);
  if (!conversation)
    return;
  GhConversationInfoDialog *dialog = gh_conversation_info_dialog_new(conversation, &services);
  adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(attach->window));
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
  };
  g_action_map_add_action_entries(G_ACTION_MAP(window), entries, G_N_ELEMENTS(entries), attach);

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
