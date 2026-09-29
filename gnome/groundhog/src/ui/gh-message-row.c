#include "gh-message-row.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-delivery-indicator.h"
#include "gh-link-policy.h"

#include <glib/gi18n.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

/* Bubble text width caps (charter §7.6): 60 characters, 32 when compact. */
#define BODY_CHARS 60
#define BODY_CHARS_COMPACT 32

struct _GhMessageRow {
  GtkWidget parent_instance;
  GtkLabel *sender_label;
  GtkBox *bubble;
  GtkLabel *body_label;
  GtkBox *preview_box;
  GtkButton *preview_button;
  GtkLabel *preview_title;
  GtkLabel *preview_text;
  GtkBox *meta_box;
  GtkImage *timer_icon;
  GtkLabel *time_label;
  GhDeliveryIndicator *delivery;
  GtkButton *retry_button;

  GhMessage *message;
  GhConversationView *view; /* the enclosing view while rooted; not a reference */
  GBinding *compact_binding;
  gchar *preview_uri;       /* the first previewable link, or NULL */
  gchar *summary;
  gboolean run_start;
  gboolean run_end;
  gboolean show_sender;
  gboolean compact;
  gboolean undecryptable;
};

enum {
  PROP_0,
  PROP_MESSAGE,
  PROP_RUN_START,
  PROP_RUN_END,
  PROP_SHOW_SENDER,
  PROP_COMPACT,
  PROP_UNDECRYPTABLE,
  PROP_SUMMARY,
  N_PROPS
};
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhMessageRow, gh_message_row, GTK_TYPE_WIDGET)

/* ---- text -------------------------------------------------------------------- */

gchar *
gh_message_row_display_name(const gchar *pubkey_hex)
{
  g_return_val_if_fail(pubkey_hex != NULL, NULL);
  guint8 bytes[32];
  char *npub = NULL;
  if (strlen(pubkey_hex) != 64 || !nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gsize length = strlen(npub);
  gchar *out = length > 16 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                           : g_strdup(npub);
  free(npub);
  return out;
}

gchar *
gh_message_row_sender_name(GhMessage *message)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  return gh_message_is_self(message) ? g_strdup(_("You"))
                                     : gh_message_row_display_name(gh_message_get_sender(message));
}

static gboolean
ends_sentence(const gchar *text)
{
  gsize length = strlen(text);
  if (length == 0)
    return FALSE;
  gunichar last = g_utf8_get_char(g_utf8_prev_char(text + length));
  return last == '.' || last == '!' || last == '?' || last == 0x2026 /* … */;
}

/* Appends one sentence to a label, separated as prose. */
static void
append_sentence(GString *out, const gchar *sentence)
{
  if (!sentence || !*sentence)
    return;
  if (!ends_sentence(out->str))
    g_string_append_c(out, '.');
  g_string_append_c(out, ' ');
  g_string_append(out, sentence);
  if (!ends_sentence(sentence))
    g_string_append_c(out, '.');
}

static gchar *
compose_summary(GhMessage *message, GDateTime *now, gboolean undecryptable)
{
  g_autofree gchar *sender = gh_message_row_sender_name(message);
  g_autofree gchar *time =
    gh_conversation_row_format_message_time(gh_message_get_created_at(message), now);
  const gchar *body = undecryptable ? _("Unable to decrypt yet") : gh_message_get_content(message);
  /* TRANSLATORS: a message's accessible label: sender, time, text. */
  GString *out = g_string_new(NULL);
  g_string_printf(out, _("%s, %s: %s"), sender, time, body);
  if (gh_message_get_expires_at(message) > 0)
    append_sentence(out, _("Disappearing message"));
  if (gh_message_is_self(message))
    append_sentence(out, gh_message_status_get_label(gh_message_get_status(message)));
  return g_string_free(out, FALSE);
}

gchar *
gh_message_row_compose_summary(GhMessage *message, GDateTime *now)
{
  g_return_val_if_fail(GH_IS_MESSAGE(message), NULL);
  g_return_val_if_fail(now != NULL, NULL);
  return compose_summary(message, now, FALSE);
}

