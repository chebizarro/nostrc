#include "gh-relay-list-setup.h"

#include "gh-auth-policy.h"
#include "gh-identity.h"

#include <glib/gi18n.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define KIND_RELAY_LIST GH_INBOX_SETUP_RELAY_LIST_KIND

enum { CHECK_PENDING, CHECK_NONE, CHECK_FAILED };

struct _GhRelayListSetup {
  GObject parent_instance;
  GhInboxSetupConfig config;   /* accounts, account_relays are owned refs */
  GhRelayListSetupState state;
  GhRelayListOffer mode;
  GError *error;
  guint64 generation;
  gchar *pubkey;
  GStrv write_relays;
  GStrv targets;
  gchar *base_json;            /* ADD_WRITE: the list extended */
  gchar *base_id;
  gint64 base_created_at;
  gint kind;                   /* EDIT: kind to check for (default KIND_RELAY_LIST) */
  gchar *edit_unsigned;        /* EDIT: the unsigned event the caller provided */
  GCancellable *cancellable;
  GhRelayScope *check;
  GHashTable *answers;         /* target URL -> CHECK_* */
  gboolean found;              /* a target holds another list */
  guint check_timer;
  GhRelayPublish *publish;
  guint accepted;
  gchar *signed_json;          /* signed, awaiting the second check (F1) */
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhRelayListSetup, gh_relay_list_setup, G_TYPE_OBJECT)

static void
emit_changed(GhRelayListSetup *self)
{
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

GhRelayListOffer
gh_relay_list_offer(const GhInboxSetupConfig *config)
{
  g_return_val_if_fail(config != NULL, GH_RELAY_LIST_OFFER_NONE);
  GhAccountRelays *relays = config->account_relays;
  GhAccountController *accounts = config->accounts;
  if (!config->offer_relay_list || !relays || !accounts ||
      gh_account_controller_get_state(accounts) != GH_ACCOUNT_STATE_ACTIVE ||
      gh_account_relays_get_generation(relays) != gh_account_controller_get_generation(accounts) ||
      gh_account_relays_get_state(relays) != GH_ACCOUNT_RELAYS_COMPLETE ||
      !gh_account_relays_get_all_answered(relays))
    return GH_RELAY_LIST_OFFER_NONE;
  if (!gh_account_relays_has_relay_list(relays))
    return GH_RELAY_LIST_OFFER_CREATE;
  const gchar *const *write = gh_account_relays_get_write_relays(relays);
  return (!write || !write[0]) && gh_account_relays_get_relay_list_json(relays)
           ? GH_RELAY_LIST_OFFER_ADD_WRITE : GH_RELAY_LIST_OFFER_NONE;
}

/* ---- ending ---------------------------------------------------------------- */

static void
stop_network(GhRelayListSetup *self)
{
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  if (self->check_timer) {
    g_source_remove(self->check_timer);
    self->check_timer = 0;
  }
  if (self->check) {
    gh_relay_scope_cancel(self->check);
    g_clear_pointer(&self->check, gh_relay_scope_unref);
  }
  if (self->publish)
    gh_relay_publish_cancel(self->publish);
}

static gboolean
running(GhRelayListSetup *self)
{
  return self->state == GH_RELAY_LIST_SETUP_CHECKING ||
         self->state == GH_RELAY_LIST_SETUP_SIGNING ||
         self->state == GH_RELAY_LIST_SETUP_PUBLISHING;
}

static void
finish(GhRelayListSetup *self, GhRelayListSetupState state, GError *error)
{
  if (!running(self)) {
    g_clear_error(&error);
    return;
  }
  stop_network(self);
  g_clear_error(&self->error);
  self->error = error;
  self->state = state;
  emit_changed(self);
}

static void
fail(GhRelayListSetup *self, const gchar *message)
{
  finish(self, GH_RELAY_LIST_SETUP_FAILED, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                                              message));
}

/* ---- the list ---------------------------------------------------------------- */

static gboolean
same_url(const gchar *a, const gchar *b)
{
  g_autofree gchar *na = gh_inbox_setup_normalize_url(a, NULL);
  g_autofree gchar *nb = gh_inbox_setup_normalize_url(b, NULL);
  return na && nb && g_str_equal(na, nb);
}

