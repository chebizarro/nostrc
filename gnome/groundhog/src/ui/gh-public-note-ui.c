#include "gh-public-note-ui.h"
#include "gh-public-post-dialog.h"
#include "gh-conversation-view.h"
#include "gh-relay-scope.h"
#include "gh-auth-policy.h"
#include "gh-store-public-notes.h"
#include <nostr-filter.h>
#include <glib/gi18n.h>

#define MAX_FIND_SOURCES 16u
#define FIND_DEADLINE_SECONDS 20u

typedef struct _GhPublicNoteUi GhPublicNoteUi;
typedef GObjectClass GhPublicNoteUiClass;
struct _GhPublicNoteUi {
  GObject parent_instance;
  GWeakRef view;
  GhAccountController *accounts;
  GhAccountStore *store;
  GhAccountRelays *relays;
  GSettings *settings;
  guint64 generation;
  GHashTable *by_id;       /* id -> GhPublicNote, owned */
  GHashTable *by_address;  /* coordinate -> borrowed GhPublicNote */
  GhRelayScope *find_scope;
  GnNostrReference *find_reference;
  GhPublicNote *find_best;
  GHashTable *find_pending; /* URL -> pending (1) or settled (2) */
  guint find_left;
  guint find_failed;
  guint find_deadline;
};
G_DEFINE_FINAL_TYPE(GhPublicNoteUi, gh_public_note_ui, G_TYPE_OBJECT)

static gchar *
address_key(gint kind, const gchar *pubkey, const gchar *dtag)
{
  return pubkey && dtag ? g_strdup_printf("%d:%s:%s", kind, pubkey, dtag) : NULL;
}

static gboolean
newer(const GhPublicNote *a, const GhPublicNote *b)
{
  return !b || a->created_at > b->created_at ||
    (a->created_at == b->created_at && g_strcmp0(a->id, b->id) < 0);
}

static void
rebuild_addresses(GhPublicNoteUi *self)
{
  g_hash_table_remove_all(self->by_address);
  GHashTableIter iter;
  gpointer value;
  g_hash_table_iter_init(&iter, self->by_id);
  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    GhPublicNote *note = value;
    g_autofree gchar *key = address_key(note->kind, note->pubkey, note->dtag);
    if (!key) continue;
    GhPublicNote *old = g_hash_table_lookup(self->by_address, key);
    if (newer(note, old))
      g_hash_table_replace(self->by_address, g_steal_pointer(&key), note);
  }
}

static void
cache_note(GhPublicNoteUi *self, GhPublicNote *note)
{
  g_hash_table_replace(self->by_id, g_strdup(note->id), note);
  rebuild_addresses(self);
  g_autoptr(GhConversationView) view = g_weak_ref_get(&self->view);
  if (view) gh_conversation_view_references_changed(view);
}

static const GhPublicNote *
lookup(GhPublicNoteUi *self, const GnNostrReference *reference)
{
  if (!reference || reference->type == GN_NOSTR_REFERENCE_PERSON || !reference->id)
    return NULL;
  if (reference->type == GN_NOSTR_REFERENCE_EVENT)
    return g_hash_table_lookup(self->by_id, reference->id);
  if (!reference->author || reference->kind <= 0) return NULL;
  g_autofree gchar *key = address_key(reference->kind, reference->author, reference->id);
  return g_hash_table_lookup(self->by_address, key);
}

static gchar *
summary(const GnNostrReference *reference, gpointer data)
{
  GhPublicNoteUi *self = data;
  const GhPublicNote *note = lookup(self, reference);
  if (!note) return NULL;
  return g_strdup_printf(_("Public event by %.12s: %s"), note->pubkey,
                         note->preview ? note->preview : "");
}

static void
notice(GhPublicNoteUi *self, const gchar *title, const gchar *body)
{
  g_autoptr(GhConversationView) view = g_weak_ref_get(&self->view);
  if (!view) return;
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(title, body));
  adw_alert_dialog_add_response(alert, "ok", _("OK"));
  adw_dialog_present(ADW_DIALOG(alert), GTK_WIDGET(view));
}