/* ---- updates ------------------------------------------------------------------ */

static void
set_summary(GhMessageRow *self, gchar *summary)
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
update_summary(GhMessageRow *self)
{
  if (!self->message) {
    set_summary(self, g_strdup(""));
    return;
  }
  g_autoptr(GDateTime) now = g_date_time_new_now_local();
  set_summary(self, compose_summary(self->message, now, self->undecryptable));
}

static void
set_class(GtkWidget *widget, const gchar *name, gboolean on)
{
  if (on)
    gtk_widget_add_css_class(widget, name);
  else
    gtk_widget_remove_css_class(widget, name);
}

static void
update_width(GhMessageRow *self)
{
  gint chars = self->compact ? BODY_CHARS_COMPACT : BODY_CHARS;
  gtk_label_set_max_width_chars(self->body_label, chars);
  gtk_label_set_max_width_chars(self->preview_title, chars);
  gtk_label_set_max_width_chars(self->preview_text, chars);
}

static void
update_runs(GhMessageRow *self)
{
  set_class(GTK_WIDGET(self), "run-start", self->run_start);
  set_class(GTK_WIDGET(self), "run-end", self->run_end);
  gboolean outgoing = self->message && gh_message_is_self(self->message);
  gtk_widget_set_visible(GTK_WIDGET(self->sender_label),
                         self->message && self->show_sender && !outgoing);
}

/* The meta line: always at the end of a run, and on any message whose
 * state needs saying (disappearing, or an own message not simply "Sent"). */
static void
update_meta(GhMessageRow *self)
{
  GhMessage *message = self->message;
  if (!message) {
    gtk_widget_set_visible(GTK_WIDGET(self->meta_box), FALSE);
    return;
  }
  GhMessageStatus status = gh_message_is_self(message) ? gh_message_get_status(message)
                                                       : GH_MESSAGE_STATUS_NONE;
  gboolean expiring = gh_message_get_expires_at(message) > 0;
  gboolean noteworthy = status != GH_MESSAGE_STATUS_NONE && status != GH_MESSAGE_STATUS_SENT;
  gtk_widget_set_visible(GTK_WIDGET(self->meta_box), self->run_end || expiring || noteworthy);
  gtk_widget_set_visible(GTK_WIDGET(self->retry_button), status == GH_MESSAGE_STATUS_NOT_SENT);
}

static void
update_expiry(GhMessageRow *self)
{
  gint64 expires = self->message ? gh_message_get_expires_at(self->message) : 0;
  gtk_widget_set_visible(GTK_WIDGET(self->timer_icon), expires > 0);
  if (expires > 0) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *when = gh_conversation_row_format_message_time(expires, now);
    g_autofree gchar *tooltip = g_strdup_printf(_("Disappears %s"), when);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->timer_icon), tooltip);
  }
}

static void
update_preview(GhMessageRow *self)
{
  gboolean offered = self->message && self->preview_uri;
  gtk_widget_set_visible(GTK_WIDGET(self->preview_box), offered);
  if (!offered)
    return;
  const gchar *title = NULL;
  const gchar *description = NULL;
  GhLinkPreviewState state =
    self->view ? gh_conversation_view_get_link_preview(self->view, self->message, &title,
                                                       &description)
               : GH_LINK_PREVIEW_NONE;
  const gchar *button = NULL;
  const gchar *text = NULL;
  switch (state) {
  case GH_LINK_PREVIEW_NONE:
  case GH_LINK_PREVIEW_ASKING:
    button = _("Show Preview");
    break;
  case GH_LINK_PREVIEW_LOADING:
    button = _("Loading Preview…");
    break;
  case GH_LINK_PREVIEW_FAILED:
    button = _("Show Preview");
    text = _("The preview couldn't be loaded.");
    break;
  case GH_LINK_PREVIEW_UNAVAILABLE:
    text = _("Link previews aren't available in this version of Groundhog. Nothing was loaded.");
    break;
  case GH_LINK_PREVIEW_LOADED:
    text = description;
    break;
  }
  gtk_widget_set_visible(GTK_WIDGET(self->preview_button), button != NULL);
  if (button)
    gtk_button_set_label(self->preview_button, button);
  gtk_widget_set_sensitive(GTK_WIDGET(self->preview_button),
                           state == GH_LINK_PREVIEW_NONE || state == GH_LINK_PREVIEW_FAILED);
  gboolean has_title = state == GH_LINK_PREVIEW_LOADED && title && *title;
  gtk_label_set_text(self->preview_title, has_title ? title : "");
  gtk_widget_set_visible(GTK_WIDGET(self->preview_title), has_title);
  gtk_label_set_text(self->preview_text, text ? text : "");
  gtk_widget_set_visible(GTK_WIDGET(self->preview_text), text && *text);
  set_class(GTK_WIDGET(self->preview_box), "card", state == GH_LINK_PREVIEW_LOADED);
}

