#include "gh-conversation-row.h"

#include "gh-privacy-summary.h"

#include <glib/gi18n.h>

struct _GhConversationRow {
  GtkWidget parent_instance;
  AdwAvatar *avatar;
  GtkImage *kind_icon;
  GtkLabel *title_label;
  GtkLabel *time_label;
  GtkLabel *preview_label;
  GtkLabel *request_label;
  GtkLabel *unread_badge;
  GhConversation *conversation;
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
  if (!conversation) {
    adw_avatar_set_text(self->avatar, NULL);
    gtk_label_set_text(self->title_label, "");
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
  const gchar *subject = request ? gh_conversation_get_subject(conversation) : NULL;
  const gchar *preview = self->show_preview ? gh_conversation_get_preview(conversation) : NULL;
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  g_autofree gchar *time = gh_conversation_row_format_time(
    gh_conversation_get_last_activity(conversation), now);
  /* TRANSLATORS: a message request's subject, then its newest message. */
  g_autofree gchar *secondary = subject && preview && *preview
    ? g_strdup_printf(_("%s — %s"), subject, preview)
    : g_strdup(subject ? subject : preview ? preview : "");

  /* Charter §7.5: a relay group shows a network glyph and says it is not
   * end-to-end encrypted; the strings table names every kind (G20b). */
  GhPrivacyBackend backend = (GhPrivacyBackend)gh_conversation_get_backend(conversation);
  const gchar *kind = gh_privacy_summary_kind(backend);
  gboolean relay_group = backend == GH_PRIVACY_BACKEND_NIP29;
  gtk_widget_set_visible(GTK_WIDGET(self->kind_icon), relay_group);
  if (relay_group) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->kind_icon), kind);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->kind_icon),
                                   GTK_ACCESSIBLE_PROPERTY_LABEL, kind, -1);
  }

  adw_avatar_set_text(self->avatar, title);
  gtk_label_set_text(self->title_label, title);
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
  g_set_object(&self->conversation, conversation);
  if (conversation) {
    static const gchar *const watched[] = {
      "notify::title", "notify::subject", "notify::preview", "notify::last-activity",
      "notify::unread-count", "notify::is-request",
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
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, time_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, preview_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, request_label);
  gtk_widget_class_bind_template_child(widget_class, GhConversationRow, unread_badge);
}

static void
gh_conversation_row_init(GhConversationRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->summary = g_strdup("");
}
