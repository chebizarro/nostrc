#include "gh-account-relays.h"
#include "gh-auth-policy.h"
#include "gh-identity.h"

#include <nostr-event.h>
#include <nostr-gobject-1.0/gnostr-relays.h>

#define KIND_RELAY_LIST 10002 /* NIP-65 */
#define KIND_DM_INBOX   10050 /* NIP-17 */

enum { SOURCE_PENDING, SOURCE_EOSE, SOURCE_FAILED };

/* The admitted replaceable event for one kind (NIP-01 ordering). */
typedef struct {
  gint64 created_at;
  gchar *id; /* NULL until one is admitted */
} Revision;

struct _GhAccountRelays {
  GObject parent_instance;
  GhAccountController *accounts; /* NULL once disposed */
  GSettings *settings;
  GhRelayTransport transport;
  gpointer transport_data;
  gboolean custom_transport;

  guint64 generation; /* 0 while no account is active */
  gchar *pubkey_hex;
  GhRelayScope *scope;
  GHashTable *sources; /* discovery URL -> SOURCE_* */
  GhAccountRelaysState state;

  Revision relay_list;
  Revision inbox;
  GStrv read;
  GStrv write;
  GStrv inbox_urls;
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhAccountRelays, gh_account_relays, G_TYPE_OBJECT)

static void
revision_clear(Revision *revision)
{
  g_clear_pointer(&revision->id, g_free);
  revision->created_at = 0;
}

/* Cancel first: the scope revokes its generation and closes every transport
 * before any list or state for another account exists. */
static void
teardown(GhAccountRelays *self)
{
  if (self->scope) {
    gh_relay_scope_cancel(self->scope);
    g_clear_pointer(&self->scope, gh_relay_scope_unref);
  }
  g_hash_table_remove_all(self->sources);
  revision_clear(&self->relay_list);
  revision_clear(&self->inbox);
  g_clear_pointer(&self->read, g_strfreev);
  g_clear_pointer(&self->write, g_strfreev);
  g_clear_pointer(&self->inbox_urls, g_strfreev);
  g_clear_pointer(&self->pubkey_hex, g_free);
  self->generation = 0;
}

static GhAccountRelaysState
compute_state(GhAccountRelays *self)
{
  if (!self->generation)
    return GH_ACCOUNT_RELAYS_INACTIVE;
  if (g_hash_table_size(self->sources) == 0)
    return GH_ACCOUNT_RELAYS_NO_SOURCES;
  gboolean any_eose = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->sources);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    if (GPOINTER_TO_UINT(value) == SOURCE_PENDING)
      return GH_ACCOUNT_RELAYS_DISCOVERING;
    any_eose |= GPOINTER_TO_UINT(value) == SOURCE_EOSE;
  }
  return any_eose ? GH_ACCOUNT_RELAYS_COMPLETE : GH_ACCOUNT_RELAYS_UNREACHABLE;
}

