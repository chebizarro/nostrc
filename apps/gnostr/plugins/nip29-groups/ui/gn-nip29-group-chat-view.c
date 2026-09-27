/* SPDX-License-Identifier: GPL-3.0-or-later
 * gn-nip29-group-chat-view.c - Chat view for a NIP-29 group
 */

#include "gn-nip29-group-chat-view.h"
#include "gn-nip29-message-row.h"
#include "gn-nip29-composer.h"
#include "../model/gn-nip29-message-list-model.h"
#include "../model/gn-nip29-message-item.h"
#include <gnostr-plugin-api.h>
#include <json-glib/json-glib.h>
#include <nip29.h>

struct _GnNip29GroupChatView
{
  GtkBox parent_instance;

  /* Dependencies */
  GnNip29GroupService     *service;
  GnNip29GroupItem        *group_item;
  GnostrPluginContext     *plugin_context;

  /* Header widgets */
  GtkLabel                *name_label;
  GtkLabel                *meta_label;
  GtkButton               *join_button;
  GtkButton               *leave_button;
  GtkLabel                *status_label;

  /* Message area */
  GtkListView             *message_list;
  GtkScrolledWindow       *scroll;
  GtkWidget               *empty_box;
  GtkStack                *msg_stack;

  /* Composer */
  GnNip29Composer         *composer;
  GCancellable            *action_cancellable;
  gboolean                 action_busy;

  /* Model */
  GnNip29MessageListModel *msg_model;
  gulong                   sig_items_changed;

  /* nostrc-prjb: pinned strip, subgroup navigation, live rows (weak) */
  GtkWidget               *pinned_box;
  GtkWidget               *subgroup_box;
  GPtrArray               *rows;

  /* nostrc-7n4t: "this group may have moved or been forked" */
  AdwBanner               *relocation_banner;
  gchar                   *relocation_relay;
  gulong                   sig_group_updated;
};

G_DEFINE_TYPE(GnNip29GroupChatView, gn_nip29_group_chat_view, GTK_TYPE_BOX)

/* ── Factory callbacks ───────────────────────────────────────────── */

static void on_row_pin_toggled(GnNip29MessageRow *row, const char *id, gboolean pin,
                               gpointer user_data);
static void apply_row_pin_state(GnNip29GroupChatView *self, GnNip29MessageRow *row);
static void on_row_finalized(gpointer data, GObject *where_the_object_was);

static void
on_msg_factory_setup(GtkSignalListItemFactory *factory,
                     GtkListItem              *list_item,
                     gpointer                  user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  GnNip29MessageRow *row = gn_nip29_message_row_new();
  /* nostrc-prjb: admins pin/unpin from the row. */
  g_signal_connect(row, "pin-toggled", G_CALLBACK(on_row_pin_toggled), self);
  g_ptr_array_add(self->rows, row);
  g_object_weak_ref(G_OBJECT(row), on_row_finalized, self);
  gtk_list_item_set_child(list_item, GTK_WIDGET(row));
}

static void
on_msg_factory_bind(GtkSignalListItemFactory *factory,
                    GtkListItem              *list_item,
                    gpointer                  user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  GnNip29MessageRow *row = GN_NIP29_MESSAGE_ROW(gtk_list_item_get_child(list_item));
  GnNip29MessageItem *item = gtk_list_item_get_item(list_item);

  const char *user_pk = gn_nip29_group_service_get_current_pubkey(self->service);
  gn_nip29_message_row_bind(row, item, user_pk, self->plugin_context);
  g_object_set_data_full(G_OBJECT(row), "gn-nip29-item", g_object_ref(item), g_object_unref);
  apply_row_pin_state(self, row);
}

static void
on_msg_factory_unbind(GtkSignalListItemFactory *factory,
                      GtkListItem              *list_item,
                      gpointer                  user_data)
{
  GnNip29MessageRow *row = GN_NIP29_MESSAGE_ROW(gtk_list_item_get_child(list_item));
  g_object_set_data(G_OBJECT(row), "gn-nip29-item", NULL);
  gn_nip29_message_row_unbind(row);
}

/* ── Auto-scroll on new messages ─────────────────────────────────── */

