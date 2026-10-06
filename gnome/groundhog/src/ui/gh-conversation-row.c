#include "gh-conversation-row.h"

#include "gh-privacy-summary.h"
#include "gh-picture-cache.h"

#include <glib/gi18n.h>

struct _GhConversationRow {
  GtkWidget parent_instance;
  AdwAvatar *avatar;
  GtkImage *kind_icon;
  GtkLabel *title_label;
  GtkImage *pinned_icon;
  GtkLabel *time_label;
  GtkLabel *preview_label;
  GtkLabel *request_label;
  GtkLabel *unread_badge;
  GtkPopover *context_popover;
  GhConversation *conversation;
  GhPictureCache *pictures;                 /* nullable, ref'd */
  GhConversationRowPictureUri picture_uri;  /* with pictures */
  gpointer picture_data;
  gchar *summary;
  gboolean show_preview;
};

enum { PROP_0, PROP_CONVERSATION, PROP_SHOW_PREVIEW, PROP_SUMMARY, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhConversationRow, gh_conversation_row, GTK_TYPE_WIDGET)

/* ---- time ------------------------------------------------------------------- */

static gboolean
clock_is_24h(void)
{
  static gsize once = 0;
  static GSettings *interface = NULL;
  if (g_once_init_enter(&once)) {
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    g_autoptr(GSettingsSchema) schema =
      source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.interface", TRUE)
             : NULL;
    if (schema && g_settings_schema_has_key(schema, "clock-format"))
      interface = g_settings_new("org.gnome.desktop.interface");
    g_once_init_leave(&once, 1);
  }
  if (!interface)
    return TRUE;
  g_autofree gchar *format = g_settings_get_string(interface, "clock-format");
  return g_strcmp0(format, "12h") != 0;
}

static gchar *
time_of_day(GDateTime *when)
{
  return g_date_time_format(when, clock_is_24h() ? "%H:%M" : "%l:%M %p");
}

/* Whole local calendar days from when to now (negative in the future).
 * Compared at noon, which (unlike midnight) exists on every day. */
static gint
days_before(GDateTime *when, GDateTime *now)
{
  g_autoptr(GDateTime) day = g_date_time_new_local(g_date_time_get_year(when),
                                                   g_date_time_get_month(when),
                                                   g_date_time_get_day_of_month(when), 12, 0, 0);
  g_autoptr(GDateTime) today = g_date_time_new_local(g_date_time_get_year(now),
                                                     g_date_time_get_month(now),
                                                     g_date_time_get_day_of_month(now), 12, 0, 0);
  if (!day || !today)
    return G_MAXINT;
  /* Rounded: a DST change makes a day 23 or 25 hours long. */
  GTimeSpan span = g_date_time_difference(today, day);
  return (gint)((span + (span >= 0 ? G_TIME_SPAN_HOUR * 12 : -G_TIME_SPAN_HOUR * 12)) /
                G_TIME_SPAN_DAY);
}

static gchar *
day_label(GDateTime *when, GDateTime *now)
{
  gint days = days_before(when, now);
  if (days == 1)
    return g_strdup(_("Yesterday"));
  if (days > 1 && days < 7)
    return g_date_time_format(when, "%A");
  return g_date_time_format(when, "%x");
}

gchar *
gh_conversation_row_format_time(gint64 timestamp, GDateTime *now)
{
  g_return_val_if_fail(now != NULL, NULL);
  if (timestamp <= 0)
    return g_strdup("");
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(timestamp);
  if (!utc)
    return g_strdup("");
  g_autoptr(GDateTime) when = g_date_time_to_local(utc);
  g_autoptr(GDateTime) local_now = g_date_time_to_local(now);
  gchar *text = days_before(when, local_now) == 0 ? time_of_day(when)
                                                  : day_label(when, local_now);
  return g_strstrip(text);
}

gchar *
gh_conversation_row_format_message_time(gint64 timestamp, GDateTime *now)
{
  g_return_val_if_fail(now != NULL, NULL);
  if (timestamp <= 0)
    return g_strdup("");
  g_autoptr(GDateTime) utc = g_date_time_new_from_unix_utc(timestamp);
  if (!utc)
    return g_strdup("");
  g_autoptr(GDateTime) when = g_date_time_to_local(utc);
  g_autoptr(GDateTime) local_now = g_date_time_to_local(now);
  g_autofree gchar *time = g_strstrip(time_of_day(when));
  if (days_before(when, local_now) == 0)
    return g_steal_pointer(&time);
  g_autofree gchar *day = day_label(when, local_now);
  /* TRANSLATORS: a message's day ("Yesterday", a weekday or a date) and
   * time of day. */
  return g_strdup_printf(_("%s %s"), day, time);
}