static void
update_state(GhAccountRelays *self, gboolean force_emit)
{
  GhAccountRelaysState state = compute_state(self);
  if (!force_emit && state == self->state)
    return;
  self->state = state;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static GStrv
steal_strv(GPtrArray *urls)
{
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(urls, FALSE);
}

static void
admit_event(GhAccountRelays *self, const GhRelayUpdate *update)
{
  NostrEvent *event = nostr_event_new();
  if (!event)
    return;
  gboolean authored = nostr_event_deserialize_signed(event, update->event_json, NULL) ==
                        NOSTR_EVENT_VALIDATION_OK &&
                      g_strcmp0(nostr_event_get_pubkey(event), self->pubkey_hex) == 0;
  int kind = nostr_event_get_kind(event);
  gint64 created_at = nostr_event_get_created_at(event);
  nostr_event_free(event);
  Revision *revision = kind == KIND_RELAY_LIST ? &self->relay_list
                     : kind == KIND_DM_INBOX   ? &self->inbox
                                               : NULL;
  /* A relay can answer with anything; only the account's own list counts. */
  if (!authored || !revision)
    return;
  if (revision->id &&
      (created_at < revision->created_at ||
       (created_at == revision->created_at &&
        g_strcmp0(update->event_id, revision->id) >= 0)))
    return;

  if (kind == KIND_RELAY_LIST) {
    /* A signed 10002 that does not parse still exists: it has no usable
     * relay, but nothing may publish over it (nostrc-0bdg). */
    GPtrArray *parsed = gnostr_nip65_parse_event(update->event_json, NULL);
    g_strfreev(self->read);
    g_strfreev(self->write);
    self->read = steal_strv(parsed ? gnostr_nip65_get_read_relays(parsed)
                                   : g_ptr_array_new_with_free_func(g_free));
    self->write = steal_strv(parsed ? gnostr_nip65_get_write_relays(parsed)
                                    : g_ptr_array_new_with_free_func(g_free));
    if (parsed)
      g_ptr_array_unref(parsed);
  } else {
    GPtrArray *parsed = gnostr_nip17_parse_dm_relays_event(update->event_json, NULL);
    if (!parsed)
      return;
    g_strfreev(self->inbox_urls);
    self->inbox_urls = steal_strv(parsed);
  }
  g_free(revision->id);
  revision->id = g_strdup(update->event_id);
  revision->created_at = created_at;
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhAccountRelays *self = data;
  /* The controller revokes a generation before "changed" reaches teardown;
   * anything delivered in that window belongs to the previous account. */
  if (scope != self->scope || !self->accounts ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return;
  guint status = GPOINTER_TO_UINT(g_hash_table_lookup(self->sources, update->url));
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    admit_event(self, update);
    return;
  case GH_RELAY_NOTICE_EOSE:
    status = SOURCE_EOSE;
    break;
  case GH_RELAY_NOTICE_ERROR:
    /* A source that already answered stays answered while it redials. */
    if (status == SOURCE_PENDING)
      status = SOURCE_FAILED;
    break;
  case GH_RELAY_NOTICE_CLOSED:
    /* The relay ended the REQ: no answer or live update will follow unless a
     * reconnect re-issues it, whose EOSE restores the source. */
    status = SOURCE_FAILED;
    break;
  default:
    return;
  }
  if (g_hash_table_contains(self->sources, update->url)) {
    g_hash_table_insert(self->sources, g_strdup(update->url), GUINT_TO_POINTER(status));
    update_state(self, FALSE);
  }
}

static NostrFilters *
account_filters(const gchar *pubkey_hex)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  if (!filters || !filter) {
    if (filters)
      nostr_filters_free(filters);
    if (filter)
      nostr_filter_free(filter);
    return NULL;
  }
  const int kinds[] = { KIND_RELAY_LIST, KIND_DM_INBOX };
  const char *const authors[] = { pubkey_hex };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_set_authors(filter, authors, G_N_ELEMENTS(authors));
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

static void
start_discovery(GhAccountRelays *self, const gchar *npub)
{
  self->pubkey_hex = gh_identity_pubkey_hex(npub);
  if (!self->pubkey_hex)
    return;
  g_auto(GStrv) urls = g_settings_get_strv(self->settings, "discovery-relays");
  if (!urls[0])
    return;
  NostrFilters *filters = account_filters(self->pubkey_hex);
  if (!filters)
    return;
  self->scope = self->custom_transport
    ? gh_relay_scope_new_with_transport(self->generation, filters, &self->transport,
                                        self->transport_data, on_scope_update, self)
    : gh_relay_scope_new(self->generation, filters, on_scope_update, self);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
  for (guint i = 0; urls[i]; i++) {
    g_autoptr(GError) error = NULL;
    if (!gh_relay_scope_add_url(self->scope, urls[i], &error)) {
      g_message("Groundhog ignores discovery relay \"%s\": %s", urls[i], error->message);
      continue;
    }
    /* Own list discovery (charter §4.3): a throwaway key, and only if the
     * relay demands AUTH for the REQ; never the account. */
    gh_auth_policy_apply_scope(policy, self->scope, GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY, urls[i],
                               NULL);
    g_hash_table_insert(self->sources, g_strdup(urls[i]), GUINT_TO_POINTER(SOURCE_PENDING));
  }
  if (g_hash_table_size(self->sources) == 0) {
    gh_relay_scope_cancel(self->scope);
    g_clear_pointer(&self->scope, gh_relay_scope_unref);
    return;
  }
  gh_relay_scope_start(self->scope);
}

/* restart rebuilds the scope even for the same account (sources changed). */
static void
rebind(GhAccountRelays *self, gboolean restart)
{
  if (!self->accounts)
    return;
  guint64 generation = 0;
  const gchar *npub = NULL;
  if (gh_account_controller_get_state(self->accounts) == GH_ACCOUNT_STATE_ACTIVE) {
    generation = gh_account_controller_get_generation(self->accounts);
    npub = gh_account_controller_get_active_npub(self->accounts);
  }
  if (!restart && generation == self->generation)
    return;
  teardown(self);
  self->generation = generation;
  if (generation)
    start_discovery(self, npub);
  update_state(self, TRUE);
}

static void
on_accounts_changed(GhAccountRelays *self)
{
  rebind(self, FALSE);
}

static void
on_sources_changed(GhAccountRelays *self)
{
  if (self->generation)
    rebind(self, TRUE);
}

GhAccountRelays *
gh_account_relays_new(GhAccountController *accounts, GSettings *settings,
                      const GhRelayTransport *transport, gpointer transport_data)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(accounts), NULL);
  g_return_val_if_fail(G_IS_SETTINGS(settings), NULL);
  g_return_val_if_fail(!transport || (transport->open && transport->close), NULL);
  GhAccountRelays *self = g_object_new(GH_TYPE_ACCOUNT_RELAYS, NULL);
  self->accounts = g_object_ref(accounts);
  self->settings = g_object_ref(settings);
  if (transport) {
    self->transport = *transport;
    self->transport_data = transport_data;
    self->custom_transport = TRUE;
  }
  g_signal_connect_object(accounts, "changed", G_CALLBACK(on_accounts_changed), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(settings, "changed::discovery-relays",
                          G_CALLBACK(on_sources_changed), self, G_CONNECT_SWAPPED);
  rebind(self, TRUE);
  return self;
}

