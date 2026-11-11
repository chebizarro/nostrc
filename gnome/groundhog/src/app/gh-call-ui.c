/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "gh-call-ui.h"

#include "gh-call-view.h"
#include "gh-conversation-view.h"
#include "gh-display-name.h"

#include <glib/gi18n.h>
#include <string.h>

#define ATTACH_DATA "groundhog-call-ui"
#define PAGE_CALL "call"
#define PAGE_MAIN "main"

typedef struct {
  GhWindow *window;
  GhCallService *service;
  GhConversationStore *conversations;
  GhCallUiConfig config;
  GObject *notifier;
  GhCallView *view;
  GhConversationView *conversation_view;
  GhCall *call;                 /* the call the page shows (current or just ended) */
  GhCallMediaBackend *media;    /* its media whose signals are connected */
  GhCallState last_state;
  GhCallType last_type;
  gboolean last_reconnecting;
  guint tick;
  gboolean muted, camera;
  gboolean placing;             /* place() in progress: its error is the toast */
  gboolean close_confirmed;
  AdwDialog *dialog;            /* weak: the headset or close question */
  GObject *selected[3];         /* chosen devices: input, output, camera */
  gchar *last_toast;
  AdwToast *toast;              /* weak: the last one shown */
} CallUi;

static const gchar *const DEVICE_CLASSES[] = { "Audio/Source", "Audio/Sink", "Video/Source" };

static void sync_call(CallUi *ui);
static void sync_buttons(CallUi *ui);

/* ---- helpers ------------------------------------------------------------------- */

static CallUi *
ui_of(GhWindow *window)
{
  return window ? g_object_get_data(G_OBJECT(window), ATTACH_DATA) : NULL;
}

static void
toast(CallUi *ui, const gchar *text)
{
  g_free(ui->last_toast);
  ui->last_toast = g_strdup(text);
  AdwToast *shown = adw_toast_new(text);
  g_set_weak_pointer(&ui->toast, shown);
  adw_toast_overlay_add_toast(gh_window_get_toasts(ui->window), shown);
}

static GhCallUiMedia
probe(CallUi *ui)
{
  if (!ui->config.probe)
    return GH_CALL_UI_MEDIA_READY;
  g_autofree gchar *reason = NULL;
  GhCallUiMedia media = ui->config.probe(ui->config.data, &reason);
  if (media != GH_CALL_UI_MEDIA_READY && reason)
    g_debug("calls: %s", reason);
  return media;
}

static void
set_enabled(CallUi *ui, const gchar *name, gboolean enabled)
{
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(ui->window), name);
  if (G_IS_SIMPLE_ACTION(action))
    g_simple_action_set_enabled(G_SIMPLE_ACTION(action), enabled);
}

static void
set_state(CallUi *ui, const gchar *name, gboolean value)
{
  GAction *action = g_action_map_lookup_action(G_ACTION_MAP(ui->window), name);
  if (G_IS_SIMPLE_ACTION(action))
    g_simple_action_set_state(G_SIMPLE_ACTION(action), g_variant_new_boolean(value));
}

static gboolean
ongoing(GhCall *call)
{
  return call && gh_call_get_state(call) != GH_CALL_ENDED && gh_call_get_state(call) != GH_CALL_IDLE;
}

static gboolean
in_call(GhCall *call)
{
  GhCallState state = call ? gh_call_get_state(call) : GH_CALL_IDLE;
  return state == GH_CALL_OUTGOING || state == GH_CALL_CONNECTING || state == GH_CALL_ACTIVE;
}

/* A one-to-one private conversation with peer, preferring an accepted one. */
static GhConversation *
conversation_for(CallUi *ui, const gchar *peer)
{
  if (!ui->conversations || !peer)
    return NULL;
  GListModel *model = G_LIST_MODEL(ui->conversations);
  GhConversation *found = NULL;
  for (guint i = 0; i < g_list_model_get_n_items(model); i++) {
    g_autoptr(GhConversation) c = g_list_model_get_item(model, i);
    const gchar *const *peers = gh_conversation_get_peers(c);
    if (gh_conversation_get_backend(c) != GH_CONVERSATION_BACKEND_NIP17 ||
        !peers || !peers[0] || peers[1] || strcmp(peers[0], peer))
      continue;
    if (!gh_conversation_get_is_request(c))
      return c; /* the model keeps a reference */
    found = c;
  }
  return found;
}

static gchar *
peer_name(CallUi *ui, const gchar *peer)
{
  GhConversation *conversation = conversation_for(ui, peer);
  if (conversation && !gh_conversation_get_is_request(conversation))
    return g_strdup(gh_conversation_get_title(conversation));
  /* Charter §7.9: someone not accepted is named by their npub, never by a
   * name they chose. */
  return gh_display_name_short_npub(peer);
}

/* The peer of the shown one-to-one private conversation, or NULL. */
static const gchar *
shown_peer(CallUi *ui)
{
  GhContentPage *content = gh_window_get_content(ui->window);
  GtkWidget *view = gh_content_page_get_view(content);
  if (!gh_content_page_get_conversation_shown(content) || !GH_IS_CONVERSATION_VIEW(view))
    return NULL;
  GhConversation *conversation = gh_conversation_view_get_conversation(GH_CONVERSATION_VIEW(view));
  const gchar *const *peers = conversation ? gh_conversation_get_peers(conversation) : NULL;
  if (!conversation || gh_conversation_get_backend(conversation) != GH_CONVERSATION_BACKEND_NIP17 ||
      gh_conversation_get_is_request(conversation) || !peers || !peers[0] || peers[1])
    return NULL;
  return peers[0];
}

