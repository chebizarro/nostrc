#include "gh-conversation-list.h"
#include "gh-conversation-row.h"
#include "gh-conversation-view.h"

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
  GhConversationStore *store;
  GtkFilter *accepted;        /* not a message request */
  GtkFilter *requests;        /* a message request */
  GtkFilter *accepted_search; /* the sidebar's search text */
  GtkFilter *requests_search;
  GhConversation *shown;
  /* Older history (gh_conversation_list_set_history_source()). */
  GhConversationListLoadOlder load_older;
  gpointer load_older_data;
  GDestroyNotify load_older_destroy;
  guint load_idle;
  GhConversation *loading; /* the room whose older page load_idle lists */
} GhConversationList;

static void
list_free(gpointer data)
{
  GhConversationList *list = data;
  g_clear_handle_id(&list->load_idle, g_source_remove);
  g_clear_object(&list->loading);
  if (list->load_older_destroy)
    list->load_older_destroy(list->load_older_data);
  g_clear_object(&list->view);
  g_clear_object(&list->store);
  g_clear_object(&list->accepted);
  g_clear_object(&list->requests);
  g_clear_object(&list->accepted_search);
  g_clear_object(&list->requests_search);
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
   * is only part of the subtitle (charter §7.9). */
  const gchar *subject = gh_conversation_get_is_request(list->shown)
                           ? gh_conversation_get_subject(list->shown) : NULL;
  g_autofree gchar *subtitle =
    subject ? g_strdup_printf(_("“%s” · Message request · end-to-end encrypted"), subject)
    : g_strdup(gh_conversation_get_is_request(list->shown)
                 ? _("Message request · end-to-end encrypted")
                 : _("Private · end-to-end encrypted"));
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
  /* Before marking it read: the unread count decides where the view opens. */
  gh_conversation_view_set_conversation(list->view, conversation);
  gh_content_page_set_conversation_shown(list->content, conversation != NULL);
  update_title(list);
  mark_read_if_visible(window, list);
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

  gh_sidebar_page_set_models(list->sidebar, conversations, requests);
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
