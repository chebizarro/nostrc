#include "gh-app-outbox.h"

#include "gh-contact-directory.h"
#include "gh-dm-send.h"
#include "gh-outbox.h"

#ifndef GROUNDHOG_HAVE_NIP29
#define GROUNDHOG_HAVE_NIP29 0
#endif
#if GROUNDHOG_HAVE_NIP29
#include "gh-nip29-service.h"
#endif
#ifndef GROUNDHOG_HAVE_MLS
#define GROUNDHOG_HAVE_MLS 0
#endif
#if GROUNDHOG_HAVE_MLS
#include "gh-mls-service.h"
#endif

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
  /* G20a: the open store's NIP-29 groups, made and disposed with its outbox
   * (so always before that store closes). */
  GhConversationStore *conversations; /* borrowed: the app's model */
  GSettings *settings;                /* network-mode for the NIP-11 fetches; nullable */
  GObject *nip29;                     /* GhNip29Service, or NULL */
  GObject *nip29_outbox;              /* the outbox it was made beside (weak) */
  /* qp24.13: the open store's encrypted groups, likewise. */
  gboolean encrypted_groups;
  GObject *inbox;                     /* GhDmInbox for Welcomes; borrowed */
  GObject *mls;                       /* GhMlsService, or NULL */
  GObject *mls_outbox;                /* the outbox it was made beside (weak) */
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
  self->conversations = config->conversations;
  self->encrypted_groups = config->encrypted_groups;
  self->settings = config->settings ? g_object_ref(config->settings) : NULL;
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
static void unbind_nip29(gpointer data, GObject *outbox);
static void unbind_mls(gpointer data, GObject *outbox);

void
gh_app_outbox_free(GhAppOutbox *self)
{
  if (!self)
    return;
  if (self->mls_outbox) {
    g_object_weak_unref(self->mls_outbox, unbind_mls, self);
    unbind_mls(self, self->mls_outbox);
  }
  if (self->nip29_outbox) {
    g_object_weak_unref(self->nip29_outbox, unbind_nip29, self);
    unbind_nip29(self, self->nip29_outbox);
  }
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
  g_clear_object(&self->settings);
  g_free(self);
}

/* G20a: the store's NIP-29 service beside its outbox; the account store
 * disposes the outbox (and so the service) before it closes the store. */
static void
bind_nip29(GhAppOutbox *self, GhStore *store, GObject *outbox)
{
#if GROUNDHOG_HAVE_NIP29
  if (self->nip29_outbox) {
    g_object_weak_unref(self->nip29_outbox, unbind_nip29, self);
    unbind_nip29(self, self->nip29_outbox);
  }
  if (!self->conversations ||
      g_strcmp0(gh_conversation_store_get_account(self->conversations),
                gh_store_get_account_pubkey(store)) != 0)
    return;
  GhNip29ServiceConfig config = {
    .store = store,
    .accounts = self->accounts,
    .conversations = self->conversations,
    .settings = self->settings,
  };
  g_autoptr(GError) error = NULL;
  GhNip29Service *service = gh_nip29_service_new(&config, &error);
  if (!service) {
    g_message("Groundhog runs without its NIP-29 groups: %s", error->message);
    return;
  }
  self->nip29 = G_OBJECT(service);
  self->nip29_outbox = outbox;
  g_object_weak_ref(outbox, unbind_nip29, self);
#else
  (void)self;
  (void)store;
  (void)outbox;
#endif
}

/* The outbox the NIP-29 service was made beside is being disposed. */
static void
unbind_nip29(gpointer data, GObject *outbox)
{
  GhAppOutbox *self = data;
  if (self->nip29_outbox != outbox)
    return;
  self->nip29_outbox = NULL;
  if (self->nip29) {
    g_object_run_dispose(self->nip29);
    g_clear_object(&self->nip29);
  }
}

/* qp24.13: the store's encrypted groups beside its outbox, like NIP-29's;
 * only while GH_FEATURE_ENCRYPTED_GROUPS is on (part 2 ships the UI). */
static void
bind_mls(GhAppOutbox *self, GhStore *store, GObject *outbox)
{
#if GROUNDHOG_HAVE_MLS
  if (self->mls_outbox) {
    g_object_weak_unref(self->mls_outbox, unbind_mls, self);
    unbind_mls(self, self->mls_outbox);
  }
  if (!self->encrypted_groups || !self->conversations || gh_store_is_ephemeral(store) ||
      g_strcmp0(gh_conversation_store_get_account(self->conversations),
                gh_store_get_account_pubkey(store)) != 0)
    return;
  GhMlsServiceConfig config = {
    .store = store,
    .accounts = self->accounts,
    .conversations = self->conversations,
    .account_relays = self->account_relays,
    .inboxes = self->inboxes,
    .settings = self->settings,
    .inbox = self->inbox ? GH_DM_INBOX(self->inbox) : NULL,
  };
  g_autoptr(GError) error = NULL;
  GhMlsService *service = gh_mls_service_new(&config, &error);
  if (!service) {
    g_message("Groundhog runs without its encrypted groups: %s", error->message);
    return;
  }
  self->mls = G_OBJECT(service);
  self->mls_outbox = outbox;
  g_object_weak_ref(outbox, unbind_mls, self);
#else
  (void)self;
  (void)store;
  (void)outbox;
#endif
}

static void
unbind_mls(gpointer data, GObject *outbox)
{
  GhAppOutbox *self = data;
  if (self->mls_outbox != outbox)
    return;
  self->mls_outbox = NULL;
  if (self->mls) {
    g_object_run_dispose(self->mls);
    g_clear_object(&self->mls);
  }
}

GObject *
gh_app_outbox_get_mls_service(GhAppOutbox *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->mls;
}

void
gh_app_outbox_set_inbox(GhAppOutbox *self, GObject *inbox)
{
  g_return_if_fail(self != NULL);
  self->inbox = inbox;
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
  if (outbox) {
    bind_nip29(self, store, outbox);
    bind_mls(self, store, outbox);
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
  self->conversations = conversations;
  if (self->directory)
    gh_contact_directory_set_conversations(self->directory, conversations);
}

GhContactDirectory *
gh_app_outbox_get_directory(GhAppOutbox *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->directory;
}

GObject *
gh_app_outbox_get_nip29_service(GhAppOutbox *self)
{
  g_return_val_if_fail(self != NULL, NULL);
  return self->nip29;
}

void
gh_app_outbox_prune(GObject *outbox)
{
  g_return_if_fail(!outbox || GH_IS_OUTBOX(outbox));
  if (outbox)
    gh_outbox_prune(GH_OUTBOX(outbox));
}
