#include "gh-shell.h"

#include <glib/gi18n.h>

/* ---- GhSidebarPage ---------------------------------------------------------- */

struct _GhSidebarPage {
  AdwNavigationPage parent_instance;
  AdwHeaderBar *header;
  AdwWindowTitle *window_title;
  GtkButton *back_button;
  GtkToggleButton *search_button;
  GtkButton *new_button;
  GtkSearchBar *search_bar;
  GtkSearchEntry *search_entry;
  AdwBanner *status_banner;
  GtkStack *stack;
  GtkButton *requests_button;
  GtkLabel *requests_count;
  GtkListView *list;
  AdwStatusPage *error_page;
  AdwStatusPage *onboarding_unavailable; /* not in the stack unless added */

  GtkSingleSelection *selection; /* over the visible list's model */
  GListModel *conversations;
  GListModel *requests;
  GhStatus *status;
  gchar *search_text;            /* trimmed; "" when not searching */
  gchar *account_page;           /* NULL: the conversation pages */
  gchar *error_description;      /* the template's default */
  gchar *title;                  /* the template's window title */
  GhStatusBanner banner;         /* last shown */
  gboolean show_requests;
  gboolean show_previews;
};

enum {
  SIDEBAR_PROP_0,
  SIDEBAR_PROP_SELECTED,
  SIDEBAR_PROP_SEARCH_TEXT,
  SIDEBAR_PROP_SHOW_REQUESTS,
  SIDEBAR_PROP_SHOW_PREVIEWS,
  SIDEBAR_N_PROPS
};
static GParamSpec *sidebar_props[SIDEBAR_N_PROPS];

G_DEFINE_FINAL_TYPE(GhSidebarPage, gh_sidebar_page, ADW_TYPE_NAVIGATION_PAGE)

static guint
model_n_items(GListModel *model)
{
  return model ? g_list_model_get_n_items(model) : 0;
}

/* The conversation page to show when no account page is set. */
static const gchar *
conversation_page(GhSidebarPage *self)
{
  guint conversations = model_n_items(self->conversations);
  guint requests = model_n_items(self->requests);
  gboolean listed = self->show_requests ? requests > 0 : conversations + requests > 0;
  if (listed)
    return "conversations";
  if (*self->search_text)
    return "no-results";
  if (self->status && gh_status_get_inbox(self->status) == GH_STATUS_INBOX_ERROR)
    return "error";
  return "empty";
}

