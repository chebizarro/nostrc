#include "gh-list-adapter.h"
#include <jansson.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr-event.h>
#include <string.h>

typedef struct {
  GhListAdapter *adapter;
  GNostrListService *service; /* detached synchronously by close_subscription */
  GhRelayScope *scope;
  guint64 serial;
  gint kind;
  gchar *identifier, *source;
  GHashTable *disconnected;
} ListSubscription;
typedef struct {
  GhListAdapter *adapter;
  GTask *task;
  GThread *thread;
  GhRelayPublish *publish;
  GVariantBuilder outcomes;
  gulong cancel_handler;
  gint completing;
} ListPublication;
struct _GhListAdapter {
  GhStore *store;
  GNostrListService *service; /* owner; cleared on revoke */
  gchar *account;
  guint64 generation;
  gboolean revoked;
  GHashTable *memory;
  GhListAdapterNetwork network;
  GPtrArray *subscriptions, *publications;
  GhListDeriveFunc derive;
  gpointer derive_data;
  gint hide_decision; /* 0: ask, 1: accepted, -1: keep this list unsynced */
  gchar *pending_import;
  gint64 pending_import_time;
  gchar *pending_import_id;
  guint import_accounts, import_items;
};

GhListAdapter *
gh_list_adapter_new_volatile(const gchar *account, guint64 generation)
{
  GhListAdapter *self = g_new0(GhListAdapter, 1);
  self->account = g_strdup(account);
  self->generation = generation;
  self->memory = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  self->subscriptions = g_ptr_array_new();
  self->publications = g_ptr_array_new();
  return self;
}
GhListAdapter *
gh_list_adapter_new(GhStore *store, guint64 generation)
{
  GhListAdapter *self = gh_list_adapter_new_volatile(gh_store_get_account_pubkey(store), generation);
  self->store = store;
  return self;
}
static gboolean
generation_valid(gpointer data, const char *account, guint64 generation)
{
  GhListAdapter *self = data;
  return !self->revoked && self->generation == generation && !g_strcmp0(self->account, account) &&
    (!self->network.accounts || gh_account_controller_is_current(self->network.accounts, generation));
}
void gh_list_adapter_bind_service(GhListAdapter *self, GNostrListService *service) { self->service = service; }
static gboolean current(GhListAdapter *self) { return generation_valid(self, self->account, self->generation); }
static void
accounts_changed(GhAccountController *accounts, GhListAdapter *self)
{
  (void)accounts;
  if (!current(self)) gh_list_adapter_revoke(self);
}
void
gh_list_adapter_network_copy(GhListAdapterNetwork *dest, const GhListAdapterNetwork *source)
{
  *dest = *source;
  if (source->accounts) dest->accounts = g_object_ref(source->accounts);
  if (source->scope_transport) dest->scope_transport = g_memdup2(source->scope_transport, sizeof *source->scope_transport);
  if (source->scope_auth_transport) dest->scope_auth_transport = g_memdup2(source->scope_auth_transport, sizeof *source->scope_auth_transport);
  if (source->publish_transport) dest->publish_transport = g_memdup2(source->publish_transport, sizeof *source->publish_transport);
  if (source->publish_auth_transport) dest->publish_auth_transport = g_memdup2(source->publish_auth_transport, sizeof *source->publish_auth_transport);
}
void
gh_list_adapter_network_clear(GhListAdapterNetwork *network)
{
  g_clear_object(&network->accounts);
  g_free((gpointer)network->scope_transport);
  g_free((gpointer)network->scope_auth_transport);
  g_free((gpointer)network->publish_transport);
  g_free((gpointer)network->publish_auth_transport);
  *network = (GhListAdapterNetwork){0};
}
void
gh_list_adapter_set_network(GhListAdapter *self, const GhListAdapterNetwork *network)
{
  g_return_if_fail(self->subscriptions->len == 0 && self->publications->len == 0);
  if (self->network.accounts) g_signal_handlers_disconnect_by_data(self->network.accounts, self);
  gh_list_adapter_network_clear(&self->network);
  gh_list_adapter_network_copy(&self->network, network);
  if (network->accounts) {
    g_signal_connect(self->network.accounts, "changed", G_CALLBACK(accounts_changed), self);
  }
}
void
gh_list_adapter_set_derive(GhListAdapter *self, GhListDeriveFunc derive, gpointer data)
{
  self->derive = derive;
  self->derive_data = data;
}
static void
close_subscription(gpointer data, gpointer subscription)
{
  GhListAdapter *self = data;
  ListSubscription *sub = subscription;
  sub->service = NULL;
  gh_relay_scope_cancel(sub->scope);
  gh_relay_scope_unref(sub->scope);
  g_ptr_array_remove_fast(self->subscriptions, sub);
  g_hash_table_unref(sub->disconnected);
  g_free(sub->identifier);
  g_free(sub->source);
  g_free(sub);
}
void
gh_list_adapter_revoke(GhListAdapter *self)
{
  self->revoked = TRUE;
  self->store = NULL;
  self->service = NULL;
  self->derive = NULL;
  for (guint i = 0; i < self->subscriptions->len; i++) {
    ListSubscription *sub = self->subscriptions->pdata[i];
    gh_relay_scope_cancel(sub->scope);
  }
  for (guint i = 0; i < self->publications->len; i++) {
    ListPublication *pub = self->publications->pdata[i];
    if (pub->publish) gh_relay_publish_cancel(pub->publish);
    g_cancellable_cancel(g_task_get_cancellable(pub->task));
  }
  g_clear_pointer(&self->pending_import, g_free);
}
void
gh_list_adapter_free(GhListAdapter *self)
{
  g_assert(self->subscriptions->len == 0 && self->publications->len == 0);
  if (self->network.accounts) g_signal_handlers_disconnect_by_data(self->network.accounts, self);
  gh_list_adapter_network_clear(&self->network);
  g_hash_table_unref(self->memory);
  g_ptr_array_unref(self->subscriptions);
  g_ptr_array_unref(self->publications);
  g_free(self->pending_import);
  g_free(self->pending_import_id);
  g_free(self->account);
  g_free(self);
}
static gchar *
load(gpointer data, gint kind, const gchar *identifier, GError **error)
{
  GhListAdapter *self = data;
  if (!current(self)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage closed");
    return NULL;
  }
  g_autofree gchar *key = g_strdup_printf("%d:%s", kind, identifier);
  gchar *state = self->store ? gh_store_social_load(self->store, kind, identifier, error)
                            : g_strdup(g_hash_table_lookup(self->memory, key));
  if (kind == NOSTR_KIND_MUTE_LIST && state) {
    json_t *root = json_loads(state, 0, NULL);
    self->hide_decision = json_integer_value(json_object_get(root, "groundhog_hide_import"));
    json_decref(root);
  }
  return state;
}
static gboolean
save(GhListAdapter *self, GNostrListDocument *snapshot, const gchar *state, GError **error)
{
  gint kind = gnostr_list_document_get_kind(snapshot);
  json_t *root = json_loads(state, 0, NULL);
  if (!root) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid list replay state");
    return FALSE;
  }
  if (kind == NOSTR_KIND_MUTE_LIST)
    json_object_set_new(root, "groundhog_hide_import", json_integer(self->hide_decision));
  gchar *saved = json_dumps(root, JSON_COMPACT);
  json_decref(root);
  gboolean ok = saved && (!self->store || gh_store_social_commit(self->store, snapshot, saved, error));
  if (ok && !self->store) {
    gchar *key = g_strdup_printf("%d:%s", kind, gnostr_list_document_get_identifier(snapshot));
    g_hash_table_replace(self->memory, key, g_strdup(saved));
  }
  free(saved);
  return ok;
}
static gboolean
commit(gpointer data, GNostrListDocument *snapshot, const gchar *state, GError **error)
{
  GhListAdapter *self = data;
  if (!current(self)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage closed");
    return FALSE;
  }
  if (gnostr_list_document_get_kind(snapshot) == NOSTR_KIND_MUTE_LIST &&
      gnostr_list_document_get_event_id(snapshot) && self->hide_decision == 0) {
    json_t *root = json_loads(state, 0, NULL);
    const gchar *signed_json = json_string_value(json_object_get(root, "signed"));
    NostrEvent *signed_event = nostr_event_new();
    gboolean verified = signed_json && *signed_json &&
      nostr_event_deserialize_signed(signed_event, signed_json, NULL) == NOSTR_EVENT_VALIDATION_OK;
    gchar *signed_id = verified ? nostr_event_get_id(signed_event) : NULL;
    gboolean own = signed_id && !g_strcmp0(signed_id, gnostr_list_document_get_event_id(snapshot));
    free(signed_id); nostr_event_free(signed_event);
    json_decref(root);
    if (own) self->hide_decision = 1; /* already approved local projection, not a first import */
  }
  if (gnostr_list_document_get_kind(snapshot) == NOSTR_KIND_MUTE_LIST &&
      gnostr_list_document_get_event_id(snapshot) && self->hide_decision != 1) {
    if (self->hide_decision == 0) {
      gint64 at = gnostr_list_document_get_created_at(snapshot);
      const gchar *id = gnostr_list_document_get_event_id(snapshot);
      if (self->pending_import_id && (at < self->pending_import_time ||
          (at == self->pending_import_time && g_strcmp0(id, self->pending_import_id) > 0))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Newer hide list import awaits confirmation");
        return FALSE;
      }
      self->pending_import_time = at;
      g_free(self->pending_import_id); self->pending_import_id = g_strdup(id);
      const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
      self->import_accounts = self->import_items = 0;
      for (guint priv = 0; priv < 2; priv++)
        for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, priv); i++) {
          const NostrListTag *tag = nostr_nip51_document_tag(codec, priv, i);
          const gchar *name = nostr_nip51_tag_field(tag, 0);
          if (!g_strcmp0(name, "p")) self->import_accounts++;
          else if (!g_strcmp0(name, "word") || !g_strcmp0(name, "t") || !g_strcmp0(name, "e")) self->import_items++;
        }
      g_free(self->pending_import);
      self->pending_import = g_strdup(gnostr_list_document_get_source_json(snapshot));
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Hide list import needs confirmation");
    return FALSE;
  }
  return save(self, snapshot, state, error);
}
const gchar *gh_list_adapter_get_import_id(GhListAdapter *self) { return self->pending_import ? self->pending_import_id : NULL; }
gboolean gh_list_adapter_hide_unsynced(GhListAdapter *self) { return self->hide_decision < 0; }
gboolean
gh_list_adapter_get_import(GhListAdapter *self, guint *accounts, guint *items)
{
  if (accounts) *accounts = self->import_accounts;
  if (items) *items = self->import_items;
  return self->pending_import != NULL;
}
gboolean
gh_list_adapter_confirm_import(GhListAdapter *self, GNostrListService *service, gboolean accept, GError **error)
{
  if (!current(self) || !self->pending_import) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "No pending account list import");
    return FALSE;
  }
  g_autoptr(GNostrListDocument) snapshot = gnostr_list_service_lookup(service, NOSTR_KIND_MUTE_LIST, "");
  g_autofree gchar *state = load(self, NOSTR_KIND_MUTE_LIST, "", error);
  if (!state) return FALSE;
  gint old = self->hide_decision;
  self->hide_decision = accept ? 1 : -1;
  if (!save(self, snapshot, state, error)) { self->hide_decision = old; return FALSE; }
  g_autofree gchar *wire = g_steal_pointer(&self->pending_import);
  if (accept)
    for (guint i = 0; i < self->subscriptions->len; i++) {
      ListSubscription *sub = self->subscriptions->pdata[i];
      if (sub->kind == NOSTR_KIND_MUTE_LIST) {
        /* Re-opened plans normally supply this source; the signed event is
         * re-verified and decrypted by the engine, never installed by the UI. */
        gnostr_list_service_notify(service, sub->serial, sub->kind, sub->identifier,
                                   sub->source, GNOSTR_LIST_EVENT, wire);
        break;
      }
    }
  if (accept) gnostr_list_service_retry(service, NOSTR_KIND_MUTE_LIST, "");
  return TRUE;
}
static gint64
now(gpointer data)
{
  GhListAdapter *self = data;
  return self->store ? gh_clock_get_unix(gh_store_get_clock(self->store)) : g_get_real_time() / G_USEC_PER_SEC;
}
static gboolean
purpose_from_string(const gchar *name, GhAuthPurpose *out)
{
  /* A private-sync purpose also requires the destination check below. */
  const GhAuthPurpose allowed[] = { GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY,
    GH_AUTH_PURPOSE_OWN_LIST_PUBLISH, GH_AUTH_PURPOSE_PUBLIC_POST,
    GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_READ, GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_PUBLISH };
  for (guint i = 0; i < G_N_ELEMENTS(allowed); i++)
    if (!g_strcmp0(name, gh_auth_purpose_to_string(allowed[i]))) { *out = allowed[i]; return TRUE; }
  return FALSE;
}
static gboolean
private_targets_allowed(GhListAdapter *self, const gchar *const *targets, GError **error)
{
  GhStoreSocialPreferences preferences;
  g_autoptr(GNostrListDocument) snapshot = NULL;
  if (!current(self) || !self->store || !self->service || gh_store_is_ephemeral(self->store) ||
      gh_store_is_read_only(self->store) || !gh_store_social_get_preferences(self->store, &preferences, error) ||
      !preferences.sync_enabled) goto denied;
  snapshot = gnostr_list_service_lookup(self->service, NOSTR_KIND_PRIVATE_RELAY_LIST, "");
  if (!snapshot || gnostr_list_document_get_private_state(snapshot) == GNOSTR_LIST_PRIVATE_UNREADABLE) goto denied;
  const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
  for (guint i = 0; targets && targets[i]; i++) {
    gboolean allowed = FALSE;
    for (gsize j = 0; j < nostr_nip51_document_tag_count(codec, TRUE); j++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, TRUE, j);
      if (!g_strcmp0(nostr_nip51_tag_field(tag, 0), "relay") &&
          !g_strcmp0(nostr_nip51_tag_field(tag, 1), targets[i])) allowed = TRUE;
    }
    if (!allowed) goto denied;
  }
  if (targets && targets[0]) return TRUE;
