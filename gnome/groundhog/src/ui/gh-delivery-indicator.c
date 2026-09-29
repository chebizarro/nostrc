#include "gh-delivery-indicator.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-message-row.h"

#include <glib/gi18n.h>

/* ---- GhDeliveryReport ----------------------------------------------------------- */

static void
target_free(GhDeliveryTarget *target)
{
  g_free(target->recipient);
  g_free(target->relay_url);
  g_free(target->outcome);
  g_free(target);
}

GhDeliveryReport *
gh_delivery_report_new(void)
{
  GhDeliveryReport *report = g_new0(GhDeliveryReport, 1);
  report->targets = g_ptr_array_new_with_free_func((GDestroyNotify)target_free);
  return report;
}

void
gh_delivery_report_add(GhDeliveryReport *report, const gchar *recipient, const gchar *relay_url,
                       gboolean accepted, const gchar *outcome)
{
  g_return_if_fail(report != NULL);
  g_return_if_fail(relay_url != NULL || recipient != NULL);
  GhDeliveryTarget *target = g_new0(GhDeliveryTarget, 1);
  target->recipient = g_strdup(recipient);
  target->relay_url = g_strdup(relay_url);
  target->accepted = accepted;
  target->outcome = g_strdup(outcome);
  g_ptr_array_add(report->targets, target);
}

void
gh_delivery_report_free(GhDeliveryReport *report)
{
  if (!report)
    return;
  g_ptr_array_unref(report->targets);
  g_free(report->detail);
  g_free(report);
}

/* ---- GhDeliveryIndicator ------------------------------------------------------ */

struct _GhDeliveryIndicator {
  GtkWidget parent_instance;
  GtkMenuButton *button;
  GtkImage *status_icon;
  GtkLabel *status_label;
  GtkPopover *details;
  GtkLabel *details_title;
  GtkLabel *details_summary;
  GtkLabel *details_retry;
  GtkListBox *relay_list;
  GtkLabel *details_unavailable;
  GtkLabel *details_note;
  GhMessage *message;
  GhMessageStatus status;
};

enum { PROP_0, PROP_MESSAGE, PROP_STATUS, N_PROPS };
static GParamSpec *props[N_PROPS];

G_DEFINE_FINAL_TYPE(GhDeliveryIndicator, gh_delivery_indicator, GTK_TYPE_WIDGET)

const gchar *
gh_delivery_indicator_status_text(GhMessageStatus status)
{
  if (status == GH_MESSAGE_STATUS_NONE || status == GH_MESSAGE_STATUS_SENT)
    return NULL;
  return gh_message_status_get_label(status);
}

/* The style class of a status: problems stand out, progress stays quiet. */
static const gchar *
status_style(GhMessageStatus status)
{
  switch (status) {
  case GH_MESSAGE_STATUS_NOT_SENT:
  case GH_MESSAGE_STATUS_CANNOT_SEND_NO_INBOX:
    return "error";
  case GH_MESSAGE_STATUS_PARTIALLY_SENT:
  case GH_MESSAGE_STATUS_RETRYING:
    return "warning";
  default:
    return "dim-label";
  }
}

static void
set_style(GtkWidget *widget, const gchar *style)
{
  static const gchar *const styles[] = { "error", "warning", "dim-label" };
  for (guint i = 0; i < G_N_ELEMENTS(styles); i++)
    if (g_strcmp0(styles[i], style) != 0)
      gtk_widget_remove_css_class(widget, styles[i]);
  if (style)
    gtk_widget_add_css_class(widget, style);
}

/* "wss://relay.example/inbox" -> "relay.example"; the URL when unparsable. */
static gchar *
relay_host(const gchar *url)
{
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  return g_strdup(host && *host ? host : url);
}

static void
clear_relays(GhDeliveryIndicator *self)
{
  gtk_list_box_remove_all(self->relay_list);
}

