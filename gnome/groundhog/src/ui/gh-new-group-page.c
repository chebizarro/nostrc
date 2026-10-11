#include "gh-new-group-page.h"

#include "gh-group-copy.h"
#include "gh-net-session.h"
#include "gh-shell.h"

#include <glib/gi18n.h>

struct _GhNewGroupPage {
  AdwNavigationPage parent_instance;
  AdwNavigationView *navigation;
  AdwEntryRow *relay_row;
  AdwEntryRow *name_row;
  AdwEntryRow *about_row;
  AdwSwitchRow *private_switch;
  AdwSwitchRow *closed_switch;
  GtkLabel *error_label;
  GtkImage *status_icon;
  GtkSpinner *spinner;
  GtkLabel *status_title;
  GtkLabel *status_description;
  GtkWidget *open_button;
  GtkWidget *back_button;
  AdwActionRow *contact_row;
  GtkWidget *form_back_button;
  GtkWidget *encrypted_choice;

  GhNip29Service *service; /* weak */
  AdwNavigationPage *encrypted_page; /* qp24.13 part 2: added, or NULL */
  GhNip29Room *room;
  gchar *relay_url;        /* the room's, kept after the service drops a refused one */
  gchar *announced;
};

enum { SIGNAL_OPEN_GROUP, SIGNAL_FINISHED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhNewGroupPage, gh_new_group_page, ADW_TYPE_NAVIGATION_PAGE)

static void
show_error(GhNewGroupPage *self, const gchar *message)
{
  gtk_label_set_text(self->error_label, message ? message : "");
  gtk_widget_set_visible(GTK_WIDGET(self->error_label), message != NULL);
  if (message)
    gtk_accessible_announce(GTK_ACCESSIBLE(self), message,
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_HIGH);
}

static void
sync_status(GhNewGroupPage *self)
{
  if (!self->room)
    return;
  GhNip29JoinState join = gh_nip29_room_get_join_state(self->room);
  g_autoptr(GhNip29Op) request = gh_nip29_room_dup_request_op(self->room);
  g_autofree gchar *host = gh_group_relay_host(self->relay_url);
  /* Created and pending until the relay's own 39000 arrives (§7.9). */
  const GhNip29Group *group = gh_nip29_room_get_group(self->room);
  gboolean has_state = group && gh_nip29_group_get_snapshot_id(group, 39000, NULL) != NULL;
  g_autoptr(GhGroupStateCopy) copy = gh_group_create_copy(join, request, has_state,
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
  gtk_widget_set_visible(self->back_button, copy->can_retry);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-group.open", copy->can_open);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-group.back", copy->can_retry);
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

static const gchar *
create_error(const GError *error)
{
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED))
    return _("Choose an account to create groups.");
  if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TOO_MANY_OPEN_FILES))
    return _("Groundhog follows at most 16 groups on one relay. Leave one there first.");
  return _("The new group couldn’t be saved on this device, so nothing was sent.");
}

static void
action_create(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(widget);
  if (!self->service)
    return;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *relay = gh_group_parse_relay(gtk_editable_get_text(
                                                   GTK_EDITABLE(self->relay_row)), &error);
  if (!relay || !gh_group_relay_reachable(relay, gh_group_network_is_tor(), &error)) {
    show_error(self, error->message);
    gtk_widget_grab_focus(GTK_WIDGET(self->relay_row));
    return;
  }
  g_autofree gchar *name = entry_text(self->name_row);
  if (!name) {
    show_error(self, _("Give the group a name."));
    gtk_widget_grab_focus(GTK_WIDGET(self->name_row));
    return;
  }
  g_autoptr(GhNip29Metadata) metadata = gh_nip29_metadata_new();
  metadata->name = g_steal_pointer(&name);
  metadata->about = entry_text(self->about_row);
  metadata->is_private = adw_switch_row_get_active(self->private_switch);
  metadata->is_closed = adw_switch_row_get_active(self->closed_switch);
  g_autoptr(GhNip29Room) room = gh_nip29_service_create_group(self->service, relay, NULL,
                                                              metadata, &error);
  if (!room) {
    g_message("Groundhog could not create a group: %s", error->message);
    show_error(self, create_error(error));
    return;
  }
  show_error(self, NULL);
  if (self->room)
    g_signal_handlers_disconnect_by_data(self->room, self);
  g_set_object(&self->room, room);
  g_free(self->relay_url);
  self->relay_url = g_strdup(gh_nip29_room_get_relay_url(room));
  g_clear_pointer(&self->announced, g_free);
  g_signal_connect_object(room, "notify", G_CALLBACK(sync_status), self, G_CONNECT_SWAPPED);
  g_signal_connect_object(room, "group-changed", G_CALLBACK(sync_status), self,
                          G_CONNECT_SWAPPED);
  sync_status(self);
  show_status_page(self->navigation);
}