static void
update_page(GhSidebarPage *self)
{
  guint requests = model_n_items(self->requests);
  /* Nothing left to list in Message Requests (accepted, or filtered away
   * by a search that has since been cleared): return to conversations. */
  if (self->show_requests && requests == 0 && !*self->search_text) {
    gh_sidebar_page_set_show_requests(self, FALSE);
    return;
  }
  gtk_widget_set_visible(GTK_WIDGET(self->requests_button),
                         !self->show_requests && requests > 0);
  g_autofree gchar *count = g_strdup_printf("%u", requests);
  gtk_label_set_text(self->requests_count, count);
  g_autofree gchar *label = g_strdup_printf(
    g_dngettext(NULL, "Message Requests, %u conversation", "Message Requests, %u conversations",
                requests), requests);
  gtk_accessible_update_property(GTK_ACCESSIBLE(self->requests_button),
                                 GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  gtk_stack_set_visible_child_name(self->stack,
                                   self->account_page ? self->account_page
                                                      : conversation_page(self));
}

static void
update_error_page(GhSidebarPage *self)
{
  const gchar *error = self->status ? gh_status_get_inbox_error(self->status) : NULL;
  adw_status_page_set_description(self->error_page, error ? error : self->error_description);
}

static void
update_banner(GhSidebarPage *self)
{
  GhStatusBanner banner = self->status ? gh_status_get_banner(self->status)
                                       : GH_STATUS_BANNER_NONE;
  if (banner != GH_STATUS_BANNER_NONE) {
    adw_banner_set_title(self->status_banner, gh_status_banner_get_title(banner));
    adw_banner_set_button_label(self->status_banner, gh_status_banner_get_button_label(banner));
    gtk_actionable_set_action_name(GTK_ACTIONABLE(self->status_banner),
                                   gh_status_banner_get_action(banner));
  }
  /* The last title stays while the banner hides, so it does not blank out
   * mid-animation. */
  adw_banner_set_revealed(self->status_banner, banner != GH_STATUS_BANNER_NONE);
  if (banner != self->banner && gh_status_banner_is_problem(banner))
    gtk_accessible_announce(GTK_ACCESSIBLE(self), gh_status_banner_get_title(banner),
                            GTK_ACCESSIBLE_ANNOUNCEMENT_PRIORITY_MEDIUM);
  self->banner = banner;
}

static void
on_status_banner(GhSidebarPage *self)
{
  update_banner(self);
}

static void
on_status_inbox(GhSidebarPage *self)
{
  update_error_page(self);
  update_page(self);
}

static void
on_models_changed(GhSidebarPage *self)
{
  update_page(self);
}

static void
on_selected_item(GhSidebarPage *self)
{
  g_object_notify_by_pspec(G_OBJECT(self), sidebar_props[SIDEBAR_PROP_SELECTED]);
}

static void
on_search_changed(GhSidebarPage *self)
{
  g_autofree gchar *text = g_strstrip(g_strdup(gtk_editable_get_text(
    GTK_EDITABLE(self->search_entry))));
  if (g_strcmp0(text, self->search_text) == 0)
    return;
  g_free(self->search_text);
  self->search_text = g_steal_pointer(&text);
  g_object_notify_by_pspec(G_OBJECT(self), sidebar_props[SIDEBAR_PROP_SEARCH_TEXT]);
  update_page(self);
}

static void
show_requests_action(GtkWidget *widget, const char *action_name, GVariant *parameter)
{
  (void)action_name;
  gh_sidebar_page_set_show_requests(GH_SIDEBAR_PAGE(widget), g_variant_get_boolean(parameter));
}

/* Escape leaves Message Requests; otherwise it propagates (search bar,
 * collapsed navigation). */
static gboolean
leave_requests(GtkWidget *widget, GVariant *args, gpointer data)
{
  GhSidebarPage *self = GH_SIDEBAR_PAGE(widget);
  (void)args;
  (void)data;
  if (!self->show_requests)
    return FALSE;
  gh_sidebar_page_set_show_requests(self, FALSE);
  return TRUE;
}

static void
gh_sidebar_page_get_property(GObject *object, guint id, GValue *value, GParamSpec *pspec)
{
  GhSidebarPage *self = GH_SIDEBAR_PAGE(object);
  switch (id) {
  case SIDEBAR_PROP_SELECTED:
    g_value_set_object(value, gh_sidebar_page_get_selected(self));
    break;
  case SIDEBAR_PROP_SEARCH_TEXT:
    g_value_set_string(value, self->search_text);
    break;
  case SIDEBAR_PROP_SHOW_REQUESTS:
    g_value_set_boolean(value, self->show_requests);
    break;
  case SIDEBAR_PROP_SHOW_PREVIEWS:
    g_value_set_boolean(value, self->show_previews);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_sidebar_page_set_property(GObject *object, guint id, const GValue *value, GParamSpec *pspec)
{
  GhSidebarPage *self = GH_SIDEBAR_PAGE(object);
  switch (id) {
  case SIDEBAR_PROP_SHOW_REQUESTS:
    gh_sidebar_page_set_show_requests(self, g_value_get_boolean(value));
    break;
  case SIDEBAR_PROP_SHOW_PREVIEWS:
    gh_sidebar_page_set_show_previews(self, g_value_get_boolean(value));
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, pspec);
  }
}

static void
gh_sidebar_page_dispose(GObject *object)
{
  GhSidebarPage *self = GH_SIDEBAR_PAGE(object);
  if (self->list)
    gtk_list_view_set_model(self->list, NULL);
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_SIDEBAR_PAGE);
  if (self->selection)
    g_signal_handlers_disconnect_by_data(self->selection, self);
  g_clear_object(&self->selection);
  g_clear_object(&self->conversations);
  g_clear_object(&self->requests);
  g_clear_object(&self->status);
  G_OBJECT_CLASS(gh_sidebar_page_parent_class)->dispose(object);
}

static void
gh_sidebar_page_finalize(GObject *object)
{
  GhSidebarPage *self = GH_SIDEBAR_PAGE(object);
  g_free(self->search_text);
  g_free(self->account_page);
  g_free(self->error_description);
  g_free(self->title);
  G_OBJECT_CLASS(gh_sidebar_page_parent_class)->finalize(object);
}

static void
gh_sidebar_page_class_init(GhSidebarPageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  object_class->get_property = gh_sidebar_page_get_property;
  object_class->set_property = gh_sidebar_page_set_property;
  object_class->dispose = gh_sidebar_page_dispose;
  object_class->finalize = gh_sidebar_page_finalize;
  sidebar_props[SIDEBAR_PROP_SELECTED] = g_param_spec_object("selected", NULL, NULL,
    G_TYPE_OBJECT, G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  sidebar_props[SIDEBAR_PROP_SEARCH_TEXT] = g_param_spec_string("search-text", NULL, NULL,
    "", G_PARAM_READABLE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  sidebar_props[SIDEBAR_PROP_SHOW_REQUESTS] = g_param_spec_boolean("show-requests", NULL,
    NULL, FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  sidebar_props[SIDEBAR_PROP_SHOW_PREVIEWS] = g_param_spec_boolean("show-previews", NULL,
    NULL, FALSE, G_PARAM_READWRITE | G_PARAM_EXPLICIT_NOTIFY | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties(object_class, SIDEBAR_N_PROPS, sidebar_props);

  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-sidebar-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, header);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, window_title);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, back_button);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, search_button);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, search_bar);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, search_entry);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, status_banner);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, stack);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, requests_button);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, requests_count);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, list);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, error_page);
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, onboarding_unavailable);
  /* Bound so tests and callers can reach them by name
   * (gtk_widget_get_template_child); no field is needed here. */
  gtk_widget_class_bind_template_child(widget_class, GhSidebarPage, new_button);
  gtk_widget_class_bind_template_child_full(widget_class, "primary_button", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "empty_page", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "no_results_page", FALSE, 0);

  gtk_widget_class_install_action(widget_class, "sidebar.show-requests", "b",
                                  show_requests_action);
  gtk_widget_class_add_binding(widget_class, GDK_KEY_Escape, 0, leave_requests, NULL);
}

