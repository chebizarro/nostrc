#include "gh-social-lists.h"
#include "gh-list-adapter.h"
#include "gh-message-policy.h"
#include "gh-relay-deny-policy.h"
#include "gh-relay-scope.h"
#include <jansson.h>
#include <string.h>
#include <nostr-kinds.h>

#define MAX_FOLLOW_SOURCES 16 /* the relay scope's bound */

struct _GhSocialLists {
  GObject parent_instance;
  GhAccountStore *account_store;
  GSettings *legacy_settings;
  GhRelayDenyPolicy *deny_policy;
  GhListAdapterNetwork network;
  GhAccountRelays *relays;
  GStrv group_urls;
  gboolean group_waiting;
  GhListDeriveFunc group_derive;
  gpointer group_data;
  GhStore *store;
  GhListAdapter *adapter; /* owned by service; revoked before service release */
  GNostrListService *service;
  GhMessagePolicy *message_policy;
  guint64 generation;
  gboolean sync_enabled;
  gchar *error;
  gchar *visibility_error;
  /* Follows (kind 3): read only while enabled, from discovery plus write relays. */
  gboolean follows_enabled;
  GStrv discovery;
  GStrv follow_sources;    /* the union handed to the read plan */
  GHashTable *follows;     /* pubkey (lowercase hex) -> 1, from the kind-3 snapshot */
  GStrv follow_list;       /* the same, sorted, for gh_social_lists_get_follows */
};
G_DEFINE_TYPE(GhSocialLists, gh_social_lists, G_TYPE_OBJECT)
enum { CHANGED, DOCUMENT_CHANGED, SYNC_STATE_CHANGED, FOLLOWS_CHANGED, HIDE_POLICY_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];
static gboolean update_plan(GhSocialLists *self, GError **error);
static void rebuild_follows(GhSocialLists *self, gboolean force);

void
gh_social_lists_close(GhSocialLists *self)
{
  if (self->deny_policy) gh_relay_deny_policy_revoke(self->deny_policy);
  if (self->adapter)
    gh_list_adapter_revoke(self->adapter);
  if (self->service) {
    g_signal_handlers_disconnect_by_data(self->service, self);
    gnostr_list_service_close(self->service);
    g_clear_object(&self->service);
  }
  self->adapter = NULL;
  if (self->message_policy) {
    g_signal_handlers_disconnect_by_data(self->message_policy, self);
    gh_message_policy_close(self->message_policy);
  }
  g_clear_object(&self->message_policy);
  self->store = NULL;
  self->generation = 0;
  self->sync_enabled = FALSE;
  g_clear_pointer(&self->group_urls, g_strfreev);
  self->group_derive = NULL;
  self->group_data = NULL;
  g_clear_pointer(&self->error, g_free);
  g_clear_pointer(&self->visibility_error, g_free);
  g_signal_emit(self, signals[HIDE_POLICY_CHANGED], 0);
  rebuild_follows(self, FALSE); /* nothing of the old account's follows survives */
  g_signal_emit(self, signals[CHANGED], 0);
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
}

static void
hide_policy_changed(GhMessagePolicy *policy, GhSocialLists *self)
{
  (void)policy;
  g_signal_emit(self, signals[HIDE_POLICY_CHANGED], 0);
}

static void
policy_progress(GhMessagePolicy *policy, GhSocialLists *self)
{
  g_free(self->visibility_error);
  self->visibility_error = g_strdup(gh_message_policy_get_error(policy));
  hide_policy_changed(policy, self);
  g_signal_emit(self, signals[CHANGED], 0);
}

static void
update_deny_policy(GhSocialLists *self, GNostrListDocument *snapshot)
{
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  const NostrListDocument *codec = snapshot ? gnostr_list_document_get_codec(snapshot) : NULL;
  for (guint p = 0; codec && p < 2; p++) {
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      if (!g_strcmp0(nostr_nip51_tag_field(tag, 0), "relay")) {
        gchar *url = gh_relay_deny_policy_normalize(nostr_nip51_tag_field(tag, 1));
        if (url) g_ptr_array_add(urls, url);
      }
    }
  }
  g_ptr_array_add(urls, NULL);
  gh_relay_deny_policy_bind(self->deny_policy, self->generation, (const gchar *const *)urls->pdata);
}