/* A group header above the first relay of each receiver. */
static void
update_header(GtkListBoxRow *row, GtkListBoxRow *before, gpointer data)
{
  (void)data;
  const gchar *group = g_object_get_data(G_OBJECT(row), "groundhog-group");
  const gchar *previous = before ? g_object_get_data(G_OBJECT(before), "groundhog-group") : NULL;
  if (before && g_strcmp0(group, previous) == 0) {
    gtk_list_box_row_set_header(row, NULL);
    return;
  }
  GtkWidget *header = gtk_list_box_row_get_header(row);
  if (!GTK_IS_LABEL(header)) {
    header = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(header), 0);
    gtk_label_set_wrap(GTK_LABEL(header), TRUE);
    gtk_widget_add_css_class(header, "groundhog-delivery-group");
    gtk_widget_add_css_class(header, "caption-heading");
    gtk_list_box_row_set_header(row, header);
  }
  gtk_label_set_text(GTK_LABEL(header), g_object_get_data(G_OBJECT(row), "groundhog-header"));
}

static gchar *
group_title(const gchar *recipient, guint accepted, guint total)
{
  g_autofree gchar *who = recipient ? gh_message_row_display_name(recipient)
                                    : g_strdup(_("Your other devices"));
  if (total == 0)
    /* TRANSLATORS: a recipient the message could not be sent to at all,
     * e.g. "npub1abcde…wxyz · No message relays". */
    return g_strdup_printf(_("%s · No message relays"), who);
  g_autofree gchar *count = g_strdup_printf(
    g_dngettext(NULL, "%u of %u relay accepted", "%u of %u relays accepted", total), accepted,
    total);
  /* TRANSLATORS: a receiver and how many of its message relays accepted the
   * message, e.g. "npub1abcde…wxyz · 2 of 3 relays accepted". */
  return g_strdup_printf(_("%s · %s"), who, count);
}

static void
fill_relays(GhDeliveryIndicator *self, GhDeliveryReport *report)
{
  clear_relays(self);
  /* Per receiver ("" is the self-copy): accepted and total relays. */
  g_autoptr(GHashTable) accepted = g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(GHashTable) totals = g_hash_table_new(g_str_hash, g_str_equal);
  for (guint i = 0; i < report->targets->len; i++) {
    GhDeliveryTarget *target = g_ptr_array_index(report->targets, i);
    const gchar *key = target->recipient ? target->recipient : "";
    if (!target->relay_url)
      continue; /* a recipient without message relays: counts none */
    g_hash_table_insert(totals, (gpointer)key,
                        GUINT_TO_POINTER(GPOINTER_TO_UINT(g_hash_table_lookup(totals, key)) + 1));
    if (target->accepted)
      g_hash_table_insert(accepted, (gpointer)key, GUINT_TO_POINTER(GPOINTER_TO_UINT(
                            g_hash_table_lookup(accepted, key)) + 1));
  }
  for (guint i = 0; i < report->targets->len; i++) {
    GhDeliveryTarget *target = g_ptr_array_index(report->targets, i);
    const gchar *key = target->recipient ? target->recipient : "";
    GtkWidget *row = adw_action_row_new();
    g_autofree gchar *host = target->relay_url ? relay_host(target->relay_url)
                                               : g_strdup(_("Not sent to them"));
    /* Relay hosts and outcomes are shown as text, never as markup. */
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), host);
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row),
                                target->outcome ? target->outcome
                                                : (target->accepted ? _("Accepted by this relay.")
                                                                    : _("Not answered yet.")));
    if (target->relay_url)
      gtk_widget_set_tooltip_text(row, target->relay_url);
    g_object_set_data_full(G_OBJECT(row), "groundhog-group", g_strdup(key), g_free);
    g_object_set_data_full(G_OBJECT(row), "groundhog-header",
                           group_title(target->recipient,
                                       GPOINTER_TO_UINT(g_hash_table_lookup(accepted, key)),
                                       GPOINTER_TO_UINT(g_hash_table_lookup(totals, key))),
                           g_free);
    gtk_list_box_append(self->relay_list, row);
  }
}