static void
update_status(GhMessageRow *self)
{
  update_meta(self);
  update_summary(self);
}

static void
update_all(GhMessageRow *self)
{
  GhMessage *message = self->message;
  gboolean outgoing = message && gh_message_is_self(message);
  set_class(GTK_WIDGET(self->bubble), "outgoing", message && outgoing);
  set_class(GTK_WIDGET(self->bubble), "incoming", message && !outgoing);
  GtkAlign align = outgoing ? GTK_ALIGN_END : GTK_ALIGN_START;
  gtk_widget_set_halign(GTK_WIDGET(self->bubble), align);
  gtk_widget_set_halign(GTK_WIDGET(self->preview_box), align);
  gtk_widget_set_halign(GTK_WIDGET(self->meta_box), align);

  g_clear_pointer(&self->preview_uri, g_free);
  set_class(GTK_WIDGET(self->body_label), "groundhog-undecryptable", self->undecryptable);
  if (!message) {
    gtk_label_set_text(self->body_label, "");
    gtk_label_set_text(self->sender_label, "");
    gtk_label_set_text(self->time_label, "");
  } else {
    if (self->undecryptable) {
      gtk_label_set_text(self->body_label, _("Unable to decrypt yet"));
    } else {
      g_autofree gchar *markup = gh_link_policy_to_markup(gh_message_get_content(message));
      gtk_label_set_markup(self->body_label, markup);
      self->preview_uri = gh_link_policy_dup_preview_uri(gh_message_get_content(message));
    }
    g_autofree gchar *sender = gh_message_row_sender_name(message);
    gtk_label_set_text(self->sender_label, sender);
    gint64 created = gh_message_get_created_at(message);
    g_autofree gchar *time = gh_conversation_row_format_time_of_day(created);
    gtk_label_set_text(self->time_label, time);
    g_autoptr(GDateTime) when = g_date_time_new_from_unix_local(created);
    g_autofree gchar *full = when ? g_date_time_format(when, "%c") : NULL;
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->time_label), full);
    const gchar *rumor = gh_message_get_rumor_id(message);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->retry_button), "s", rumor);
    gtk_actionable_set_action_target(GTK_ACTIONABLE(self->preview_button), "s", rumor);
    if (self->preview_uri) {
      g_autofree gchar *host = gh_link_policy_dup_host(self->preview_uri);
      g_autofree gchar *tooltip = g_strdup_printf(_("Load a preview from %s"), host);
      gtk_widget_set_tooltip_text(GTK_WIDGET(self->preview_button), tooltip);
    }
  }
  gh_delivery_indicator_set_message(self->delivery, message);
  update_runs(self);
  update_expiry(self);
  update_meta(self);
  update_preview(self);
  update_summary(self);
}

/* ---- links and the enclosing view ------------------------------------------------ */

/* Every click on a link goes through the view's policy; GTK's default
 * handler, which would open any URI, never runs. */
static gboolean
on_activate_link(GhMessageRow *self, const gchar *uri)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "conversation.open-link", "s", uri);
  return TRUE;
}