static const gchar *
unavailable_text(const GError *error)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    return _("Calls are unavailable in Tor mode");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED))
    return _("Calls are unavailable without an account");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
    return _("Calls are unavailable: call support is not installed");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY))
    return _("A call is already in progress");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT))
    return _("This person can’t be called");
  return _("The call couldn’t be started");
}

static gchar *
duration_text(gint64 microseconds)
{
  guint64 seconds = microseconds > 0 ? (guint64)microseconds / G_USEC_PER_SEC : 0;
  if (seconds >= 3600)
    return g_strdup_printf("%u:%02u:%02u", (guint)(seconds / 3600), (guint)(seconds / 60 % 60),
                           (guint)(seconds % 60));
  return g_strdup_printf("%u:%02u", (guint)(seconds / 60), (guint)(seconds % 60));
}

gchar *
gh_call_ui_describe(GhCall *call, gint64 now)
{
  g_return_val_if_fail(GH_IS_CALL(call), NULL);
  gboolean outgoing = gh_call_get_direction(call) == GH_CALL_OUTBOUND;
  switch (gh_call_get_state(call)) {
  case GH_CALL_RINGING:
    return g_strdup(gh_call_get_call_type(call) == GH_CALL_VIDEO ? _("Incoming video call")
                                                                 : _("Incoming voice call"));
  case GH_CALL_OUTGOING:
    return g_strdup(_("Calling…"));
  case GH_CALL_CONNECTING:
    return g_strdup(_("Connecting…"));
  case GH_CALL_ACTIVE:
    if (gh_call_get_reconnecting(call))
      return g_strdup(_("Reconnecting…"));
    return duration_text(now - gh_call_get_connected_time(call));
  case GH_CALL_ENDED:
    switch (gh_call_get_end_reason(call)) {
    case GH_CALL_END_DECLINED: return g_strdup(_("Call declined"));
    case GH_CALL_END_BUSY: return g_strdup(_("Busy"));
    case GH_CALL_END_TIMEOUT: return g_strdup(outgoing ? _("No answer") : _("Missed call"));
    case GH_CALL_END_REQUEST: return g_strdup(_("Missed call"));
    case GH_CALL_END_ELSEWHERE: return g_strdup(_("Answered or declined on another device"));
    case GH_CALL_END_ACCOUNT_CHANGED: return g_strdup(_("Call ended because the account changed"));
    case GH_CALL_END_MEDIA_FAILED:
      return g_strdup(gh_call_get_connected_time(call) ? _("Call failed: the connection was lost")
                                                       : _("Call failed: no connection could be made"));
    case GH_CALL_END_SIGNALLING_FAILED: return g_strdup(_("Call failed: it couldn’t be delivered"));
    case GH_CALL_END_PRIVACY: return g_strdup(_("Call ended by a privacy setting"));
    case GH_CALL_END_HANGUP:
    case GH_CALL_END_NONE:
    default: return g_strdup(_("Call ended"));
    }
  case GH_CALL_IDLE:
  default:
    return g_strdup("");
  }
}

/* ---- the header buttons ------------------------------------------------------------ */

static void
sync_buttons(CallUi *ui)
{
  if (gtk_widget_in_destruction(GTK_WIDGET(ui->window)))
    return;
  const gchar *peer = shown_peer(ui);
  g_autofree gchar *reason = NULL;
  gboolean headset = FALSE;
  if (peer) {
    g_autoptr(GError) error = NULL;
    GhCallUiMedia media = GH_CALL_UI_MEDIA_READY;
    const gchar *no_inbox = ui->conversation_view
      ? gh_conversation_view_get_recipient_without_inbox(ui->conversation_view) : NULL;
    if (!gh_call_service_check_available(ui->service, &error))
      reason = g_strdup(unavailable_text(error));
    else if ((media = probe(ui)) == GH_CALL_UI_MEDIA_MISSING)
      reason = g_strdup(_("Calls are unavailable: call support is not installed"));
    else if (no_inbox)
      reason = g_strdup_printf(_("%s can’t be called: they haven’t set up private messaging yet"),
                               no_inbox);
    else if (ongoing(gh_call_service_get_current(ui->service)))
      reason = g_strdup(_("A call is already in progress"));
    headset = media == GH_CALL_UI_MEDIA_HEADSET_ONLY;
  }
  for (guint video = 0; video <= 1; video++) {
    GtkWidget *button = gh_content_page_get_call_button(gh_window_get_content(ui->window), video);
    const gchar *label = video ? _("Start Video Call") : _("Start Voice Call");
    g_autofree gchar *tooltip = reason ? g_strdup(reason)
      : headset ? g_strdup_printf("%s\n%s", label, _("Use a headset: echo cancellation isn’t installed"))
                : g_strdup(label);
    gtk_widget_set_visible(button, peer != NULL);
    gtk_widget_set_tooltip_text(button, tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(button), GTK_ACCESSIBLE_PROPERTY_LABEL, label,
                                   GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                   reason ? reason
                                   : headset ? _("Use a headset: echo cancellation isn’t installed")
                                             : "", -1);
  }
  set_enabled(ui, "call-voice", peer && !reason);
  set_enabled(ui, "call-video", peer && !reason);
}