static void
gh_sidebar_page_init(GhSidebarPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->search_text = g_strdup("");
  self->error_description = g_strdup(adw_status_page_get_description(self->error_page));
  self->title = g_strdup(adw_window_title_get_title(self->window_title));
  self->selection = gtk_single_selection_new(NULL);
  gtk_single_selection_set_autoselect(self->selection, FALSE);
  gtk_single_selection_set_can_unselect(self->selection, TRUE);
  g_signal_connect_swapped(self->selection, "notify::selected-item",
                           G_CALLBACK(on_selected_item), self);
  gtk_list_view_set_model(self->list, GTK_SELECTION_MODEL(self->selection));
  gtk_search_bar_connect_entry(self->search_bar, GTK_EDITABLE(self->search_entry));
  g_signal_connect_swapped(self->search_entry, "search-changed",
                           G_CALLBACK(on_search_changed), self);
  update_page(self);
}

AdwHeaderBar *
gh_sidebar_page_get_header(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->header;
}

AdwWindowTitle *
gh_sidebar_page_get_window_title(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->window_title;
}

GtkStack *
gh_sidebar_page_get_stack(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->stack;
}

GtkListView *
gh_sidebar_page_get_list(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->list;
}

AdwBanner *
gh_sidebar_page_get_banner(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->status_banner;
}

void
gh_sidebar_page_show_onboarding(GhSidebarPage *self)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  g_return_if_fail(gtk_stack_get_child_by_name(self->stack, "onboarding") == NULL);

  gtk_stack_add_named(self->stack, GTK_WIDGET(self->onboarding_unavailable), "onboarding");
  gh_sidebar_page_set_account_page(self, "onboarding");
}

