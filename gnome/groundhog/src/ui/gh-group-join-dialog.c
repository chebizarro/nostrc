#include "gh-group-join-dialog.h"

#include "gh-group-copy.h"

#include <glib/gi18n.h>

struct _GhGroupJoinDialog {
  AdwDialog parent_instance;
  AdwNavigationView *navigation;
  AdwEntryRow *address_row;
  AdwEntryRow *code_row;
  GtkLabel *error_label;
  GtkImage *status_icon;
  GtkSpinner *spinner;
  GtkLabel *status_title;
  GtkLabel *status_description;
  GtkWidget *code_list;
  AdwEntryRow *retry_code_row;
  GtkWidget *open_button;
  GtkWidget *retry_button;

  GhNip29Service *service; /* weak */
  GhNip29Room *room;
  gboolean already_member; /* MEMBER before the user asked */
  gchar *announced;
};

enum { SIGNAL_OPEN_GROUP, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhGroupJoinDialog, gh_group_join_dialog, ADW_TYPE_DIALOG)

static void
show_error(GhGroupJoinDialog *self, const gchar *message)
{
  gtk_label_set_text(self->error_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), message != NULL);
  if (message)
    gtk_accessible_announce(GTK_ACCESSIBLE(self), message,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
}

static void
sync_status(GhGroupJoinDialog *self)
{
  if (!self->room)
    return;
  GhNip29JoinState join = gh_nip29_room_get_join_state(self->room);
  g_autoptr(GhNip29Op) request = gh_nip29_room_dup_request_op(self->room);
  g_autofree gchar *host = gh_group_relay_host(gh_nip29_room_get_relay_url(self->room));
  g_autoptr(GhGroupStateCopy) copy = gh_group_join_copy(join, request, self->already_member,
                                                        gh_nip29_room_get_detail(self->room),
                                                        host);
  gboolean progress = copy->tone == GH_GROUP_TONE_PROGRESS;
  gtk_image_set_from_icon_name(self->status_icon, copy->icon_name);
  gtk_widget_set_visible(GTK_WIDGET(self->status_icon), !progress);
  gtk_widget_set_visible(GTK_WIDGET(self->spinner), progress);
  gtk_spinner_set_spinning(self->spinner, progress);
  gtk_label_set_text(self->status_title, copy->title);
  gtk_label_set_text(self->status_description, copy->description);
  gtk_widget_set_visible(self->open_button, copy->can_open);
  gtk_widget_set_visible(self->code_list, copy->needs_code);
  gtk_widget_set_visible(self->retry_button, copy->can_retry || copy->needs_code);
  gtk_button_set_label(GTK_BUTTON(self->retry_button),
                       copy->needs_code ? _("_Ask Again with Code") : _("_Try Again"));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "join.open", copy->can_open);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "join.retry",
                                copy->can_retry || copy->needs_code);
  /* One announcement per state, not per property change. */
  if (g_strcmp0(self->announced, copy->title) != 0) {
    g_free(self->announced);
    self->announced = g_strdup(copy->title);
    g_autofree gchar *spoken = g_strdup_printf(_("%s. %s"), copy->title, copy->description);
    gtk_accessible_announce(GTK_ACCESSIBLE(self), spoken,
                            copy->tone == GH_GROUP_TONE_ERROR
                              ? GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH
                              : GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  }
}

static void
on_op_changed(GhGroupJoinDialog *self)
{
  sync_status(self); /* the signer's approval shows as it happens */
}

static void
watch_room(GhGroupJoinDialog *self, GhNip29Room *room)
{
  if (self->room == room)
    return;
  if (self->room)
    g_signal_handlers_disconnect_by_data(self->room, self);
  g_set_object(&self->room, room);
  g_signal_connect_object(room, "notify", G_CALLBACK(sync_status), self, G_CONNECT_SWAPPED);
}

static const gchar *
join_error(const GError *error)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    return _("Choose an account to join groups.");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES))
    return _("Groundhog follows at most 16 groups on one relay. Leave one there first.");
  if (error && error->domain == GH_NIP29_ERROR)
    return _("This group address isn’t valid.");
  return _("The join request couldn’t be saved on this device, so nothing was sent.");
}

/* Asks the service to join (the network is contacted from here on). */
static gboolean
ask(GhGroupJoinDialog *self, const gchar *relay, const gchar *group_id, const gchar *code)
{
  if (!self->service)
    return FALSE;
  g_autoptr(GhNip29Room) before = gh_nip29_service_lookup(self->service, relay, group_id);
  self->already_member = before &&
                         gh_nip29_room_get_join_state(before) == GH_NIP29_JOIN_MEMBER;
  g_autoptr(GError) error = NULL;
  g_autoptr(GhNip29Room) room = gh_nip29_service_join(self->service, relay, group_id, NULL, code,
                                                      &error);
  if (!room) {
    g_message("Groundhog could not ask to join a group: %s", error->message);
    show_error(self, join_error(error));
    return FALSE;
  }
  show_error(self, NULL);
  watch_room(self, room);
  g_clear_pointer(&self->announced, g_free);
  sync_status(self);
  return TRUE;
}

/* The status page, once (a second request from it stays there). */
static void
show_status_page(AdwNavigationView *navigation)
{
  AdwNavigationPage *visible = adw_navigation_view_get_visible_page(navigation);
  if (!visible || g_strcmp0(adw_navigation_page_get_tag(visible), "status") != 0)
    adw_navigation_view_push_by_tag(navigation, "status");
}

static gchar *
entry_text(AdwEntryRow *row)
{
  g_autofree gchar *text = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(row))));
  return *text ? g_steal_pointer(&text) : NULL;
}