static void
on_messages_changed(GListModel *model,
                    guint       position,
                    guint       removed,
                    guint       added,
                    gpointer    user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  guint n = g_list_model_get_n_items(model);

  /* Toggle empty/list */
  if (n == 0)
    gtk_stack_set_visible_child(self->msg_stack, self->empty_box);
  else
    gtk_stack_set_visible_child(self->msg_stack, GTK_WIDGET(self->scroll));

  /* Auto-scroll to bottom when new messages arrive */
  if (added > 0 && n > 0)
    {
      GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(self->scroll);
      gdouble upper = gtk_adjustment_get_upper(adj);
      gtk_adjustment_set_value(adj, upper);
    }
}

/* ── User-authored action hooks ─────────────────────────────────── */

static void
set_action_status(GnNip29GroupChatView *self,
                  const char           *message,
                  gboolean              is_error)
{
  gtk_label_set_text(self->status_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->status_label),
                         message != NULL && message[0] != '\0');
  if (is_error)
    gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "error");
  else
    gtk_widget_remove_css_class(GTK_WIDGET(self->status_label), "error");
}

static void
set_action_busy(GnNip29GroupChatView *self,
                gboolean              busy)
{
  self->action_busy = busy;
  gtk_widget_set_sensitive(GTK_WIDGET(self->join_button), !busy);
  gtk_widget_set_sensitive(GTK_WIDGET(self->leave_button), !busy);
  gn_nip29_composer_set_send_sensitive(self->composer, !busy);
}

static const char *
current_group_key(GnNip29GroupChatView *self)
{
  return self->group_item ? gn_nip29_group_item_get_key(self->group_item) : NULL;
}

static void
on_send_done(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  (void)source;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_send_message_finish(self->service,
                                                           result,
                                                           &error);
  g_clear_object(&self->action_cancellable);
  set_action_busy(self, FALSE);

  if (ok)
    {
      gn_nip29_composer_clear(self->composer);
      set_action_status(self, "Message published. Refreshing relay state…", FALSE);
    }
  else
    {
      set_action_status(self, error ? error->message : "Failed to send message", TRUE);
    }

  g_object_unref(self);
}

static void
on_join_done(GObject      *source,
             GAsyncResult *result,
             gpointer      user_data)
{
  (void)source;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_join_group_finish(self->service,
                                                         result,
                                                         &error);
  g_clear_object(&self->action_cancellable);
  set_action_busy(self, FALSE);
  set_action_status(self,
                    ok ? "Join request published. Pending relay state refresh…"
                       : (error ? error->message : "Failed to join group"),
                    !ok);
  g_object_unref(self);
}

static void
on_leave_done(GObject      *source,
              GAsyncResult *result,
              gpointer      user_data)
{
  (void)source;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);

  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_leave_group_finish(self->service,
                                                          result,
                                                          &error);
  g_clear_object(&self->action_cancellable);
  set_action_busy(self, FALSE);
  set_action_status(self,
                    ok ? "Leave request published. Pending relay state refresh…"
                       : (error ? error->message : "Failed to leave group"),
                    !ok);
  g_object_unref(self);
}

static void
start_action_cancellable(GnNip29GroupChatView *self,
                         const char           *status)
{
  g_clear_object(&self->action_cancellable);
  self->action_cancellable = g_cancellable_new();
  set_action_busy(self, TRUE);
  set_action_status(self, status, FALSE);
}

static void
on_send_requested(GnNip29Composer *composer,
                  const char      *text,
                  gpointer         user_data)
{
  (void)composer;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->action_busy)
    return;

  start_action_cancellable(self, "Signing and publishing message…");
  gn_nip29_group_service_send_message_async(self->service,
                                            current_group_key(self),
                                            text,
                                            self->action_cancellable,
                                            on_send_done,
                                            g_object_ref(self));
}

static void
on_join_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->action_busy)
    return;

  start_action_cancellable(self, "Signing and publishing join request…");
  gn_nip29_group_service_join_group_async(self->service,
                                          current_group_key(self),
                                          NULL,
                                          NULL,
                                          self->action_cancellable,
                                          on_join_done,
                                          g_object_ref(self));
}