/* ADD_WRITE: the base list with the write relays added ("read" entries of
 * them unmarked), everything else as it is; dated after the base. */
static gchar *
build_extended(GhRelayListSetup *self, gint64 now)
{
  NostrEvent *base = nostr_event_new();
  if (!base || nostr_event_deserialize_compact(base, self->base_json, NULL) != 1) {
    if (base)
      nostr_event_free(base);
    return NULL;
  }
  NostrTags *in = nostr_event_get_tags(base);
  NostrTags *tags = nostr_tags_new(0);
  gsize n_write = g_strv_length(self->write_relays);
  g_autofree gboolean *covered = g_new0(gboolean, n_write + 1);
  for (size_t i = 0; in && i < nostr_tags_size(in); i++) {
    NostrTag *tag = nostr_tags_get(in, i);
    if (!tag || nostr_tag_size(tag) < 1)
      continue;
    gboolean is_r = g_strcmp0(nostr_tag_get(tag, 0), "r") == 0 && nostr_tag_size(tag) >= 2;
    gint match = -1;
    for (gsize w = 0; is_r && w < n_write && match < 0; w++)
      if (same_url(nostr_tag_get(tag, 1), self->write_relays[w]))
        match = (gint)w;
    if (match >= 0 && nostr_tag_size(tag) >= 3 &&
        g_strcmp0(nostr_tag_get(tag, 2), "read") == 0) {
      /* Read and write now: unmarked. */
      nostr_tags_append(tags, nostr_tag_new("r", nostr_tag_get(tag, 1), NULL));
      covered[match] = TRUE;
      continue;
    }
    if (match >= 0)
      covered[match] = TRUE;
    NostrTag *copy = nostr_tag_new(nostr_tag_get(tag, 0), NULL);
    for (size_t j = 1; j < nostr_tag_size(tag); j++)
      nostr_tag_append(copy, nostr_tag_get(tag, j));
    nostr_tags_append(tags, copy);
  }
  for (gsize w = 0; w < n_write; w++)
    if (!covered[w])
      nostr_tags_append(tags, nostr_tag_new("r", self->write_relays[w], "write", NULL));
  NostrEvent *event = nostr_event_new();
  nostr_event_set_kind(event, KIND_RELAY_LIST);
  nostr_event_set_created_at(event, MAX(now, self->base_created_at + 1));
  const gchar *content = nostr_event_get_content(base);
  nostr_event_set_content(event, content ? content : "");
  nostr_event_set_pubkey(event, self->pubkey);
  nostr_event_set_tags(event, tags);
  nostr_event_free(base);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *json = raw ? g_strdup(raw) : NULL;
  free(raw);
  return json;
}

/* ---- publish ----------------------------------------------------------------- */

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  GhRelayListSetup *self = data;
  if (publish != self->publish)
    return;
  if (result->outcome == GH_RELAY_PUBLISH_ACCEPTED)
    self->accepted++;
  emit_changed(self);
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  GhRelayListSetup *self = data;
  if (publish != self->publish || self->state != GH_RELAY_LIST_SETUP_PUBLISHING)
    return;
  if (summary->any_accepted)
    finish(self, GH_RELAY_LIST_SETUP_DONE, NULL);
  else
    fail(self, _("No relay accepted your relay list."));
}

static gboolean start_check(GhRelayListSetup *self, GError **error);

static gboolean
start_publish(GhRelayListSetup *self, const gchar *signed_json, GError **error)
{
  const GhInboxSetupConfig *config = &self->config;
  self->publish = config->publish_transport
    ? gh_relay_publish_new_with_transport(self->generation, signed_json,
                                          config->publish_transport,
                                          config->publish_transport_data, on_publish_update,
                                          on_publish_done, self, error)
    : gh_relay_publish_new(self->generation, signed_json, on_publish_update, on_publish_done,
                           self, error);
  if (!self->publish)
    return FALSE;
  if (config->publish_transport && config->publish_auth_transport)
    gh_relay_publish_set_auth_transport(self->publish, config->publish_auth_transport);
  /* Own list publish (charter §4.3): account AUTH only on challenge. */
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(config->accounts);
  for (guint i = 0; self->targets[i]; i++)
    if (!gh_relay_publish_add_url(self->publish, self->targets[i], error) ||
        !gh_auth_policy_apply_publish(policy, self->publish, GH_AUTH_PURPOSE_OWN_LIST_PUBLISH,
                                      self->targets[i], error))
      return FALSE;
  self->state = GH_RELAY_LIST_SETUP_PUBLISHING;
  emit_changed(self);
  return gh_relay_publish_start(self->publish, error);
}