/* The people an own message goes to (its "p" tags; a room: several). */
static guint
n_recipients(GhMessage *message)
{
  const gchar *const *recipients = message ? gh_message_get_recipients(message) : NULL;
  return recipients ? g_strv_length((gchar **) recipients) : 0;
}

static void
fill_details(GhDeliveryIndicator *self)
{
  GhMessageStatus status = self->status;
  if (status == GH_MESSAGE_STATUS_NONE)
    return;
  GtkWidget *view = gtk_widget_get_ancestor(GTK_WIDGET(self), GH_TYPE_CONVERSATION_VIEW);
  g_autoptr(GhDeliveryReport) report =
    view && self->message
      ? gh_conversation_view_dup_delivery_report(GH_CONVERSATION_VIEW(view), self->message)
      : NULL;
  gtk_label_set_text(self->details_title, gh_message_status_get_label(status));
  gtk_label_set_text(self->details_summary,
                     report && report->detail && *report->detail
                       ? report->detail
                       : gh_message_status_get_accessible_description_for(
                           status, n_recipients(self->message)));
  gboolean relays = report && report->targets->len > 0;
  if (relays)
    fill_relays(self, report);
  else
    clear_relays(self);
  gtk_widget_set_visible(GTK_WIDGET(self->relay_list), relays);
  gtk_widget_set_visible(GTK_WIDGET(self->details_unavailable), !relays);

  gboolean retry = report && report->next_attempt_at > 0;
  if (retry) {
    g_autoptr(GDateTime) now = g_date_time_new_now_local();
    g_autofree gchar *when =
      gh_conversation_row_format_message_time(report->next_attempt_at, now);
    g_autofree gchar *text = g_strdup_printf(_("Groundhog will try again at %s."), when);
    gtk_label_set_text(self->details_retry, text);
  }
  gtk_widget_set_visible(GTK_WIDGET(self->details_retry), retry);

  gboolean note = report && report->self_copy_missing;
  if (note)
    gtk_label_set_text(self->details_note, gh_message_status_get_self_copy_note());
  gtk_widget_set_visible(GTK_WIDGET(self->details_note), note);
}

static void
update(GhDeliveryIndicator *self)
{
  GhMessageStatus status = self->message && gh_message_is_self(self->message)
                             ? gh_message_get_status(self->message)
                             : GH_MESSAGE_STATUS_NONE;
  gboolean changed = status != self->status;
  self->status = status;
  gboolean shown = status != GH_MESSAGE_STATUS_NONE;
  gtk_widget_set_visible(GTK_WIDGET(self), shown);
  if (shown) {
    const gchar *label = gh_message_status_get_label(status);
    const gchar *text = gh_delivery_indicator_status_text(status);
    gtk_image_set_from_icon_name(self->status_icon, gh_message_status_get_icon_name(status));
    gtk_label_set_text(self->status_label, text ? text : "");
    gtk_widget_set_visible(GTK_WIDGET(self->status_label), text != NULL);
    set_style(GTK_WIDGET(self->status_icon), status_style(status));
    set_style(GTK_WIDGET(self->status_label), status_style(status));
    g_autofree gchar *tooltip = g_strdup_printf(_("%s · Delivery Details"), label);
    gtk_widget_set_tooltip_text(GTK_WIDGET(self->button), tooltip);
    gtk_accessible_update_property(GTK_ACCESSIBLE(self->button), GTK_ACCESSIBLE_PROPERTY_LABEL,
                                   label, GTK_ACCESSIBLE_PROPERTY_DESCRIPTION,
                                   gh_message_status_get_accessible_description_for(
                                     status, n_recipients(self->message)), -1);
  } else {
    gtk_popover_popdown(self->details);
  }
  if (shown && gtk_widget_get_visible(GTK_WIDGET(self->details)))
    fill_details(self);
  if (changed)
    g_object_notify_by_pspec(G_OBJECT(self), props[PROP_STATUS]);
}

static void
on_details_show(GhDeliveryIndicator *self)
{
  fill_details(self);
}