static GhStore *
writable_store(GhPublicNoteUi *self)
{
  GhAccountStoreState state = gh_account_store_get_state(self->store);
  if ((state != GH_ACCOUNT_STORE_OPEN && state != GH_ACCOUNT_STORE_EPHEMERAL) ||
      gh_account_store_get_generation(self->store) != self->generation ||
      !gh_account_controller_is_current(self->accounts, self->generation))
    return NULL;
  GhStore *store = gh_account_store_get_store(self->store);
  return store && !gh_store_is_read_only(store) ? store : NULL;
}

static void
cancel_find(GhPublicNoteUi *self)
{
  g_clear_handle_id(&self->find_deadline, g_source_remove);
  if (self->find_scope) {
    gh_relay_scope_cancel(self->find_scope);
    gh_relay_scope_unref(self->find_scope);
    self->find_scope = NULL;
  }
  g_clear_pointer(&self->find_reference, gn_nostr_reference_free);
  g_clear_pointer(&self->find_best, gh_public_note_free);
  g_clear_pointer(&self->find_pending, g_hash_table_unref);
  self->find_left = 0;
  self->find_failed = 0;
}

static void
reload(GhPublicNoteUi *self)
{
  guint64 generation = gh_account_controller_get_generation(self->accounts);
  if (self->generation != generation) {
    cancel_find(self);
    self->generation = generation;
    g_hash_table_remove_all(self->by_address);
    g_hash_table_remove_all(self->by_id);
  }
  /* An in-flight REQ belongs to the account's writable cache. Closing or
   * locking that store withdraws consent for the lookup, even if the account
   * generation itself has not changed. */
  if (self->find_scope && !writable_store(self)) cancel_find(self);
  GhAccountStoreState state = gh_account_store_get_state(self->store);
  if ((state != GH_ACCOUNT_STORE_OPEN && state != GH_ACCOUNT_STORE_EPHEMERAL &&
       state != GH_ACCOUNT_STORE_CORRUPT) ||
      gh_account_store_get_generation(self->store) != generation) {
    g_hash_table_remove_all(self->by_address);
    g_hash_table_remove_all(self->by_id);
  } else {
    GhStore *store = gh_account_store_get_store(self->store);
    g_autoptr(GError) error = NULL;
    g_autoptr(GPtrArray) notes = store ? gh_store_public_notes_load(store, &error) : NULL;
    g_hash_table_remove_all(self->by_address);
    g_hash_table_remove_all(self->by_id);
    for (guint i = 0; notes && i < notes->len; i++) {
      GhPublicNote *note = g_ptr_array_index(notes, i);
      g_hash_table_insert(self->by_id, g_strdup(note->id), note);
      g_ptr_array_index(notes, i) = NULL;
    }
    rebuild_addresses(self);
    if (error) g_warning("Groundhog could not load verified public-note cache: %s", error->message);
  }
  g_autoptr(GhConversationView) view = g_weak_ref_get(&self->view);
  if (view) gh_conversation_view_references_changed(view);
}

static gboolean
reference_matches_note(const GnNostrReference *reference, const GhPublicNote *note)
{
  if (reference->type == GN_NOSTR_REFERENCE_EVENT)
    return g_ascii_strcasecmp(reference->id, note->id) == 0;
  return reference->type == GN_NOSTR_REFERENCE_ADDRESS && reference->author &&
    note->dtag && reference->kind == note->kind &&
    g_ascii_strcasecmp(reference->author, note->pubkey) == 0 &&
    g_strcmp0(reference->id, note->dtag) == 0;
}