static void
action_open(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(widget);
  if (!self->room)
    return;
  g_autoptr(GhNip29Room) room = g_object_ref(self->room);
  g_object_ref(self);
  g_signal_emit(self, signals[SIGNAL_OPEN_GROUP], 0, room);
  g_signal_emit(self, signals[SIGNAL_FINISHED], 0);
  g_object_unref(self);
}

/* The encrypted page opened its group: New Group is done. */
static void
on_encrypted_opened(GhNewGroupPage *self)
{
  g_signal_emit(self, signals[SIGNAL_FINISHED], 0);
}

static void
action_close(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  gh_new_group_page_close(GH_NEW_GROUP_PAGE(widget));
}

void
gh_new_group_page_close(GhNewGroupPage *self)
{
  g_return_if_fail(GH_IS_NEW_GROUP_PAGE(self));
  gh_sidebar_panel_leave(ADW_NAVIGATION_PAGE(self));
}

/* Escape: one step back inside New Group (status to form, form or the
 * encrypted page to the chooser), else out of it (to New Message). */
static gboolean
on_escape(GtkWidget *widget, GVariant *args, gpointer data)
{
  (void)args;
  (void)data;
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(widget);
  if (adw_navigation_view_get_previous_page(
        self->navigation, adw_navigation_view_get_visible_page(self->navigation)))
    adw_navigation_view_pop(self->navigation);
  else
    gh_new_group_page_close(self);
  return TRUE;
}

/* The first field of the inner page showing takes focus (charter §7.14):
 * the chooser's first row, or the relay. */
static void
focus_first(GhNewGroupPage *self)
{
  AdwNavigationPage *visible = adw_navigation_view_get_visible_page(self->navigation);
  const gchar *tag = visible ? adw_navigation_page_get_tag(visible) : NULL;
  if (g_strcmp0(tag, "type") == 0)
    gtk_widget_grab_focus(self->encrypted_choice);
  else if (g_strcmp0(tag, "form") == 0)
    gtk_widget_grab_focus(GTK_WIDGET(self->relay_row));
}

static void
on_inner_pushed(GhNewGroupPage *self)
{
  AdwNavigationPage *visible = adw_navigation_view_get_visible_page(self->navigation);
  if (g_strcmp0(adw_navigation_page_get_tag(visible), "form") == 0)
    gtk_widget_grab_focus(GTK_WIDGET(self->relay_row));
}

static void
gh_new_group_page_shown(AdwNavigationPage *page)
{
  focus_first(GH_NEW_GROUP_PAGE(page));
  if (ADW_NAVIGATION_PAGE_CLASS(gh_new_group_page_parent_class)->shown)
    ADW_NAVIGATION_PAGE_CLASS(gh_new_group_page_parent_class)->shown(page);
}

static void
action_back(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(widget);
  adw_navigation_view_pop_to_tag(self->navigation, "form");
}

static void
action_choose_relay(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(widget);
  adw_navigation_view_push_by_tag(self->navigation, "form");
}

static void
action_choose_encrypted(GtkWidget *widget, const gchar *action, GVariant *parameter)
{
  (void)action;
  (void)parameter;
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(widget);
  if (self->encrypted_page)
    adw_navigation_view_push(self->navigation, self->encrypted_page);
}

void
gh_new_group_page_add_encrypted_page(GhNewGroupPage *self, AdwNavigationPage *page)
{
  g_return_if_fail(GH_IS_NEW_GROUP_PAGE(self));
  g_return_if_fail(ADW_IS_NAVIGATION_PAGE(page));
  g_return_if_fail(self->encrypted_page == NULL);
  self->encrypted_page = page;
  adw_navigation_page_set_tag(page, "encrypted");
  adw_navigation_view_add(self->navigation, page);
  static const gchar *const root[] = { "type" };
  adw_navigation_view_replace_with_tags(self->navigation, root, 1);
  gtk_widget_set_visible(self->form_back_button, FALSE); /* the form is pushed now */
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-group.choose-encrypted", TRUE);
  if (g_signal_lookup("open-group", G_OBJECT_TYPE(page)))
    g_signal_connect_object(page, "open-group", G_CALLBACK(on_encrypted_opened), self,
                            G_CONNECT_SWAPPED | G_CONNECT_AFTER);
}

AdwNavigationPage *
gh_new_group_page_get_encrypted_page(GhNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_NEW_GROUP_PAGE(self), NULL);
  return self->encrypted_page;
}

static void
on_op_changed(GhNewGroupPage *self)
{
  sync_status(self);
}

static void
on_service_gone(gpointer data, GObject *where)
{
  GhNewGroupPage *self = data;
  (void)where;
  self->service = NULL;
  gh_new_group_page_close(self);
}

GhNewGroupPage *
gh_new_group_page_new(GhNip29Service *service)
{
  g_return_val_if_fail(GH_IS_NIP29_SERVICE(service), NULL);
  GhNewGroupPage *self = g_object_new(GH_TYPE_NEW_GROUP_PAGE, NULL);
  self->service = service;
  g_object_weak_ref(G_OBJECT(service), on_service_gone, self);
  g_signal_connect_object(gh_nip29_service_get_outbox(service), "op-changed",
                          G_CALLBACK(on_op_changed), self, G_CONNECT_SWAPPED);
  return self;
}