static void
watch_conversation_view(CallUi *ui)
{
  GtkWidget *view = gh_content_page_get_view(gh_window_get_content(ui->window));
  GhConversationView *watched = GH_IS_CONVERSATION_VIEW(view) ? GH_CONVERSATION_VIEW(view) : NULL;
  if (watched == ui->conversation_view)
    return;
  if (ui->conversation_view)
    g_signal_handlers_disconnect_by_data(ui->conversation_view, ui);
  g_set_weak_pointer(&ui->conversation_view, watched);
  if (watched) {
    g_signal_connect_swapped(watched, "notify::conversation", G_CALLBACK(sync_buttons), ui);
    g_signal_connect_swapped(watched, "notify::recipient-without-inbox", G_CALLBACK(sync_buttons), ui);
  }
}

/* ---- the page ------------------------------------------------------------------------- */

static gboolean
on_page(CallUi *ui)
{
  return g_strcmp0(gtk_stack_get_visible_child_name(gh_window_get_root_stack(ui->window)),
                   PAGE_CALL) == 0;
}

static void
show_page(CallUi *ui)
{
  if (!ui->call)
    return;
  /* An earlier call's toast would cover this call's controls. */
  if (ui->toast)
    adw_toast_dismiss(ui->toast);
  gtk_stack_set_visible_child_name(gh_window_get_root_stack(ui->window), PAGE_CALL);
}

static void
leave_page(CallUi *ui)
{
  if (on_page(ui))
    gtk_stack_set_visible_child_name(gh_window_get_root_stack(ui->window), PAGE_MAIN);
}

static void
sync_banner(CallUi *ui)
{
  if (gtk_widget_in_destruction(GTK_WIDGET(ui->window)))
    return;
  GhCall *call = ui->call;
  gboolean show = ongoing(call) && !on_page(ui);
  g_autofree gchar *title = NULL;
  if (show) {
    g_autofree gchar *name = peer_name(ui, gh_call_get_peer(call));
    GhCallState state = gh_call_get_state(call);
    title = state == GH_CALL_RINGING    ? g_strdup_printf(_("Incoming call from %s"), name)
            : state == GH_CALL_OUTGOING ? g_strdup_printf(_("Calling %s…"), name)
                                        : g_strdup_printf(_("Call with %s"), name);
  }
  AdwBanner *banners[] = {
    gh_content_page_get_call_banner(gh_window_get_content(ui->window)),
    gh_sidebar_page_get_call_banner(gh_window_get_sidebar(ui->window)),
  };
  /* Both are on screen side by side when expanded: then only the content's. */
  gboolean collapsed = adw_navigation_split_view_get_collapsed(gh_window_get_split(ui->window));
  for (guint i = 0; i < G_N_ELEMENTS(banners); i++) {
    if (title)
      adw_banner_set_title(banners[i], title);
    adw_banner_set_revealed(banners[i], show && (i == 0 || collapsed));
  }
}

static void
sync_status(CallUi *ui)
{
  if (!ui->call)
    return;
  g_autofree gchar *status = gh_call_ui_describe(ui->call, g_get_monotonic_time());
  gh_call_view_set_status(ui->view, status);
}

static gboolean
on_tick(gpointer data)
{
  sync_status(data);
  return G_SOURCE_CONTINUE;
}

static void
apply_media_state(CallUi *ui)
{
  const GhCallMediaOps *ops = ui->config.ops;
  if (!ui->media || !ops)
    return;
  if (ops->set_muted)
    ops->set_muted(ui->media, ui->muted);
  if (ops->set_camera)
    ops->set_camera(ui->media, ui->camera);
}

static void
sync_video(CallUi *ui)
{
  const GhCallMediaOps *ops = ui->config.ops;
  gboolean video = ui->call && in_call(ui->call) && gh_call_get_call_type(ui->call) == GH_CALL_VIDEO;
  GdkPaintable *remote = video && ui->media && ops && ops->remote_video ? ops->remote_video(ui->media) : NULL;
  GdkPaintable *local = video && ui->media && ops && ops->local_preview ? ops->local_preview(ui->media) : NULL;
  gh_call_view_set_camera(ui->view, ui->camera);
  gh_call_view_set_video(ui->view, remote, local);
}

/* "display-name", cached on the object so the text outlives the call. */
static const gchar *
device_display_name(GObject *device)
{
  if (!device)
    return NULL;
  const gchar *cached = g_object_get_data(device, "groundhog-call-device-name");
  if (cached)
    return cached;
  gchar *name = NULL;
  if (g_object_class_find_property(G_OBJECT_GET_CLASS(device), "display-name"))
    g_object_get(device, "display-name", &name, NULL);
  if (!name)
    name = g_strdup(G_OBJECT_TYPE_NAME(device));
  g_object_set_data_full(device, "groundhog-call-device-name", name, g_free);
  return name;
}

static gchar *
device_class(GObject *device)
{
  gchar *klass = NULL;
  if (device && g_object_class_find_property(G_OBJECT_GET_CLASS(device), "device-class"))
    g_object_get(device, "device-class", &klass, NULL);
  return klass;
}