static void
on_preview_changed(GhMessageRow *self, const gchar *rumor_id)
{
  if (self->message && g_strcmp0(gh_message_get_rumor_id(self->message), rumor_id) == 0)
    update_preview(self);
}

static void
gh_message_row_root(GtkWidget *widget)
{
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  GTK_WIDGET_CLASS(gh_message_row_parent_class)->root(widget);
  GtkWidget *view = gtk_widget_get_ancestor(widget, GH_TYPE_CONVERSATION_VIEW);
  if (!view)
    return;
  self->view = GH_CONVERSATION_VIEW(view);
  self->compact_binding = g_object_bind_property(view, "compact", self, "compact",
                                                 G_BINDING_SYNC_CREATE);
  g_signal_connect_object(view, "preview-changed", G_CALLBACK(on_preview_changed), self,
                          G_CONNECT_SWAPPED);
  update_preview(self);
}

static void
gh_message_row_unroot(GtkWidget *widget)
{
  GhMessageRow *self = GH_MESSAGE_ROW(widget);
  if (self->view) {
    g_signal_handlers_disconnect_by_func(self->view, on_preview_changed, self);
    g_clear_pointer(&self->compact_binding, g_binding_unbind);
    self->view = NULL;
  }
  GTK_WIDGET_CLASS(gh_message_row_parent_class)->unroot(widget);
}

/* ---- public ------------------------------------------------------------------- */

GtkWidget *
gh_message_row_new(void)
{
  return g_object_new(GH_TYPE_MESSAGE_ROW, NULL);
}

void
gh_message_row_set_message(GhMessageRow *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  g_return_if_fail(!message || GH_IS_MESSAGE(message));
  if (self->message == message)
    return;
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  g_set_object(&self->message, message);
  if (message) {
    g_signal_connect_object(message, "notify::status", G_CALLBACK(update_status), self,
                            G_CONNECT_SWAPPED);
    g_signal_connect_object(message, "notify::expires-at", G_CALLBACK(update_all), self,
                            G_CONNECT_SWAPPED);
  }
  update_all(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MESSAGE]);
}

GhMessage *
gh_message_row_get_message(GhMessageRow *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), NULL);
  return self->message;
}

static void
set_flag(GhMessageRow *self, gboolean *flag, gboolean value, guint prop)
{
  if (*flag == !!value)
    return;
  *flag = !!value;
  g_object_notify_by_pspec(G_OBJECT(self), props[prop]);
}

void
gh_message_row_set_run(GhMessageRow *self, gboolean run_start, gboolean run_end)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  set_flag(self, &self->run_start, run_start, PROP_RUN_START);
  set_flag(self, &self->run_end, run_end, PROP_RUN_END);
  update_runs(self);
  update_meta(self);
}

void
gh_message_row_set_show_sender(GhMessageRow *self, gboolean show_sender)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  set_flag(self, &self->show_sender, show_sender, PROP_SHOW_SENDER);
  update_runs(self);
}

void
gh_message_row_set_compact(GhMessageRow *self, gboolean compact)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  set_flag(self, &self->compact, compact, PROP_COMPACT);
  update_width(self);
}

void
gh_message_row_set_undecryptable(GhMessageRow *self, gboolean undecryptable)
{
  g_return_if_fail(GH_IS_MESSAGE_ROW(self));
  if (self->undecryptable == !!undecryptable)
    return;
  self->undecryptable = !!undecryptable;
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_UNDECRYPTABLE]);
  update_all(self);
}

const gchar *
gh_message_row_get_summary(GhMessageRow *self)
{
  g_return_val_if_fail(GH_IS_MESSAGE_ROW(self), NULL);
  return self->summary;
}