static void
on_leave_clicked(GtkButton *button, gpointer user_data)
{
  (void)button;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->action_busy)
    return;

  start_action_cancellable(self, "Signing and publishing leave request…");
  gn_nip29_group_service_leave_group_async(self->service,
                                           current_group_key(self),
                                           NULL,
                                           self->action_cancellable,
                                           on_leave_done,
                                           g_object_ref(self));
}

/* ── Pins and subgroups (nostrc-prjb) ────────────────────────────── */

static const nostr_group_t *
current_group(GnNip29GroupChatView *self)
{
  return gn_nip29_group_service_get_group_data(self->service, current_group_key(self));
}

static gboolean
current_user_is_admin(GnNip29GroupChatView *self)
{
  const nostr_group_t *group = current_group(self);
  const char *me = gn_nip29_group_service_get_current_pubkey(self->service);
  return group != NULL && group->admins_loaded && me != NULL && *me != '\0' &&
         nostr_group_get_admin((nostr_group_t *)group, me) != NULL;
}

static gboolean
group_pins_contain(const nostr_group_t *group, const char *id)
{
  for (size_t i = 0; group != NULL && id != NULL && i < group->pins_len; i++)
    if (group->pins[i].type == NOSTR_GROUP_PIN_EVENT &&
        g_ascii_strcasecmp(group->pins[i].value, id) == 0)
      return TRUE;
  return FALSE;
}

static void
apply_row_pin_state(GnNip29GroupChatView *self, GnNip29MessageRow *row)
{
  GnNip29MessageItem *item = g_object_get_data(G_OBJECT(row), "gn-nip29-item");
  const char *id = item ? gn_nip29_message_item_get_id(item) : NULL;
  gn_nip29_message_row_set_pin_state(row, id != NULL && current_user_is_admin(self),
                                     group_pins_contain(current_group(self), id));
}

static void
on_set_pins_done(GObject *source, GAsyncResult *result, gpointer user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  g_autoptr(GError) error = NULL;
  gboolean ok = gn_nip29_group_service_set_pins_finish(GN_NIP29_GROUP_SERVICE(source),
                                                       result, &error);
  if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) && self->service != NULL)
    set_action_status(self, ok ? "Pinned list updated." :
                      (error ? error->message : "Could not update the pinned list"), !ok);
  g_object_unref(self);
}

/* The whole ordered list goes out (kind:9010): current pins, with @id
 * appended or removed. */
static void
request_pin(GnNip29GroupChatView *self, const char *id, gboolean pin)
{
  const nostr_group_t *group = current_group(self);
  GPtrArray *refs = g_ptr_array_new();
  for (size_t i = 0; group != NULL && i < group->pins_len; i++)
    {
      const char *value = group->pins[i].value;
      if (!pin && group->pins[i].type == NOSTR_GROUP_PIN_EVENT &&
          g_ascii_strcasecmp(value, id) == 0)
        continue;
      g_ptr_array_add(refs, (gpointer)value);
    }
  if (pin && !group_pins_contain(group, id))
    g_ptr_array_add(refs, (gpointer)id);
  g_ptr_array_add(refs, NULL);
  set_action_status(self, pin ? "Pinning…" : "Unpinning…", FALSE);
  gn_nip29_group_service_set_pins_async(self->service, current_group_key(self),
                                        (const char * const *)refs->pdata, NULL,
                                        on_set_pins_done, g_object_ref(self));
  g_ptr_array_unref(refs);
}

static void
on_row_pin_toggled(GnNip29MessageRow *row, const char *id, gboolean pin, gpointer user_data)
{
  (void)row;
  request_pin(GN_NIP29_GROUP_CHAT_VIEW(user_data), id, pin);
}

static void
on_strip_unpin_clicked(GtkButton *button, gpointer user_data)
{
  const char *id = g_object_get_data(G_OBJECT(button), "gn-nip29-pin");
  if (id != NULL)
    request_pin(GN_NIP29_GROUP_CHAT_VIEW(user_data), id, FALSE);
}

