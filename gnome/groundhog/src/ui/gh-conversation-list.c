#include "gh-conversation-list.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"
#include "gh-privacy-summary.h"
#include "gh-requests-view.h"

#include <glib/gi18n.h>
#include <nostr-utils.h>
#include <nostr/nip19/nip19.h>
#include <stdlib.h>
#include <string.h>

#define LIST_DATA "groundhog-conversation-list"

typedef struct {
  GhWindow *window;       /* not owned: the struct is its data */
  GhSidebarPage *sidebar;
  GhContentPage *content;
  GhConversationView *view; /* a reference: a queued load may outlive the window's dispose */
  GhRequestsView *requests_view; /* the content page's "requests" page (G18) */
  GhConversationStore *store;
  GtkFilter *accepted;        /* not a message request */
  GtkFilter *requests;        /* a message request */
  GtkFilter *accepted_search; /* the sidebar's search text */
  GtkFilter *requests_search;
  GListModel *listed;         /* the accepted conversations as listed: pinned first */
  GtkListItemFactory *sections; /* "Pinned" and "Recent" headers */
  GhConversation *shown;
  /* Older history (gh_conversation_list_set_history_source()). */
  GhConversationListLoadOlder load_older;
  gpointer load_older_data;
  GDestroyNotify load_older_destroy;
  guint load_idle;
  GhConversation *loading; /* the room whose older page load_idle lists */
  /* An encrypted group's member count (gh_conversation_list_set_member_count_func()). */
  GhConversationListMemberCount member_count;
  gpointer member_count_data;
  GDestroyNotify member_count_destroy;
} GhConversationList;

static void
list_free(gpointer data)
{
  GhConversationList *list = data;
  g_clear_handle_id(&list->load_idle, g_source_remove);
  g_clear_object(&list->loading);
  if (list->load_older_destroy)
    list->load_older_destroy(list->load_older_data);
  if (list->member_count_destroy)
    list->member_count_destroy(list->member_count_data);
  g_clear_object(&list->view);
  g_clear_object(&list->store);
  g_clear_object(&list->accepted);
  g_clear_object(&list->requests);
  g_clear_object(&list->accepted_search);
  g_clear_object(&list->requests_search);
  g_clear_object(&list->listed);
  g_clear_object(&list->sections);
  g_clear_object(&list->shown);
  g_free(list);
}

static GhConversationList *
list_of(GhWindow *window)
{
  return g_object_get_data(G_OBJECT(window), LIST_DATA);
}

/* "npub1…" for a lowercase hex pubkey, abbreviated to "npub1abcde…wxyz". */
static gchar *
npub_of(const gchar *pubkey_hex, gboolean abbreviated)
{
  guint8 bytes[32];
  char *npub = NULL;
  if (!nostr_hex2bin(bytes, pubkey_hex, sizeof bytes) ||
      nostr_nip19_encode_npub(bytes, &npub) != 0 || !npub)
    return g_strdup(pubkey_hex);
  gsize length = strlen(npub);
  gchar *out = abbreviated && length > 16
                 ? g_strdup_printf("%.10s…%s", npub, npub + length - 4)
                 : g_strdup(npub);
  free(npub);
  return out;
}

/* ---- conversation rows ------------------------------------------------------ */

static void
setup_row(GtkSignalListItemFactory *factory, GtkListItem *item, GhSidebarPage *sidebar)
{
  (void)factory;
  GtkWidget *row = gh_conversation_row_new();
  gtk_list_item_set_child(item, row);
  g_object_bind_property(sidebar, "show-previews", row, "show-preview", G_BINDING_SYNC_CREATE);
  g_object_bind_property(row, "summary", item, "accessible-label", G_BINDING_SYNC_CREATE);
}

static void
bind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  gh_conversation_row_set_conversation(GH_CONVERSATION_ROW(gtk_list_item_get_child(item)),
                                       GH_CONVERSATION(gtk_list_item_get_item(item)));
}

static void
unbind_row(GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
  (void)factory;
  (void)data;
  gh_conversation_row_set_conversation(GH_CONVERSATION_ROW(gtk_list_item_get_child(item)),
                                       NULL);
}

/* ---- sections: Pinned and Recent (charter §7.5, nostrc-qp24.86) --------------- */