static void
finish_find(GhPublicNoteUi *self, gboolean timed_out)
{
  if (!self->find_scope) return;
  if (!gh_account_controller_is_current(self->accounts, self->generation)) {
    cancel_find(self);
    return;
  }
  if (self->find_best) {
    GhStore *store = writable_store(self);
    g_autoptr(GError) error = NULL;
    if (store && gh_store_public_note_put(store, self->find_best, &error)) {
      GhPublicNote *note = g_steal_pointer(&self->find_best);
      cache_note(self, note);
      const gboolean ephemeral = gh_account_store_get_state(self->store) == GH_ACCOUNT_STORE_EPHEMERAL;
      notice(self, _("Public Note Found"),
        ephemeral && (timed_out || self->find_failed)
          ? _("A signed event is available in memory until Groundhog closes, not on disk. Some discovery relays did not answer; this may not be the newest addressable event.")
          : ephemeral
            ? _("A matching signed public event is available in memory until Groundhog closes. It was not saved to disk.")
          : timed_out || self->find_failed
            ? _("A signed event was saved locally. Some discovery relays did not answer; this may not be the newest addressable event.")
            : _("A matching signed public event was saved locally from the selected discovery relays."));
    } else
      notice(self, _("Could Not Save Public Note"),
             error ? error->message : _("Unlock the encrypted message store, then try again."));
  } else
    notice(self, _("Public Note Not Found"),
      timed_out || self->find_failed
        ? _("No matching signed event arrived; some discovery relays did not answer.")
        : _("The selected discovery relays had no matching signed event."));
  cancel_find(self);
}

static gboolean
find_deadline(gpointer data)
{
  GhPublicNoteUi *self = data;
  self->find_deadline = 0;
  finish_find(self, TRUE);
  return G_SOURCE_REMOVE;
}

static void
find_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhPublicNoteUi *self = data;
  if (scope != self->find_scope ||
      !gh_account_controller_is_current(self->accounts, self->generation)) return;
  if (update->notice == GH_RELAY_NOTICE_EVENT && update->event_json) {
    g_autoptr(GhPublicNote) note = gh_public_note_from_signed_json(update->event_json, NULL);
    if (note && reference_matches_note(self->find_reference, note) &&
        newer(note, self->find_best)) {
      g_clear_pointer(&self->find_best, gh_public_note_free);
      self->find_best = g_steal_pointer(&note);
      if (self->find_reference->type == GN_NOSTR_REFERENCE_EVENT) {
        finish_find(self, FALSE);
        return;
      }
    }
  }
  if (update->notice != GH_RELAY_NOTICE_EOSE &&
      update->notice != GH_RELAY_NOTICE_CLOSED &&
      update->notice != GH_RELAY_NOTICE_ERROR &&
      update->notice != GH_RELAY_NOTICE_DISCONNECTED) return;
  if (GPOINTER_TO_UINT(g_hash_table_lookup(self->find_pending, update->url)) != 1) return;
  g_hash_table_replace(self->find_pending, g_strdup(update->url), GUINT_TO_POINTER(2));
  self->find_left--;
  if (update->notice != GH_RELAY_NOTICE_EOSE) self->find_failed++;
  if (!self->find_left) finish_find(self, FALSE);
}