GhNip29Room *
gh_new_group_page_get_room(GhNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_NEW_GROUP_PAGE(self), NULL);
  return self->room;
}

const gchar *
gh_new_group_page_get_status_title(GhNewGroupPage *self)
{
  g_return_val_if_fail(GH_IS_NEW_GROUP_PAGE(self), NULL);
  return gtk_label_get_text(self->status_title);
}

static void
on_activated(GhNewGroupPage *self)
{
  gtk_widget_activate_action(GTK_WIDGET(self), "new-group.create", NULL);
}

static void
gh_new_group_page_dispose(GObject *object)
{
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(object);
  if (self->service) {
    g_object_weak_unref(G_OBJECT(self->service), on_service_gone, self);
    self->service = NULL;
  }
  if (self->room)
    g_signal_handlers_disconnect_by_data(self->room, self);
  g_clear_object(&self->room);
  /* navigation is both the page's child and a bound template child. */
  adw_navigation_page_set_child(ADW_NAVIGATION_PAGE(object), NULL);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_NEW_GROUP_PAGE);
  G_OBJECT_CLASS(gh_new_group_page_parent_class)->dispose(object);
}

static void
gh_new_group_page_finalize(GObject *object)
{
  GhNewGroupPage *self = GH_NEW_GROUP_PAGE(object);
  g_free(self->relay_url);
  g_free(self->announced);
  G_OBJECT_CLASS(gh_new_group_page_parent_class)->finalize(object);
}

static void
gh_new_group_page_class_init(GhNewGroupPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
  object_class->dispose = gh_new_group_page_dispose;
  object_class->finalize = gh_new_group_page_finalize;
  ADW_NAVIGATION_PAGE_CLASS(klass)->shown = gh_new_group_page_shown;
  signals[SIGNAL_OPEN_GROUP] = g_signal_new("open-group", G_TYPE_FROM_CLASS(klass),
                                            G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                            1, GH_TYPE_NIP29_ROOM);
  signals[SIGNAL_FINISHED] = g_signal_new("finished", G_TYPE_FROM_CLASS(klass),
                                          G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE,
                                          0);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-new-group-page.ui");
#define BIND(name) gtk_widget_class_bind_template_child(widget_class, GhNewGroupPage, name)
  BIND(navigation);
  BIND(relay_row);
  BIND(name_row);
  BIND(about_row);
  BIND(private_switch);
  BIND(closed_switch);
  BIND(error_label);
  BIND(status_icon);
  BIND(spinner);
  BIND(status_title);
  BIND(status_description);
  BIND(open_button);
  BIND(back_button);
  BIND(contact_row);
  BIND(form_back_button);
  BIND(encrypted_choice);
#undef BIND
  gtk_widget_class_install_action(widget_class, "new-group.create", NULL, action_create);
  gtk_widget_class_install_action(widget_class, "new-group.open", NULL, action_open);
  gtk_widget_class_install_action(widget_class, "new-group.back", NULL, action_back);
  gtk_widget_class_install_action(widget_class, "new-group.choose-relay", NULL,
                                  action_choose_relay);
  gtk_widget_class_install_action(widget_class, "new-group.choose-encrypted", NULL,
                                  action_choose_encrypted);
  gtk_widget_class_install_action(widget_class, "new-group.close", NULL, action_close);
  gtk_widget_class_add_binding(widget_class, GDK_KEY_Escape, 0, on_escape, NULL);
}

static void
sync_contact_copy(GhNewGroupPage *self)
{
  adw_action_row_set_subtitle(self->contact_row,
                              gh_group_contact_copy(TRUE, gh_group_network_is_tor()));
}

static void
gh_new_group_page_init(GhNewGroupPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  sync_contact_copy(self);
  g_autoptr(GhNetSession) network = gh_net_session_dup_default();
  if (network)
    g_signal_connect_object(network, "changed", G_CALLBACK(sync_contact_copy), self,
                            G_CONNECT_SWAPPED);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-group.open", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-group.back", FALSE);
  gtk_widget_action_set_enabled(GTK_WIDGET(self), "new-group.choose-encrypted", FALSE);
  /* Relay groups only until an encrypted page is added: no chooser, no
   * placeholder (charter §7.9). */
  static const gchar *const root[] = { "form" };
  adw_navigation_view_replace_with_tags(self->navigation, root, 1);
  g_signal_connect_swapped(self->relay_row, "entry-activated", G_CALLBACK(on_activated), self);
  g_signal_connect_swapped(self->name_row, "entry-activated", G_CALLBACK(on_activated), self);
  g_signal_connect_swapped(self->about_row, "entry-activated", G_CALLBACK(on_activated), self);
  g_signal_connect_swapped(self->navigation, "pushed", G_CALLBACK(on_inner_pushed), self);
}