void
gh_delivery_indicator_set_message(GhDeliveryIndicator *self, GhMessage *message)
{
  g_return_if_fail(GH_IS_DELIVERY_INDICATOR(self));
  g_return_if_fail(!message || GH_IS_MESSAGE(message));
  if (self->message == message)
    return;
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  g_set_object(&self->message, message);
  if (message)
    g_signal_connect_object(message, "notify::status", G_CALLBACK(update), self,
                            G_CONNECT_SWAPPED);
  update(self);
  g_object_notify_by_pspec(G_OBJECT(self), props[PROP_MESSAGE]);
}

GhMessage *
gh_delivery_indicator_get_message(GhDeliveryIndicator *self)
{
  g_return_val_if_fail(GH_IS_DELIVERY_INDICATOR(self), NULL);
  return self->message;
}

GhMessageStatus
gh_delivery_indicator_get_status(GhDeliveryIndicator *self)
{
  g_return_val_if_fail(GH_IS_DELIVERY_INDICATOR(self), GH_MESSAGE_STATUS_NONE);
  return self->status;
}

void
gh_delivery_indicator_show_details(GhDeliveryIndicator *self)
{
  g_return_if_fail(GH_IS_DELIVERY_INDICATOR(self));
  if (self->status != GH_MESSAGE_STATUS_NONE)
    gtk_menu_button_popup(self->button);
}

void
gh_delivery_indicator_hide_details(GhDeliveryIndicator *self)
{
  g_return_if_fail(GH_IS_DELIVERY_INDICATOR(self));
  gtk_menu_button_popdown(self->button);
}

static void
gh_delivery_indicator_get_property(GObject *object, guint id, GValue *value,
                                   GParamSpec *pspec)
{
  GhDeliveryIndicator *self = GH_DELIVERY_INDICATOR(object);
  switch (id) {
  case PROP_MESSAGE:
    g_value_set_object(value, self->message);
    break;
  case PROP_STATUS:
    g_value_set_enum(value, self->status);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_delivery_indicator_set_property(GObject *object, guint id, const GValue *value,
                                   GParamSpec *pspec)
{
  switch (id) {
  case PROP_MESSAGE:
    gh_delivery_indicator_set_message(GH_DELIVERY_INDICATOR(object), g_value_get_object(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_delivery_indicator_dispose(GObject *object)
{
  GhDeliveryIndicator *self = GH_DELIVERY_INDICATOR(object);
  if (self->message)
    g_signal_handlers_disconnect_by_data(self->message, self);
  g_clear_object(&self->message);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_DELIVERY_INDICATOR);
  G_OBJECT_CLASS(gh_delivery_indicator_parent_class)->dispose(object);
}

static void
gh_delivery_indicator_class_init(GhDeliveryIndicatorClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_delivery_indicator_get_property;
  object_class->set_property = gh_delivery_indicator_set_property;
  object_class->dispose = gh_delivery_indicator_dispose;
  props[PROP_MESSAGE] = g_param_spec_object("message", NULL, NULL, GH_TYPE_MESSAGE,
    G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  props[PROP_STATUS] = g_param_spec_enum("status", NULL, NULL, GH_TYPE_MESSAGE_STATUS,
    GH_MESSAGE_STATUS_NONE, G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, N_PROPS, props);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-delivery-details.ui");
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, button);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, status_icon);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, status_label);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, details);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, details_title);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, details_summary);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, details_retry);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, relay_list);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, details_unavailable);
  gtk_widget_class_bind_template_child(widget_class, GhDeliveryIndicator, details_note);
  gtk_widget_class_bind_template_child_full(widget_class, "details_honesty", FALSE, 0);
  gtk_widget_class_set_css_name(widget_class, "groundhog-delivery");
}

static void
gh_delivery_indicator_init(GhDeliveryIndicator *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->status = GH_MESSAGE_STATUS_NONE;
  gtk_list_box_set_header_func(self->relay_list, update_header, NULL, NULL);
  g_signal_connect_swapped(self->details, "show", G_CALLBACK(on_details_show), self);
}