void
gh_sidebar_page_set_account_page(GhSidebarPage *self, const gchar *name)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  g_return_if_fail(!name || gtk_stack_get_child_by_name(self->stack, name));
  if (g_strcmp0(self->account_page, name) != 0) {
    g_free(self->account_page);
    self->account_page = g_strdup(name);
  }
  update_page(self);
}

void
gh_sidebar_page_set_status(GhSidebarPage *self, GhStatus *status)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  g_return_if_fail(GH_IS_STATUS(status));
  g_return_if_fail(self->status == NULL);
  self->status = g_object_ref(status);
  g_signal_connect_object(status, "notify::banner", G_CALLBACK(on_status_banner), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(status, "notify::inbox", G_CALLBACK(on_status_inbox), self,
                          G_CONNECT_SWAPPED);
  update_banner(self);
  on_status_inbox(self);
}

void
gh_sidebar_page_set_models(GhSidebarPage *self, GListModel *conversations,
                           GListModel *requests)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  g_return_if_fail(G_IS_LIST_MODEL(conversations));
  g_return_if_fail(G_IS_LIST_MODEL(requests));
  g_return_if_fail(self->conversations == NULL);
  self->conversations = g_object_ref(conversations);
  self->requests = g_object_ref(requests);
  /* After the selection's own handler: update_page() may swap the
   * selection's model (the last request left), which is only safe once the
   * selection has seen the change. */
  g_signal_connect_object(conversations, "items-changed", G_CALLBACK(on_models_changed), self,
                          G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  g_signal_connect_object(requests, "items-changed", G_CALLBACK(on_models_changed), self,
                          G_CONNECT_SWAPPED | G_CONNECT_AFTER);
  gtk_single_selection_set_model(self->selection,
                                 self->show_requests ? requests : conversations);
  update_page(self);
}

gpointer
gh_sidebar_page_get_selected(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return gtk_single_selection_get_selected_item(self->selection);
}

void
gh_sidebar_page_unselect(GhSidebarPage *self)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  gtk_single_selection_set_selected(self->selection, GTK_INVALID_LIST_POSITION);
}

gboolean
gh_sidebar_page_select_relative(GhSidebarPage *self, gint delta)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), FALSE);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->selection));
  guint current = gtk_single_selection_get_selected(self->selection);
  guint next;
  if (n == 0 || delta == 0 || self->account_page)
    return FALSE;
  if (current == GTK_INVALID_LIST_POSITION)
    next = delta > 0 ? 0 : n - 1;
  else if (delta < 0)
    next = current > 0 ? current - 1 : GTK_INVALID_LIST_POSITION;
  else
    next = current + 1 < n ? current + 1 : GTK_INVALID_LIST_POSITION;
  if (next == GTK_INVALID_LIST_POSITION)
    return FALSE;
  gboolean focus_in_list =
    (gtk_widget_get_state_flags(GTK_WIDGET(self->list)) & GTK_STATE_FLAG_FOCUS_WITHIN) != 0;
  gtk_single_selection_set_selected(self->selection, next);
  gtk_list_view_scroll_to(self->list, next,
                          focus_in_list ? GTK_LIST_SCROLL_FOCUS : GTK_LIST_SCROLL_NONE, NULL);
  return TRUE;
}

static gboolean
find_item(GListModel *model, gpointer item, guint *position)
{
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GObject) candidate = g_list_model_get_item(model, i);
    if (candidate == item) {
      *position = i;
      return TRUE;
    }
  }
  return FALSE;
}