static void
document_changed(GNostrListService *service, GNostrListDocument *snapshot, GhSocialLists *self)
{
  (void)service;
  if (gnostr_list_document_get_kind(snapshot) == NOSTR_KIND_MUTE_LIST && self->message_policy) {
    g_autoptr(GError) error = NULL;
    if (!gh_message_policy_refresh(self->message_policy, &error)) {
      g_free(self->visibility_error);
      self->visibility_error = g_strdup("Hidden messages could not be updated");
    }
  }
  if (gnostr_list_document_get_kind(snapshot) == NOSTR_KIND_FOLLOW_LIST &&
      !*gnostr_list_document_get_identifier(snapshot))
    rebuild_follows(self, FALSE);
  if (gnostr_list_document_get_kind(snapshot) == NOSTR_KIND_BLOCKED_RELAY_LIST)
    update_deny_policy(self, snapshot);
  g_signal_emit(self, signals[DOCUMENT_CHANGED], 0, snapshot);
  g_signal_emit(self, signals[CHANGED], 0);
}

/* ---- follows (NIP-02) -------------------------------------------------------- */

static gboolean
hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isxdigit(*p))
      return FALSE;
  return TRUE;
}

/* Discovery relays plus the account's NIP-65 write relays, validated,
 * deduplicated, at most 16, for the kind-3 read; NULL when none. */
