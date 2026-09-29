#include "gh-app-outbox.h"

#include "gh-contact-directory.h"
#include "gh-dm-send.h"
#include "gh-outbox.h"

struct _GhAppOutbox {
  GhAccountController *accounts;
  GhAccountRelays *account_relays;
  GhInboxResolver *inboxes;
  GhContactDirectory *directory; /* when inboxes is the one made here */
  GObject *bound_outbox;         /* the outbox whose store the directory has (weak) */
  GhDmSender *sender;
  GhRelayPublishTransport transport;
  gboolean custom_transport;
  gpointer transport_data;
};

GhAppOutbox *
gh_app_outbox_new(const GhAppOutboxConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(config->account_relays), NULL);
  g_return_val_if_fail(config->inboxes || G_IS_SETTINGS(config->settings), NULL);
  GhAppOutbox *self = g_new0(GhAppOutbox, 1);
  self->accounts = g_object_ref(config->accounts);
  self->account_relays = g_object_ref(config->account_relays);
  if (config->inboxes) {
    self->inboxes = g_object_ref(config->inboxes);
  } else {
    /* The cached contact directory (G10) answers recipient lookups: no
     * send-time REQ for a known contact (charter §4.5 S2). */
    GhContactDirectoryConfig directory = {
      .accounts = config->accounts,
      .settings = config->settings,
    };
    self->directory = gh_contact_directory_new(&directory);
    self->inboxes = GH_INBOX_RESOLVER(g_object_ref(self->directory));
    gh_contact_directory_set_conversations(self->directory, config->conversations);
  }
  if (config->transport) {
    self->transport = *config->transport;
    self->custom_transport = TRUE;
    self->transport_data = config->transport_data;
  }
  self->sender = gh_dm_sender_new(config->accounts, config->account_relays, self->inboxes,
                                  self->custom_transport ? &self->transport : NULL,
                                  self->transport_data);
  return self;
}

static void unbind_directory(gpointer data, GObject *outbox);

void
gh_app_outbox_free(GhAppOutbox *self)
{
  if (!self)
    return;
  if (self->bound_outbox) {
    g_object_weak_unref(self->bound_outbox, unbind_directory, self);
    unbind_directory(self, self->bound_outbox);
  }
  g_object_run_dispose(G_OBJECT(self->sender));
  g_clear_object(&self->sender);
  g_object_run_dispose(G_OBJECT(self->inboxes));
  g_clear_object(&self->inboxes);
  g_clear_object(&self->directory);
  g_clear_object(&self->account_relays);
  g_clear_object(&self->accounts);
  g_free(self);
}

GObject *
gh_app_outbox_create(GhStore *store, gpointer user_data, GError **error)
{
  GhAppOutbox *self = user_data;
  g_return_val_if_fail(store != NULL && self != NULL, NULL);
  GhOutboxConfig config = {
    .store = store,
    .accounts = self->accounts,
    .account_relays = self->account_relays,
    .inboxes = self->inboxes,
    .sender = self->sender,
    .transport = self->custom_transport ? &self->transport : NULL,
    .transport_data = self->transport_data,
  };
  GObject *outbox = G_OBJECT(gh_outbox_new(&config, error));
  if (outbox && self->directory) {
    /* The directory caches in this store while the outbox runs over it; the
     * account store disposes the outbox before it closes the store. */
    g_autoptr(GError) bind_error = NULL;
    if (self->bound_outbox) {
      g_object_weak_unref(self->bound_outbox, unbind_directory, self);
      unbind_directory(self, self->bound_outbox);
    }
    if (gh_contact_directory_set_store(self->directory, store, &bind_error)) {
      self->bound_outbox = outbox;
      g_object_weak_ref(outbox, unbind_directory, self);
    } else {
      g_message("Groundhog keeps its contact directory in memory only: %s", bind_error->message);
    }
  }
  return outbox;
}

/* The bound outbox is being disposed (its store closes next). */
static void
unbind_directory(gpointer data, GObject *outbox)
{
  GhAppOutbox *self = data;
  if (self->bound_outbox != outbox)
    return;
  self->bound_outbox = NULL;
  gh_contact_directory_set_store(self->directory, NULL, NULL);
}

void
gh_app_outbox_set_conversations(GhAppOutbox *self, GhConversationStore *conversations)
{
  g_return_if_fail(self != NULL);
  if (self->directory)
    gh_contact_directory_set_conversations(self->directory, conversations);
}

GhContactDirectory *
gh_app_outbox_get_directory(GhAppOutbox *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->directory;
}

void
gh_app_outbox_prune(GObject *outbox)
{
  g_return_if_fail(!outbox || GH_IS_OUTBOX(outbox));
  if (outbox)
    gh_outbox_prune(GH_OUTBOX(outbox));
}