/* Content of a loaded message, for the pinned strip. */
static gchar *
loaded_message_snippet(GnNip29GroupChatView *self, const char *id)
{
  const char *key = current_group_key(self);
  guint n = gn_nip29_group_service_get_message_count_for_key(self->service, key);
  for (guint i = 0; i < n; i++)
    {
      GnNip29MessageRef ref = { 0 };
      if (!gn_nip29_group_service_get_message_at(self->service, key, i, &ref) ||
          g_ascii_strcasecmp(ref.id, id) != 0 || ref.event_json == NULL)
        continue;
      g_autoptr(JsonParser) parser = json_parser_new();
      if (!json_parser_load_from_data(parser, ref.event_json, -1, NULL))
        return NULL;
      JsonNode *root = json_parser_get_root(parser);
      JsonObject *obj = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
      const char *content = obj && json_object_has_member(obj, "content")
                              ? json_object_get_string_member(obj, "content") : NULL;
      if (content == NULL)
        return NULL;
      g_autofree gchar *flat = g_strdelimit(g_strdup(content), "\n\r\t", ' ');
      return g_utf8_strlen(flat, -1) > 90 ? g_strdup_printf("%.*s…", 
               (int)(g_utf8_offset_to_pointer(flat, 90) - flat), flat)
                                          : g_steal_pointer(&flat);
    }
  return NULL;
}

static void
clear_box(GtkWidget *box)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(box)) != NULL)
    gtk_box_remove(GTK_BOX(box), child);
}

/* NIP-29 lets clients show the kind:39005 events at the top of the group. */
static void
rebuild_pinned_strip(GnNip29GroupChatView *self)
{
  clear_box(self->pinned_box);
  const nostr_group_t *group = current_group(self);
  if (group == NULL || group->pins_len == 0)
    {
      gtk_widget_set_visible(self->pinned_box, FALSE);
      return;
    }
  gboolean admin = current_user_is_admin(self);
  GtkWidget *heading = gtk_label_new("Pinned");
  gtk_label_set_xalign(GTK_LABEL(heading), 0);
  gtk_widget_add_css_class(heading, "caption-heading");
  gtk_box_append(GTK_BOX(self->pinned_box), heading);
  for (size_t i = 0; i < group->pins_len && i < 5; i++)
    {
      const nostr_group_pin_t *pin = &group->pins[i];
      g_autofree gchar *text = pin->type == NOSTR_GROUP_PIN_EVENT
        ? loaded_message_snippet(self, pin->value) : NULL;
      if (text == NULL)
        text = pin->type == NOSTR_GROUP_PIN_EVENT
          ? g_strdup_printf("Event %.12s…", pin->value)
          : g_strdup_printf("Address %s", pin->value);
      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      GtkWidget *label = gtk_label_new(text);
      gtk_label_set_xalign(GTK_LABEL(label), 0);
      gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
      gtk_widget_set_hexpand(label, TRUE);
      gtk_box_append(GTK_BOX(row), label);
      if (admin && pin->type == NOSTR_GROUP_PIN_EVENT)
        {
          GtkWidget *unpin = gtk_button_new_from_icon_name("window-close-symbolic");
          gtk_widget_add_css_class(unpin, "flat");
          gtk_widget_add_css_class(unpin, "circular");
          gtk_widget_set_tooltip_text(unpin, "Unpin from the group");
          g_object_set_data_full(G_OBJECT(unpin), "gn-nip29-pin", g_strdup(pin->value), g_free);
          g_signal_connect(unpin, "clicked", G_CALLBACK(on_strip_unpin_clicked), self);
          gtk_box_append(GTK_BOX(row), unpin);
        }
      gtk_box_append(GTK_BOX(self->pinned_box), row);
    }
  if (group->pins_len > 5)
    {
      g_autofree gchar *more = g_strdup_printf("and %zu more", group->pins_len - 5);
      GtkWidget *label = gtk_label_new(more);
      gtk_label_set_xalign(GTK_LABEL(label), 0);
      gtk_widget_add_css_class(label, "dim-label");
      gtk_box_append(GTK_BOX(self->pinned_box), label);
    }
  gtk_widget_set_visible(self->pinned_box, TRUE);
}

static void
on_subgroup_clicked(GtkButton *button, gpointer user_data)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  const char *id = g_object_get_data(G_OBJECT(button), "gn-nip29-group-id");
  const char *relay = gn_nip29_group_item_get_relay_url(self->group_item);
  g_autoptr(GError) error = NULL;
  if (id != NULL && gn_nip29_group_service_track_group(self->service, relay, id, NULL, &error))
    {
      g_autofree gchar *msg = g_strdup_printf("Added “%s” to your groups.", id);
      set_action_status(self, msg, FALSE);
    }
  else if (error != NULL)
    set_action_status(self, error->message, TRUE);
}