static void
setup_section(GtkSignalListItemFactory *factory, GObject *item, gpointer data)
{
  (void)factory;
  (void)data;
  g_autoptr(GtkBuilder) builder =
    gtk_builder_new_from_resource("/org/nostr/Groundhog/ui/gh-conversation-section.ui");
  gtk_list_header_set_child(GTK_LIST_HEADER(item),
                            GTK_WIDGET(gtk_builder_get_object(builder, "section_label")));
}

static void
bind_section(GtkSignalListItemFactory *factory, GObject *item, gpointer data)
{
  (void)factory;
  (void)data;
  gpointer first = gtk_list_header_get_item(GTK_LIST_HEADER(item));
  gtk_label_set_text(GTK_LABEL(gtk_list_header_get_child(GTK_LIST_HEADER(item))),
                     GH_IS_CONVERSATION(first) && gh_conversation_get_pinned(first)
                       ? _("Pinned") : _("Recent"));
}

/* Headers only while a pinned conversation is listed (pinned ones come
 * first), and never over Message Requests. */
static void
sync_sections(GhConversationList *list)
{
  g_autoptr(GhConversation) first = g_list_model_get_n_items(list->listed) > 0
                                      ? g_list_model_get_item(list->listed, 0) : NULL;
  gboolean pinned = first && gh_conversation_get_pinned(first) &&
                    !gh_sidebar_page_get_show_requests(list->sidebar);
  GtkListView *view = gh_sidebar_page_get_list(list->sidebar);
  if ((gtk_list_view_get_header_factory(view) != NULL) != pinned)
    gtk_list_view_set_header_factory(view, pinned ? list->sections : NULL);
}

static void
on_listed_changed(GhWindow *window)
{
  sync_sections(g_object_get_data(G_OBJECT(window), LIST_DATA));
}

/* The store lists pinned conversations first; this only groups them. */
static GListModel *
sectioned(GListModel *conversations)
{
  GtkExpression *pinned = gtk_property_expression_new(GH_TYPE_CONVERSATION, NULL, "pinned");
  GtkNumericSorter *by_pin = gtk_numeric_sorter_new(pinned);
  gtk_numeric_sorter_set_sort_order(by_pin, GTK_SORT_DESCENDING);
  GtkSortListModel *model = gtk_sort_list_model_new(g_object_ref(conversations), NULL);
  gtk_sort_list_model_set_section_sorter(model, GTK_SORTER(by_pin));
  g_object_unref(by_pin);
  return G_LIST_MODEL(model);
}

/* ---- filters ---------------------------------------------------------------- */

/* What a search matches: the title, the subject (a request's secondary
 * text) and every participant's full npub. */
static gchar *
search_key(GObject *item, gpointer data)
{
  (void)data;
  if (!GH_IS_CONVERSATION(item))
    return NULL;
  GhConversation *conversation = GH_CONVERSATION(item);
  g_autoptr(GString) key = g_string_new(gh_conversation_get_title(conversation));
  const gchar *subject = gh_conversation_get_subject(conversation);
  if (subject) {
    g_string_append_c(key, '\n');
    g_string_append(key, subject);
  }
  for (const gchar *const *p = gh_conversation_get_participants(conversation); *p; p++) {
    g_autofree gchar *npub = npub_of(*p, FALSE);
    g_string_append_c(key, '\n');
    g_string_append(key, npub);
  }
  return g_string_free(g_steal_pointer(&key), FALSE);
}

static GtkFilter *
search_filter(GhSidebarPage *sidebar)
{
  GtkExpression *expression = gtk_cclosure_expression_new(G_TYPE_STRING, NULL, 0, NULL,
                                                          G_CALLBACK(search_key), NULL, NULL);
  GtkStringFilter *filter = gtk_string_filter_new(expression);
  gtk_string_filter_set_ignore_case(filter, TRUE);
  gtk_string_filter_set_match_mode(filter, GTK_STRING_FILTER_MATCH_MODE_SUBSTRING);
  g_object_bind_property(sidebar, "search-text", filter, "search", G_BINDING_SYNC_CREATE);
  return GTK_FILTER(filter);
}

static GListModel *
filtered(GhConversationStore *store, GtkFilter *kind, GtkFilter *search)
{
  GtkEveryFilter *every = gtk_every_filter_new();
  gtk_multi_filter_append(GTK_MULTI_FILTER(every), g_object_ref(kind));
  gtk_multi_filter_append(GTK_MULTI_FILTER(every), g_object_ref(search));
  return G_LIST_MODEL(gtk_filter_list_model_new(g_object_ref(G_LIST_MODEL(store)),
                                                GTK_FILTER(every)));
}

