#include "gh-report-service.h"
#include "gh-auth-policy.h"
#include "gh-relay-scope.h"
#include "gh-relay-deny-policy.h"
#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-kinds.h>
#include <stdlib.h>
#include <string.h>

struct _GhReportService {
  GObject parent_instance;
  GhSocialLists *lists;
  GhAccountStore *account_store;
  GhStore *store; /* borrowed strictly between bound/closing */
  guint64 generation, serial;
  GhListAdapterNetwork network;
  GhReportSigner signer;
  GCancellable *cancel;
  GhRelayPublish *publish;
  GhStoreReport *active;
  JsonObject *outcomes;
  gchar *retry_url, *error;
  gboolean busy;
};
G_DEFINE_TYPE(GhReportService, gh_report_service, G_TYPE_OBJECT)
static void changed(GhReportService *s) { g_signal_emit_by_name(s, "changed"); }
static gboolean invalid(GError **error, const gchar *text)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, text); return FALSE;
}
static gchar *
build(const gchar *account, const gchar *target, const gchar *event_id,
      NostrNip56Category category, const gchar *explanation, gint64 now, GError **error)
{
  if (!account || !target || !g_ascii_strcasecmp(account, target)
      || !explanation || strlen(explanation) > 8192
      || !g_utf8_validate(explanation, -1, NULL)) {
    invalid(error, "Choose another account and an explanation of at most 8,192 UTF-8 bytes"); return NULL;
  }
  NostrNip56Target tags[] = {{NOSTR_NIP56_ACCOUNT, target, category},
                           {NOSTR_NIP56_EVENT, event_id, category}};
  NostrEvent *event = nostr_nip56_build_unsigned(account, now, tags, event_id ? 2 : 1,
                                               event_id != NULL, explanation);
  if (!event) { invalid(error, "The report identity or category is invalid"); return NULL; }
  gchar *json = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  return json;
}
gchar *
gh_report_build_account(const gchar *account, const gchar *target, NostrNip56Category category,
                        const gchar *explanation, gint64 now, GError **error)
{
  return build(account, target, NULL, category, explanation, now, error);
}
gchar *
gh_report_build_public_message(const gchar *account, const gchar *json, NostrNip56Category category,
                               const gchar *explanation, gint64 now, GError **error)
{
  NostrEvent *event = nostr_event_new();
  gchar *result = NULL;
  if (json && nostr_event_deserialize_signed(event, json, NULL) == NOSTR_EVENT_VALIDATION_OK
      && nostr_event_validate(event, NULL) == NOSTR_EVENT_VALIDATION_OK
      && nostr_event_get_kind(event) >= NOSTR_KIND_SIMPLE_GROUP_CHAT_MESSAGE
      && nostr_event_get_kind(event) <= NOSTR_KIND_SIMPLE_GROUP_REPLY) {
    char *id = nostr_event_get_id(event);
    result = build(account, nostr_event_get_pubkey(event), id, category, explanation, now, error);
    free(id);
  } else invalid(error, "A verified public NIP-29 message is required");
  nostr_event_free(event);
  return result;
}
gboolean
gh_report_matches_signed(const gchar *approved, const gchar *json, GError **error)
{
  NostrEvent *a = nostr_event_new(), *b = nostr_event_new();
  gboolean okay = approved && json
      && nostr_event_deserialize_unsigned(a, approved, NULL) == NOSTR_EVENT_VALIDATION_OK
      && nostr_nip56_validate(a, FALSE) == 0
      && nostr_event_deserialize_signed(b, json, NULL) == NOSTR_EVENT_VALIDATION_OK
      && nostr_event_validate(b, NULL) == NOSTR_EVENT_VALIDATION_OK;
  if (okay) {
    char *expected = nostr_event_get_id(a), *actual = nostr_event_get_id(b);
    okay = expected && actual && !strcmp(expected, actual);
    free(expected); free(actual);
  }
  nostr_event_free(a); nostr_event_free(b);
  return okay || invalid(error, "The signer returned a different or invalid report");
}
static gchar *json_object_text(JsonObject *object)
{
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, object);
  gchar *text = json_to_string(node, FALSE);
  json_node_free(node); return text;
}
static gchar *targets_text(const gchar *const *targets)
{
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_array(b);
  for (guint i = 0; targets && targets[i]; i++) json_builder_add_string_value(b, targets[i]);
  json_builder_end_array(b);
  g_autoptr(JsonNode) node = json_builder_get_root(b);
  return json_to_string(node, FALSE);
}
static gboolean current(GhReportService *s)
{
  return s->store && (!s->network.accounts
      || gh_account_controller_is_current(s->network.accounts, s->generation))
      && s->generation == gh_social_lists_get_generation(s->lists)
      && !g_strcmp0(gh_store_get_account_pubkey(s->store), gh_social_lists_get_account(s->lists));
}
static gboolean save(GhReportService *s, const gchar *signed_json, const gchar *state)
{
  g_autofree gchar *outcomes = json_object_text(s->outcomes);
  g_autoptr(GError) error = NULL;
  if (!current(s) || !gh_store_report_update(s->store, s->active->id, signed_json,
                                            outcomes, state, &error)) {
    g_free(s->error); s->error = g_strdup("Report delivery state could not be saved"); return FALSE;
  }
  return TRUE;
}
static void fail(GhReportService *s, const gchar *message)
{
  g_free(s->error); s->error = g_strdup(message);
  save(s, NULL, "failed"); s->busy = FALSE; changed(s);
}
static void update(GhRelayPublish *p, const GhRelayPublishResult *r, gpointer data)
{
  (void)p;
  GhReportService *s = data;
  if (!current(s)) return;
  /* Several relays acknowledge an already-stored event as OK false with
   * duplicate:. That is still the immutable report's successful acceptance. */
  gboolean duplicate = r->outcome == GH_RELAY_PUBLISH_REJECTED
      && r->prefix == GH_RELAY_OK_PREFIX_DUPLICATE;
  json_object_set_int_member(s->outcomes, r->url, duplicate ? GH_RELAY_PUBLISH_ACCEPTED : r->outcome);
  if (!save(s, NULL, "publishing")) {
    gh_relay_publish_cancel(s->publish);
    s->busy = FALSE;
  }
  changed(s);
}
static void done(GhRelayPublish *p, const GhRelayPublishSummary *summary, gpointer data)
{
  (void)p; (void)summary;
  GhReportService *s = data;
  gboolean accepted = FALSE;
  GList *members = json_object_get_members(s->outcomes);
  for (GList *l = members; l; l = l->next)
    accepted |= json_object_get_int_member(s->outcomes, l->data) == GH_RELAY_PUBLISH_ACCEPTED;
  g_list_free(members);
  save(s, NULL, accepted ? "accepted" : "failed");
  s->busy = FALSE; changed(s);
}
GStrv gh_report_service_dup_targets(GhReportService *s)
{
  g_autoptr(GPtrArray) targets = g_ptr_array_new_with_free_func(g_free);
  g_auto(GStrv) own = current(s) ? gh_social_lists_dup_public_targets(s->lists) : NULL;
  for (guint i = 0; own && own[i] && targets->len < 16; i++)
    if (gh_relay_url_validate(own[i], NULL)
        && gh_relay_deny_policy_check(s->generation, own[i], FALSE, NULL)
        && !g_ptr_array_find_with_equal_func(targets, own[i], g_str_equal, NULL))
      g_ptr_array_add(targets, g_strdup(own[i]));
  g_ptr_array_add(targets, NULL);
  return (GStrv)g_ptr_array_free(g_steal_pointer(&targets), FALSE);
}
static void publish(GhReportService *s)
{
  g_autoptr(GError) error = NULL;
  if (!current(s)) return;
  g_auto(GStrv) allowed = gh_report_service_dup_targets(s);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, s->active->targets_json, -1, &error)
      || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) {
    fail(s, "The approved report destinations could not be read"); return;
  }
  if (s->publish) { gh_relay_publish_unref(s->publish); s->publish = NULL; }
  s->publish = s->network.publish_transport
    ? gh_relay_publish_new_with_transport(s->generation, s->active->signed_json,
        s->network.publish_transport, s->network.publish_data, update, done, s, &error)
    : gh_relay_publish_new(s->generation, s->active->signed_json, update, done, s, &error);
  if (!s->publish) { fail(s, "The signed report is invalid"); return; }
  if (s->network.publish_auth_transport)
    gh_relay_publish_set_auth_transport(s->publish, s->network.publish_auth_transport);
  if (s->network.publish_deadline) gh_relay_publish_set_deadline(s->publish, s->network.publish_deadline);
  JsonArray *targets = json_node_get_array(json_parser_get_root(parser));
  guint n = 0;
  for (guint i = 0; i < json_array_get_length(targets); i++) {
    const gchar *url = json_array_get_string_element(targets, i);
    if (s->retry_url && g_strcmp0(url, s->retry_url)) continue;
    if (json_object_has_member(s->outcomes, url)
        && json_object_get_int_member(s->outcomes, url) == GH_RELAY_PUBLISH_ACCEPTED) continue;
    if (!g_strv_contains((const gchar *const *)allowed, url)) {
      json_object_set_int_member(s->outcomes, url, GH_RELAY_PUBLISH_BLOCKED_BY_LOCAL_POLICY); continue;
    }
    if (!gh_relay_publish_add_url(s->publish, url, &error)
        || !gh_auth_policy_apply_publish(gh_auth_policy_get_for_accounts(s->network.accounts),
            s->publish, GH_AUTH_PURPOSE_PUBLIC_POST, url, &error)) {
      fail(s, "The report destination could not be authorized"); return;
    }
    n++;
  }
  if (!save(s, NULL, "publishing")) { s->busy = FALSE; changed(s); return; }
  if (!n) { done(s->publish, NULL, s); return; }
  if (!gh_relay_publish_start(s->publish, &error)) fail(s, "The report could not be published");
}
typedef struct { GhReportService *service; guint64 serial; } SignCall;
static void signed_done(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  SignCall *call = data;
  GhReportService *s = call->service;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *json = s->signer.sign_finish ? s->signer.sign_finish(result, &error)
      : gh_account_controller_sign_finish(result, &error);
  if (call->serial == s->serial && current(s) && s->busy) {
    if (!json) fail(s, "The report was not signed; it has not been published");
    else if (!gh_report_matches_signed(s->active->unsigned_json, json, &error))
      fail(s, "The signer returned a different or invalid report");
    else if (save(s, json, "signed")) {
      g_free(s->active->signed_json); s->active->signed_json = g_strdup(json); publish(s);
    } else { s->busy = FALSE; changed(s); }
  }
  g_object_unref(s); g_free(call);
}
void gh_report_service_cancel(GhReportService *s)
{
  s->serial++;
  if (s->cancel) g_cancellable_cancel(s->cancel);
  g_clear_object(&s->cancel);
  if (s->publish) { gh_relay_publish_cancel(s->publish); gh_relay_publish_unref(s->publish); s->publish = NULL; }
  if (s->busy && current(s)) save(s, NULL, "cancelled");
  s->busy = FALSE;
}
void gh_report_service_close(GhReportService *s)
{
  gh_report_service_cancel(s); s->store = NULL; s->generation = 0; changed(s);
}
void gh_report_service_bind(GhReportService *s, GhStore *store, guint64 generation)
{
  gh_report_service_close(s); s->store = store; s->generation = generation; changed(s);
}
static void bound(GhAccountStore *store, GhReportService *s)
{
  gh_report_service_bind(s, gh_account_store_get_store(store), gh_account_store_get_generation(store));
}
static void closing(GhAccountStore *store, GhReportService *s) { (void)store; gh_report_service_close(s); }
static void accounts_changed(GhAccountController *accounts, GhReportService *s)
{
  if (s->store && !gh_account_controller_is_current(accounts, s->generation))
    gh_report_service_close(s);
}
gboolean gh_report_service_retry(GhReportService *s, gint64 id, const gchar *url, GError **error)
{
  if (!current(s) || s->busy) return invalid(error, "Account storage is unavailable or a report is in progress");
  g_autoptr(GhStoreReport) report = gh_store_report_load(s->store, id, error);
  if (!report) return FALSE;
  if (report->signed_json && !gh_report_matches_signed(report->unsigned_json, report->signed_json, error)) return FALSE;
  gh_report_service_cancel(s);
  g_clear_pointer(&s->active, gh_store_report_free); s->active = g_steal_pointer(&report);
  g_clear_pointer(&s->outcomes, json_object_unref);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, s->active->outcomes_json, -1, error)
      || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return FALSE;
  s->outcomes = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
  g_free(s->retry_url); s->retry_url = g_strdup(url);
  g_clear_pointer(&s->error, g_free);
  s->busy = TRUE;
  if (s->active->signed_json) publish(s);
  else {
    if (!save(s, NULL, "signing")) { s->busy = FALSE; return FALSE; }
    s->cancel = g_cancellable_new();
    SignCall *call = g_new0(SignCall, 1); call->service = g_object_ref(s); call->serial = s->serial;
    if (s->signer.sign_async) s->signer.sign_async(s->signer.data, s->active->unsigned_json,
        s->cancel, signed_done, call);
    else if (s->network.accounts) {
      GhSignerContext context = { .reason = "Publish a public account report", .purpose = "public-report", .flow_id = "report" };
      gh_account_controller_sign_with_context_async(s->network.accounts, s->active->unsigned_json,
          &context, s->cancel, signed_done, call);
    } else {
      g_object_unref(call->service); g_free(call); fail(s, "No signer is available; report saved on this device");
    }
  }
  changed(s); return TRUE;
}
gint64 gh_report_service_submit(GhReportService *s, const gchar *approved, const gchar *const *targets,
                                gboolean hide, GError **error)
{
  if (!current(s) || s->busy) { invalid(error, "Account storage is unavailable or a report is in progress"); return 0; }
  NostrEvent *event = nostr_event_new();
  NostrNip56Target parsed[2]; size_t count = 0;
  gboolean valid = approved && nostr_event_deserialize_unsigned(event, approved, NULL) == NOSTR_EVENT_VALIDATION_OK
      && !g_strcmp0(nostr_event_get_pubkey(event), gh_social_lists_get_account(s->lists))
      && nostr_nip56_parse(event, FALSE, parsed, 2, &count) == 0 && count >= 1;
  /* Only the codec's exact p[/e] shape is accepted: no extra evidence tags. */
  g_autoptr(JsonParser) parser = json_parser_new();
  valid = valid && json_parser_load_from_data(parser, approved, -1, NULL);
  JsonObject *object = valid ? json_node_get_object(json_parser_get_root(parser)) : NULL;
  JsonArray *tags = object ? json_object_get_array_member(object, "tags") : NULL;
  valid = valid && tags && json_array_get_length(tags) == count
      && parsed[0].type == NOSTR_NIP56_ACCOUNT
      && (count == 1 || (parsed[1].type == NOSTR_NIP56_EVENT && parsed[1].category == parsed[0].category))
      && g_strcmp0(parsed[0].id, gh_social_lists_get_account(s->lists));
  for (guint i = 0; valid && i < count; i++)
    valid = json_array_get_length(json_array_get_array_element(tags, i)) == 3;
  g_auto(GStrv) allowed = gh_report_service_dup_targets(s);
  valid = valid && targets && targets[0];
  for (guint i = 0; valid && targets[i]; i++)
    valid = i < 16 && g_strv_contains((const gchar *const *)allowed, targets[i]);
  if (!valid) { nostr_event_free(event); invalid(error, "The report or its approved destinations are invalid"); return 0; }
  if (hide && !gh_social_lists_hide_account(s->lists, parsed[0].id, error)) { nostr_event_free(event); return 0; }
  g_autofree gchar *destinations = targets_text(targets);
  gint64 id = gh_store_report_approve(s->store, approved, destinations, nostr_event_get_created_at(event), error);
  nostr_event_free(event);
  if (id) gh_report_service_retry(s, id, NULL, error);
  return id;
}
GPtrArray *gh_report_service_list(GhReportService *s, GError **error)
{
  if (!current(s)) { invalid(error, "Account storage is unavailable"); return NULL; }
  return gh_store_report_list(s->store, error);
}
const gchar *gh_report_service_get_error(GhReportService *s) { return s->error; }
gboolean gh_report_service_get_busy(GhReportService *s) { return s->busy; }
static void dispose(GObject *object)
{
  GhReportService *s = GH_REPORT_SERVICE(object);
  gh_report_service_close(s);
  if (s->account_store) g_signal_handlers_disconnect_by_data(s->account_store, s);
  g_clear_object(&s->account_store); g_clear_object(&s->lists);
  if (s->network.accounts) g_signal_handlers_disconnect_by_data(s->network.accounts, s);
  gh_list_adapter_network_clear(&s->network);
  g_clear_pointer(&s->active, gh_store_report_free);
  g_clear_pointer(&s->outcomes, json_object_unref);
  g_clear_pointer(&s->retry_url, g_free); g_clear_pointer(&s->error, g_free);
  G_OBJECT_CLASS(gh_report_service_parent_class)->dispose(object);
}
static void gh_report_service_init(GhReportService *s) { (void)s; }
static void gh_report_service_class_init(GhReportServiceClass *klass)
{
  G_OBJECT_CLASS(klass)->dispose = dispose;
  g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}
GhReportService *gh_report_service_new(GhSocialLists *lists, GhAccountStore *store,
    const GhListAdapterNetwork *network, const GhReportSigner *signer)
{
  GhReportService *s = g_object_new(GH_TYPE_REPORT_SERVICE, NULL);
  s->lists = g_object_ref(lists);
  if (network) gh_list_adapter_network_copy(&s->network, network);
  if (s->network.accounts)
    g_signal_connect(s->network.accounts, "changed", G_CALLBACK(accounts_changed), s);
  if (signer) s->signer = *signer;
  if (store) {
    s->account_store = g_object_ref(store);
    g_signal_connect(store, "store-bound", G_CALLBACK(bound), s);
    g_signal_connect(store, "store-closing", G_CALLBACK(closing), s);
    if (gh_account_store_get_store(store)) bound(store, s);
  }
  return s;
}