/* Name of a group on the same relay if it is tracked, else its id. */
static const char *
sibling_display_name(GnNip29GroupChatView *self, const char *id)
{
  g_autofree gchar *key = g_strdup_printf("%s'%s",
                                          gn_nip29_group_item_get_relay_url(self->group_item), id);
  const nostr_group_t *g = gn_nip29_group_service_get_group_data(self->service, key);
  return g != NULL && g->name != NULL && *g->name ? g->name : id;
}

static void
add_subgroup_button(GnNip29GroupChatView *self, const char *label_prefix, const char *id)
{
  g_autofree gchar *label = g_strdup_printf("%s%s", label_prefix,
                                            sibling_display_name(self, id));
  GtkWidget *button = gtk_button_new_with_label(label);
  gtk_widget_add_css_class(button, "pill");
  gtk_widget_add_css_class(button, "small");
  gtk_widget_set_tooltip_text(button, "Add this group on the same relay to your groups");
  g_object_set_data_full(G_OBJECT(button), "gn-nip29-group-id", g_strdup(id), g_free);
  g_signal_connect(button, "clicked", G_CALLBACK(on_subgroup_clicked), self);
  gtk_flow_box_append(GTK_FLOW_BOX(self->subgroup_box), button);
}

/* Subgroups are scoped to one relay: the parent and children named in this
 * group's kind:39000 are groups on the same relay. */
static void
rebuild_subgroups(GnNip29GroupChatView *self)
{
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(self->subgroup_box)) != NULL)
    gtk_flow_box_remove(GTK_FLOW_BOX(self->subgroup_box), child);
  const nostr_group_t *group = current_group(self);
  gboolean any = FALSE;
  if (group != NULL && group->parent != NULL && *group->parent)
    {
      add_subgroup_button(self, "↑ ", group->parent);
      any = TRUE;
    }
  for (size_t i = 0; group != NULL && i < group->children_len && i < 12; i++)
    {
      add_subgroup_button(self, "↳ ", group->children[i]);
      any = TRUE;
    }
  gtk_widget_set_visible(self->subgroup_box, any);
}

static void
refresh_group_extras(GnNip29GroupChatView *self)
{
  rebuild_pinned_strip(self);
  rebuild_subgroups(self);
  for (guint i = 0; i < self->rows->len; i++)
    apply_row_pin_state(self, g_ptr_array_index(self->rows, i));
}

static void
on_row_finalized(gpointer data, GObject *where_the_object_was)
{
  GnNip29GroupChatView *self = data;
  g_ptr_array_remove_fast(self->rows, where_the_object_was);
}

/* ── Migration / fork notice (nostrc-7n4t) ──────────────────────── */

static void
update_relocation_banner(GnNip29GroupChatView *self)
{
  const char *relay = NULL;
  guint authors = 0;
  GnNip29RelocationState st =
    gn_nip29_group_service_get_relocation(self->service, current_group_key(self),
                                          &relay, &authors);
  g_clear_pointer(&self->relocation_relay, g_free);
  g_autofree gchar *title = NULL;
  const char *button = NULL;
  switch (st)
    {
    case GN_NIP29_RELOCATION_FOUND:
      self->relocation_relay = g_strdup(relay);
      title = g_strdup_printf(g_dngettext(NULL,
          "This group may have moved or been forked: %u trusted list names it on %s.",
          "This group may have moved or been forked: %u trusted lists name it on %s.",
          authors), authors, relay);
      button = "Open There";
      break;
    case GN_NIP29_RELOCATION_CHECKING:
      title = g_strdup("The group’s relay is not answering. Looking for it on other relays…");
      break;
    case GN_NIP29_RELOCATION_UNREACHABLE:
      title = g_strdup("The group’s relay is not answering, and none of its admins list the "
                       "group on another relay.");
      break;
    case GN_NIP29_RELOCATION_NONE:
    default:
      break;
    }
  if (title != NULL)
    {
      adw_banner_set_title(self->relocation_banner, title);
      adw_banner_set_button_label(self->relocation_banner, button);
    }
  adw_banner_set_revealed(self->relocation_banner, title != NULL);
}