static gint
class_index(const gchar *klass)
{
  for (guint i = 0; i < G_N_ELEMENTS(DEVICE_CLASSES); i++)
    if (klass && g_str_has_prefix(klass, DEVICE_CLASSES[i]))
      return (gint)i;
  return -1;
}

static void
on_device_removed(GObject *media, GObject *device, CallUi *ui)
{
  (void)media;
  g_autofree gchar *klass = device_class(device);
  gint index = class_index(klass);
  if (index < 0)
    return;
  if (ui->selected[index] == device)
    g_clear_object(&ui->selected[index]);
  const gchar *texts[] = { _("Microphone disconnected"), _("Audio output disconnected"),
                           _("Camera disconnected") };
  toast(ui, texts[index]);
}

static void
watch_media(CallUi *ui, GhCallMediaBackend *media)
{
  if (ui->media == media)
    return;
  if (ui->media)
    g_signal_handlers_disconnect_by_data(ui->media, ui);
  g_set_object(&ui->media, media);
  if (media && g_signal_lookup("device-removed", G_OBJECT_TYPE(media)))
    g_signal_connect(media, "device-removed", G_CALLBACK(on_device_removed), ui);
  if (media)
    apply_media_state(ui);
}

static GhCallViewControls
controls_for(CallUi *ui, GhCall *call)
{
  gboolean video = gh_call_get_call_type(call) == GH_CALL_VIDEO;
  gboolean devices = ui->config.ops && ui->config.ops->devices && ui->media;
  switch (gh_call_get_state(call)) {
  case GH_CALL_RINGING:
    return GH_CALL_VIEW_DECLINE | GH_CALL_VIEW_ANSWER | (video ? GH_CALL_VIEW_ANSWER_VIDEO : 0);
  case GH_CALL_OUTGOING:
  case GH_CALL_CONNECTING:
  case GH_CALL_ACTIVE:
    return GH_CALL_VIEW_MUTE | (video ? GH_CALL_VIEW_CAMERA : 0) |
           (!video && gh_call_get_state(call) == GH_CALL_ACTIVE ? GH_CALL_VIEW_SWITCH_VIDEO : 0) |
           (devices ? GH_CALL_VIEW_DEVICES : 0) | GH_CALL_VIEW_HANG_UP;
  case GH_CALL_IDLE:
  case GH_CALL_ENDED:
  default:
    return 0;
  }
}

static void
sync_actions(CallUi *ui)
{
  GhCall *call = ui->call;
  GhCallState state = call ? gh_call_get_state(call) : GH_CALL_IDLE;
  gboolean video = call && gh_call_get_call_type(call) == GH_CALL_VIDEO;
  set_enabled(ui, "call-answer", state == GH_CALL_RINGING);
  set_enabled(ui, "call-answer-video", state == GH_CALL_RINGING && video);
  set_enabled(ui, "call-decline", state == GH_CALL_RINGING);
  set_enabled(ui, "call-hang-up", in_call(call));
  set_enabled(ui, "call-toggle-mute", in_call(call));
  set_enabled(ui, "call-toggle-camera", in_call(call) && video);
  set_enabled(ui, "call-switch-video", state == GH_CALL_ACTIVE && !video &&
                                           !gh_call_get_reconnecting(call));
  set_enabled(ui, "call-show", call != NULL && !on_page(ui));
  set_enabled(ui, "call-leave-view", on_page(ui));
  set_state(ui, "call-toggle-mute", ui->muted);
  set_state(ui, "call-toggle-camera", ui->camera);
}