gchar *
gh_conversation_row_format_time_of_day(gint64 timestamp)
{
  if (timestamp <= 0)
    return g_strdup("");
  g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(timestamp);
  return when ? g_strstrip(time_of_day(when)) : g_strdup("");
}

/* ---- context menu (charter §7.4; nostrc-qp24.74) ----------------------------- */

/* Which of the menu's actions apply: to a private (NIP-17) conversation,
 * none to a group (its dialogs are its own). Of each pair only the one that
 * applies is enabled (its menu item shows): Pin or Unpin, Mark as Read (while
 * something is unread) or Mark as Unread (nostrc-qp24.86). */
static void
sync_actions(GhConversationRow *self)
{
  GhConversation *conversation = self->conversation;
  gboolean nip17 = conversation &&
                   gh_conversation_get_backend(conversation) == GH_CONVERSATION_BACKEND_NIP17;
  /* A message request is never notified (nothing to mute) and lives in
   * Message Requests (nothing to pin or mark). */
  gboolean accepted = nip17 && !gh_conversation_get_is_request(conversation);
  gboolean pinned = accepted && gh_conversation_get_pinned(conversation);
  gboolean unread = accepted && gh_conversation_get_unread_count(conversation) > 0;
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.pin", accepted && !pinned);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.unpin", pinned);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.mark-read", unread);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.mark-unread",
                                accepted && gh_conversation_can_mark_unread(conversation));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.mute", accepted);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.info", nip17);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "row.delete", nip17);
}

static gboolean
menu_available(GhConversationRow *self)
{
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(self));
  return self->conversation &&
         gh_conversation_get_backend(self->conversation) == GH_CONVERSATION_BACKEND_NIP17 &&
         G_IS_ACTION_GROUP(root) &&
         g_action_group_has_action(G_ACTION_GROUP(root), "delete-conversation");
}

/* Each row action runs its window action (gh_conversation_menu_attach())
 * with the row's room id. */
static void
row_action(GtkWidget *widget, const char *name, GVariant *parameter)
{
  GhConversationRow *self = GH_CONVERSATION_ROW(widget);
  (void)parameter;
  if (!self->conversation)
    return;
  static const struct {
    const gchar *row;
    const gchar *window;
  } forward[] = {
    { "row.pin", "win.pin-conversation" },
    { "row.unpin", "win.unpin-conversation" },
    { "row.mark-read", "win.mark-conversation-read" },
    { "row.mark-unread", "win.mark-conversation-unread" },
    { "row.mute", "win.mute-conversation" },
    { "row.info", "win.show-conversation-info" },
    { "row.delete", "win.delete-conversation" },
  };
  const gchar *action = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(forward) && !action; i++)
    if (g_str_equal(name, forward[i].row))
      action = forward[i].window;
  if (!action)
    return;
  gtk_widget_activate_action(widget, action, "s",
                             gh_conversation_get_room_id(self->conversation));
}

static gboolean
popup_at(GhConversationRow *self, const GdkRectangle *point)
{
  if (!menu_available(self))
    return FALSE;
  GdkRectangle whole = { 0, 0, gtk_widget_get_width(GTK_WIDGET(self)),
                         gtk_widget_get_height(GTK_WIDGET(self)) };
  gtk_popover_set_pointing_to(self->context_popover, point ? point : &whole);
  gtk_popover_popup(self->context_popover);
  return TRUE;
}

gboolean
gh_conversation_row_popup_menu(GhConversationRow *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_ROW(self), FALSE);
  return popup_at(self, NULL);
}

GtkPopover *
gh_conversation_row_get_menu(GhConversationRow *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_ROW(self), NULL);
  return self->context_popover;
}