static void
on_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  GhRelayListSetup *self = data; /* a reference held for this call */
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  if (self->state != GH_RELAY_LIST_SETUP_SIGNING) {
    g_object_unref(self);
    return;
  }
  NostrEvent *event = signed_json ? nostr_event_new() : NULL;
  gboolean ok = event && nostr_event_deserialize_signed(event, signed_json, NULL) ==
                           NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_get_kind(event) == (self->kind ? self->kind : KIND_RELAY_LIST) &&
                g_strcmp0(nostr_event_get_pubkey(event), self->pubkey) == 0;
  if (event)
    nostr_event_free(event);
  if (!signed_json)
    finish(self, GH_RELAY_LIST_SETUP_FAILED, g_steal_pointer(&error));
  else if (!ok || !gh_account_controller_is_current(self->config.accounts, self->generation))
    fail(self, _("The relay list was not signed for this account."));
  else {
    /* The signer may have taken minutes: another client may have published
     * a list meanwhile. Every target is asked again, right before
     * publishing (final review F1). */
    self->signed_json = g_steal_pointer(&signed_json);
    if (!start_check(self, &error))
      finish(self, GH_RELAY_LIST_SETUP_FAILED, g_steal_pointer(&error));
  }
  g_object_unref(self);
}

static void
sign_list(GhRelayListSetup *self)
{
  /* Discovery may have found a list meanwhile: the offer must still hold.
   * EDIT mode is externally driven: skip the offer re-check. */
  if (self->mode != GH_RELAY_LIST_OFFER_EDIT &&
      gh_relay_list_offer(&self->config) != self->mode) {
    finish(self, GH_RELAY_LIST_SETUP_SKIPPED, NULL);
    return;
  }
  gint64 now = g_get_real_time() / G_USEC_PER_SEC;
  g_autofree gchar *unsigned_json = NULL;
  if (self->mode == GH_RELAY_LIST_OFFER_EDIT)
    unsigned_json = g_strdup(self->edit_unsigned);
  else if (self->mode == GH_RELAY_LIST_OFFER_CREATE)
    unsigned_json = gh_inbox_setup_build_relay_list_unsigned(self->pubkey,
                                                 (const gchar *const *)self->write_relays, now);
  else
    unsigned_json = build_extended(self, now);
  if (!unsigned_json) {
    fail(self, _("Could not build the relay list."));
    return;
  }
  self->state = GH_RELAY_LIST_SETUP_SIGNING;
  emit_changed(self);
  gh_account_controller_sign_with_cancellable_async(self->config.accounts, unsigned_json,
                                                    self->cancellable, on_signed,
                                                    g_object_ref(self));
}

/* ---- the pre-publish check --------------------------------------------------- */

static void
check_settled(GhRelayListSetup *self)
{
  if (self->state != GH_RELAY_LIST_SETUP_CHECKING)
    return;
  if (self->found) {
    finish(self, GH_RELAY_LIST_SETUP_SKIPPED, NULL);
    return;
  }
  gboolean failed = FALSE;
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->answers);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    if (GPOINTER_TO_UINT(value) == CHECK_PENDING)
      return;
    failed |= GPOINTER_TO_UINT(value) == CHECK_FAILED;
  }
  if (self->check_timer) {
    g_source_remove(self->check_timer);
    self->check_timer = 0;
  }
  gh_relay_scope_cancel(self->check);
  g_clear_pointer(&self->check, gh_relay_scope_unref);
  if (failed) {
    fail(self, _("Not every relay answered, so Groundhog couldn't confirm that you have no "
                 "relay list there. Nothing was published."));
    return;
  }
  if (!self->signed_json) {
    sign_list(self);
    return;
  }
  /* Second check, after the signer: still clean, and still the offer. */
  if (self->mode != GH_RELAY_LIST_OFFER_EDIT &&
      gh_relay_list_offer(&self->config) != self->mode) {
    finish(self, GH_RELAY_LIST_SETUP_SKIPPED, NULL);
    return;
  }
  g_autoptr(GError) error = NULL;
  if (!start_publish(self, self->signed_json, &error))
    finish(self, GH_RELAY_LIST_SETUP_FAILED, g_steal_pointer(&error));
}