static void
gh_message_row_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  switch (id) {
  case PROP_MESSAGE:
    g_value_set_object(value, self->message);
    break;
  case PROP_RUN_START:
    g_value_set_boolean(value, self->run_start);
    break;
  case PROP_RUN_END:
    g_value_set_boolean(value, self->run_end);
    break;
  case PROP_SHOW_SENDER:
    g_value_set_boolean(value, self->show_sender);
    break;
  case PROP_COMPACT:
    g_value_set_boolean(value, self->compact);
    break;
  case PROP_UNDECRYPTABLE:
    g_value_set_boolean(value, self->undecryptable);
    break;
  case PROP_SUMMARY:
    g_value_set_string(value, self->summary);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_message_row_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  switch (id) {
  case PROP_MESSAGE:
    gh_message_row_set_message(self, g_value_get_object(value));
    break;
  case PROP_RUN_START:
    gh_message_row_set_run(self, g_value_get_boolean(value), self->run_end);
    break;
  case PROP_RUN_END:
    gh_message_row_set_run(self, self->run_start, g_value_get_boolean(value));
    break;
  case PROP_SHOW_SENDER:
    gh_message_row_set_show_sender(self, g_value_get_boolean(value));
    break;
  case PROP_COMPACT:
    gh_message_row_set_compact(self, g_value_get_boolean(value));
    break;
  case PROP_UNDECRYPTABLE:
    gh_message_row_set_undecryptable(self, g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_message_row_dispose(GObject *object)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  g_clear_object(&self->message);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_MESSAGE_ROW);
  G_OBJECT_CLASS(gh_message_row_parent_class)->dispose(object);
}

static void
gh_message_row_finalize(GObject *object)
{
  GhMessageRow *self = GH_MESSAGE_ROW(object);
  g_free(self->preview_uri);
  g_free(self->summary);
  G_OBJECT_CLASS(gh_message_row_parent_class)->finalize(object);
}

static void
gh_message_row_class_init(GhMessageRowClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_message_row_get_property;
  object_class->set_property = gh_message_row_set_property;
  object_class->dispose = gh_message_row_dispose;
  object_class->finalize = gh_message_row_finalize;
  widget_class->root = gh_message_row_root;
  widget_class->unroot = gh_message_row_unroot;

  const GParamFlags rw = G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS;
  props[PROP_MESSAGE] = g_param_spec_object("message", NULL, NULL, GH_TYPE_MESSAGE, rw);
  props[PROP_RUN_START] = g_param_spec_boolean("run-start", NULL, NULL, TRUE, rw);
  props[PROP_RUN_END] = g_param_spec_boolean("run-end", NULL, NULL, TRUE, rw);
  props[PROP_SHOW_SENDER] = g_param_spec_boolean("show-sender", NULL, NULL, FALSE, rw);
  props[PROP_COMPACT] = g_param_spec_boolean("compact", NULL, NULL, FALSE, rw);
  props[PROP_UNDECRYPTABLE] = g_param_spec_boolean("undecryptable", NULL, NULL, FALSE, rw);
  props[PROP_SUMMARY] = g_param_spec_string("summary", NULL, NULL, "",
    G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  g_type_ensure(GH_TYPE_DELIVERY_INDICATOR);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-message-row.ui");
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, sender_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, bubble);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, body_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_button);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_title);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, preview_text);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, meta_box);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, timer_icon);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, time_label);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, delivery);
  gtk_widget_class_bind_template_child(widget_class, GhMessageRow, retry_button);
  gtk_widget_class_bind_template_child_full(widget_class, "attachment_slot", FALSE, 0);
  gtk_widget_class_set_css_name(widget_class, "groundhog-message");
}

static void
gh_message_row_init(GhMessageRow *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  /* The row's actions take the message's rumor id: a row set up (or rooted
   * again, e.g. when the window collapses) before a message is bound names
   * none, so GTK never meets a target-less "s" action. */
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->retry_button), "s", "");
  gtk_actionable_set_action_target(GTK_ACTIONABLE(self->preview_button), "s", "");
  self->run_start = TRUE;
  self->run_end = TRUE;
  self->summary = g_strdup("");
  g_signal_connect_swapped(self->body_label, "activate-link", G_CALLBACK(on_activate_link),
                           self);
  update_width(self);
  update_all(self);
}