gboolean
gh_sidebar_page_select_item(GhSidebarPage *self, gpointer item)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), FALSE);
  g_return_val_if_fail(G_IS_OBJECT(item), FALSE);
  if (!self->conversations || self->account_page)
    return FALSE;
  guint position = 0;
  gboolean request = FALSE;
  gboolean found = find_item(self->conversations, item, &position) ||
                   (request = find_item(self->requests, item, &position));
  if (!found && *self->search_text) {
    /* A search hides it. Clearing the entry reports "" at once. */
    gtk_editable_set_text(GTK_EDITABLE(self->search_entry), "");
    gtk_search_bar_set_search_mode(self->search_bar, FALSE);
    found = find_item(self->conversations, item, &position) ||
            (request = find_item(self->requests, item, &position));
  }
  if (!found)
    return FALSE;
  gh_sidebar_page_set_show_requests(self, request);
  gtk_single_selection_set_selected(self->selection, position);
  gtk_list_view_scroll_to(self->list, position, GTK_LIST_SCROLL_NONE, NULL);
  return TRUE;
}

void
gh_sidebar_page_start_search(GhSidebarPage *self)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  gtk_search_bar_set_search_mode(self->search_bar, TRUE);
  gtk_widget_grab_focus(GTK_WIDGET(self->search_entry));
}

void
gh_sidebar_page_set_key_capture_widget(GhSidebarPage *self, GtkWidget *widget)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  gtk_search_bar_set_key_capture_widget(self->search_bar, widget);
}

const gchar *
gh_sidebar_page_get_search_text(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  return self->search_text;
}

gboolean
gh_sidebar_page_get_show_requests(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), FALSE);
  return self->show_requests;
}

void
gh_sidebar_page_set_show_requests(GhSidebarPage *self, gboolean show_requests)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  show_requests = !!show_requests;
  if (self->show_requests == show_requests)
    return;
  /* Sampled before the button that was used hides. */
  gboolean focus_within =
    (gtk_widget_get_state_flags(GTK_WIDGET(self)) & GTK_STATE_FLAG_FOCUS_WITHIN) != 0;
  self->show_requests = show_requests;
  /* The other list's items are never this one's: switching unselects. */
  gtk_single_selection_set_selected(self->selection, GTK_INVALID_LIST_POSITION);
  if (self->conversations)
    gtk_single_selection_set_model(self->selection,
                                   show_requests ? self->requests : self->conversations);
  /* At 360 px the header also holds the account, search and menu buttons
   * and the window controls: in Message Requests the back button replaces
   * New Message, and the title is short. */
  gtk_widget_set_visible(GTK_WIDGET(self->back_button), show_requests);
  gtk_widget_set_visible(GTK_WIDGET(self->new_button), !show_requests);
  adw_window_title_set_title(self->window_title, show_requests ? _("Requests") : self->title);
  g_object_notify_by_pspec(G_OBJECT(self), sidebar_props[SIDEBAR_PROP_SHOW_REQUESTS]);
  update_page(self);
  /* Keep keyboard focus in the sidebar (the button that was used is gone),
   * but never pull it in from elsewhere. */
  if (focus_within)
    gtk_widget_grab_focus(show_requests ? GTK_WIDGET(self->list)
                                        : GTK_WIDGET(self->search_button));
}

gboolean
gh_sidebar_page_get_show_previews(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), FALSE);
  return self->show_previews;
}

void
gh_sidebar_page_set_show_previews(GhSidebarPage *self, gboolean show_previews)
{
  g_return_if_fail(GH_IS_SIDEBAR_PAGE(self));
  if (self->show_previews == !!show_previews)
    return;
  self->show_previews = !!show_previews;
  g_object_notify_by_pspec(G_OBJECT(self), sidebar_props[SIDEBAR_PROP_SHOW_PREVIEWS]);
}

GtkWidget *
gh_sidebar_page_get_focus_target(GhSidebarPage *self)
{
  g_return_val_if_fail(GH_IS_SIDEBAR_PAGE(self), NULL);
  if (self->account_page)
    return NULL;
  if (g_str_equal(conversation_page(self), "no-results"))
    return GTK_WIDGET(self->search_entry);
  return NULL;
}

/* ---- GhContentPage ---------------------------------------------------------- */

struct _GhContentPage {
  AdwNavigationPage parent_instance;
  AdwWindowTitle *window_title;
  GtkStack *content_stack;
  AdwBin *conversation_slot;
  GhComposer *composer;
  gchar *default_title;
};