/* Whether event_json is a signed kind 10002 by the account that is not the
 * list being extended (nor older than it). */
static gboolean
other_list(GhRelayListSetup *self, const gchar *event_json, const gchar *event_id)
{
  NostrEvent *event = nostr_event_new();
  gint check_kind = self->kind ? self->kind : KIND_RELAY_LIST;
  gboolean mine = event && nostr_event_deserialize_signed(event, event_json, NULL) ==
                             NOSTR_EVENT_VALIDATION_OK &&
                  nostr_event_get_kind(event) == check_kind &&
                  g_strcmp0(nostr_event_get_pubkey(event), self->pubkey) == 0;
  gint64 created_at = mine ? nostr_event_get_created_at(event) : 0;
  if (event)
    nostr_event_free(event);
  if (!mine)
    return FALSE;
  if (self->mode == GH_RELAY_LIST_OFFER_CREATE)
    return TRUE;
  /* ADD_WRITE and EDIT: the base event is ours; a newer one is another client's. */
  if (g_strcmp0(event_id, self->base_id) == 0)
    return FALSE;
  return created_at >= self->base_created_at;
}

static void
on_check_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhRelayListSetup *self = data;
  if (scope != self->check || !update->url ||
      !g_hash_table_contains(self->answers, update->url))
    return;
  guint answer = GPOINTER_TO_UINT(g_hash_table_lookup(self->answers, update->url));
  switch (update->notice) {
  case GH_RELAY_NOTICE_EVENT:
    if (other_list(self, update->event_json, update->event_id)) {
      self->found = TRUE;
      check_settled(self);   /* the user's list: nothing to wait for */
    }
    return;
  case GH_RELAY_NOTICE_EOSE:
    if (answer == CHECK_PENDING)
      answer = CHECK_NONE;
    break;
  case GH_RELAY_NOTICE_CLOSED:
  case GH_RELAY_NOTICE_DISCONNECTED:
  case GH_RELAY_NOTICE_ERROR:
    if (answer == CHECK_PENDING)
      answer = CHECK_FAILED;
    break;
  default:
    return;
  }
  g_hash_table_insert(self->answers, g_strdup(update->url), GUINT_TO_POINTER(answer));
  check_settled(self);
}

static gboolean
check_deadline(gpointer data)
{
  GhRelayListSetup *self = data;
  self->check_timer = 0;
  /* A relay that did not answer is unknown, never "none". */
  GHashTableIter iter;
  gpointer key, value;
  g_autoptr(GPtrArray) silent = g_ptr_array_new();
  g_hash_table_iter_init(&iter, self->answers);
  while (g_hash_table_iter_next(&iter, &key, &value))
    if (GPOINTER_TO_UINT(value) == CHECK_PENDING)
      g_ptr_array_add(silent, key);
  for (guint i = 0; i < silent->len; i++)
    g_hash_table_insert(self->answers, g_strdup(g_ptr_array_index(silent, i)),
                        GUINT_TO_POINTER(CHECK_FAILED));
  check_settled(self);
  return G_SOURCE_REMOVE;
}