static void
on_secondary_pressed(GtkGestureClick *gesture, gint n_press, gdouble x, gdouble y,
                     GhConversationRow *self)
{
  (void)n_press;
  GdkRectangle point = { (int)x, (int)y, 1, 1 };
  if (popup_at(self, &point))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static void
on_long_pressed(GtkGestureLongPress *gesture, gdouble x, gdouble y, GhConversationRow *self)
{
  GdkRectangle point = { (int)x, (int)y, 1, 1 };
  if (popup_at(self, &point))
    gtk_gesture_set_state(GTK_GESTURE(gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

/* ---- row -------------------------------------------------------------------- */

static void
set_summary(GhConversationRow *self, gchar *summary)
{
  if (g_strcmp0(self->summary, summary) == 0) {
    g_free(summary);
    return;
  }
  g_free(self->summary);
  self->summary = summary;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SUMMARY]);
}

static void
update(GhConversationRow *self)
{
  GhConversation *conversation = self->conversation;
  sync_actions(self);
  if (!conversation) {
    adw_avatar_set_text(self->avatar, NULL);
    gtk_label_set_text(self->title_label, "");
    gtk_widget_set_visible(GTK_WIDGET(self->pinned_icon), FALSE);
    gtk_label_set_text(self->time_label, "");
    gtk_label_set_text(self->preview_label, "");
    gtk_widget_set_visible(GTK_WIDGET(self->request_label), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->unread_badge), FALSE);
    gtk_widget_set_visible(GTK_WIDGET(self->kind_icon), FALSE);
    gtk_widget_remove_css_class(GTK_WIDGET(self->title_label), "groundhog-unread");
    set_summary(self, g_strdup(""));
    return;
  }

  /* A request is titled by the sender's npub (gh_conversation_get_title());
   * the subject it carries is only secondary text, before any preview. */
  const gchar *title = gh_conversation_get_title(conversation);
  guint unread = gh_conversation_get_unread_count(conversation);
  gboolean request = gh_conversation_get_is_request(conversation);
  gboolean pinned = gh_conversation_get_pinned(conversation);
  const gchar *subject = request ? gh_conversation_get_subject(conversation) : NULL;
  const gchar *preview = self->show_preview ? gh_conversation_get_preview(conversation) : NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree gchar *time = gh_conversation_row_format_time(
    gh_conversation_get_last_activity(conversation), now);
  /* TRANSLATORS: a message request's subject, then its newest message. */
  g_autofree gchar *secondary = subject && preview && *preview
    ? g_strdup_printf(_("%s — %s"), subject, preview)
    : g_strdup(subject ? subject : preview ? preview : "");

  /* Charter §7.5: a relay group shows a network glyph, an encrypted group a
   * lock (qp24.13 part 2), and every DM shows its protocol badge (NIP-17 or
   * Marmot); the strings table names every kind (G20b, W26 slice A). */
  GhPrivacyBackend backend = (GhPrivacyBackend)gh_conversation_get_backend(conversation);
  gboolean is_direct = gh_conversation_get_is_direct(conversation);
  const gchar *kind = gh_privacy_summary_kind(backend, is_direct);
  gboolean relay_group = backend == GH_PRIVACY_BACKEND_NIP29;
  gboolean encrypted_group = backend == GH_PRIVACY_BACKEND_MLS && !is_direct;
  gboolean nip17_dm = backend == GH_PRIVACY_BACKEND_NIP17 && is_direct;
  gboolean marmot_dm = backend == GH_PRIVACY_BACKEND_MLS && is_direct;
  gboolean show_badge = relay_group || encrypted_group || nip17_dm || marmot_dm;
  gtk_widget_set_visible(GTK_WIDGET(self->kind_icon), show_badge);
  if (show_badge) {
    const gchar *icon = relay_group     ? "network-workgroup-symbolic"
                      : encrypted_group ? "channel-secure-symbolic"
                      : marmot_dm       ? "channel-secure-symbolic"
                      :                   "mail-unread-symbolic";
    gtk_image_set_from_icon_name(self->kind_icon, icon);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->kind_icon), kind);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->kind_icon),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, kind, -1);
  }

  adw_avatar_set_text(self->avatar, title);
  /* The peer's picture, when the shared cache has it with consent. */
  GdkTexture *picture = NULL;
  if (self->pictures && self->picture_uri && is_direct && backend != GH_PRIVACY_BACKEND_NIP29) {
    const gchar *const *peers = gh_conversation_get_peers(self->conversation);
    const gchar *peer = peers && peers[0] && !peers[1] ? peers[0] : NULL;
    if (peer && gh_picture_cache_is_allowed(self->pictures, peer)) {
      g_autofree gchar *uri = self->picture_uri(peer, self->picture_data);
      picture = gh_picture_cache_get(self->pictures, peer, uri);
    }
  }
  adw_avatar_set_custom_image(self->avatar, picture ? GDK_PAINTABLE(picture) : NULL);
  gtk_label_set_text(self->title_label, title);
  gtk_widget_set_visible(GTK_WIDGET(self->pinned_icon), pinned);
  gtk_label_set_text(self->time_label, time);
  gtk_label_set_text(self->preview_label, secondary);
  gtk_widget_set_visible(GTK_WIDGET(self->request_label), request);
  gtk_widget_set_visible(GTK_WIDGET(self->unread_badge), unread > 0);
  if (unread > 0) {
    g_autofree gchar *count = unread > 99 ? g_strdup("99+") : g_strdup_printf("%u", unread);
    gtk_label_set_text(self->unread_badge, count);
    gtk_widget_add_css_class(GTK_WIDGET(self->title_label), "groundhog-unread");
  } else {
    gtk_widget_remove_css_class(GTK_WIDGET(self->title_label), "groundhog-unread");
  }

  /* Charter §7.5: "Alice. Private conversation. 2 unread. 10:42. Hello". */
  g_autoptr(GPtrArray) parts = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(parts, g_strdup(title));
  g_ptr_array_add(parts, g_strdup(request ? _("Message request")
                                           : kind ? kind : _("Private conversation")));
  if (pinned)
    g_ptr_array_add(parts, g_strdup(_("Pinned")));
  if (subject)
    /* TRANSLATORS: a message request's subject, read out after its sender. */
    g_ptr_array_add(parts, g_strdup_printf(_("Subject: %s"), subject));
  if (unread > 0)
    g_ptr_array_add(parts, g_strdup_printf(g_dngettext(NULL, "%u unread", "%u unread", unread),
                                           unread));
  if (*time)
    g_ptr_array_add(parts, g_strdup(time));
  if (preview && *preview)
    g_ptr_array_add(parts, g_strdup(preview));
  g_ptr_array_add(parts, NULL);
  set_summary(self, g_strjoinv(". ", (gchar **)parts->pdata));
}