/* ---- the shown conversation ------------------------------------------------ */

static void
update_title(GhConversationList *list)
{
  if (!list->shown) {
    gh_content_page_set_title(list->content, NULL, NULL);
    return;
  }
  /* A request is titled by its sender's npub; the subject the sender chose
   * is only part of the subtitle (charter §7.9), which says what the
   * conversation protects (charter §2.2 surface 1): one strings table for
   * every backend, so a NIP-29 relay group, readable by its relay's
   * operators, is never called encrypted here either. */
  GhPrivacyContext context = {
    .backend = (GhPrivacyBackend)gh_conversation_get_backend(list->shown),
    .is_request = gh_conversation_get_is_request(list->shown),
    .subject = gh_conversation_get_subject(list->shown),
  };
  /* "Encrypted group · N members" (§2.2 surface 1): the group's own count,
   * the account included; a room has no peers to count. */
  if (context.backend == GH_PRIVACY_BACKEND_MLS && list->member_count)
    context.n_people = list->member_count(list->shown, list->member_count_data);
  g_autofree gchar *subtitle = gh_privacy_summary_dup_subtitle(&context);
  gh_content_page_set_title(list->content, gh_conversation_get_title(list->shown), subtitle);
}

/* Local only (charter P8): nothing is published. The shown conversation is
 * read once its messages are on screen: when chosen or navigated to, and for
 * new messages only while the window is active. A collapsed list the
 * keyboard moves through marks nothing. */
static void
mark_read_if_visible(GhWindow *window, GhConversationList *list)
{
  if (list->shown && gh_window_get_content_visible(window))
    gh_conversation_mark_read(list->shown);
}

static void
mark_read_if_seen(GhWindow *window, GhConversationList *list)
{
  if (gtk_window_is_active(GTK_WINDOW(window)))
    mark_read_if_visible(window, list);
}

static void
on_selected(GhWindow *window)
{
  GhConversationList *list = list_of(window);
  gpointer selected = gh_sidebar_page_get_selected(list->sidebar);
  GhConversation *conversation = GH_IS_CONVERSATION(selected) ? selected : NULL;
  if (conversation == list->shown)
    return;
  g_set_object(&list->shown, conversation);
  /* A message request opens in the Message Requests page (charter §7.9,
   * G18): its npub, count and first message as plain text, with Accept,
   * Delete and Block; its messages are read in the conversation view only
   * once it is accepted. */
  gboolean request = conversation && gh_conversation_get_is_request(conversation);
  gh_requests_view_set_request(list->requests_view, request ? conversation : NULL);
  /* Before marking it read: the unread count decides where the view opens. */
  gh_conversation_view_set_conversation(list->view, request ? NULL : conversation);
  if (request)
    gtk_stack_set_visible_child_name(gh_content_page_get_stack(list->content), "requests");
  else
    gh_content_page_set_conversation_shown(list->content, conversation != NULL);
  update_title(list);
  mark_read_if_visible(window, list);
}

/* Accepted from the Message Requests page: open it as a conversation. */
static void
on_request_accepted(GhWindow *window, GhConversation *conversation)
{
  gh_window_open_item(window, conversation);
}

static void
on_content_shown(GhWindow *window)
{
  mark_read_if_visible(window, list_of(window));
}

static void
on_active_changed(GhWindow *window)
{
  mark_read_if_seen(window, list_of(window));
}

/* ---- older history ----------------------------------------------------------- */

/* In an idle: the view asks while it lays out or scrolls, and listing the
 * page changes its model. */
static gboolean
run_load_older(gpointer data)
{
  GhConversationList *list = data;
  list->load_idle = 0;
  g_autoptr(GhConversation) conversation = g_steal_pointer(&list->loading);
  /* The view moved on (another room, or it was disposed): nothing waits. */
  if (!conversation || conversation != gh_conversation_view_get_conversation(list->view) ||
      !gh_conversation_view_get_loading_older(list->view))
    return G_SOURCE_REMOVE;
  g_autoptr(GError) error = NULL;
  if (!list->load_older || !list->load_older(conversation, &error, list->load_older_data)) {
    g_message("Groundhog could not list earlier messages: %s",
              error ? error->message : "no message history is available");
    gh_conversation_view_fail_loading_older(list->view);
    return G_SOURCE_REMOVE;
  }
  gh_conversation_view_finish_loading_older(list->view);
  /* What is listed now is read like the rest of the room once it is on
   * screen; unread messages still unloaded stay unread
   * (gh_conversation_mark_read()). */
  if (conversation == list->shown)
    mark_read_if_visible(list->window, list);
  return G_SOURCE_REMOVE;
}