static gboolean
start_check(GhRelayListSetup *self, GError **error)
{
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  const int kinds[] = { self->kind ? self->kind : KIND_RELAY_LIST };
  const char *const authors[] = { self->pubkey };
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_set_authors(filter, authors, G_N_ELEMENTS(authors));
  nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  const GhInboxSetupConfig *config = &self->config;
  g_hash_table_remove_all(self->answers);
  self->found = FALSE;
  self->check = config->probe_transport
    ? gh_relay_scope_new_with_transport(self->generation, filters, config->probe_transport,
                                        config->probe_transport_data, on_check_update, self)
    : gh_relay_scope_new(self->generation, filters, on_check_update, self);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(config->accounts);
  for (guint i = 0; self->targets[i]; i++) {
    if (!gh_relay_scope_add_url(self->check, self->targets[i], error))
      return FALSE;
    /* Own list discovery (§4.3): a throwaway key on demand, never the account. */
    gh_auth_policy_apply_scope(policy, self->check, GH_AUTH_PURPOSE_OWN_LIST_DISCOVERY,
                               self->targets[i], NULL);
    g_hash_table_insert(self->answers, g_strdup(self->targets[i]),
                        GUINT_TO_POINTER(CHECK_PENDING));
  }
  self->state = GH_RELAY_LIST_SETUP_CHECKING;
  self->check_timer = g_timeout_add_seconds(GH_RELAY_LIST_SETUP_CHECK_S, check_deadline, self);
  emit_changed(self);
  gh_relay_scope_start(self->check);
  return TRUE;
}

/* ---- public ------------------------------------------------------------------ */

static void
add_unique(GPtrArray *urls, const gchar *url)
{
  g_autofree gchar *normal = gh_inbox_setup_normalize_url(url, NULL);
  if (normal && !g_ptr_array_find_with_equal_func(urls, normal, g_str_equal, NULL))
    g_ptr_array_add(urls, g_steal_pointer(&normal));
}

gboolean
gh_relay_list_setup_start(GhRelayListSetup *self, const gchar *const *write_relays,
                          const gchar *const *targets, GError **error)
{
  g_return_val_if_fail(GH_IS_RELAY_LIST_SETUP(self), FALSE);
  if (self->state != GH_RELAY_LIST_SETUP_IDLE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "this setup already ran");
    return FALSE;
  }
  self->mode = gh_relay_list_offer(&self->config);
  if (self->mode == GH_RELAY_LIST_OFFER_NONE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        _("There is no relay list to set up for this account."));
    return FALSE;
  }
  g_autoptr(GPtrArray) write = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; write_relays && write_relays[i]; i++)
    add_unique(write, write_relays[i]);
  g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; i < write->len; i++)
    add_unique(all, g_ptr_array_index(write, i));
  for (guint i = 0; targets && targets[i]; i++)
    add_unique(all, targets[i]);
  if (write->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Choose at least one relay."));
    return FALSE;
  }
  GhAccountController *accounts = self->config.accounts;
  self->pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(accounts));
  if (!self->pubkey) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        _("Choose an account first."));
    return FALSE;
  }
  if (self->mode == GH_RELAY_LIST_OFFER_ADD_WRITE) {
    self->base_json = g_strdup(gh_account_relays_get_relay_list_json(self->config.account_relays));
    NostrEvent *base = nostr_event_new();
    if (base && nostr_event_deserialize_compact(base, self->base_json, NULL) == 1) {
      char *id = nostr_event_get_id(base);
      self->base_id = id ? g_strdup(id) : NULL;
      free(id);
      self->base_created_at = nostr_event_get_created_at(base);
    }
    if (base)
      nostr_event_free(base);
  }
  g_ptr_array_add(write, NULL);
  g_ptr_array_add(all, NULL);
  self->write_relays = (GStrv)g_ptr_array_free(g_steal_pointer(&write), FALSE);
  self->targets = (GStrv)g_ptr_array_free(g_steal_pointer(&all), FALSE);
  self->generation = gh_account_controller_get_generation(accounts);
  self->cancellable = g_cancellable_new();
  return start_check(self, error);
}

gboolean
gh_relay_list_setup_start_edit(GhRelayListSetup *self, const gchar *unsigned_json,
                               const gchar *base_id, gint64 base_created_at,
                               gint kind, const gchar *const *targets, GError **error)
{
  g_return_val_if_fail(GH_IS_RELAY_LIST_SETUP(self), FALSE);
  if (self->state != GH_RELAY_LIST_SETUP_IDLE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "this setup already ran");
    return FALSE;
  }
  if (!unsigned_json || !*unsigned_json) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "no event to sign");
    return FALSE;
  }
  g_autoptr(GPtrArray) all = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; targets && targets[i]; i++)
    add_unique(all, targets[i]);
  if (all->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        _("Choose at least one relay."));
    return FALSE;
  }
  GhAccountController *accounts = self->config.accounts;
  self->pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(accounts));
  if (!self->pubkey) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        _("Choose an account first."));
    return FALSE;
  }
  self->mode = GH_RELAY_LIST_OFFER_EDIT;
  self->kind = kind;
  self->edit_unsigned = g_strdup(unsigned_json);
  self->base_id = g_strdup(base_id);
  self->base_created_at = base_created_at;
  g_ptr_array_add(all, NULL);
  self->targets = (GStrv)g_ptr_array_free(g_steal_pointer(&all), FALSE);
  self->generation = gh_account_controller_get_generation(accounts);
  self->cancellable = g_cancellable_new();
  return start_check(self, error);
}