static void
on_conversation_notify(GhConversationRow *self)
{
  update(self);
}

void
gh_conversation_row_set_conversation(GhConversationRow *self, GhConversation *conversation)
{
  g_return_if_fail(GH_IS_CONVERSATION_ROW(self));
  g_return_if_fail(!conversation || GH_IS_CONVERSATION(conversation));
  if (self->conversation == conversation)
    return;
  if (self->conversation)
    g_signal_handlers_disconnect_by_data(self->conversation, self);
  /* A recycled row's menu was for the conversation it showed. */
  gtk_popover_popdown(self->context_popover);
  g_set_object(&self->conversation, conversation);
  if (conversation) {
    static const gchar *const watched[] = {
      "notify::title", "notify::subject", "notify::preview", "notify::last-activity",
      "notify::unread-count", "notify::is-request", "notify::pinned", "notify::is-direct",
    };
    for (guint i = 0; i < G_N_ELEMENTS(watched); i++)
      g_signal_connect_swapped(conversation, watched[i], G_CALLBACK(on_conversation_notify),
                               self);
  }
  update(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_CONVERSATION]);
}

GhConversation *
gh_conversation_row_get_conversation(GhConversationRow *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_ROW(self), NULL);
  return self->conversation;
}

void
gh_conversation_row_set_show_preview(GhConversationRow *self, gboolean show_preview)
{
  g_return_if_fail(GH_IS_CONVERSATION_ROW(self));
  if (self->show_preview == !!show_preview)
    return;
  self->show_preview = !!show_preview;
  update(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_SHOW_PREVIEW]);
}

const gchar *
gh_conversation_row_get_summary(GhConversationRow *self)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_ROW(self), NULL);
  return self->summary;
}

static void
on_picture_changed(GhPictureCache *cache, const gchar *pubkey, GhConversationRow *self)
{
  (void)cache; (void)pubkey;
  if (self->conversation) update(self);
}

void
gh_conversation_row_set_picture_cache(GhConversationRow *self, GObject *cache,
                                      GhConversationRowPictureUri uri, gpointer data)
{
  g_return_if_fail(GH_IS_CONVERSATION_ROW(self));
  if (self->pictures)
    g_signal_handlers_disconnect_by_data(self->pictures, self);
  GhPictureCache *pictures = cache ? GH_PICTURE_CACHE(cache) : NULL;
  g_set_object(&self->pictures, pictures);
  self->picture_uri = uri;
  self->picture_data = data;
  if (self->pictures)
    g_signal_connect_object(self->pictures, "picture-changed", G_CALLBACK(on_picture_changed),
                            self, 0);
  if (self->conversation) update(self);
}