static GStrv
follow_sources(GhSocialLists *self, const gchar *const *write)
{
  GPtrArray *urls = g_ptr_array_new_with_free_func(g_free);
  const gchar *const *sets[] = { (const gchar *const *)self->discovery, write };
  for (guint s = 0; s < G_N_ELEMENTS(sets); s++)
    for (guint i = 0; sets[s] && sets[s][i] && urls->len < MAX_FOLLOW_SOURCES; i++)
      if (gh_relay_url_validate(sets[s][i], NULL) &&
          !g_ptr_array_find_with_equal_func(urls, sets[s][i], g_str_equal, NULL))
        g_ptr_array_add(urls, g_strdup(sets[s][i]));
  if (!urls->len) {
    g_ptr_array_unref(urls);
    return NULL;
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(urls, FALSE);
}

static gint
compare_strings(gconstpointer a, gconstpointer b)
{
  return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

/* The followed set from the kind-3 snapshot's public p tags (the engine
 * admitted it: the account's own, NIP-01 newest). "follows-changed" when
 * the answers changed (or force). */
static void
rebuild_follows(GhSocialLists *self, gboolean force)
{
  g_autoptr(GHashTable) next = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  g_autoptr(GNostrListDocument) snapshot = self->service
    ? gnostr_list_service_lookup(self->service, NOSTR_KIND_FOLLOW_LIST, "") : NULL;
  const NostrListDocument *codec = snapshot ? gnostr_list_document_get_codec(snapshot) : NULL;
  for (gsize i = 0; codec && i < nostr_nip51_document_tag_count(codec, FALSE); i++) {
    const NostrListTag *tag = nostr_nip51_document_tag(codec, FALSE, i);
    const gchar *name = nostr_nip51_tag_field(tag, 0), *value = nostr_nip51_tag_field(tag, 1);
    if (!name || strcmp(name, "p") || !hex64(value))
      continue;
    g_hash_table_add(next, g_ascii_strdown(value, -1));
  }
  gboolean same = !force && g_hash_table_size(next) == g_hash_table_size(self->follows);
  GHashTableIter iter;
  gpointer key;
  g_hash_table_iter_init(&iter, next);
  while (same && g_hash_table_iter_next(&iter, &key, NULL))
    same = g_hash_table_contains(self->follows, key);
  if (same)
    return;
  g_hash_table_unref(self->follows);
  self->follows = g_steal_pointer(&next);
  GPtrArray *list = g_ptr_array_new_with_free_func(g_free);
  g_hash_table_iter_init(&iter, self->follows);
  while (g_hash_table_iter_next(&iter, &key, NULL))
    g_ptr_array_add(list, g_strdup(key));
  g_ptr_array_sort(list, compare_strings);
  g_ptr_array_add(list, NULL);
  g_clear_pointer(&self->follow_list, g_strfreev);
  self->follow_list = (GStrv)g_ptr_array_free(list, FALSE);
  g_signal_emit(self, signals[FOLLOWS_CHANGED], 0);
}

void
gh_social_lists_set_follow_reads(GhSocialLists *self, gboolean enabled,
                                 const gchar *const *discovery_relays)
{
  g_return_if_fail(GH_IS_SOCIAL_LISTS(self));
  enabled = !!enabled;
  gboolean was = self->follows_enabled;
  self->follows_enabled = enabled;
  g_clear_pointer(&self->discovery, g_strfreev);
  self->discovery = g_strdupv((gchar **)discovery_relays);
  g_autoptr(GError) error = NULL;
  if (!update_plan(self, &error)) {
    g_free(self->error);
    self->error = g_strdup("Account list relay plan could not be updated");
  }
  /* Off: the set is kept but unused; on: it applies again. Either way the
   * answers changed for everyone followed. */
  if (was != enabled && g_hash_table_size(self->follows))
    g_signal_emit(self, signals[FOLLOWS_CHANGED], 0);
}

gboolean
gh_social_lists_is_followed(GhSocialLists *self, const gchar *pubkey)
{
  g_return_val_if_fail(GH_IS_SOCIAL_LISTS(self), FALSE);
  if (!self->follows_enabled || !pubkey)
    return FALSE;
  g_autofree gchar *key = g_ascii_strdown(pubkey, -1);
  return g_hash_table_contains(self->follows, key);
}

const gchar *const *
gh_social_lists_get_follows(GhSocialLists *self)
{
  static const gchar *const none[] = { NULL };
  g_return_val_if_fail(GH_IS_SOCIAL_LISTS(self), none);
  return self->follows_enabled && self->follow_list ? (const gchar *const *)self->follow_list
                                                      : none;
}

static void
document_state_changed(GhSocialLists *self)
{
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
}

static GNostrListReadPlanEntry
local_plan(gint kind, const gchar *identifier)
{
  const NostrListSchema *schema = nostr_nip51_schema_lookup(kind);
  return (GNostrListReadPlanEntry){
    .kind = kind, .identifier = identifier,
    .read_purpose = "own-list-discovery", .publish_purpose = "own-list-publish",
    .publish_policy = nostr_nip51_schema_read_only(schema) ? GNOSTR_LIST_READ_ONLY : GNOSTR_LIST_EXPLICIT_ONLY,
    .required_sources = kind == NOSTR_KIND_SIMPLE_GROUP_LIST ? GNOSTR_LIST_SOURCES_ALL : GNOSTR_LIST_SOURCES_MAJORITY,
  };
}

static gboolean
update_plan(GhSocialLists *self, GError **error)
{
  if (!self->service) return TRUE;
  GNostrListReadPlanEntry entries[256];
  g_autoptr(GPtrArray) snapshots = g_ptr_array_new_with_free_func(g_object_unref);
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->service));
  for (guint i = 0; i < n; i++) {
    GNostrListDocument *snapshot = g_list_model_get_item(G_LIST_MODEL(self->service), i);
    g_ptr_array_add(snapshots, snapshot);
    entries[i] = local_plan(gnostr_list_document_get_kind(snapshot), gnostr_list_document_get_identifier(snapshot));
  }
  const gint kinds[] = { NOSTR_KIND_MUTE_LIST, NOSTR_KIND_BLOCKED_RELAY_LIST,
    NOSTR_KIND_PRIVATE_RELAY_LIST, NOSTR_KIND_EMOJI_LIST, NOSTR_KIND_SLASH_COMMAND_LIST,
    NOSTR_KIND_SIMPLE_GROUP_LIST, NOSTR_KIND_FOLLOW_LIST };
  for (guint k = 0; self->network.accounts && k < G_N_ELEMENTS(kinds); k++) {
    gboolean found = FALSE;
    for (guint i = 0; i < n; i++) if (entries[i].kind == kinds[k] && !*entries[i].identifier) found = TRUE;
    if (!found) {
      if (n == G_N_ELEMENTS(entries)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Too many account list documents");
        return FALSE;
      }
      entries[n++] = local_plan(kinds[k], "");
    }
  }
  gboolean ready = self->relays && self->network.accounts &&
    gh_account_relays_get_generation(self->relays) == self->generation &&
    gh_account_relays_get_state(self->relays) != GH_ACCOUNT_RELAYS_DISCOVERING;
  const gchar *const *write = ready ? gh_account_relays_get_write_relays(self->relays) : NULL;
  g_clear_pointer(&self->follow_sources, g_strfreev);
  if (self->follows_enabled && ready)
    self->follow_sources = follow_sources(self, write);
  g_auto(GStrv) public_targets = gh_social_lists_dup_public_targets(self);
  for (guint i = 0; i < n; i++) {
    GNostrListReadPlanEntry *entry = &entries[i];
    if (entry->kind == NOSTR_KIND_BLOSSOM_SERVER_LIST || entry->kind == NOSTR_KIND_PROFILE_BADGE_LIST) {
      entry->publish_policy = GNOSTR_LIST_EXPLICIT_ONLY;
      entry->publish_purpose = "public-post";
      entry->targets = (const gchar *const *)public_targets;
      entry->sources = self->sync_enabled ? write : NULL;
      entry->waiting_for_dependency = self->network.accounts && !ready;
    } else if (entry->kind == NOSTR_KIND_SIMPLE_GROUP_LIST) {
      entry->publish_policy = GNOSTR_LIST_AUTO_ALWAYS;
      entry->sources = entry->targets = (const gchar *const *)self->group_urls;
      entry->waiting_for_dependency = self->group_waiting || !self->group_derive;
    } else if (entry->kind == NOSTR_KIND_FOLLOW_LIST && !*entry->identifier) {
      /* Read-only (never published), only while load-profile-pictures is
       * on, from discovery plus write relays (plan §3.6, PD-12). */
      entry->publish_policy = GNOSTR_LIST_READ_ONLY;
      entry->sources = (const gchar *const *)self->follow_sources;
      entry->targets = NULL;
      entry->waiting_for_dependency = !self->follows_enabled || !ready || !self->follow_sources;
    } else if (entry->kind == NOSTR_KIND_MUTE_LIST && gh_list_adapter_hide_unsynced(self->adapter)) {
      entry->publish_policy = GNOSTR_LIST_EXPLICIT_ONLY;
    } else if (self->sync_enabled) {
      const NostrListSchema *schema = nostr_nip51_schema_lookup(entry->kind);
      if (!nostr_nip51_schema_read_only(schema) && entry->kind != NOSTR_KIND_PROFILE_BADGE_LIST &&
          entry->kind != NOSTR_KIND_BLOSSOM_SERVER_LIST)
        entry->publish_policy = GNOSTR_LIST_AUTO_WHEN_SYNC_ENABLED;
      entry->sources = entry->targets = write;
      entry->waiting_for_dependency = !ready;
      if (entry->publish_policy == GNOSTR_LIST_EXPLICIT_ONLY) entry->publish_purpose = "public-post";
    }
  }
  if (!gnostr_list_service_update_read_plan(self->service, entries, n, error)) return FALSE;
  /* Save a local baseline so a declined first import can persist its decision
   * without ever storing the proposed remote projection as usable state. */
  g_autoptr(GNostrListDocument) hide = gnostr_list_service_lookup(self->service, NOSTR_KIND_MUTE_LIST, "");
  if (hide && !gnostr_list_document_get_local_revision(hide)) {
    g_autoptr(GPtrArray) empty = g_ptr_array_new();
    g_autoptr(GNostrListDocument) saved = gnostr_list_service_apply(self->service, NOSTR_KIND_MUTE_LIST, "", 0, empty, error);
    if (!saved) return FALSE;
  }
  g_clear_pointer(&self->error, g_free);
  if (self->sync_enabled && ready && (!write || !write[0])) self->error = g_strdup("Needs a Relay List");
  else if (self->sync_enabled && gh_list_adapter_hide_unsynced(self->adapter)) self->error = g_strdup("Hidden items are not synced");
  return TRUE;
}
static void
relays_changed(GhAccountRelays *relays, GhSocialLists *self)
{
  (void)relays;
  g_autoptr(GError) error = NULL;
  if (!update_plan(self, &error)) {
    g_free(self->error);
    self->error = g_strdup("Account list relay plan could not be updated");
  }
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
}
void
gh_social_lists_set_network(GhSocialLists *self, const GhListAdapterNetwork *network, GhAccountRelays *relays)
{
  g_return_if_fail(!self->service);
  if (self->relays) g_signal_handlers_disconnect_by_data(self->relays, self);
  g_set_object(&self->relays, relays);
  gh_list_adapter_network_clear(&self->network);
  gh_list_adapter_network_copy(&self->network, network);
  if (relays) g_signal_connect(relays, "changed", G_CALLBACK(relays_changed), self);
}
GhListAdapter *gh_social_lists_get_adapter(GhSocialLists *self) { return self->adapter; }
void
gh_social_lists_configure_groups(GhSocialLists *self, const gchar *const *urls, gboolean waiting,
                                GhListDeriveFunc derive, gpointer data)
{
  self->group_derive = derive; self->group_data = data;
  self->group_waiting = waiting;
  g_clear_pointer(&self->group_urls, g_strfreev);
  self->group_urls = g_strdupv((gchar **)urls);
  if (self->adapter) gh_list_adapter_set_derive(self->adapter, derive, data);
  g_autoptr(GError) error = NULL;
  if (!update_plan(self, &error)) {
    g_free(self->error); self->error = g_strdup("Account group list plan could not be updated");
  }
}
gboolean
gh_social_lists_get_pending_import(GhSocialLists *self, guint *accounts, guint *items)
{
  return self->sync_enabled && self->adapter && gh_list_adapter_get_import(self->adapter, accounts, items);
}
const gchar *gh_social_lists_get_pending_import_id(GhSocialLists *self)
{ return self->adapter ? gh_list_adapter_get_import_id(self->adapter) : NULL; }
gboolean
gh_social_lists_confirm_import(GhSocialLists *self, gboolean accept, GError **error)
{
  if (!self->sync_enabled) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Private synchronization is off");
    return FALSE;
  }
  if (!self->adapter || !gh_list_adapter_confirm_import(self->adapter, self->service, accept, error)) return FALSE;
  if (!accept && !update_plan(self, error)) return FALSE;
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
  return TRUE;
}