static void
on_service_group_updated(GnNip29GroupService *service,
                         const char          *group_key,
                         gpointer             user_data)
{
  (void)service;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (g_strcmp0(group_key, current_group_key(self)) == 0)
    {
      update_relocation_banner(self);
      refresh_group_extras(self);
    }
}

/* Fetch the group from the relay its admins now name. A fork is a separate
 * group (keyed relay'id), so this adds it next to the current one. */
static void
on_relocation_open_clicked(AdwBanner *banner, gpointer user_data)
{
  (void)banner;
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(user_data);
  if (self->relocation_relay == NULL || self->group_item == NULL)
    return;
  const char *alias = gn_nip29_group_item_get_display_name(self->group_item);
  g_autoptr(GError) error = NULL;
  if (gn_nip29_group_service_track_group(self->service, self->relocation_relay,
                                         gn_nip29_group_item_get_group_id(self->group_item),
                                         alias, &error))
    {
      g_autofree gchar *msg = g_strdup_printf("Added the group on %s to your groups.",
                                              self->relocation_relay);
      set_action_status(self, msg, FALSE);
    }
  else
    set_action_status(self, error ? error->message : "Could not add the group", TRUE);
}

/* ── GObject lifecycle ───────────────────────────────────────────── */

static void
gn_nip29_group_chat_view_dispose(GObject *object)
{
  GnNip29GroupChatView *self = GN_NIP29_GROUP_CHAT_VIEW(object);

  if (self->service != NULL && self->sig_group_updated > 0)
    g_signal_handler_disconnect(self->service, self->sig_group_updated);
  self->sig_group_updated = 0;
  /* Rows may outlive us briefly: stop them from calling back. */
  for (guint i = 0; self->rows != NULL && i < self->rows->len; i++)
    {
      GObject *row = g_ptr_array_index(self->rows, i);
      g_object_weak_unref(row, on_row_finalized, self);
      g_signal_handlers_disconnect_by_data(row, self);
    }
  g_clear_pointer(&self->rows, g_ptr_array_unref);
  g_clear_pointer(&self->relocation_relay, g_free);

  if (self->msg_model != NULL && self->sig_items_changed > 0)
    {
      g_signal_handler_disconnect(self->msg_model, self->sig_items_changed);
      self->sig_items_changed = 0;
    }

  if (self->action_cancellable != NULL)
    g_cancellable_cancel(self->action_cancellable);
  g_clear_object(&self->action_cancellable);
  g_clear_object(&self->service);
  g_clear_object(&self->group_item);
  g_clear_object(&self->msg_model);
  self->plugin_context = NULL;

  G_OBJECT_CLASS(gn_nip29_group_chat_view_parent_class)->dispose(object);
}

static void
gn_nip29_group_chat_view_class_init(GnNip29GroupChatViewClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gn_nip29_group_chat_view_dispose;
}

static void
gn_nip29_group_chat_view_init(GnNip29GroupChatView *self)
{
  gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);
  self->rows = g_ptr_array_new();
  gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
  gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
}

/* ── Metadata header helper ──────────────────────────────────────── */

static gchar *
build_meta_text(GnNip29GroupItem *item)
{
  GString *s = g_string_new(NULL);

  const char *relay = gn_nip29_group_item_get_relay_url(item);
  if (relay != NULL)
    g_string_append_printf(s, "%s", relay);

  if (gn_nip29_group_item_get_is_private(item))
    g_string_append(s, " · private");
  if (gn_nip29_group_item_get_is_closed(item))
    g_string_append(s, " · closed");
  if (gn_nip29_group_item_get_is_hidden(item))
    g_string_append(s, " · hidden");
  if (gn_nip29_group_item_get_is_restricted(item))
    g_string_append(s, " · restricted");

  if (gn_nip29_group_item_get_members_loaded(item))
    {
      guint mc = gn_nip29_group_item_get_member_count(item);
      gboolean partial = gn_nip29_group_item_get_members_may_be_partial(item);
      g_string_append_printf(s, " · %u member%s%s",
                             mc, mc == 1 ? "" : "s",
                             partial ? "+" : "");
    }
  else
    {
      g_string_append(s, " · members unknown");
    }

  if (!gn_nip29_group_item_get_admins_loaded(item))
    g_string_append(s, " · admin state unknown");

  return g_string_free(s, FALSE);
}