/* The page follows the service's current call. */
static void
sync_call(CallUi *ui)
{
  if (gtk_widget_in_destruction(GTK_WIDGET(ui->window)))
    return;
  GhCall *current = gh_call_service_get_current(ui->service);
  /* An ended call is shown only if it ended here; once left, it is gone. */
  if (ongoing(current) && current != ui->call) {
    g_set_object(&ui->call, current);
    ui->muted = FALSE;
    ui->camera = gh_call_get_call_type(current) == GH_CALL_VIDEO;
    ui->last_state = GH_CALL_IDLE;
    ui->last_type = gh_call_get_call_type(current);
    for (guint i = 0; i < G_N_ELEMENTS(ui->selected); i++)
      g_clear_object(&ui->selected[i]);
    g_autofree gchar *name = peer_name(ui, gh_call_get_peer(current));
    gh_call_view_set_peer(ui->view, name);
  }
  GhCall *call = ui->call;
  if (!call) {
    leave_page(ui);
    sync_actions(ui);
    sync_banner(ui);
    return;
  }
  watch_media(ui, in_call(call) ? gh_call_get_media(call) : NULL);
  if (gh_call_get_call_type(call) != ui->last_type) {
    /* An upgrade to video by the other side leaves this camera off until
     * it is turned on here; Switch to Video turned it on first. */
    ui->last_type = gh_call_get_call_type(call);
    apply_media_state(ui);
  }
  gh_call_view_set_controls(ui->view, controls_for(ui, call));
  gh_call_view_set_muted(ui->view, ui->muted);
  sync_video(ui);
  sync_status(ui);
  GhCallState state = gh_call_get_state(call);
  gboolean reconnecting = gh_call_get_reconnecting(call);
  if (state != ui->last_state || reconnecting != ui->last_reconnecting) {
    g_autofree gchar *status = gh_call_ui_describe(call, g_get_monotonic_time());
    gtk_accessible_announce(GTK_ACCESSIBLE(ui->view), status, GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
    if (state == GH_CALL_ENDED && ui->last_state != GH_CALL_IDLE && !on_page(ui) && !ui->placing)
      toast(ui, status);
    ui->last_state = state;
    ui->last_reconnecting = reconnecting;
  }
  if (state == GH_CALL_ACTIVE && !ui->tick)
    ui->tick = g_timeout_add_seconds(1, on_tick, ui);
  else if (state != GH_CALL_ACTIVE && ui->tick)
    g_clear_handle_id(&ui->tick, g_source_remove);
  sync_actions(ui);
  sync_banner(ui);
}

static void
on_state_changed(GhCallService *service, GhCall *call, CallUi *ui)
{
  (void)service;
  if (call == gh_call_service_get_current(ui->service) || call == ui->call)
    sync_call(ui);
  sync_buttons(ui);
}

static void
on_incoming_ring(GhCallService *service, GhCall *call, CallUi *ui)
{
  (void)service;
  sync_call(ui);
  if (ui->call == call)
    show_page(ui);
}

static void
on_root_page(CallUi *ui)
{
  if (!on_page(ui) && ui->call && !ongoing(ui->call))
    g_clear_object(&ui->call); /* an ended call goes once the page is left */
  sync_call(ui);
}

/* ---- headset ---------------------------------------------------------------------------- */

typedef void (*Proceed)(CallUi *ui, gpointer data);

typedef struct {
  GWeakRef window;
  GtkDropDown *outputs;
  Proceed proceed;
  gpointer data;
  GDestroyNotify destroy;
} HeadsetAsk;

static void
headset_ask_free(gpointer p)
{
  HeadsetAsk *ask = p;
  g_weak_ref_clear(&ask->window);
  if (ask->destroy)
    ask->destroy(ask->data);
  g_free(ask);
}

static void
on_headset_response(AdwAlertDialog *dialog, const gchar *response, HeadsetAsk *ask)
{
  (void)dialog;
  g_autoptr(GhWindow) window = g_weak_ref_get(&ask->window);
  CallUi *ui = ui_of(window);
  if (!ui || g_strcmp0(response, "continue") != 0)
    return;
  GObject *output = gtk_drop_down_get_selected_item(ask->outputs);
  g_autoptr(GError) error = NULL;
  if (!output || !ui->config.choose_headset ||
      !ui->config.choose_headset(ui->config.data, output, &error)) {
    toast(ui, _("The headset couldn’t be selected"));
    return;
  }
  ask->proceed(ui, ask->data);
}

static gchar *
output_name(GObject *item)
{
  return g_strdup(device_display_name(item));
}

/* Without echo cancellation only a headset keeps the other person from
 * hearing themselves: ask which output is the headset, then proceed. */
static void
with_headset(CallUi *ui, const gchar *affirmative, Proceed proceed, gpointer data, GDestroyNotify destroy)
{
  if (probe(ui) != GH_CALL_UI_MEDIA_HEADSET_ONLY) {
    proceed(ui, data);
    if (destroy)
      destroy(data);
    return;
  }
  GListModel *outputs = ui->config.headset_outputs ? ui->config.headset_outputs(ui->config.data) : NULL;
  gboolean any = outputs && g_list_model_get_n_items(outputs) > 0;
  AdwDialog *dialog = adw_alert_dialog_new(_("Use a Headset"), NULL);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_set_body(alert, any
    ? _("Echo cancellation isn’t installed, so through speakers the other person would hear "
        "themselves. Choose the headset for this call.")
    : _("Echo cancellation isn’t installed, so through speakers the other person would hear "
        "themselves. Connect a headset to make calls."));
  GtkExpression *expression = gtk_cclosure_expression_new(G_TYPE_STRING, NULL, 0, NULL,
                                                          G_CALLBACK(output_name), NULL, NULL);
  GtkDropDown *drop = GTK_DROP_DOWN(gtk_drop_down_new(outputs ? g_object_ref(outputs) : NULL, expression));
  gtk_accessible_update_property(GTK_ACCESSIBLE(drop), GTK_ACCESSIBLE_PROPERTY_LABEL, _("Headset"), -1);
  gtk_widget_set_visible(GTK_WIDGET(drop), any);
  adw_alert_dialog_set_extra_child(alert, GTK_WIDGET(drop));
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "continue", affirmative, NULL);
  adw_alert_dialog_set_response_appearance(alert, "continue", ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_response_enabled(alert, "continue", any);
  adw_alert_dialog_set_default_response(alert, any ? "continue" : "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  HeadsetAsk *ask = g_new0(HeadsetAsk, 1);
  g_weak_ref_init(&ask->window, ui->window);
  ask->outputs = drop;
  ask->proceed = proceed;
  ask->data = data;
  ask->destroy = destroy;
  g_signal_connect_data(dialog, "response", G_CALLBACK(on_headset_response), ask,
                        (GClosureNotify)(void (*)(void))headset_ask_free, 0);
  g_set_weak_pointer(&ui->dialog, dialog);
  adw_dialog_present(dialog, GTK_WIDGET(ui->window));
}

/* ---- placing and answering ---------------------------------------------------------------- */

typedef struct {
  gchar *peer;
  GhCallType type;
} PlaceRequest;

static void
place_request_free(gpointer p)
{
  PlaceRequest *request = p;
  g_free(request->peer);
  g_free(request);
}

static void
do_place(CallUi *ui, gpointer data)
{
  PlaceRequest *request = data;
  g_autoptr(GError) error = NULL;
  if (ui->config.set_next_camera)
    ui->config.set_next_camera(ui->config.data, request->type == GH_CALL_VIDEO);
  ui->placing = TRUE;
  GhCall *call = gh_call_service_place(ui->service, request->peer, request->type, &error);
  ui->placing = FALSE;
  if (!call) {
    /* A call that ended while starting leaves the page with it. */
    if (ui->call && !ongoing(ui->call))
      g_clear_object(&ui->call);
    sync_call(ui);
    toast(ui, unavailable_text(error));
    return;
  }
  sync_call(ui);
  show_page(ui);
}

static void
start_call(CallUi *ui, GhCallType type)
{
  const gchar *peer = shown_peer(ui);
  if (!peer)
    return;
  PlaceRequest *request = g_new0(PlaceRequest, 1);
  request->peer = g_strdup(peer);
  request->type = type;
  with_headset(ui, _("_Call"), do_place, request, place_request_free);
}

static void
do_answer(CallUi *ui, gpointer data)
{
  gboolean video = GPOINTER_TO_INT(data);
  GhCall *call = gh_call_service_get_current(ui->service);
  if (!call || gh_call_get_state(call) != GH_CALL_RINGING)
    return;
  gboolean camera = video && gh_call_get_call_type(call) == GH_CALL_VIDEO;
  if (ui->config.set_next_camera)
    ui->config.set_next_camera(ui->config.data, camera);
  g_autoptr(GError) error = NULL;
  if (!gh_call_service_answer(ui->service, &error)) {
    toast(ui, _("The call couldn’t be answered"));
    sync_call(ui);
    return;
  }
  ui->camera = camera;
  apply_media_state(ui);
  sync_call(ui);
  show_page(ui);
}

static void
answer(CallUi *ui, gboolean video)
{
  with_headset(ui, _("_Answer"), do_answer, GINT_TO_POINTER(video), NULL);
}

/* ---- actions ------------------------------------------------------------------------------ */

static void
on_call_voice(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  start_call(data, GH_CALL_VOICE);
}

static void
on_call_video(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  start_call(data, GH_CALL_VIDEO);
}

static void
on_answer(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  answer(data, FALSE);
}

static void
on_answer_video(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  answer(data, TRUE);
}

static void
on_decline(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  CallUi *ui = data;
  gh_call_service_decline(ui->service);
}

static void
on_hang_up(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  CallUi *ui = data;
  gh_call_service_hang_up(ui->service);
}

static void
on_mute(GSimpleAction *action, GVariant *value, gpointer data)
{
  CallUi *ui = data;
  ui->muted = g_variant_get_boolean(value);
  g_simple_action_set_state(action, value);
  if (ui->media && ui->config.ops && ui->config.ops->set_muted)
    ui->config.ops->set_muted(ui->media, ui->muted);
  gh_call_view_set_muted(ui->view, ui->muted);
}

static void
on_camera(GSimpleAction *action, GVariant *value, gpointer data)
{
  CallUi *ui = data;
  ui->camera = g_variant_get_boolean(value);
  g_simple_action_set_state(action, value);
  if (ui->media && ui->config.ops && ui->config.ops->set_camera)
    ui->config.ops->set_camera(ui->media, ui->camera);
  sync_video(ui);
}

static void
on_switch_video(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  CallUi *ui = data;
  g_autoptr(GError) error = NULL;
  ui->camera = TRUE;
  if (!gh_call_service_upgrade_video(ui->service, &error)) {
    ui->camera = FALSE;
    toast(ui, _("The call can’t switch to video now"));
    return;
  }
  apply_media_state(ui);
  sync_call(ui);
}

static void
on_show(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  show_page(data);
}

static void
on_leave(GSimpleAction *a, GVariant *p, gpointer data)
{
  (void)a; (void)p;
  leave_page(data);
}

/* ---- devices -------------------------------------------------------------------------------- */

typedef struct {
  CallUi *ui;
  guint index;
  GPtrArray *devices; /* NULL first: the default */
} DeviceChoice;

static void
unref_nullable(gpointer object)
{
  if (object)
    g_object_unref(object);
}

static void
device_choice_free(gpointer p, GClosure *closure)
{
  (void)closure;
  DeviceChoice *choice = p;
  g_ptr_array_unref(choice->devices);
  g_free(choice);
}

static void
on_device_selected(GtkDropDown *drop, GParamSpec *pspec, DeviceChoice *choice)
{
  (void)pspec;
  CallUi *ui = choice->ui;
  guint position = gtk_drop_down_get_selected(drop);
  if (position >= choice->devices->len || !ui->media || !ui->config.ops->select_device)
    return;
  GObject *device = g_ptr_array_index(choice->devices, position);
  g_autoptr(GError) error = NULL;
  if (!ui->config.ops->select_device(ui->media, DEVICE_CLASSES[choice->index], device, &error)) {
    toast(ui, _("That device couldn’t be used"));
    return;
  }
  g_set_object(&ui->selected[choice->index], device);
}

static void
fill_devices(CallUi *ui)
{
  GtkBox *box = gh_call_view_get_devices_box(ui->view);
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(box))))
    gtk_box_remove(box, child);
  const GhCallMediaOps *ops = ui->config.ops;
  g_autoptr(GError) error = NULL;
  GListModel *devices = ui->media && ops && ops->devices ? ops->devices(ui->media, &error) : NULL;
  if (!devices) {
    gtk_box_append(box, gtk_label_new(_("No devices are available")));
    return;
  }
  const gchar *headings[] = { _("Microphone"), _("Audio Output"), _("Camera") };
  gboolean video = ui->call && gh_call_get_call_type(ui->call) == GH_CALL_VIDEO;
  for (guint i = 0; i < G_N_ELEMENTS(DEVICE_CLASSES); i++) {
    if (i == 2 && !video)
      continue;
    g_autoptr(GtkStringList) names = gtk_string_list_new(NULL);
    DeviceChoice *choice = g_new0(DeviceChoice, 1);
    choice->ui = ui;
    choice->index = i;
    choice->devices = g_ptr_array_new_with_free_func(unref_nullable);
    gtk_string_list_append(names, _("Default"));
    g_ptr_array_add(choice->devices, NULL);
    guint selected = 0;
    for (guint j = 0; j < g_list_model_get_n_items(devices); j++) {
      g_autoptr(GObject) device = g_list_model_get_item(devices, j);
      g_autofree gchar *klass = device_class(device);
      if (class_index(klass) != (gint)i)
        continue;
      if (device == ui->selected[i])
        selected = choice->devices->len;
      gtk_string_list_append(names, device_display_name(device));
      g_ptr_array_add(choice->devices, g_object_ref(device));
    }
    GtkWidget *heading = gtk_label_new(headings[i]);
    gtk_widget_set_halign(heading, GTK_ALIGN_START);
    gtk_widget_add_css_class(heading, "heading");
    GtkWidget *drop = gtk_drop_down_new(G_LIST_MODEL(g_object_ref(names)), NULL);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), selected);
    gtk_accessible_update_relation(GTK_ACCESSIBLE(drop), GTK_ACCESSIBLE_RELATION_LABELLED_BY,
                                   heading, NULL, -1);
    g_signal_connect_data(drop, "notify::selected", G_CALLBACK(on_device_selected), choice,
                          device_choice_free, 0);
    gtk_box_append(box, heading);
    gtk_box_append(box, drop);
  }
}