gboolean
gh_social_lists_bind_store(GhSocialLists *self, GhStore *store, guint64 generation, GError **error)
{
  gh_social_lists_close(self);
  g_autoptr(GPtrArray) identities = NULL;
  g_autoptr(GNostrListDocument) denied = NULL;
  GhStoreSocialPreferences preferences;
  if (!gh_store_social_get_preferences(store, &preferences, error)) goto fail;
  if (!preferences.blossom_imported && !gh_store_is_read_only(store)) {
    g_auto(GStrv) legacy = self->legacy_settings ? g_settings_get_strv(self->legacy_settings, "blossom-servers") : NULL;
    if (!gh_store_social_import_blossom_legacy(store, (const gchar *const *)legacy, error)) goto fail;
  }
  identities = gh_store_social_list(store, error);
  if (!identities) goto fail;
  if (identities->len > 256) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Too many account list documents");
    goto fail;
  }
  self->store = store;
  self->generation = generation;
  self->sync_enabled = preferences.sync_enabled;
  GhStoreConversations *conversations = self->account_store ?
    gh_account_store_get_conversations(self->account_store) : NULL;
  GhMessagePolicy *policy = conversations ? gh_store_conversations_get_policy(conversations) : NULL;
  self->message_policy = policy ? g_object_ref(policy) : gh_message_policy_new(gh_store_get_account_pubkey(store));
  if (!gh_message_policy_is_loaded(self->message_policy) &&
      !gh_message_policy_load(self->message_policy, store, error)) {
    gh_social_lists_close(self);
    goto fail;
  }
  g_signal_connect(self->message_policy, "changed", G_CALLBACK(hide_policy_changed), self);
  g_signal_connect(self->message_policy, "failed", G_CALLBACK(policy_progress), self);
  g_signal_connect(self->message_policy, "projected", G_CALLBACK(policy_progress), self);
  self->adapter = gh_list_adapter_new(store, generation);
  if (self->network.accounts) gh_list_adapter_set_network(self->adapter, &self->network);
  self->service = gnostr_list_service_new(gh_store_get_account_pubkey(store), generation,
    gh_list_adapter_get_callbacks(), self->adapter, (GDestroyNotify)gh_list_adapter_free);
  gh_list_adapter_bind_service(self->adapter, self->service);
  GNostrListReadPlanEntry entries[256];
  for (guint i = 0; i < identities->len; i++) {
    GhStoreSocialIdentity *id = identities->pdata[i];
    entries[i] = local_plan(id->kind, id->identifier);
  }
  if (!gnostr_list_service_start(self->service, entries, identities->len, error)) {
    gh_social_lists_close(self);
    goto fail;
  }
  denied = gnostr_list_service_lookup(self->service, NOSTR_KIND_BLOCKED_RELAY_LIST, "");
  update_deny_policy(self, denied);
  g_signal_connect(self->service, "document-changed", G_CALLBACK(document_changed), self);
  g_signal_connect_swapped(self->service, "state-changed", G_CALLBACK(document_state_changed), self);
  if (!update_plan(self, error)) { gh_social_lists_close(self); goto fail; }
  gnostr_list_service_set_private_sync_enabled(self->service, self->sync_enabled);
  rebuild_follows(self, FALSE); /* the persisted kind 3, if any */
  g_signal_emit(self, signals[HIDE_POLICY_CHANGED], 0);
  g_signal_emit(self, signals[CHANGED], 0);
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
  return TRUE;