static void
on_load_older(GhConversationView *view, GhConversation *conversation, gpointer data)
{
  GhConversationList *list = data;
  (void)view;
  g_set_object(&list->loading, conversation);
  if (!list->load_idle)
    list->load_idle = g_idle_add(run_load_older, list);
}

void
gh_conversation_list_set_history_source(GhWindow *window, GhConversationListLoadOlder load_older,
                                        gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  GhConversationList *list = list_of(window);
  g_return_if_fail(list != NULL);
  if (list->load_older_destroy)
    list->load_older_destroy(list->load_older_data);
  list->load_older = load_older;
  list->load_older_data = user_data;
  list->load_older_destroy = destroy;
  gh_conversation_view_set_history_loader(list->view, load_older ? on_load_older : NULL, list,
                                          NULL);
}

static void
on_message_added(GhWindow *window, GhConversation *conversation, GhMessage *message)
{
  GhConversationList *list = list_of(window);
  (void)message;
  if (conversation == list->shown)
    mark_read_if_seen(window, list);
}

/* is-request only turns off (accepting, or replying); the filters do not
 * watch item properties, so they are told. */
static void
on_request_changed(GhWindow *window, GParamSpec *pspec, GhConversation *conversation)
{
  GhConversationList *list = list_of(window);
  (void)pspec;
  gtk_filter_changed(list->accepted, GTK_FILTER_CHANGE_DIFFERENT);
  gtk_filter_changed(list->requests, GTK_FILTER_CHANGE_DIFFERENT);
  if (conversation == list->shown)
    update_title(list);
}

static void
on_title_changed(GhWindow *window, GParamSpec *pspec, GhConversation *conversation)
{
  GhConversationList *list = list_of(window);
  (void)pspec;
  if (*gh_sidebar_page_get_search_text(list->sidebar)) {
    gtk_filter_changed(list->accepted_search, GTK_FILTER_CHANGE_DIFFERENT);
    gtk_filter_changed(list->requests_search, GTK_FILTER_CHANGE_DIFFERENT);
  }
  if (conversation == list->shown)
    update_title(list);
}

static void
watch(GhWindow *window, GhConversation *conversation)
{
  /* A moved conversation is re-added: never watch it twice. */
  g_signal_handlers_disconnect_by_func(conversation, on_request_changed, window);
  g_signal_handlers_disconnect_by_func(conversation, on_title_changed, window);
  g_signal_connect_object(conversation, "notify::is-request", G_CALLBACK(on_request_changed),
                          window, G_CONNECT_SWAPPED);
  g_signal_connect_object(conversation, "notify::title", G_CALLBACK(on_title_changed),
                          window, G_CONNECT_SWAPPED);
  g_signal_connect_object(conversation, "notify::subject", G_CALLBACK(on_title_changed),
                          window, G_CONNECT_SWAPPED);
}

static void
on_store_changed(GhWindow *window, guint position, guint removed, guint added,
                 GListModel *store)
{
  (void)removed;
  for (guint i = position; i < position + added; i++) {
    g_autoptr(GhConversation) conversation = g_list_model_get_item(store, i);
    watch(window, conversation);
  }
}