denied:
  if (!error || !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
    "Private sync requires current account 10013 destinations and opt-in");
  return FALSE;
}
static void
scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  (void)scope;
  ListSubscription *sub = data;
  if (!sub->service || !current(sub->adapter)) return;
  GNostrListReadNotification type;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT: type = GNOSTR_LIST_EVENT; break;
  case GH_RELAY_NOTICE_EOSE: type = update->incomplete ? GNOSTR_LIST_ERROR : GNOSTR_LIST_EOSE; break;
  case GH_RELAY_NOTICE_CLOSED: type = GNOSTR_LIST_CLOSED; break;
  case GH_RELAY_NOTICE_AUTH: type = GNOSTR_LIST_AUTH_REQUIRED; break;
  case GH_RELAY_NOTICE_ERROR: type = GNOSTR_LIST_ERROR; break;
  case GH_RELAY_NOTICE_DISCONNECTED:
    g_hash_table_add(sub->disconnected, g_strdup(update->url));
    type = GNOSTR_LIST_DISCONNECTED; break;
  default: return;
  }
  if ((type == GNOSTR_LIST_EVENT || type == GNOSTR_LIST_EOSE || type == GNOSTR_LIST_AUTH_REQUIRED) &&
      g_hash_table_remove(sub->disconnected, update->url))
    gnostr_list_service_notify(sub->service, sub->serial, sub->kind, sub->identifier,
                               update->url, GNOSTR_LIST_RECONNECTED, NULL);
  gnostr_list_service_notify(sub->service, sub->serial, sub->kind, sub->identifier,
                             update->url, type, update->event_json);
}
static gpointer
open_subscription(gpointer data, GNostrListService *service, const char *account,
                  guint64 generation, guint64 serial, const GNostrListReadPlanEntry *entry, GError **error)
{
  GhListAdapter *self = data;
  GhAuthPurpose purpose;
  if (!generation_valid(self, account, generation) || !self->network.accounts ||
      !purpose_from_string(entry->read_purpose, &purpose) ||
      (purpose != GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY && purpose != GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_READ)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "List subscription purpose or account unavailable");
    return NULL;
  }
  if (purpose == GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_READ &&
      (entry->kind != NOSTR_KIND_DRAFT_WRAP || !private_targets_allowed(self, entry->sources, error))) return NULL;
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const char *authors[] = { account };
  nostr_filter_set_authors(filter, authors, 1);
  nostr_filter_set_kinds(filter, &entry->kind, 1);
  nostr_filter_set_limit(filter, 1);
  if (nostr_nip51_schema_requires_identifier(nostr_nip51_schema_lookup(entry->kind))) {
    NostrTags *tags = nostr_tags_new(0);
    nostr_tags_append(tags, nostr_tag_new("d", entry->identifier, NULL));
    nostr_filter_set_tags(filter, tags);
  }
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  ListSubscription *sub = g_new0(ListSubscription, 1);
  sub->adapter = self; sub->service = service; sub->serial = serial;
  sub->kind = entry->kind; sub->identifier = g_strdup(entry->identifier);
  sub->source = g_strdup(entry->sources[0]);
  sub->disconnected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  sub->scope = self->network.scope_transport
    ? gh_relay_scope_new_with_transport(generation, filters, self->network.scope_transport,
                                        self->network.scope_data, scope_update, sub)
    : gh_relay_scope_new(generation, filters, scope_update, sub);
  g_ptr_array_add(self->subscriptions, sub);
  if (self->network.scope_auth_transport)
    gh_relay_scope_set_auth_transport(sub->scope, self->network.scope_auth_transport);
  /* Each document owns separate sockets and a random isolation label. */
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->network.accounts);
  for (guint i = 0; entry->sources && entry->sources[i]; i++)
    if (!gh_relay_scope_add_url(sub->scope, entry->sources[i], error) ||
        !gh_auth_policy_apply_scope(policy, sub->scope, purpose, entry->sources[i], error)) {
      close_subscription(self, sub); return NULL;
    }
  gh_relay_scope_start(sub->scope);
  return sub;
}
static gboolean
signer_ready(GhListAdapter *self, const gchar *account, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data)
{
  if (!g_strcmp0(account, self->account) && current(self) && self->network.accounts &&
      !g_cancellable_is_cancelled(cancel)) return TRUE;
  GTask *task = g_task_new(NULL, cancel, callback, data);
  g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "List signer generation unavailable");
  g_object_unref(task);
  return FALSE;
}
static void
encrypt_async(gpointer data, const char *account, const char *text, GCancellable *cancel,
              GAsyncReadyCallback callback, gpointer user_data)
{
  GhListAdapter *self = data;
  if (!signer_ready(self, account, cancel, callback, user_data)) return;
  gh_account_controller_nip44_encrypt_with_cancellable_async(self->network.accounts, text,
    self->account, cancel, callback, user_data);
}
static void
decrypt_async(gpointer data, const char *account, const char *text, gboolean legacy, GCancellable *cancel,
              GAsyncReadyCallback callback, gpointer user_data)
{
  GhListAdapter *self = data;
  if (!signer_ready(self, account, cancel, callback, user_data)) return;
  if (legacy) gh_account_controller_nip04_decrypt_async(self->network.accounts, text, self->account, cancel, callback, user_data);
  else gh_account_controller_nip44_decrypt_with_cancellable_async(self->network.accounts, text,
    self->account, cancel, callback, user_data);
}
static void
sign_async(gpointer data, const char *json, GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data)
{
  GhListAdapter *self = data;
  if (signer_ready(self, self->account, cancel, callback, user_data))
    gh_account_controller_sign_with_cancellable_async(self->network.accounts, json, cancel, callback, user_data);
}
static gchar *crypto_finish(gpointer data, GAsyncResult *result, GError **error)
{ (void)data; return gh_account_controller_nip44_finish(result, error); }
static gchar *sign_finish(gpointer data, GAsyncResult *result, GError **error)
{ (void)data; return gh_account_controller_sign_finish(result, error); }
static void
publication_free(gpointer data)
{
  ListPublication *pub = data;
  if (pub->cancel_handler) g_cancellable_disconnect(g_task_get_cancellable(pub->task), pub->cancel_handler);
  g_clear_pointer(&pub->publish, gh_relay_publish_unref);
  g_variant_builder_clear(&pub->outcomes);
  g_ptr_array_remove_fast(pub->adapter->publications, pub);
  g_free(pub);
}
static gboolean
publication_complete(gpointer data)
{
  GTask *task = data;
  ListPublication *pub = g_task_get_task_data(task);
  if (g_cancellable_is_cancelled(g_task_get_cancellable(task)) || !current(pub->adapter)) {
    gh_relay_publish_cancel(pub->publish);
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "List publication cancelled");
  } else g_task_return_pointer(task, g_variant_ref_sink(g_variant_builder_end(&pub->outcomes)), (GDestroyNotify)g_variant_unref);
  return G_SOURCE_REMOVE;
}
static void
publication_schedule(ListPublication *pub)
{
  if (!g_atomic_int_compare_and_exchange(&pub->completing, FALSE, TRUE)) return;
  GSource *source = g_idle_source_new();
  g_source_set_callback(source, publication_complete, pub->task, g_object_unref);
  g_source_attach(source, g_task_get_context(pub->task));
  g_source_unref(source);
}
static void publication_cancelled(GCancellable *cancel, gpointer data)
{
  (void)cancel;
  ListPublication *pub = data;
  /* Owner-context cancellation closes sockets before the next account can
   * open. Cross-thread cancellation completes on the captured context. */
  if (g_thread_self() == pub->thread && pub->publish) gh_relay_publish_cancel(pub->publish);
  publication_schedule(pub);
}
static void
publication_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  (void)publish;
  ListPublication *pub = data;
  GNostrListPublishOutcome outcome = GNOSTR_LIST_TRANSPORT_FAILURE;
  switch (result->outcome) {
  case GH_RELAY_PUBLISH_ACCEPTED:
    outcome = result->prefix == GH_RELAY_OK_PREFIX_DUPLICATE ? GNOSTR_LIST_DUPLICATE : GNOSTR_LIST_ACCEPTED; break;
  case GH_RELAY_PUBLISH_REJECTED: outcome = GNOSTR_LIST_REJECTED; break;
  case GH_RELAY_PUBLISH_AUTH_REQUIRED: outcome = GNOSTR_LIST_AUTH_FAILURE; break;
  case GH_RELAY_PUBLISH_CANCELLED: outcome = GNOSTR_LIST_CANCELLED; break;
  default: break;
  }
  g_variant_builder_add(&pub->outcomes, "{si}", result->url, outcome);
}
static void publication_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{ (void)publish; (void)summary; publication_schedule(data); }
static void
publish_async(gpointer data, const char *json, const char *const *targets, const char *purpose_name,
              GCancellable *cancel, GAsyncReadyCallback callback, gpointer user_data)
{
  GhListAdapter *self = data;
  g_autoptr(GCancellable) effective = cancel ? g_object_ref(cancel) : g_cancellable_new();
  GTask *task = g_task_new(NULL, effective, callback, user_data);
  GhAuthPurpose purpose;
  if (!current(self) || !self->network.accounts || !targets || !targets[0] ||
      !purpose_from_string(purpose_name, &purpose) || purpose == GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY ||
      purpose == GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_READ) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "List publication purpose or account unavailable");
    g_object_unref(task); return;
  }
  if (purpose == GH_AUTH_PURPOSE_OWN_PRIVATE_SYNC_PUBLISH) {
    g_autoptr(GError) error = NULL;
    NostrEvent *event = nostr_event_new();
    gboolean draft = nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_get_kind(event) == NOSTR_KIND_DRAFT_WRAP && !g_strcmp0(nostr_event_get_pubkey(event), self->account);
    nostr_event_free(event);
    if (!draft || !private_targets_allowed(self, targets, &error)) {
      g_task_return_error(task, error ? g_steal_pointer(&error) : g_error_new_literal(G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED, "Private sync publication must be an own encrypted draft"));
      g_object_unref(task); return;
    }
  }
  /* Public addressable lists use the public-post AUTH identity. The engine
   * separately verifies visibility approval; preserving a pre-existing public
   * field must not silently opt a public publication into account AUTH. */
  NostrEvent *event = nostr_event_new();
  if (nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
      nostr_event_get_kind(event) >= 30000) {
    NostrTags *tags = nostr_event_get_tags(event);
    for (gsize i = 0; i < nostr_tags_size(tags); i++) {
      const gchar *name = nostr_tag_get(nostr_tags_get(tags, i), 0);
      if (g_strcmp0(name, "d")) { purpose = GH_AUTH_PURPOSE_PUBLIC_POST; break; }
    }
  }
  nostr_event_free(event);
  ListPublication *pub = g_new0(ListPublication, 1);
  pub->adapter = self; pub->task = task; pub->thread = g_thread_self();
  g_variant_builder_init(&pub->outcomes, G_VARIANT_TYPE("a{si}"));
  g_ptr_array_add(self->publications, pub);
  g_task_set_task_data(task, pub, publication_free);
  g_autoptr(GError) error = NULL;
  pub->publish = self->network.publish_transport
    ? gh_relay_publish_new_with_transport(self->generation, json, self->network.publish_transport,
      self->network.publish_data, publication_update, publication_done, pub, &error)
    : gh_relay_publish_new(self->generation, json, publication_update, publication_done, pub, &error);
  if (pub->publish) {
    if (self->network.publish_auth_transport)
      gh_relay_publish_set_auth_transport(pub->publish, self->network.publish_auth_transport);
    if (self->network.publish_deadline) gh_relay_publish_set_deadline(pub->publish, self->network.publish_deadline);
    gh_relay_publish_set_retries(pub->publish, 2, 500);
    GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->network.accounts);
    for (guint i = 0; targets[i] && !error; i++)
      if (gh_relay_publish_add_url(pub->publish, targets[i], &error))
        gh_auth_policy_apply_publish(policy, pub->publish, purpose, targets[i], &error);
  }
  if (!error && pub->publish && gh_relay_publish_start(pub->publish, &error)) {
    pub->cancel_handler = g_cancellable_connect(effective, G_CALLBACK(publication_cancelled), pub, NULL);
    return;
  }
  g_task_return_error(task, error ? g_steal_pointer(&error) : g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "List publish unavailable"));
  g_object_unref(task);
}
static GVariant *publish_finish(gpointer data, GAsyncResult *result, GError **error)
{ (void)data; return g_task_propagate_pointer(G_TASK(result), error); }
static GPtrArray *derive(gpointer data, GNostrListDocument *snapshot, GError **error)
{
  GhListAdapter *self = data;
  return self->derive ? self->derive(self->derive_data, snapshot, error) : NULL;
}
const GNostrListAdapter *
gh_list_adapter_get_callbacks(void)
{
  static const GNostrListAdapter callbacks = {
    .generation_valid = generation_valid, .load = load, .commit_snapshot = commit, .now = now,
    .open_subscription = open_subscription, .close_subscription = close_subscription,
    .encrypt_async = encrypt_async, .encrypt_finish = crypto_finish,
    .decrypt_async = decrypt_async, .decrypt_finish = crypto_finish,
    .sign_async = sign_async, .sign_finish = sign_finish,
    .publish_async = publish_async, .publish_finish = publish_finish, .derive_operations = derive,
  };
  return &callbacks;
}