fail:
  self->error = g_strdup("Account lists could not be loaded");
  g_signal_emit(self, signals[CHANGED], 0);
  return FALSE;
}

static void
store_bound(GhAccountStore *store, GhSocialLists *self)
{
  g_autoptr(GError) error = NULL;
  gh_social_lists_bind_store(self, gh_account_store_get_store(store),
                             gh_account_store_get_generation(store), &error);
}

static void
store_closing(GhAccountStore *store, GhSocialLists *self)
{
  (void)store;
  gh_social_lists_close(self);
}

static void
gh_social_lists_dispose(GObject *object)
{
  GhSocialLists *self = GH_SOCIAL_LISTS(object);
  if (self->account_store)
    g_signal_handlers_disconnect_by_data(self->account_store, self);
  gh_social_lists_close(self);
  g_clear_object(&self->account_store);
  g_clear_object(&self->legacy_settings);
  g_clear_object(&self->deny_policy);
  if (self->relays) g_signal_handlers_disconnect_by_data(self->relays, self);
  g_clear_object(&self->relays);
  gh_list_adapter_network_clear(&self->network);
  g_clear_pointer(&self->discovery, g_strfreev);
  g_clear_pointer(&self->follow_sources, g_strfreev);
  g_clear_pointer(&self->follow_list, g_strfreev);
  G_OBJECT_CLASS(gh_social_lists_parent_class)->dispose(object);
}

