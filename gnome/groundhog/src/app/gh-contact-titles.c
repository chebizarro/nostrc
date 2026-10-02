#include "gh-contact-titles.h"

struct _GhContactTitles {
  GObject parent_instance;
  GhConversationStore *model;
  GhContactDirectory *directory;
};

G_DEFINE_FINAL_TYPE(GhContactTitles, gh_contact_titles, G_TYPE_OBJECT)

static void
refresh(GhContactTitles *self, GhConversation *conversation)
{
  GhConversationBackend backend = gh_conversation_get_backend(conversation);
  /* A non-DM MLS group's title is its group name. A NIP-29 group's title is
   * its relay-signed name. Only NIP-17 conversations and Marmot DMs use
   * contact names as titles. */
  if (backend == GH_CONVERSATION_BACKEND_NIP29)
    return;
  if (backend == GH_CONVERSATION_BACKEND_MLS && !gh_conversation_get_is_direct(conversation))
    return;
  /* NULL for a request, a note to self or a peer without a cached name. */
  g_autofree gchar *title =
    gh_contact_directory_dup_conversation_title(self->directory, conversation);
  gh_conversation_set_contact_title(conversation, title);
}

static void
on_request_changed(GhConversation *conversation, GParamSpec *pspec, GhContactTitles *self)
{
  (void)pspec;
  refresh(self, conversation);
}

static void
on_items_changed(GListModel *model, guint position, guint removed, guint added,
                 GhContactTitles *self)
{
  (void)removed;
  for (guint i = position; i < position + added; i++) {
    g_autoptr(GhConversation) conversation = g_list_model_get_item(model, i);
    /* A moved conversation is removed and added again: watched once. */
    if (!g_signal_handler_find(conversation, G_SIGNAL_MATCH_FUNC | G_SIGNAL_MATCH_DATA, 0, 0,
                               NULL, on_request_changed, self)) {
      g_signal_connect_object(conversation, "notify::is-request",
                              G_CALLBACK(on_request_changed), self, 0);
      g_signal_connect_object(conversation, "notify::is-direct",
                              G_CALLBACK(on_request_changed), self, 0);
    }
    refresh(self, conversation);
  }
}

static void
on_profile_changed(GhContactDirectory *directory, const gchar *pubkey, GhContactTitles *self)
{
  (void)directory;
  GListModel *model = G_LIST_MODEL(self->model);
  guint n = g_list_model_get_n_items(model);
  for (guint i = 0; i < n; i++) {
    g_autoptr(GhConversation) conversation = g_list_model_get_item(model, i);
    const gchar *const *peers = gh_conversation_get_peers(conversation);
    if (peers && g_strv_contains(peers, pubkey))
      refresh(self, conversation);
  }
}

static void
gh_contact_titles_dispose(GObject *object)
{
  GhContactTitles *self = GH_CONTACT_TITLES(object);
  g_clear_object(&self->model);
  g_clear_object(&self->directory);
  G_OBJECT_CLASS(gh_contact_titles_parent_class)->dispose(object);
}

static void
gh_contact_titles_class_init(GhContactTitlesClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_contact_titles_dispose;
}

static void
gh_contact_titles_init(GhContactTitles *self)
{
  (void)self;
}

GhContactTitles *
gh_contact_titles_new(GhConversationStore *model, GhContactDirectory *directory)
{
  g_return_val_if_fail(GH_IS_CONVERSATION_STORE(model), NULL);
  g_return_val_if_fail(GH_IS_CONTACT_DIRECTORY(directory), NULL);
  GhContactTitles *self = g_object_new(GH_TYPE_CONTACT_TITLES, NULL);
  self->model = g_object_ref(model);
  self->directory = g_object_ref(directory);
  g_signal_connect_object(model, "items-changed", G_CALLBACK(on_items_changed), self, 0);
  g_signal_connect_object(directory, "profile-changed", G_CALLBACK(on_profile_changed), self, 0);
  on_items_changed(G_LIST_MODEL(model), 0, 0, g_list_model_get_n_items(G_LIST_MODEL(model)),
                   self);
  return self;
}