GtkWidget *
gh_conversation_row_new(void)
{
  return g_object_new(GH_TYPE_CONVERSATION_ROW, NULL);
}

static void
gh_conversation_row_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhConversationRow *self = GH_CONVERSATION_ROW(object);
  switch (id) {
  case PROP_CONVERSATION:
    g_value_set_object(value, self->conversation);
    break;
  case PROP_SHOW_PREVIEW:
    g_value_set_boolean(value, self->show_preview);
    break;
  case PROP_SUMMARY:
    g_value_set_string(value, self->summary);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_conversation_row_set_property(GObject *object, guint id, const GValue *value,
                                 GParamSpec *pspec)
{
  GhConversationRow *self = GH_CONVERSATION_ROW(object);
  switch (id) {
  case PROP_CONVERSATION:
    gh_conversation_row_set_conversation(self, g_value_get_object(value));
    break;
  case PROP_SHOW_PREVIEW:
    gh_conversation_row_set_show_preview(self, g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_conversation_row_dispose(GObject *object)
{
  GhConversationRow *self = GH_CONVERSATION_ROW(object);
  if (self->conversation)
    g_signal_handlers_disconnect_by_data(self->conversation, self);
  g_clear_object(&self->conversation);
  if (self->pictures)
    g_signal_handlers_disconnect_by_data(self->pictures, self);
  g_clear_object(&self->pictures);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CONVERSATION_ROW);
  /* A plain GtkWidget owns its template's unnamed children too. */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(object))))
    gtk_widget_unparent(child);
  G_OBJECT_CLASS(gh_conversation_row_parent_class)->dispose(object);
}

static void
gh_conversation_row_finalize(GObject *object)
{
  g_free(GH_CONVERSATION_ROW(object)->summary);
  G_OBJECT_CLASS(gh_conversation_row_parent_class)->finalize(object);
}

static void
gh_conversation_row_class_init(GhConversationRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_conversation_row_get_property;
  object_class->set_property = gh_conversation_row_set_property;
  object_class->dispose = gh_conversation_row_dispose;
  object_class->finalize = gh_conversation_row_finalize;
  props[PROP_CONVERSATION] = g_param_spec_object("conversation", NULL, NULL,
    GH_TYPE_CONVERSATION, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_SHOW_PREVIEW] = g_param_spec_boolean("show-preview", NULL, NULL, FALSE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_SUMMARY] = g_param_spec_string("summary", NULL, NULL, "",
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-conversation-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, avatar);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, kind_icon);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, title_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, pinned_icon);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, time_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, preview_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, request_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, unread_badge);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, context_popover);
  gtk_widget_class_install_action(widget_class, "row.pin", NULL, row_action);
  gtk_widget_class_install_action(widget_class, "row.unpin", NULL, row_action);
  gtk_widget_class_install_action(widget_class, "row.mark-read", NULL, row_action);
  gtk_widget_class_install_action(widget_class, "row.mark-unread", NULL, row_action);
  gtk_widget_class_install_action(widget_class, "row.mute", NULL, row_action);
  gtk_widget_class_install_action(widget_class, "row.info", NULL, row_action);
  gtk_widget_class_install_action(widget_class, "row.delete", NULL, row_action);
}

static void
gh_conversation_row_init(GhConversationRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->summary = g_strdup("");
  sync_actions(self);
  /* The context menu: right click, or a long press on a touchscreen. The
   * keyboard's Shift+F10 and Menu are the list's (gh-conversation-menu.c). */
  GtkGesture *click = gtk_gesture_click_new();
  gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_SECONDARY);
  g_signal_connect(click, "pressed", G_CALLBACK(on_secondary_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(click));
  GtkGesture *hold = gtk_gesture_long_press_new();
  gtk_gesture_single_set_touch_only(GTK_GESTURE_SINGLE(hold), TRUE);
  g_signal_connect(hold, "pressed", G_CALLBACK(on_long_pressed), self);
  gtk_widget_add_controller(GTK_WIDGET(self), GTK_EVENT_CONTROLLER(hold));
}