static void
on_devices_popover(GtkPopover *popover, GParamSpec *pspec, CallUi *ui)
{
  (void)pspec;
  if (gtk_widget_get_visible(GTK_WIDGET(popover)))
    fill_devices(ui);
}

/* ---- the notification's Answer -------------------------------------------------------------- */

static gboolean
on_notifier_answer(GObject *notifier, GhCall *call, gboolean video, CallUi *ui)
{
  (void)notifier;
  if (call != gh_call_service_get_current(ui->service) || gh_call_get_state(call) != GH_CALL_RINGING)
    return FALSE;
  gtk_window_present(GTK_WINDOW(ui->window));
  answer(ui, video);
  return TRUE;
}

/* ---- closing ---------------------------------------------------------------------------------- */

static gboolean
close_window_later(gpointer data)
{
  g_autoptr(GtkWindow) window = g_weak_ref_get(data);
  if (window)
    gtk_window_close(window);
  return G_SOURCE_REMOVE;
}

static void
weak_free(gpointer data)
{
  g_weak_ref_clear(data);
  g_free(data);
}

static void
on_close_response(AdwAlertDialog *dialog, const gchar *response, gpointer data)
{
  (void)dialog;
  g_autoptr(GhWindow) window = g_weak_ref_get(data);
  CallUi *ui = ui_of(window);
  if (!ui || g_strcmp0(response, "end") != 0)
    return;
  ui->close_confirmed = TRUE;
  gh_call_service_hang_up(ui->service);
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, window);
  g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, close_window_later, ref, weak_free);
}