static void
gh_social_lists_finalize(GObject *object)
{
  GhSocialLists *self = GH_SOCIAL_LISTS(object);
  g_hash_table_unref(self->follows);
  G_OBJECT_CLASS(gh_social_lists_parent_class)->finalize(object);
}

static void
gh_social_lists_class_init(GhSocialListsClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_social_lists_dispose;
  signals[HIDE_POLICY_CHANGED] = g_signal_new("hide-policy-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  G_OBJECT_CLASS(klass)->finalize = gh_social_lists_finalize;
  signals[CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[FOLLOWS_CHANGED] = g_signal_new("follows-changed", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
  signals[DOCUMENT_CHANGED] = g_signal_new("document-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 1, GNOSTR_TYPE_LIST_DOCUMENT);
  signals[SYNC_STATE_CHANGED] = g_signal_new("sync-state-changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
    0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
static void
gh_social_lists_init(GhSocialLists *self)
{
  self->follows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->deny_policy = g_object_ref(gh_relay_deny_policy_get_default());
  gh_relay_deny_policy_revoke(self->deny_policy);
}

GhSocialLists *
gh_social_lists_new(GhAccountStore *account_store)
{
  return gh_social_lists_new_with_settings(account_store, NULL);
}

GhSocialLists *
gh_social_lists_new_with_settings(GhAccountStore *account_store, GSettings *legacy_settings)
{
  GhSocialLists *self = g_object_new(GH_TYPE_SOCIAL_LISTS, NULL);
  if (legacy_settings) self->legacy_settings = g_object_ref(legacy_settings);
  gh_social_lists_attach_store(self, account_store);
  return self;
}

void
gh_social_lists_attach_store(GhSocialLists *self, GhAccountStore *account_store)
{
  g_return_if_fail(self->account_store == NULL);
  if (account_store) {
    self->account_store = g_object_ref(account_store);
    g_signal_connect(account_store, "store-bound", G_CALLBACK(store_bound), self);
    g_signal_connect(account_store, "store-closing", G_CALLBACK(store_closing), self);
    if (gh_account_store_get_store(account_store))
      store_bound(account_store, self);
  }
}

GStrv
gh_social_lists_dup_public_targets(GhSocialLists *self)
{
  gboolean ready = self->store && !gh_store_is_read_only(self->store) &&
    !gh_store_is_ephemeral(self->store) && self->relays && self->network.accounts &&
    gh_account_relays_get_generation(self->relays) == self->generation &&
    gh_account_relays_get_state(self->relays) != GH_ACCOUNT_RELAYS_DISCOVERING;
  return ready ? g_strdupv((gchar **)gh_account_relays_get_write_relays(self->relays)) : g_new0(gchar *, 1);
}

GStrv gh_social_lists_dup_servers(GhSocialLists *self, GError **error)
{
  return self->store ? gh_store_social_dup_servers(self->store, error) : g_new0(gchar *, 1);
}

gboolean
gh_social_lists_set_servers(GhSocialLists *self, const gchar *const *servers, GError **error)
{
  g_autoptr(GNostrListDocument) snapshot = gh_social_lists_ensure(self, NOSTR_KIND_BLOSSOM_SERVER_LIST, "", error);
  if (!snapshot) return FALSE;
  g_autoptr(GPtrArray) ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
  for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, FALSE); i++) {
    const NostrListTag *tag = nostr_nip51_document_tag(codec, FALSE, i);
    if (!g_strcmp0(nostr_nip51_tag_field(tag, 0), "server"))
      g_ptr_array_add(ops, gnostr_list_operation_remove(FALSE, tag, G_MAXUINT));
  }
  for (guint i = 0; servers && servers[i]; i++) {
    const gchar *fields[] = { "server", servers[i] };
    GNostrListOperation *op = gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, FALSE, fields, 2, 0, 0);
    if (!op) {
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid attachment server");
      return FALSE;
    }
    g_ptr_array_add(ops, op);
  }
  g_autoptr(GNostrListDocument) saved = gh_social_lists_apply(self, NOSTR_KIND_BLOSSOM_SERVER_LIST, "",
    gnostr_list_document_get_local_revision(snapshot), ops, error);
  return saved != NULL;
}

GNostrListService *gh_social_lists_get_service(GhSocialLists *self) { return self->service; }
GhMessagePolicy *gh_social_lists_get_message_policy(GhSocialLists *self) { return self->message_policy; }
gboolean gh_social_lists_is_hidden(GhSocialLists *self, const gchar *pubkey)
{
  return !gh_message_policy_is_loaded(self->message_policy) ||
    gh_message_policy_hides_author(self->message_policy, pubkey);
}
gboolean gh_social_lists_retry_visibility(GhSocialLists *self, GError **error)
{
  if (!self->message_policy) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account hide policy is not open");
    return FALSE;
  }
  gboolean ok = gh_message_policy_refresh(self->message_policy, error);
  policy_progress(self->message_policy, self);
  return ok;
}
guint64 gh_social_lists_get_generation(GhSocialLists *self) { return self->generation; }
const gchar *gh_social_lists_get_error(GhSocialLists *self)
{ return self->visibility_error ? self->visibility_error : self->error; }
const gchar *gh_social_lists_get_visibility_error(GhSocialLists *self) { return self->visibility_error; }
gboolean gh_social_lists_get_sync_enabled(GhSocialLists *self) { return self->sync_enabled; }

gboolean
gh_social_lists_set_sync_enabled(GhSocialLists *self, gboolean enabled, GError **error)
{
  if (!self->store) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account storage is not open");
    return FALSE;
  }
  if (enabled && (!self->network.accounts || !self->relays || (gh_store_is_ephemeral(self->store) || gh_store_is_read_only(self->store)))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Synchronization needs a writable account store and signer");
    return FALSE;
  }
  if (!gh_store_social_set_sync_enabled(self->store, enabled, error))
    return FALSE;
  self->sync_enabled = enabled;
  gnostr_list_service_set_private_sync_enabled(self->service, enabled);
  if (!update_plan(self, error)) return FALSE;
  g_signal_emit(self, signals[SYNC_STATE_CHANGED], 0);
  return TRUE;
}

GNostrListDocument *
gh_social_lists_ensure(GhSocialLists *self, gint kind, const gchar *identifier, GError **error)
{
  if (!self->service) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage is not open");
    return NULL;
  }
  GNostrListDocument *existing = gnostr_list_service_lookup(self->service, kind, identifier);
  if (existing)
    return existing;
  guint n = g_list_model_get_n_items(G_LIST_MODEL(self->service));
  if (n >= 256) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Too many account list documents");
    return NULL;
  }
  GNostrListReadPlanEntry entries[256];
  GPtrArray *snapshots = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; i < n; i++) {
    GNostrListDocument *snapshot = g_list_model_get_item(G_LIST_MODEL(self->service), i);
    g_ptr_array_add(snapshots, snapshot);
    entries[i] = local_plan(gnostr_list_document_get_kind(snapshot), gnostr_list_document_get_identifier(snapshot));
  }
  entries[n] = local_plan(kind, identifier);
  gboolean ok = gnostr_list_service_update_read_plan(self->service, entries, n + 1, error);
  g_ptr_array_unref(snapshots);
  if (ok) ok = update_plan(self, error);
  return ok ? gnostr_list_service_lookup(self->service, kind, identifier) : NULL;
}

