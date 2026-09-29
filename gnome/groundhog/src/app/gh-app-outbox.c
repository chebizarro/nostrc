#include "gh-app-outbox.h"

#include "gh-dm-send.h"
#include "gh-inbox-lookup.h"
#include "gh-outbox.h"

struct _GhAppOutbox {
  GhAccountController *accounts;
  GhAccountRelays *account_relays;
  GhInboxResolver *inboxes;
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
  self->inboxes = config->inboxes
    ? g_object_ref(config->inboxes)
    : GH_INBOX_RESOLVER(gh_inbox_lookup_new(config->accounts, config->settings, NULL, NULL));
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

void
gh_app_outbox_free(GhAppOutbox *self)
{
  if (!self)
    return;
  g_object_run_dispose(G_OBJECT(self->sender));
  g_clear_object(&self->sender);
  g_object_run_dispose(G_OBJECT(self->inboxes));
  g_clear_object(&self->inboxes);
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
  return G_OBJECT(gh_outbox_new(&config, error));
}