void
gh_conversation_list_attach(GhWindow *window, GhConversationStore *store, GSettings *settings)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  g_return_if_fail(GH_IS_CONVERSATION_STORE(store));
  g_return_if_fail(!settings || G_IS_SETTINGS(settings));
  g_return_if_fail(list_of(window) == NULL);

  GhConversationList *list = g_new0(GhConversationList, 1);
  list->window = window;
  list->sidebar = gh_window_get_sidebar(window);
  list->content = gh_window_get_content(window);
  list->store = g_object_ref(store);
  g_object_set_data_full(G_OBJECT(window), LIST_DATA, list, list_free);
  list->view = g_object_ref_sink(GH_CONVERSATION_VIEW(gh_conversation_view_new()));
  gh_conversation_view_set_settings(list->view, settings);
  gh_content_page_set_view(list->content, GTK_WIDGET(list->view));
  list->requests_view = GH_REQUESTS_VIEW(gh_requests_view_new());
  gtk_stack_add_named(gh_content_page_get_stack(list->content), GTK_WIDGET(list->requests_view),
                      "requests");
  g_signal_connect_object(list->requests_view, "accepted", G_CALLBACK(on_request_accepted),
                          window, G_CONNECT_SWAPPED);

  if (settings) {
    g_autoptr(GSettingsSchema) schema = NULL;
    g_object_get(settings, "settings-schema", &schema, NULL);
    if (schema && g_settings_schema_has_key(schema, GH_CONVERSATION_LIST_PREVIEWS_KEY))
      g_settings_bind(settings, GH_CONVERSATION_LIST_PREVIEWS_KEY, list->sidebar,
                      "show-previews", G_SETTINGS_BIND_GET);
  }

  GtkExpression *is_request = gtk_property_expression_new(GH_TYPE_CONVERSATION, NULL,
                                                          "is-request");
  list->accepted = GTK_FILTER(gtk_bool_filter_new(gtk_expression_ref(is_request)));
  gtk_bool_filter_set_invert(GTK_BOOL_FILTER(list->accepted), TRUE);
  list->requests = GTK_FILTER(gtk_bool_filter_new(is_request));
  list->accepted_search = search_filter(list->sidebar);
  list->requests_search = search_filter(list->sidebar);
  g_autoptr(GListModel) conversations = filtered(store, list->accepted, list->accepted_search);
  g_autoptr(GListModel) requests = filtered(store, list->requests, list->requests_search);

  GtkListItemFactory *rows = gtk_signal_list_item_factory_new();
  g_signal_connect(rows, "setup", G_CALLBACK(setup_row), list->sidebar);
  g_signal_connect(rows, "bind", G_CALLBACK(bind_row), NULL);
  g_signal_connect(rows, "unbind", G_CALLBACK(unbind_row), NULL);
  gtk_list_view_set_factory(gh_sidebar_page_get_list(list->sidebar), rows);
  g_object_unref(rows);

  list->listed = sectioned(conversations);
  list->sections = gtk_signal_list_item_factory_new();
  g_signal_connect(list->sections, "setup", G_CALLBACK(setup_section), NULL);
  g_signal_connect(list->sections, "bind", G_CALLBACK(bind_section), NULL);
  gh_sidebar_page_set_models(list->sidebar, list->listed, requests);
  g_signal_connect_object(list->listed, "items-changed", G_CALLBACK(on_listed_changed), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(list->sidebar, "notify::show-requests", G_CALLBACK(on_listed_changed),
                          window, G_CONNECT_SWAPPED);
  sync_sections(list);
  on_store_changed(window, 0, 0, g_list_model_get_n_items(G_LIST_MODEL(store)),
                   G_LIST_MODEL(store));
  g_signal_connect_object(store, "items-changed", G_CALLBACK(on_store_changed), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(store, "message-added", G_CALLBACK(on_message_added), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(list->sidebar, "notify::selected", G_CALLBACK(on_selected), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(window, "notify::is-active", G_CALLBACK(on_active_changed), window,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(gh_window_get_split(window), "notify::show-content",
                          G_CALLBACK(on_content_shown), window, G_CONNECT_SWAPPED);
  on_selected(window);
}

void
gh_conversation_list_set_member_count_func(GhWindow *window,
                                           GhConversationListMemberCount member_count,
                                           gpointer user_data, GDestroyNotify destroy)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  GhConversationList *list = list_of(window);
  g_return_if_fail(list != NULL);
  if (list->member_count_destroy)
    list->member_count_destroy(list->member_count_data);
  list->member_count = member_count;
  list->member_count_data = user_data;
  list->member_count_destroy = destroy;
  update_title(list);
}

void
gh_conversation_list_refresh_title(GhWindow *window)
{
  g_return_if_fail(GH_IS_WINDOW(window));
  GhConversationList *list = list_of(window);
  if (list)
    update_title(list);
}

GhRequestsView *
gh_conversation_list_get_requests_view(GhWindow *window)
{
  g_return_val_if_fail(GH_IS_WINDOW(window), NULL);
  GhConversationList *list = list_of(window);
  return list ? list->requests_view : NULL;
}