static void
start_find(GhPublicNoteUi *self, const gchar *uri, const gchar *const *urls)
{
  cancel_find(self);
  if (!writable_store(self)) {
    notice(self, _("Find on Relays Unavailable"),
      _("Unlock the encrypted message store before searching for a public note."));
    return;
  }
  self->find_reference = gn_nostr_reference_parse(uri);
  if (!self->find_reference || !self->find_reference->id ||
      (self->find_reference->type != GN_NOSTR_REFERENCE_EVENT &&
       self->find_reference->type != GN_NOSTR_REFERENCE_ADDRESS)) {
    cancel_find(self);
    return;
  }
  NostrFilters *filters = nostr_filters_new();
  NostrFilter *filter = nostr_filter_new();
  if (!filters || !filter) {
    if (filters) nostr_filters_free(filters);
    if (filter) nostr_filter_free(filter);
    cancel_find(self);
    return;
  }
  if (self->find_reference->type == GN_NOSTR_REFERENCE_EVENT) {
    const char *ids[] = { self->find_reference->id };
    nostr_filter_set_ids(filter, ids, 1);
  } else {
    const char *authors[] = { self->find_reference->author };
    const int kinds[] = { self->find_reference->kind };
    nostr_filter_set_authors(filter, authors, 1);
    nostr_filter_set_kinds(filter, kinds, 1);
    nostr_filter_tags_append(filter, "d", self->find_reference->id, NULL);
    nostr_filter_set_limit(filter, 20);
  }
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter);
  if (!added) { nostr_filters_free(filters); cancel_find(self); return; }
  self->find_pending = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  self->find_scope = gh_relay_scope_new(self->generation, filters, find_update, self);
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(self->accounts);
  for (guint i = 0; urls[i]; i++) {
    if (gh_relay_scope_add_url(self->find_scope, urls[i], NULL)) {
      gh_auth_policy_apply_scope(policy, self->find_scope,
                                  GH_AUTH_PURPOSE_CONTACT_DIRECTORY, urls[i], NULL);
      g_hash_table_insert(self->find_pending, g_strdup(urls[i]), GUINT_TO_POINTER(1));
      self->find_left++;
    }
  }
  if (!self->find_left) { cancel_find(self); return; }
  self->find_deadline = g_timeout_add_seconds(FIND_DEADLINE_SECONDS, find_deadline, self);
  gh_relay_scope_start(self->find_scope);
}

static GStrv
discovery_sources(GhPublicNoteUi *self)
{
  g_auto(GStrv) configured = g_settings_get_strv(self->settings, "discovery-relays");
  g_autoptr(GPtrArray) urls = g_ptr_array_new_with_free_func(g_free);
  for (guint i = 0; configured && configured[i] && urls->len < MAX_FIND_SOURCES; i++) {
    const gchar *url = configured[i];
    if (gh_relay_url_validate(url, NULL) &&
        !g_ptr_array_find_with_equal_func(urls, url, g_str_equal, NULL))
      g_ptr_array_add(urls, g_strdup(url));
  }
  g_ptr_array_add(urls, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&urls), FALSE);
}

static void
find_response(AdwAlertDialog *alert, const gchar *response, GhPublicNoteUi *self)
{
  if (!g_str_equal(response, "find") ||
      !gh_account_controller_is_current(self->accounts, self->generation)) return;
  const gchar *uri = g_object_get_data(G_OBJECT(alert), "reference-uri");
  const gchar *const *urls = g_object_get_data(G_OBJECT(alert), "destinations");
  g_auto(GStrv) current = discovery_sources(self);
  if (urls && !g_strv_equal(urls, (const gchar *const *)current)) {
    notice(self, _("Discovery Relays Changed"),
      _("The configured destinations changed after confirmation opened. Review the new list before finding this note."));
    return;
  }
  if (uri && urls) start_find(self, uri, urls);
}

static void
confirm_find(GhPublicNoteUi *self, const gchar *uri)
{
  g_auto(GStrv) urls = discovery_sources(self);
  if (!urls[0]) {
    notice(self, _("No Discovery Relays"),
      _("Configure discovery relays before finding this public note. No relay was contacted."));
    return;
  }
  g_autoptr(GhConversationView) view = g_weak_ref_get(&self->view);
  if (!view) return;
  g_autofree gchar *destinations = g_strjoinv("\n", urls);
  g_autofree gchar *body = g_strdup_printf(
    _("Find this public note only on these configured discovery relays?\n\n%s\n\nThe relays will learn which public reference you requested. No relay hint in the message will be contacted."),
    destinations);
  AdwAlertDialog *alert = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Find on Relays?"), body));
  adw_alert_dialog_add_responses(alert, "cancel", _("Cancel"), "find", _("Find on Relays"), NULL);
  adw_alert_dialog_set_default_response(alert, "cancel");
  adw_alert_dialog_set_close_response(alert, "cancel");
  g_object_set_data_full(G_OBJECT(alert), "reference-uri", g_strdup(uri), g_free);
  g_object_set_data_full(G_OBJECT(alert), "destinations", g_steal_pointer(&urls),
                         (GDestroyNotify)g_strfreev);
  g_signal_connect_object(alert, "response", G_CALLBACK(find_response), self, 0);
  adw_dialog_present(ADW_DIALOG(alert), GTK_WIDGET(view));
}