/* ── Public API ──────────────────────────────────────────────────── */

GnNip29GroupChatView *
gn_nip29_group_chat_view_new(GnNip29GroupService *service,
                              GnNip29GroupItem    *group_item,
                              GnostrPluginContext *plugin_context)
{
  g_return_val_if_fail(GN_IS_NIP29_GROUP_SERVICE(service), NULL);
  g_return_val_if_fail(GN_IS_NIP29_GROUP_ITEM(group_item), NULL);

  GnNip29GroupChatView *self = g_object_new(GN_TYPE_NIP29_GROUP_CHAT_VIEW, NULL);
  self->service = g_object_ref(service);
  self->group_item = g_object_ref(group_item);
  self->plugin_context = plugin_context;

  const char *group_key = gn_nip29_group_item_get_key(group_item);
  const char *display = gn_nip29_group_item_get_display_name(group_item);

  /* ── Migration / fork notice (nostrc-7n4t) ─────────────────────── */
  self->relocation_banner = ADW_BANNER(adw_banner_new(""));
  g_signal_connect(self->relocation_banner, "button-clicked",
                   G_CALLBACK(on_relocation_open_clicked), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->relocation_banner));

  /* ── Header bar ────────────────────────────────────────────────── */
  GtkWidget *chat_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(chat_header, 12);
  gtk_widget_set_margin_end(chat_header, 12);
  gtk_widget_set_margin_top(chat_header, 8);
  gtk_widget_set_margin_bottom(chat_header, 8);

  self->name_label = GTK_LABEL(gtk_label_new(
    (display && *display) ? display : "Group Chat"));
  gtk_widget_add_css_class(GTK_WIDGET(self->name_label), "heading");
  gtk_label_set_ellipsize(self->name_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(self->name_label, 0);
  gtk_box_append(GTK_BOX(chat_header), GTK_WIDGET(self->name_label));

  g_autofree gchar *meta = build_meta_text(group_item);
  self->meta_label = GTK_LABEL(gtk_label_new(meta));
  gtk_widget_add_css_class(GTK_WIDGET(self->meta_label), "dim-label");
  gtk_widget_add_css_class(GTK_WIDGET(self->meta_label), "caption");
  gtk_label_set_ellipsize(self->meta_label, PANGO_ELLIPSIZE_END);
  gtk_label_set_xalign(self->meta_label, 0);
  gtk_box_append(GTK_BOX(chat_header), GTK_WIDGET(self->meta_label));

  GtkWidget *action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  gtk_widget_set_margin_top(action_row, 6);

  self->join_button = GTK_BUTTON(gtk_button_new_with_label("Join"));
  gtk_widget_add_css_class(GTK_WIDGET(self->join_button), "pill");
  g_signal_connect(self->join_button, "clicked",
                   G_CALLBACK(on_join_clicked), self);
  gtk_box_append(GTK_BOX(action_row), GTK_WIDGET(self->join_button));

  self->leave_button = GTK_BUTTON(gtk_button_new_with_label("Leave"));
  gtk_widget_add_css_class(GTK_WIDGET(self->leave_button), "pill");
  g_signal_connect(self->leave_button, "clicked",
                   G_CALLBACK(on_leave_clicked), self);
  gtk_box_append(GTK_BOX(action_row), GTK_WIDGET(self->leave_button));

  self->status_label = GTK_LABEL(gtk_label_new(NULL));
  gtk_label_set_xalign(self->status_label, 0);
  gtk_label_set_wrap(self->status_label, TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "caption");
  gtk_widget_add_css_class(GTK_WIDGET(self->status_label), "dim-label");
  gtk_widget_set_hexpand(GTK_WIDGET(self->status_label), TRUE);
  gtk_widget_set_visible(GTK_WIDGET(self->status_label), FALSE);
  gtk_box_append(GTK_BOX(action_row), GTK_WIDGET(self->status_label));

  gtk_box_append(GTK_BOX(chat_header), action_row);

  /* nostrc-prjb: parent / child groups on the same relay. */
  self->subgroup_box = gtk_flow_box_new();
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(self->subgroup_box), GTK_SELECTION_NONE);
  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(self->subgroup_box), 6);
  gtk_widget_set_margin_top(self->subgroup_box, 4);
  gtk_widget_set_visible(self->subgroup_box, FALSE);
  gtk_box_append(GTK_BOX(chat_header), self->subgroup_box);

  gtk_box_append(GTK_BOX(self), chat_header);
  gtk_box_append(GTK_BOX(self), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  /* nostrc-prjb: pinned events (kind:39005) at the top. */
  self->pinned_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_margin_start(self->pinned_box, 12);
  gtk_widget_set_margin_end(self->pinned_box, 12);
  gtk_widget_set_margin_top(self->pinned_box, 6);
  gtk_widget_set_margin_bottom(self->pinned_box, 6);
  gtk_widget_add_css_class(self->pinned_box, "card");
  gtk_widget_set_visible(self->pinned_box, FALSE);
  gtk_box_append(GTK_BOX(self), self->pinned_box);

  /* ── Message model ─────────────────────────────────────────────── */
  self->msg_model = gn_nip29_message_list_model_new(service, group_key);

  /* ── Message list view ─────────────────────────────────────────── */
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  g_signal_connect(factory, "setup", G_CALLBACK(on_msg_factory_setup), self);
  g_signal_connect(factory, "bind", G_CALLBACK(on_msg_factory_bind), self);
  g_signal_connect(factory, "unbind", G_CALLBACK(on_msg_factory_unbind), NULL);

  GtkNoSelection *no_sel =
    gtk_no_selection_new(G_LIST_MODEL(g_object_ref(self->msg_model)));
  self->message_list = GTK_LIST_VIEW(
    gtk_list_view_new(GTK_SELECTION_MODEL(no_sel), factory));
  gtk_widget_add_css_class(GTK_WIDGET(self->message_list), "navigation-sidebar");

  self->scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
  gtk_scrolled_window_set_policy(self->scroll,
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child(self->scroll, GTK_WIDGET(self->message_list));
  gtk_widget_set_vexpand(GTK_WIDGET(self->scroll), TRUE);

  /* Empty state */
  self->empty_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
  gtk_widget_set_valign(self->empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_halign(self->empty_box, GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(self->empty_box, TRUE);

  GtkWidget *empty_icon = gtk_image_new_from_icon_name("chat-bubble-text-symbolic");
  gtk_image_set_pixel_size(GTK_IMAGE(empty_icon), 48);
  gtk_widget_add_css_class(empty_icon, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_box), empty_icon);

  GtkWidget *empty_lbl = gtk_label_new("No messages yet");
  gtk_widget_add_css_class(empty_lbl, "dim-label");
  gtk_box_append(GTK_BOX(self->empty_box), empty_lbl);

  /* Stack for empty vs message list */
  self->msg_stack = GTK_STACK(gtk_stack_new());
  gtk_widget_set_vexpand(GTK_WIDGET(self->msg_stack), TRUE);
  gtk_stack_add_named(self->msg_stack, self->empty_box, "empty");
  gtk_stack_add_named(self->msg_stack, GTK_WIDGET(self->scroll), "messages");

  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->msg_model));
  if (n == 0)
    gtk_stack_set_visible_child(self->msg_stack, self->empty_box);
  else
    gtk_stack_set_visible_child(self->msg_stack, GTK_WIDGET(self->scroll));

  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->msg_stack));

  /* ── Separator + Composer ──────────────────────────────────────── */
  gtk_box_append(GTK_BOX(self), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

  self->composer = gn_nip29_composer_new();
  g_signal_connect(self->composer, "send-requested",
                   G_CALLBACK(on_send_requested), self);
  gtk_box_append(GTK_BOX(self), GTK_WIDGET(self->composer));

  /* Listen for model changes */
  self->sig_group_updated = g_signal_connect(self->service, "group-updated",
                                             G_CALLBACK(on_service_group_updated), self);
  update_relocation_banner(self);
  refresh_group_extras(self);

  self->sig_items_changed = g_signal_connect(self->msg_model, "items-changed",
                                             G_CALLBACK(on_messages_changed), self);

  return self;
}