static void
action_join(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupJoinDialog *self = GH_GROUP_JOIN_DIALOG(widget);
  g_autofree gchar *relay = NULL, *group_id = NULL, *link_code = NULL;
  g_autoptr(GError) error = NULL;
  if (!gh_group_parse_reference(gtk_editable_get_text(GTK_EDITABLE(self->address_row)), &relay,
                                &group_id, &link_code, &error)) {
    show_error(self, error->message);
    gtk_widget_grab_focus(GTK_WIDGET(self->address_row));
    return;
  }
  g_autofree gchar *typed = entry_text(self->code_row);
  const gchar *code = typed ? typed : link_code;
  if (ask(self, relay, group_id, code))
    show_status_page(self->navigation);
}

static void
action_retry(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupJoinDialog *self = GH_GROUP_JOIN_DIALOG(widget);
  if (!self->room)
    return;
  g_autofree gchar *code = gtk_widget_get_visible(self->code_list)
                             ? entry_text(self->retry_code_row) : NULL;
  g_autofree gchar *relay = g_strdup(gh_nip29_room_get_relay_url(self->room));
  g_autofree gchar *group_id = g_strdup(gh_nip29_room_get_group_id(self->room));
  /* The status page shows ask()'s reason when nothing could be sent. */
  if (!ask(self, relay, group_id, code))
    gtk_label_set_text(self->status_description, gtk_label_get_text(self->error_label));
}

static void
action_open(GtkWidget *widget, const gchar *name, GVariant *parameter)
{
  (void)name;
  (void)parameter;
  GhGroupJoinDialog *self = GH_GROUP_JOIN_DIALOG(widget);
  if (!self->room)
    return;
  g_autoptr(GhNip29Room) room = g_object_ref(self->room);
  adw_dialog_close(ADW_DIALOG(self));
  g_signal_emit(self, signals[SIGNAL_OPEN_GROUP], 0, room);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhGroupJoinDialog *self = data;
  (void)where;
  self->service = NULL;
  adw_dialog_force_close(ADW_DIALOG(self));
}

GhGroupJoinDialog *
gh_group_join_dialog_new(GhNip29Service *service)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(service), NULL);
  GhGroupJoinDialog *self = g_object_new(GH_TYPE_GROUP_JOIN_DIALOG, NULL);
  self->service = service;
  g_object_weak_ref(G_OBJECT(service), on_service_gone, self);
  g_signal_connect_object(gh_nip29_service_get_outbox(service), "op-changed",
                          G_CALLBACK(on_op_changed), self, G_CONNECT_SWAPPED);
  return self;
}

void
gh_group_join_dialog_set_address(GhGroupJoinDialog *self, const gchar *address)
{
  g_return_if_fail(GH_IS_GROUP_JOIN_DIALOG(self));
  gtk_editable_set_text(GTK_EDITABLE(self->address_row), address ? address : "");
}

GhNip29Room *
gh_group_join_dialog_get_room(GhGroupJoinDialog *self)
{
  g_return_val_if_fail(GH_IS_GROUP_JOIN_DIALOG(self), NULL);
  return self->room;
}

const gchar *
gh_group_join_dialog_get_status_title(GhGroupJoinDialog *self)
{
  g_return_val_if_fail(GH_IS_GROUP_JOIN_DIALOG(self), NULL);
  return gtk_label_get_text(self->status_title);
}

static void
on_address_activated(GhGroupJoinDialog *self)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "join.join", NULL);
}

static void
gh_group_join_dialog_dispose(GObject *object)
{
  GhGroupJoinDialog *self = GH_GROUP_JOIN_DIALOG(object);
  if (self->service) {
    g_object_weak_unref(G_OBJECT(self->service), on_service_gone, self);
    self->service = NULL;
  }
  if (self->room)
    g_signal_handlers_disconnect_by_data(self->room, self);
  g_clear_object(&self->room);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_GROUP_JOIN_DIALOG);
  G_OBJECT_CLASS(gh_group_join_dialog_parent_class)->dispose(object);
}

static void
gh_group_join_dialog_finalize(GObject *object)
{
  g_free(GH_GROUP_JOIN_DIALOG(object)->announced);
  G_OBJECT_CLASS(gh_group_join_dialog_parent_class)->finalize(object);
}

static void
gh_group_join_dialog_class_init(GhGroupJoinDialogClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_group_join_dialog_dispose;
  object_class->finalize = gh_group_join_dialog_finalize;
  signals[SIGNAL_OPEN_GROUP] = g_signal_new("open-group", G_TYPE_FROM_CLASS(klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                            1, GH_TYPE_NIP29_ROOM);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-group-join-dialog.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhGroupJoinDialog, name)
  BIND(navigation);
  BIND(address_row);
  BIND(code_row);
  BIND(error_label);
  BIND(status_icon);
  BIND(spinner);
  BIND(status_title);
  BIND(status_description);
  BIND(code_list);
  BIND(retry_code_row);
  BIND(open_button);
  BIND(retry_button);
#undef BIND
  gtk_widget_class_install_action(widget_class, "join.join", NULL, action_join);
  gtk_widget_class_install_action(widget_class, "join.open", NULL, action_open);
  gtk_widget_class_install_action(widget_class, "join.retry", NULL, action_retry);
}

static void
gh_group_join_dialog_init(GhGroupJoinDialog *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "join.open", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "join.retry", FALSE);
  g_signal_connect_swapped(self->address_row, "entry-activated",
                           G_CALLBACK(on_address_activated), self);
  g_signal_connect_swapped(self->code_row, "entry-activated", G_CALLBACK(on_address_activated),
                           self);
}