G_DEFINE_FINAL_TYPE(GhContentPage, gh_content_page, ADW_TYPE_NAVIGATION_PAGE)

static void
gh_content_page_dispose(GObject *object)
{
  gtk_widget_dispose_template(GTK_WIDGET(object), GH_TYPE_CONTENT_PAGE);
  G_OBJECT_CLASS(gh_content_page_parent_class)->dispose(object);
}

static void
gh_content_page_finalize(GObject *object)
{
  g_free(GH_CONTENT_PAGE(object)->default_title);
  G_OBJECT_CLASS(gh_content_page_parent_class)->finalize(object);
}

static void
gh_content_page_class_init(GhContentPageClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);

  G_OBJECT_CLASS(klass)->dispose = gh_content_page_dispose;
  G_OBJECT_CLASS(klass)->finalize = gh_content_page_finalize;
  g_type_ensure(GH_TYPE_COMPOSER);
  gtk_widget_class_set_template_from_resource(widget_class,
                                              "/org/nostr/Groundhog/ui/gh-content-page.ui");
  gtk_widget_class_bind_template_child(widget_class, GhContentPage, window_title);
  gtk_widget_class_bind_template_child(widget_class, GhContentPage, content_stack);
  gtk_widget_class_bind_template_child(widget_class, GhContentPage, conversation_slot);
  gtk_widget_class_bind_template_child(widget_class, GhContentPage, composer);
  gtk_widget_class_bind_template_child_full(widget_class, "none_page", FALSE, 0);
  gtk_widget_class_bind_template_child_full(widget_class, "conversation_bin", FALSE, 0);
}

static void
gh_content_page_init(GhContentPage *self)
{
  gtk_widget_init_template(GTK_WIDGET(self));
  self->default_title = g_strdup(adw_navigation_page_get_title(ADW_NAVIGATION_PAGE(self)));
}

GtkStack *
gh_content_page_get_stack(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), NULL);
  return self->content_stack;
}

AdwWindowTitle *
gh_content_page_get_window_title(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), NULL);
  return self->window_title;
}

void
gh_content_page_set_view(GhContentPage *self, GtkWidget *view)
{
  g_return_if_fail(GH_IS_CONTENT_PAGE(self));
  g_return_if_fail(GTK_IS_WIDGET(view));
  g_return_if_fail(adw_bin_get_child(self->conversation_slot) == NULL);
  adw_bin_set_child(self->conversation_slot, view);
}

GtkWidget *
gh_content_page_get_view(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), NULL);
  return adw_bin_get_child(self->conversation_slot);
}

void
gh_content_page_set_conversation_shown(GhContentPage *self, gboolean shown)
{
  g_return_if_fail(GH_IS_CONTENT_PAGE(self));
  gtk_stack_set_visible_child_name(self->content_stack, shown ? "conversation" : "none");
}

gboolean
gh_content_page_get_conversation_shown(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), FALSE);
  return g_strcmp0(gtk_stack_get_visible_child_name(self->content_stack), "conversation") == 0;
}

gboolean
gh_content_page_focus_conversation(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), FALSE);
  GtkWidget *view = adw_bin_get_child(self->conversation_slot);
  if (!view || !gh_content_page_get_conversation_shown(self))
    return FALSE;
  if (!gh_composer_get_disabled_reason(self->composer) &&
      gtk_widget_grab_focus(GTK_WIDGET(self->composer)))
    return TRUE;
  return gtk_widget_grab_focus(view);
}

void
gh_content_page_set_title(GhContentPage *self, const gchar *title, const gchar *subtitle)
{
  g_return_if_fail(GH_IS_CONTENT_PAGE(self));
  const gchar *shown = title && *title ? title : self->default_title;
  adw_navigation_page_set_title(ADW_NAVIGATION_PAGE(self), shown);
  adw_window_title_set_title(self->window_title, shown);
  adw_window_title_set_subtitle(self->window_title, subtitle ? subtitle : "");
}

GhComposer *
gh_content_page_get_composer(GhContentPage *self)
{
  g_return_val_if_fail(GH_IS_CONTENT_PAGE(self), NULL);
  return self->composer;
}