GNostrListDocument *
gh_social_lists_apply(GhSocialLists *self, gint kind, const gchar *identifier, guint64 revision,
                      GPtrArray *operations, GError **error)
{
  if (!self->service) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "Account list storage is not open");
    return NULL;
  }
  return gnostr_list_service_apply(self->service, kind, identifier, revision, operations, error);
}

const gchar *
gh_social_lists_get_account(GhSocialLists *self)
{
  return self->store ? gh_store_get_account_pubkey(self->store) : NULL;
}

gboolean
gh_social_lists_hide_account(GhSocialLists *self, const gchar *pubkey, GError **error)
{
  gboolean valid = pubkey && strlen(pubkey) == 64;
  for (guint i = 0; valid && i < 64; i++)
    valid = g_ascii_isxdigit(pubkey[i]);
  if (!valid) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "An account needs a 64-character public key");
    return FALSE;
  }
  g_autofree gchar *lower = g_ascii_strdown(pubkey, -1);
  if (!g_strcmp0(lower, gh_social_lists_get_account(self))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "The active account cannot hide itself");
    return FALSE;
  }
  g_autoptr(GNostrListDocument) doc = gh_social_lists_ensure(self, NOSTR_KIND_MUTE_LIST, "", error);
  if (!doc)
    return FALSE;
  const NostrListDocument *codec = gnostr_list_document_get_codec(doc);
  for (guint p = 0; p < 2; p++)
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      if (!g_strcmp0(nostr_nip51_tag_field(tag, 0), "p") &&
          nostr_nip51_tag_field(tag, 1) &&
          !g_ascii_strcasecmp(nostr_nip51_tag_field(tag, 1), lower))
        return TRUE;
    }
  const gchar *fields[] = { "p", lower };
  g_autoptr(GPtrArray) operations = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  g_ptr_array_add(operations, gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, TRUE, fields, 2, 0, 0));
  g_autoptr(GNostrListDocument) saved = gh_social_lists_apply(self, NOSTR_KIND_MUTE_LIST, "",
    gnostr_list_document_get_local_revision(doc), operations, error);
  return saved != NULL;
}