GhAccountRelaysState
gh_account_relays_get_state(GhAccountRelays *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(self), GH_ACCOUNT_RELAYS_INACTIVE);
  return self->state;
}

guint64
gh_account_relays_get_generation(GhAccountRelays *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(self), 0);
  return self->generation;
}

const gchar *const *
gh_account_relays_get_read_relays(GhAccountRelays *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(self), NULL);
  return (const gchar *const *)self->read;
}

gboolean
gh_account_relays_has_relay_list(GhAccountRelays *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(self), FALSE);
  return self->relay_list.id != NULL;
}

const gchar *const *
gh_account_relays_get_write_relays(GhAccountRelays *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(self), NULL);
  return (const gchar *const *)self->write;
}

const gchar *const *
gh_account_relays_get_inbox_relays(GhAccountRelays *self)
{
  g_return_val_if_fail(GH_IS_ACCOUNT_RELAYS(self), NULL);
  return (const gchar *const *)self->inbox_urls;
}

static void
gh_account_relays_dispose(GObject *object)
{
  GhAccountRelays *self = GH_ACCOUNT_RELAYS(object);
  if (self->accounts) {
    g_signal_handlers_disconnect_by_data(self->accounts, self);
    g_signal_handlers_disconnect_by_data(self->settings, self);
    teardown(self);
    self->state = GH_ACCOUNT_RELAYS_INACTIVE;
    g_clear_object(&self->accounts);
    g_clear_object(&self->settings);
  }
  G_OBJECT_CLASS(gh_account_relays_parent_class)->dispose(object);
}

static void
gh_account_relays_finalize(GObject *object)
{
  GhAccountRelays *self = GH_ACCOUNT_RELAYS(object);
  g_hash_table_unref(self->sources);
  G_OBJECT_CLASS(gh_account_relays_parent_class)->finalize(object);
}

static void
gh_account_relays_class_init(GhAccountRelaysClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_account_relays_dispose;
  object_class->finalize = gh_account_relays_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass),
                                         G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
                                         G_TYPE_NONE, 0);
}

static void
gh_account_relays_init(GhAccountRelays *self)
{
  self->sources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->state = GH_ACCOUNT_RELAYS_INACTIVE;
}