static void
public_action(GhConversationView *view, const gchar *action, const gchar *uri,
              GhPublicNoteUi *self)
{
  if (!gh_account_controller_is_current(self->accounts, self->generation)) return;
  g_autoptr(GnNostrReference) reference = gn_nostr_reference_parse(uri);
  if (!reference || (reference->type != GN_NOSTR_REFERENCE_EVENT &&
                     reference->type != GN_NOSTR_REFERENCE_ADDRESS)) return;
  if (g_str_equal(action, "find")) {
    if (!lookup(self, reference)) confirm_find(self, uri);
    return;
  }
  const GhPublicNote *note = lookup(self, reference);
  if (!note) return;
  AdwDialog *dialog = gh_public_post_dialog_new(self->accounts, self->relays, note,
                                                uri, g_str_equal(action, "quote"));
  if (dialog) adw_dialog_present(dialog, GTK_WIDGET(view));
}

static void
dispose(GObject *object)
{
  GhPublicNoteUi *self = (GhPublicNoteUi *)object;
  cancel_find(self);
  g_clear_object(&self->accounts);
  g_clear_object(&self->store);
  g_clear_object(&self->relays);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_public_note_ui_parent_class)->dispose(object);
}

static void
finalize(GObject *object)
{
  GhPublicNoteUi *self = (GhPublicNoteUi *)object;
  g_hash_table_unref(self->by_address);
  g_hash_table_unref(self->by_id);
  g_weak_ref_clear(&self->view);
  G_OBJECT_CLASS(gh_public_note_ui_parent_class)->finalize(object);
}

static void
gh_public_note_ui_class_init(GhPublicNoteUiClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  G_OBJECT_CLASS(klass)->finalize = finalize;
}

static void
gh_public_note_ui_init(GhPublicNoteUi *self)
{
  g_weak_ref_init(&self->view, NULL);
  self->by_id = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                      (GDestroyNotify)gh_public_note_free);
  self->by_address = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

void
gh_public_note_ui_attach(GhWindow *window, const GhPublicNoteUiConfig *config)
{
  g_return_if_fail(GH_IS_WINDOW(window) && config &&
    GH_IS_ACCOUNT_CONTROLLER(config->accounts) &&
    GH_IS_ACCOUNT_STORE(config->store) && GH_IS_ACCOUNT_RELAYS(config->relays) &&
    G_IS_SETTINGS(config->settings));
  GtkWidget *widget = gh_content_page_get_view(gh_window_get_content(window));
  g_return_if_fail(GH_IS_CONVERSATION_VIEW(widget));
  GhConversationView *view = GH_CONVERSATION_VIEW(widget);
  GhPublicNoteUi *self = g_object_new(gh_public_note_ui_get_type(), NULL);
  g_weak_ref_set(&self->view, G_OBJECT(view));
  self->accounts = g_object_ref(config->accounts);
  self->store = g_object_ref(config->store);
  self->relays = g_object_ref(config->relays);
  self->settings = g_object_ref(config->settings);
  self->generation = gh_account_controller_get_generation(self->accounts);
  g_signal_connect_object(view, "public-reference-action", G_CALLBACK(public_action), self, 0);
  g_signal_connect_object(self->accounts, "changed", G_CALLBACK(reload), self,
                          G_CONNECT_SWAPPED);
  g_signal_connect_object(self->store, "changed", G_CALLBACK(reload), self,
                          G_CONNECT_SWAPPED);
  reload(self);
  gh_conversation_view_set_reference_source(view, summary, g_object_ref(self), g_object_unref);
  g_object_unref(self);
}