void
gh_relay_list_setup_cancel(GhRelayListSetup *self)
{
  g_return_if_fail(GH_IS_RELAY_LIST_SETUP(self));
  finish(self, GH_RELAY_LIST_SETUP_FAILED,
         g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, _("Publishing was stopped.")));
}

GhRelayListSetupState
gh_relay_list_setup_get_state(GhRelayListSetup *self)
{
  g_return_val_if_fail(GH_IS_RELAY_LIST_SETUP(self), GH_RELAY_LIST_SETUP_IDLE);
  return self->state;
}

GhRelayListOffer
gh_relay_list_setup_get_mode(GhRelayListSetup *self)
{
  g_return_val_if_fail(GH_IS_RELAY_LIST_SETUP(self), GH_RELAY_LIST_OFFER_NONE);
  return self->mode;
}

guint
gh_relay_list_setup_get_n_accepted(GhRelayListSetup *self)
{
  g_return_val_if_fail(GH_IS_RELAY_LIST_SETUP(self), 0);
  return self->accepted;
}

const GError *
gh_relay_list_setup_get_error(GhRelayListSetup *self)
{
  g_return_val_if_fail(GH_IS_RELAY_LIST_SETUP(self), NULL);
  return self->error;
}

static void
on_accounts_changed(GhRelayListSetup *self)
{
  if (running(self) &&
      !gh_account_controller_is_current(self->config.accounts, self->generation))
    finish(self, GH_RELAY_LIST_SETUP_FAILED,
           g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, _("The account changed.")));
}

GhRelayListSetup *
gh_relay_list_setup_new(const GhInboxSetupConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  GhRelayListSetup *self = g_object_new(GH_TYPE_RELAY_LIST_SETUP, NULL);
  self->config = *config;
  g_object_ref(self->config.accounts);
  if (self->config.account_relays)
    g_object_ref(self->config.account_relays);
  self->config.settings = NULL;   /* not used */
  g_signal_connect_object(self->config.accounts, "changed", G_CALLBACK(on_accounts_changed),
                          self, G_CONNECT_SWAPPED);
  return self;
}

static void
gh_relay_list_setup_dispose(GObject *object)
{
  GhRelayListSetup *self = GH_RELAY_LIST_SETUP(object);
  if (running(self))
    self->state = GH_RELAY_LIST_SETUP_FAILED;
  stop_network(self);
  g_clear_pointer(&self->publish, gh_relay_publish_unref);
  g_clear_object(&self->cancellable);
  g_clear_object(&self->config.account_relays);
  g_clear_object(&self->config.accounts);
  G_OBJECT_CLASS(gh_relay_list_setup_parent_class)->dispose(object);
}

static void
gh_relay_list_setup_finalize(GObject *object)
{
  GhRelayListSetup *self = GH_RELAY_LIST_SETUP(object);
  g_clear_error(&self->error);
  g_free(self->pubkey);
  g_strfreev(self->write_relays);
  g_strfreev(self->targets);
  g_free(self->base_json);
  g_free(self->base_id);
  g_free(self->signed_json);
  g_free(self->edit_unsigned);
  g_hash_table_unref(self->answers);
  G_OBJECT_CLASS(gh_relay_list_setup_parent_class)->finalize(object);
}

static void
gh_relay_list_setup_class_init(GhRelayListSetupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_relay_list_setup_dispose;
  object_class->finalize = gh_relay_list_setup_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_relay_list_setup_init(GhRelayListSetup *self)
{
  self->state = GH_RELAY_LIST_SETUP_IDLE;
  self->answers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}