gchar *
gh_social_lists_export(GNostrListDocument *snapshot)
{
  const NostrListDocument *codec = gnostr_list_document_get_codec(snapshot);
  json_t *root = json_pack("{s:i,s:s}", "kind", gnostr_list_document_get_kind(snapshot),
                          "identifier", gnostr_list_document_get_identifier(snapshot));
  for (guint p = 0; p < 2; p++) {
    json_t *tags = json_array();
    for (gsize i = 0; i < nostr_nip51_document_tag_count(codec, p); i++) {
      const NostrListTag *tag = nostr_nip51_document_tag(codec, p, i);
      json_t *fields = json_array();
      for (gsize f = 0; f < nostr_nip51_tag_size(tag); f++)
        json_array_append_new(fields, json_string(nostr_nip51_tag_field(tag, f)));
      json_array_append_new(tags, fields);
    }
    json_object_set_new(root, p ? "private" : "public", tags);
  }
  gchar *result = json_dumps(root, JSON_INDENT(2));
  json_decref(root);
  return result;
}

GNostrListDocument *
gh_social_lists_import(GhSocialLists *self, const gchar *wire, GError **error)
{
  if (!wire || strlen(wire) > NOSTR_LIST_MAX_FOLLOW_BYTES) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "List import exceeds the document limit");
    return NULL;
  }
  json_t *root = json_loads(wire, JSON_REJECT_DUPLICATES, NULL);
  json_t *k = json_object_get(root, "kind"), *id = json_object_get(root, "identifier");
  json_int_t kind_value = json_integer_value(k);
  gint kind = kind_value >= 0 && kind_value <= 65535 ? (gint)kind_value : -1;
  const gchar *identifier = json_string_value(id);
  GNostrListDocument *result = NULL;
  GPtrArray *ops = g_ptr_array_new_with_free_func((GDestroyNotify)gnostr_list_operation_unref);
  gboolean valid = json_is_integer(k) && kind >= 0 && kind <= 65535 && identifier &&
    strlen(identifier) == json_string_length(id);
  for (guint p = 0; valid && p < 2; p++) {
    json_t *tags = json_object_get(root, p ? "private" : "public");
    valid = json_is_array(tags) && json_array_size(tags) <= NOSTR_LIST_MAX_TAGS;
    for (gsize i = 0; valid && i < json_array_size(tags); i++) {
      json_t *tag = json_array_get(tags, i);
      gsize n = json_array_size(tag);
      const gchar *fields[NOSTR_LIST_MAX_FIELDS];
      valid = json_is_array(tag) && n > 0 && n <= NOSTR_LIST_MAX_FIELDS;
      for (gsize f = 0; valid && f < n; f++) {
        json_t *field = json_array_get(tag, f);
        fields[f] = json_string_value(field);
        valid = fields[f] && strlen(fields[f]) == json_string_length(field);
      }
      if (!valid)
        break;
      if (!strcmp(fields[0], "d")) {
        valid = !p && n == 2 && !strcmp(fields[1], identifier);
        continue;
      }
      GNostrListOperation *op = gnostr_list_operation_new(GNOSTR_LIST_EDIT_APPEND, p, fields, n, 0, 0);
      valid = op != NULL;
      if (op)
        g_ptr_array_add(ops, op);
    }
  }
  if (!valid) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid local list import");
  } else {
    g_autoptr(GNostrListDocument) snapshot = gh_social_lists_ensure(self, kind, identifier, error);
    if (snapshot) {
      if (gnostr_list_document_get_local_revision(snapshot) > 0 || gnostr_list_document_get_event_id(snapshot)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS, "A list with this address already exists; choose a new identifier");
      } else {
        result = gh_social_lists_apply(self, kind, identifier, 0, ops, error);
      }
    }
  }
  g_ptr_array_unref(ops);
  json_decref(root);
  return result;
}