static gboolean
on_close_request(GtkWindow *window, CallUi *ui)
{
  (void)window;
  GhCall *call = gh_call_service_get_current(ui->service);
  if (ui->close_confirmed || !ongoing(call) ||
      (ui->config.keeps_running && ui->config.keeps_running(ui->config.data)))
    return FALSE;
  if (ui->dialog)
    return TRUE;
  g_autofree gchar *name = peer_name(ui, gh_call_get_peer(call));
  g_autofree gchar *body = g_strdup_printf(_("Closing the window ends the call with %s."), name);
  AdwDialog *dialog = adw_alert_dialog_new(_("End Call?"), body);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_responses(alert, "cancel", _("_Cancel"), "end", _("_End Call"), NULL);
  adw_alert_dialog_set_response_appearance(alert, "end", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  GWeakRef *ref = g_new0(GWeakRef, 1);
  g_weak_ref_init(ref, ui->window);
  g_signal_connect_data(dialog, "response", G_CALLBACK(on_close_response), ref,
                        (GClosureNotify)(void (*)(void))weak_free, 0);
  g_set_weak_pointer(&ui->dialog, dialog);
  adw_dialog_present(dialog, GTK_WIDGET(ui->window));
  return TRUE;
}

/* ---- lifetime --------------------------------------------------------------------------------- */

static void
detach(CallUi *ui)
{
  g_clear_handle_id(&ui->tick, g_source_remove);
  if (ui->service)
    g_signal_handlers_disconnect_by_data(ui->service, ui);
  if (ui->notifier)
    g_signal_handlers_disconnect_by_data(ui->notifier, ui);
  if (ui->conversation_view)
    g_signal_handlers_disconnect_by_data(ui->conversation_view, ui);
  g_clear_weak_pointer(&ui->conversation_view);
  watch_media(ui, NULL);
}

static void
ui_free(gpointer data)
{
  CallUi *ui = data;
  detach(ui);
  g_clear_weak_pointer(&ui->dialog);
  g_clear_weak_pointer(&ui->toast);
  g_clear_object(&ui->call);
  for (guint i = 0; i < G_N_ELEMENTS(ui->selected); i++)
    g_clear_object(&ui->selected[i]);
  g_clear_object(&ui->service);
  g_clear_object(&ui->conversations);
  g_clear_object(&ui->notifier);
  g_free(ui->last_toast);
  g_free(ui);
}

static void
on_window_destroy(GhWindow *window, CallUi *ui)
{
  (void)window;
  detach(ui);
}

void
gh_call_ui_attach(GhWindow *window, const GhCallUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(config != NULL && GH_IS_CALL_SERVICE(config->service));
  g_return_if_fail(ui_of(window) == NULL);
  CallUi *ui = g_new0(CallUi, 1);
  ui->window = window;
  ui->config = *config;
  ui->service = g_object_ref(config->service);
  ui->conversations = config->conversations ? g_object_ref(config->conversations) : NULL;
  ui->notifier = config->notifier ? g_object_ref(config->notifier) : NULL;
  g_object_set_data_full(G_OBJECT(window), ATTACH_DATA, ui, ui_free);

  const GActionEntry entries[] = {
    { "call-voice", on_call_voice, NULL, NULL, NULL, { 0 } },
    { "call-video", on_call_video, NULL, NULL, NULL, { 0 } },
    { "call-answer", on_answer, NULL, NULL, NULL, { 0 } },
    { "call-answer-video", on_answer_video, NULL, NULL, NULL, { 0 } },
    { "call-decline", on_decline, NULL, NULL, NULL, { 0 } },
    { "call-hang-up", on_hang_up, NULL, NULL, NULL, { 0 } },
    { "call-toggle-mute", NULL, NULL, "false", on_mute, { 0 } },
    { "call-toggle-camera", NULL, NULL, "false", on_camera, { 0 } },
    { "call-switch-video", on_switch_video, NULL, NULL, NULL, { 0 } },
    { "call-show", on_show, NULL, NULL, NULL, { 0 } },
    { "call-leave-view", on_leave, NULL, NULL, NULL, { 0 } },
  };
  g_action_map_add_action_entries(G_ACTION_MAP(window), entries, G_N_ELEMENTS(entries), ui);

  ui->view = GH_CALL_VIEW(gh_call_view_new());
  GtkStack *root = gh_window_get_root_stack(window);
  gtk_stack_add_named(root, GTK_WIDGET(ui->view), PAGE_CALL);
  g_signal_connect_swapped(root, "notify::visible-child-name", G_CALLBACK(on_root_page), ui);
  g_signal_connect_swapped(gh_window_get_split(window), "notify::collapsed",
                           G_CALLBACK(sync_banner), ui);
  g_signal_connect(gh_call_view_get_devices_popover(ui->view), "notify::visible",
                   G_CALLBACK(on_devices_popover), ui);

  g_signal_connect(ui->service, "state-changed", G_CALLBACK(on_state_changed), ui);
  g_signal_connect(ui->service, "incoming-ring", G_CALLBACK(on_incoming_ring), ui);
  g_signal_connect_swapped(ui->service, "availability-changed", G_CALLBACK(sync_buttons), ui);
  if (ui->notifier && g_signal_lookup("answer-call", G_OBJECT_TYPE(ui->notifier)))
    g_signal_connect(ui->notifier, "answer-call", G_CALLBACK(on_notifier_answer), ui);

  GhContentPage *content = gh_window_get_content(window);
  g_signal_connect_swapped(gh_content_page_get_stack(content), "notify::visible-child-name",
                           G_CALLBACK(watch_conversation_view), ui);
  g_signal_connect_swapped(gh_content_page_get_stack(content), "notify::visible-child-name",
                           G_CALLBACK(sync_buttons), ui);
  g_signal_connect(window, "close-request", G_CALLBACK(on_close_request), ui);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), ui);

  watch_conversation_view(ui);
  sync_buttons(ui);
  sync_call(ui);
  /* A window opened during a call (from the notification, or after it ran
   * in the background) comes up on the call. */
  if (ongoing(ui->call))
    show_page(ui);
}

GtkWidget *
gh_call_ui_get_view(GhWindow *window)
{
  CallUi *ui = ui_of(window);
  return ui ? GTK_WIDGET(ui->view) : NULL;
}

const gchar *
gh_call_ui_get_last_toast(GhWindow *window)
{
  CallUi *ui = ui_of(window);
  return ui ? ui->last_toast : NULL;
}
