/* SPDX-License-Identifier: MIT */
#include "grotto-profile-service.h"
#include "relay_store.h"
#include <nostr-publish/nostr-publish.h>
#include <nostr-gobject-1.0/gn-blossom-client.h>
#include <nostr/nip19/nip19.h>
#include <nostr/bud02/auth.h>
#include <nostr-event.h>
#include <json.h>
#include <nostr-kinds.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <string.h>

#define IMAGE_LIMIT (5 * 1024 * 1024)
#define IMAGE_PIXELS (24 * 1024 * 1024)
static const char *const field_names[] = {"name", "display_name", "about", "picture", "banner", "nip05", "lud16", "lud06", "website"};
static const guint kinds[] = {NOSTR_KIND_PROFILE_METADATA, NOSTR_KIND_RELAY_LIST_METADATA, NOSTR_KIND_BLOSSOM_SERVER_LIST, NOSTR_KIND_DM_RELAY_LIST};
typedef struct {
  GrottoProfileAction action;
  char *npub, hex[65];
  GrottoProfileInfo *info;
  GFile *file;
  GBytes *bytes;
  GdkTexture *preview;
  char *mime;
  char **destinations, **discovery_relays, **auth_required_relays, **bootstrap_relays;
  GrottoApprovalController *controller;
  GMainContext *ui_context;
  gboolean bootstrap_used, bootstrap_round, no_relays, crop_frozen;
  GDBusConnection *bus;
  GCancellable *cancel;
  GHashTable *capabilities;
  GObject *cache_scope;
  GMutex *cache_lock;
} Job;
static void cache_lock_free(gpointer data) { g_mutex_clear(data); g_free(data); }
static void job_free(gpointer data) {
  Job *j = data; g_free(j->npub); grotto_profile_info_free(j->info); g_clear_object(&j->file);
  g_clear_pointer(&j->bytes, g_bytes_unref); g_clear_object(&j->preview); g_free(j->mime); g_strfreev(j->destinations); g_strfreev(j->discovery_relays); g_strfreev(j->auth_required_relays); g_strfreev(j->bootstrap_relays); g_clear_object(&j->controller); g_main_context_unref(j->ui_context); g_clear_object(&j->bus); g_hash_table_unref(j->capabilities); g_clear_object(&j->cache_scope); g_free(j);
}
static const char *string(JsonObject *o, const char *key) {
  JsonNode *n = o ? json_object_get_member(o, key) : NULL;
  return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
static gboolean endpoint(const char *url, gboolean relay) {
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  if (!uri || !g_uri_get_host(uri) || g_uri_get_userinfo(uri) || g_uri_get_fragment(uri)) return FALSE;
  const char *scheme = g_uri_get_scheme(uri);
  return !g_strcmp0(scheme, relay ? "wss" : "https") || (relay && !g_strcmp0(scheme, "ws"));
}
static gboolean verified(const char *json, guint kind, const char *hex, guint64 *timestamp) {
  NostrEvent *e = nostr_event_new();
  gboolean ok = json && strlen(json) <= 131072 &&
    nostr_event_deserialize_signed(e, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(e, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_get_kind(e) == kind && !g_strcmp0(nostr_event_get_pubkey(e), hex);
  if (ok && timestamp) *timestamp = nostr_event_get_created_at(e);
  nostr_event_free(e); return ok;
}
static char *cache_path(Job *j, guint kind) {
  g_autofree char *name = g_strdup_printf("%s-%u.json", j->hex, kind);
  return g_build_filename(g_get_user_cache_dir(), "grotto", "profiles", name, NULL);
}
static char *cache_read(Job *j, guint kind) {
  g_autofree char *path = cache_path(j, kind); g_autofree char *json = NULL;
  GStatBuf st;
  if (g_stat(path, &st) || st.st_size > 131072 || !g_file_get_contents(path, &json, NULL, NULL) || !verified(json, kind, j->hex, NULL)) return NULL;
  return g_steal_pointer(&json);
}
static void cache_write(Job *j, guint kind, const char *json) {
  if (!json || g_cancellable_is_cancelled(j->cancel)) return;
  /* The host window owns this lock, not a global cache. A slower read from
   * a pinned page must not overwrite a newer acknowledged profile. */
  g_mutex_lock(j->cache_lock);
  guint64 incoming = 0, current = 0;
  g_autofree char *previous = cache_read(j, kind);
  gboolean write = verified(json, kind, j->hex, &incoming);
  if (write && previous && verified(previous, kind, j->hex, &current)) {
    if (current > incoming) write = FALSE;
    else if (current == incoming) {
      NostrEvent *a = nostr_event_new(), *b = nostr_event_new();
      nostr_event_deserialize(a, json); nostr_event_deserialize(b, previous);
      if (strcmp(nostr_event_get_id(b), nostr_event_get_id(a)) <= 0) write = FALSE;
      nostr_event_free(a); nostr_event_free(b);
    }
  }
  g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "grotto", "profiles", NULL);
  g_autofree char *path = cache_path(j, kind);
  if (write && !g_mkdir_with_parents(dir, 0700))
    g_file_set_contents_full(path, json, -1, G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE, 0600, NULL);
  g_mutex_unlock(j->cache_lock);
}
typedef struct {
  GrottoApprovalController *controller; GMainContext *context;
  char *owner; char **destinations, **bootstrap;
} ProfileCorrelation;
static gboolean release_controller(gpointer data) { g_object_unref(data); return G_SOURCE_REMOVE; }
static void correlation_free(gpointer data) {
  ProfileCorrelation *c = data;
  /* Filter destruction may run on D-Bus I/O; GTK/controller teardown must not. */
  GSource *idle = g_idle_source_new(); g_source_set_callback(idle, release_controller, c->controller, NULL);
  g_source_attach(idle, c->context); g_source_unref(idle); g_main_context_unref(c->context);
  g_free(c->owner); g_strfreev(c->destinations); g_strfreev(c->bootstrap); g_free(c);
}
static GDBusMessage *profile_submitted(GDBusConnection *bus, GDBusMessage *message, gboolean incoming, gpointer data) {
  (void)bus; ProfileCorrelation *c = data;
  if (incoming && g_dbus_message_get_message_type(message) == G_DBUS_MESSAGE_TYPE_SIGNAL &&
      !g_strcmp0(g_dbus_message_get_sender(message), c->owner) &&
      !g_strcmp0(g_dbus_message_get_path(message), "/org/nostr/signer/grotto") &&
      !g_strcmp0(g_dbus_message_get_interface(message), "org.nostr.Grotto.Control1") &&
      !g_strcmp0(g_dbus_message_get_member(message), "ProfileRequestSubmitted")) {
    GVariant *body = g_dbus_message_get_body(message);
    if (body && g_variant_is_of_type(body, G_VARIANT_TYPE("(s)"))) {
      const char *id; g_variant_get(body, "(&s)", &id);
      grotto_approval_controller_set_profile_provenance(c->controller, c->owner, id,
        (const char *const *)c->destinations, (const char *const *)c->bootstrap);
    }
  }
  return message;
}
static char *sign_event(gpointer data, const char *unsigned_json, GCancellable *cancel, GError **error) {
  Job *j = data;
  if (j->bootstrap_round) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Bootstrap discovery cannot sign"); return NULL;
  }
  /* The shared NIP-42 builder deliberately leaves identity selection to its
   * signer. Pin that selection here, before computing the expected event ID. */
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, unsigned_json, -1, error) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return NULL;
  JsonObject *object = json_node_get_object(json_parser_get_root(parser));
  if (!json_object_has_member(object, "pubkey")) json_object_set_string_member(object, "pubkey", j->hex);
  g_autofree char *pinned = json_to_string(json_parser_get_root(parser), FALSE);
  unsigned_json = pinned;
  const char *none[] = {NULL}, *relay_destination[] = {NULL, NULL};
  const char *const *destinations = j->destinations ? (const char *const *)j->destinations : none;
  if (json_object_get_int_member(object, "kind") == NOSTR_KIND_CLIENT_AUTHENTICATION) {
    JsonArray *tags = json_object_get_array_member(object, "tags");
    for (guint i = 0; tags && i < json_array_get_length(tags); i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (tag && json_array_get_length(tag) >= 2 && !g_strcmp0(json_array_get_string_element(tag, 0), "relay")) relay_destination[0] = json_array_get_string_element(tag, 1);
    }
    destinations = relay_destination;
  }
  guint filter = 0; g_autofree char *owner = NULL;
  if (j->controller) {
    g_autoptr(GVariant) name = g_dbus_connection_call_sync(j->bus, "org.freedesktop.DBus", "/org/freedesktop/DBus",
      "org.freedesktop.DBus", "GetNameOwner", g_variant_new("(s)", "org.nostr.Signer"), G_VARIANT_TYPE("(s)"), 0, 1000, cancel, NULL);
    if (name) {
      g_variant_get(name, "(s)", &owner);
      ProfileCorrelation *c = g_new0(ProfileCorrelation, 1); c->controller = g_object_ref(j->controller);
      c->context = g_main_context_ref(j->ui_context); c->owner = g_strdup(owner);
      c->destinations = g_strdupv((char **)destinations); c->bootstrap = g_strdupv(j->bootstrap_relays);
      filter = g_dbus_connection_add_filter(j->bus, profile_submitted, c, correlation_free);
    }
  }
  g_autoptr(GVariant) reply = g_dbus_connection_call_sync(j->bus, owner ? owner : "org.nostr.Signer", "/org/nostr/signer/grotto", "org.nostr.Grotto.Control1", "SignProfileEvent",
    g_variant_new("(ss^as)", j->npub, unsigned_json, destinations), G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 180000, cancel, error);
  if (filter) g_dbus_connection_remove_filter(j->bus, filter);
  if (!reply) {
    if (error && *error && !g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error(error); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
        _("Grotto could not sign this request. Check the review and try again"));
    }
    return NULL;
  }
  char *json = NULL; g_variant_get(reply, "(s)", &json);
  NostrEvent *input = nostr_event_new(), *output = nostr_event_new(); char id[65];
  gboolean valid = nostr_event_deserialize(input, unsigned_json) == 0 && nostr_event_compute_id(input, id) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_deserialize_signed(output, json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(output, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    !g_strcmp0(id, nostr_event_get_id(output)) && !g_strcmp0(nostr_event_get_pubkey(output), j->hex);
  nostr_event_free(input); nostr_event_free(output);
  if (!valid) { g_free(json); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Signer returned a different or invalid event"); return NULL; }
  return json;
}
static GBytes *read_stream(GInputStream *stream, gsize limit, GCancellable *cancel, GError **error);
static gboolean probe_relay_network(const char *url, GCancellable *cancel) {
  gboolean auth = FALSE;
  g_autofree char *http = g_strconcat(g_str_has_prefix(url, "wss:") ? "https:" : "http:", strchr(url, ':') + 1, NULL);
  g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 3, NULL);
  g_autoptr(SoupMessage) message = soup_message_new("GET", http);
  soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
  soup_message_headers_replace(soup_message_get_request_headers(message), "Accept", "application/nostr+json");
  g_autoptr(GInputStream) stream = soup_session_send(session, message, cancel, NULL);
  g_autoptr(GBytes) bytes = stream && soup_message_get_status(message) == SOUP_STATUS_OK ? read_stream(stream, 65536, cancel, NULL) : NULL;
  if (bytes) {
    gsize length; const char *data = g_bytes_get_data(bytes, &length); g_autoptr(JsonParser) parser = json_parser_new();
    if (json_parser_load_from_data(parser, data, length, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
      JsonObject *root = json_node_get_object(json_parser_get_root(parser)); JsonNode *limitation = json_object_get_member(root, "limitation");
      JsonNode *required = limitation && JSON_NODE_HOLDS_OBJECT(limitation) ? json_object_get_member(json_node_get_object(limitation), "auth_required") : NULL;
      auth = required && JSON_NODE_HOLDS_VALUE(required) && json_node_get_value_type(required) == G_TYPE_BOOLEAN && json_node_get_boolean(required);
    }
  }
  return auth;
}
static gboolean probe_relay(Job *j, const char *url) {
  gpointer known = g_hash_table_lookup(j->capabilities, url);
  if (known) return GPOINTER_TO_INT(known) == 2;
  gboolean auth = probe_relay_network(url, j->cancel);
  g_hash_table_insert(j->capabilities, g_strdup(url), GINT_TO_POINTER(auth ? 2 : 1)); return auth;
}
/* A finite metadata backfill ends only at EOSE. The watchdog is a failure,
 * never an empty-profile success. Each relay gets one bounded AUTH retry. */
typedef struct {
  Job *job; NostrPublishTransport *transport; NostrPublishSigner *signer;
  gboolean done, complete, authed, auth_wait;
  char *auth_id, *failure;
  char **events;
  NostrPublisher *publisher;
  const char *signed_json;
  char *event_id;
  gboolean connected, publishing, required_auth, authenticated, requested, probing, discovery_only;
  gint64 auth_deadline;
} Fetch;
static void reject_bootstrap_auth(Fetch *f) {
  f->done = TRUE;
  g_free(f->failure); f->failure = g_strdup("relay requires authentication");
  const char *url = nostr_publish_transport_get_url(f->transport);
  if (f->job->auth_required_relays && g_strv_contains((const char *const *)f->job->auth_required_relays, url)) return;
  g_autoptr(GStrvBuilder) skipped = g_strv_builder_new();
  if (f->job->auth_required_relays) g_strv_builder_addv(skipped, (const char **)f->job->auth_required_relays);
  g_strv_builder_add(skipped, url); g_strfreev(f->job->auth_required_relays); f->job->auth_required_relays = g_strv_builder_end(skipped);
}
static void send_req(Fetch *f) {
  if (f->publisher) {
    g_autofree char *frame = g_strdup_printf("[\"EVENT\",%s]", f->signed_json);
    if (!nostr_publish_transport_send_frame(f->transport, frame, NULL))
      nostr_publisher_record_ok(f->publisher, nostr_publish_transport_get_url(f->transport), f->event_id, FALSE, "error: relay disconnected");
    return;
  }
  f->requested = TRUE;
  g_autoptr(GString) filters = g_string_new("[\"REQ\",\"grotto-profile\"");
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
    if (f->discovery_only && kinds[i] == NOSTR_KIND_BLOSSOM_SERVER_LIST) continue;
    g_string_append_printf(filters, ",{\"authors\":[\"%s\"],\"kinds\":[%u],\"limit\":1}", f->job->hex, kinds[i]);
  }
  g_string_append(filters, "]"); const char *req = filters->str;
  if (!nostr_publish_transport_send_frame(f->transport, req, NULL)) { f->done = TRUE; f->failure = g_strdup("Relay disconnected"); }
}
static void fetch_state(NostrPublishTransport *transport, gboolean connected, const GError *error, gpointer data) {
  (void)transport; Fetch *f = data;
  if (f->done) return;
  if (connected) { f->connected = TRUE; if (!f->publisher && !f->required_auth) send_req(f); }
  else if (!f->done) {
    f->done = TRUE; f->failure = g_strdup(error ? error->message : "Relay disconnected");
    if (f->publisher && f->publishing) nostr_publisher_record_ok(f->publisher,
      nostr_publish_transport_get_url(transport), f->event_id, FALSE, "error: relay disconnected");
  }
}
static void fetch_frame(NostrPublishTransport *transport, const char *hint, const char *json, gpointer data) {
  (void)hint; Fetch *f = data;
  if (f->done || strlen(json) > 262144) return;
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json, -1, NULL) || !JSON_NODE_HOLDS_ARRAY(json_parser_get_root(parser))) return;
  JsonArray *a = json_node_get_array(json_parser_get_root(parser)); guint n = json_array_get_length(a);
  if (!n) return;
  JsonNode *type = json_array_get_element(a, 0);
  if (!JSON_NODE_HOLDS_VALUE(type) || json_node_get_value_type(type) != G_TYPE_STRING) return;
  const char *kind = json_node_get_string(type);
  JsonNode *sub = n > 1 ? json_array_get_element(a, 1) : NULL;
  const char *id = sub && JSON_NODE_HOLDS_VALUE(sub) && json_node_get_value_type(sub) == G_TYPE_STRING ? json_node_get_string(sub) : "";
  if (!strcmp(kind, "AUTH") && *id) {
    if (f->discovery_only || !f->signer) { reject_bootstrap_auth(f); return; }
    if (f->authed) return;
    f->authed = TRUE;
    g_autoptr(JsonBuilder) b = json_builder_new();
    json_builder_begin_object(b); json_builder_set_member_name(b, "pubkey"); json_builder_add_string_value(b, f->job->hex);
    json_builder_set_member_name(b, "kind"); json_builder_add_int_value(b, NOSTR_KIND_CLIENT_AUTHENTICATION);
    json_builder_set_member_name(b, "created_at"); json_builder_add_int_value(b, g_get_real_time() / G_USEC_PER_SEC);
    json_builder_set_member_name(b, "content"); json_builder_add_string_value(b, ""); json_builder_set_member_name(b, "tags"); json_builder_begin_array(b);
    json_builder_begin_array(b); json_builder_add_string_value(b, "relay"); json_builder_add_string_value(b, nostr_publish_transport_get_url(transport)); json_builder_end_array(b);
    json_builder_begin_array(b); json_builder_add_string_value(b, "challenge"); json_builder_add_string_value(b, id); json_builder_end_array(b);
    json_builder_end_array(b); json_builder_end_object(b);
    g_autoptr(JsonNode) root = json_builder_get_root(b); g_autofree char *raw = json_to_string(root, FALSE);
    g_autoptr(GError) error = NULL;
    g_autofree char *signed_json = nostr_publish_signer_sign_event_json(f->signer, raw, f->job->cancel, &error);
    if (!signed_json) {
      f->done = TRUE; f->failure = g_strdup(error ? error->message : "Relay authentication denied");
      if (f->publisher && f->publishing) nostr_publisher_record_ok(f->publisher,
        nostr_publish_transport_get_url(transport), f->event_id, FALSE, "restricted: relay authentication denied");
      return;
    }
    NostrEvent *auth = nostr_event_new(); nostr_event_deserialize(auth, signed_json);
    f->auth_id = g_strdup(nostr_event_get_id(auth)); nostr_event_free(auth); f->auth_wait = TRUE; f->auth_deadline = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    g_autofree char *frame = g_strdup_printf("[\"AUTH\",%s]", signed_json); nostr_publish_transport_send_frame(transport, frame, NULL); return;
  }
  if (!strcmp(kind, "OK") && f->auth_wait && !g_strcmp0(id, f->auth_id) && n >= 3) {
    JsonNode *accepted = json_array_get_element(a, 2);
    if (!JSON_NODE_HOLDS_VALUE(accepted) || json_node_get_value_type(accepted) != G_TYPE_BOOLEAN) return;
    f->auth_wait = FALSE;
    if (json_node_get_boolean(accepted)) { f->authenticated = TRUE; if (!f->publisher || f->publishing) send_req(f); }
    else {
      f->done = TRUE; f->failure = g_strdup("Relay authentication rejected");
      if (f->publisher && f->publishing) nostr_publisher_record_ok(f->publisher,
        nostr_publish_transport_get_url(transport), f->event_id, FALSE, "restricted: relay authentication rejected");
    } return;
  }
  if (f->publisher) {
    if (strcmp(kind, "OK") || g_strcmp0(id, f->event_id) || n < 3) return;
    JsonNode *accepted = json_array_get_element(a, 2);
    if (!JSON_NODE_HOLDS_VALUE(accepted) || json_node_get_value_type(accepted) != G_TYPE_BOOLEAN) return;
    const char *reason = n > 3 && JSON_NODE_HOLDS_VALUE(json_array_get_element(a, 3)) && json_node_get_value_type(json_array_get_element(a, 3)) == G_TYPE_STRING ? json_array_get_string_element(a, 3) : "";
    gboolean ok = json_node_get_boolean(accepted);
    if (!ok && g_str_has_prefix(reason, "auth-required:") && (!f->authed || f->auth_wait)) return;
    f->done = TRUE;
    nostr_publisher_record_ok(f->publisher, nostr_publish_transport_get_url(transport), id, ok, reason); return;
  }
  if (strcmp(id, "grotto-profile") || !f->requested) return;
  if (!strcmp(kind, "EOSE")) { f->complete = TRUE; f->done = TRUE; }
  else if (!strcmp(kind, "CLOSED")) {
    const char *reason = n > 2 && JSON_NODE_HOLDS_VALUE(json_array_get_element(a, 2)) && json_node_get_value_type(json_array_get_element(a, 2)) == G_TYPE_STRING ? json_array_get_string_element(a, 2) : "Relay closed the profile subscription";
    if (f->discovery_only && g_str_has_prefix(reason, "auth-required:")) { reject_bootstrap_auth(f); return; }
    if (g_str_has_prefix(reason, "auth-required:") && !f->authed) return; /* AUTH challenge follows */
    if (f->auth_wait) return;
    f->done = TRUE; f->failure = g_strdup(reason);
  } else if (!strcmp(kind, "EVENT") && n >= 3 && JSON_NODE_HOLDS_OBJECT(json_array_get_element(a, 2))) {
    g_autofree char *event = json_to_string(json_array_get_element(a, 2), FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) {
      guint64 timestamp = 0, old = 0;
      if ((f->discovery_only && kinds[i] == NOSTR_KIND_BLOSSOM_SERVER_LIST) || !verified(event, kinds[i], f->job->hex, &timestamp)) continue;
      if (f->events[i]) verified(f->events[i], kinds[i], f->job->hex, &old);
      gboolean replace = !f->events[i] || timestamp > old;
      if (f->events[i] && timestamp == old) {
        NostrEvent *previous = nostr_event_new(); nostr_event_deserialize(previous, f->events[i]);
        replace = strcmp(string(json_node_get_object(json_array_get_element(a, 2)), "id"), nostr_event_get_id(previous)) < 0;
        nostr_event_free(previous);
      }
      if (replace) { g_free(f->events[i]); f->events[i] = g_strdup(event); }
    }
  }
}
static gboolean deadline(gpointer data) {
  Fetch *f = data;
  if (f->auth_wait && g_get_monotonic_time() < f->auth_deadline) return G_SOURCE_CONTINUE;
  if (!f->done) { f->done = TRUE; f->failure = g_strdup("Relay did not finish profile backfill"); }
  return G_SOURCE_REMOVE;
}
static gboolean cancelled(GCancellable *cancel, gpointer data) { (void)cancel; Fetch *f = data; f->done = TRUE; return G_SOURCE_REMOVE; }
static void probe_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; Fetch *f = data;
  g_task_return_boolean(task, probe_relay_network(nostr_publish_transport_get_url(f->transport), cancel));
}
static void probe_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; Fetch *f = data; g_autoptr(GError) error = NULL;
  gboolean auth = g_task_propagate_boolean(G_TASK(result), &error);
  /* Advisory only: an actual AUTH/CLOSED frame still decides access. */
  if (!error) g_hash_table_insert(f->job->capabilities, g_strdup(nostr_publish_transport_get_url(f->transport)), GINT_TO_POINTER(auth ? 2 : 1));
  if (!error && auth && f->discovery_only && !f->done) reject_bootstrap_auth(f);
  f->probing = FALSE;
}
static guint fetch_relays(Job *j, NostrPublishSigner *signer, char **urls, char **events) {
  guint n = g_strv_length(urls), completed = 0;
  g_autofree Fetch *batch = g_new0(Fetch, n);
  g_autoptr(GCancellable) probes = g_cancellable_new();
  g_autoptr(GPtrArray) sources = g_ptr_array_new_with_free_func((GDestroyNotify)g_source_unref);
  for (guint i = 0; i < n; i++) {
    Fetch *f = &batch[i]; f->job = j; f->discovery_only = j->bootstrap_round;
    f->signer = f->discovery_only ? NULL : signer; f->events = events;
    f->transport = nostr_publish_transport_new_websocket(urls[i]);
    /* Probe NIP-11 concurrently, without delaying REQ/EOSE behind HTTP. */
    f->probing = TRUE; GTask *probe = g_task_new(NULL, probes, probe_ready, f);
    g_task_set_task_data(probe, f, NULL); g_task_run_in_thread(probe, probe_worker); g_object_unref(probe);
    nostr_publish_transport_set_state_callback(f->transport, fetch_state, f);
    nostr_publish_transport_set_listener(f->transport, fetch_frame, f);
    GSource *timeout = g_timeout_source_new_seconds(15);
    g_source_set_callback(timeout, deadline, f, NULL); g_source_attach(timeout, g_main_context_get_thread_default()); g_ptr_array_add(sources, timeout);
    GSource *cancellation = g_cancellable_source_new(j->cancel);
    g_source_set_callback(cancellation, G_SOURCE_FUNC(cancelled), f, NULL); g_source_attach(cancellation, g_main_context_get_thread_default()); g_ptr_array_add(sources, cancellation);
    nostr_publish_transport_connect_async(f->transport);
  }
  for (;;) {
    guint done = 0; completed = 0;
    for (guint i = 0; i < n; i++) { done += batch[i].done; completed += batch[i].complete; }
    if (done == n || g_cancellable_is_cancelled(j->cancel)) break;
    g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
  }
  g_cancellable_cancel(probes);
  for (;;) {
    guint pending = 0; for (guint i = 0; i < n; i++) pending += batch[i].probing;
    if (!pending) break;
    g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
  }
  for (guint i = 0; i < sources->len; i++) g_source_destroy(g_ptr_array_index(sources, i));
  for (guint i = 0; i < n; i++) {
    Fetch *f = &batch[i]; nostr_publish_transport_set_state_callback(f->transport, NULL, NULL);
    nostr_publish_transport_set_listener(f->transport, NULL, NULL);
    nostr_publish_transport_send_frame(f->transport, "[\"CLOSE\",\"grotto-profile\"]", NULL);
    nostr_publish_transport_disconnect(f->transport); nostr_publish_transport_unref(f->transport);
    g_free(f->auth_id); g_free(f->failure);
  }
  return completed;
}
static void close_publish(gpointer data) {
  Fetch *f = data;
  nostr_publish_transport_set_listener(f->transport, NULL, NULL);
  nostr_publish_transport_set_state_callback(f->transport, NULL, NULL);
  nostr_publish_transport_disconnect(f->transport); nostr_publish_transport_unref(f->transport);
  g_free(f->auth_id); g_free(f->event_id); g_free(f->failure); g_free(f);
}
static GPtrArray *open_publish(Job *j, NostrPublishSigner *signer, NostrPublisher *publisher, const char *json, char **urls) {
  GPtrArray *connections = g_ptr_array_new_with_free_func(close_publish);
  NostrEvent *event = nostr_event_new(); nostr_event_deserialize(event, json);
  for (guint i = 0; urls[i] && !g_cancellable_is_cancelled(j->cancel); i++) {
    if (!endpoint(urls[i], TRUE)) continue;
    Fetch *f = g_new0(Fetch, 1); f->job = j; f->signer = signer; f->publisher = publisher;
    f->required_auth = probe_relay(j, urls[i]);
    f->signed_json = json; f->event_id = g_strdup(nostr_event_get_id(event)); f->transport = nostr_publish_transport_new_websocket(urls[i]);
    g_ptr_array_add(connections, f);
    nostr_publish_transport_set_listener(f->transport, fetch_frame, f); nostr_publish_transport_set_state_callback(f->transport, fetch_state, f);
    g_autoptr(GSource) timer = g_timeout_source_new_seconds(15); g_source_set_callback(timer, deadline, f, NULL); g_source_attach(timer, g_main_context_get_thread_default());
    g_autoptr(GSource) cancellation = g_cancellable_source_new(j->cancel); g_source_set_callback(cancellation, G_SOURCE_FUNC(cancelled), f, NULL); g_source_attach(cancellation, g_main_context_get_thread_default());
    nostr_publish_transport_connect_async(f->transport);
    while (!f->done && (!f->connected || (f->required_auth && !f->authenticated))) g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
    g_source_destroy(timer); g_source_destroy(cancellation);
    if (f->connected && !f->done) nostr_publisher_bind_transport(publisher, urls[i], f->transport);
  }
  nostr_event_free(event); return connections;
}
static char **configured(Job *j, gboolean write) {
  RelayStore *store = relay_store_new_for_identity(j->npub); relay_store_load(store);
  if (!relay_store_identity_has_config(j->npub)) { relay_store_free(store); store = relay_store_new(); relay_store_load(store); }
  g_autoptr(GPtrArray) rows = write ? relay_store_get_write_relays(store) : relay_store_get_read_relays(store);
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  for (guint i = 0; rows && i < rows->len; i++) { const char *url = g_ptr_array_index(rows, i); if (endpoint(url, TRUE)) g_strv_builder_add(b, url); }
  if ((!rows || !rows->len) && !relay_store_identity_has_config(j->npub)) {
    g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(g_settings_schema_source_get_default(), "org.nostr.Grotto", TRUE);
    if (schema) {
      g_autoptr(GSettings) settings = g_settings_new_full(schema, NULL, NULL);
      g_autoptr(GVariant) explicit = g_settings_get_user_value(settings, "relays");
      g_auto(GStrv) urls = explicit ? g_variant_dup_strv(explicit, NULL) : NULL;
      for (guint i = 0; urls && urls[i]; i++) if (endpoint(urls[i], TRUE)) g_strv_builder_add(b, urls[i]);
    }
  }
  relay_store_free(store); return g_strv_builder_end(b);
}
static char **tag_urls(const char *event, const char *tag, gboolean relay) {
  g_autoptr(GStrvBuilder) b = g_strv_builder_new(); g_autoptr(JsonParser) parser = json_parser_new();
  if (event && json_parser_load_from_data(parser, event, -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) {
    JsonObject *o = json_node_get_object(json_parser_get_root(parser)); JsonNode *node = json_object_get_member(o, "tags");
    if (node && JSON_NODE_HOLDS_ARRAY(node)) {
      JsonArray *tags = json_node_get_array(node);
      for (guint i = 0; i < json_array_get_length(tags); i++) {
        JsonNode *n = json_array_get_element(tags, i); if (!JSON_NODE_HOLDS_ARRAY(n)) continue;
        JsonArray *t = json_node_get_array(n); if (json_array_get_length(t) < 2) continue;
        JsonNode *k = json_array_get_element(t, 0), *v = json_array_get_element(t, 1);
        if (!JSON_NODE_HOLDS_VALUE(k) || !JSON_NODE_HOLDS_VALUE(v) || json_node_get_value_type(k) != G_TYPE_STRING || json_node_get_value_type(v) != G_TYPE_STRING) continue;
        const char *url = json_node_get_string(v);
        if (!strcmp(json_node_get_string(k), tag) && endpoint(url, relay)) g_strv_builder_add(b, url);
      }
    }
  }
  return g_strv_builder_end(b);
}
static gboolean discover(Job *j, NostrPublishSigner *signer, char **events, GError **error) {
  g_auto(GStrv) reads = configured(j, FALSE), writes = configured(j, TRUE);
  g_auto(GStrv) urls = NULL;
  g_autoptr(GStrvBuilder) initial = g_strv_builder_new();
  if (events[1]) urls = nostr_publish_nip65_relays(events[1], NOSTR_PUBLISH_NIP65_WRITE, NULL);
  else {
    g_strv_builder_addv(initial, (const char **)reads); g_strv_builder_addv(initial, (const char **)writes);
    urls = g_strv_builder_end(initial);
    if (!urls[0] && !relay_store_identity_has_config(j->npub)) {
      g_autoptr(GSettings) settings = g_settings_new("org.nostr.Grotto");
      if (g_settings_get_boolean(settings, "profile-bootstrap-enabled")) {
        g_strfreev(urls); urls = g_settings_get_strv(settings, "profile-bootstrap-relays");
        j->bootstrap_used = j->bootstrap_round = TRUE;
      }
    }
  }
  guint attempted = 0, completed = 0;
  g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  for (guint round = 0; round < 2; round++) {
    g_autoptr(GStrvBuilder) batch = g_strv_builder_new();
    for (guint i = 0; urls && urls[i] && i < 16; i++) {
      g_autofree char *attempt = g_strdup_printf("%c:%s", j->bootstrap_round ? 'b' : 'o', urls[i]);
      if (endpoint(urls[i], TRUE) && g_hash_table_add(seen, g_strdup(attempt))) {
        g_strv_builder_add(batch, urls[i]);
        if (!j->discovery_relays || !g_strv_contains((const char *const *)j->discovery_relays, urls[i])) {
          g_autoptr(GStrvBuilder) contacts = g_strv_builder_new();
          if (j->discovery_relays) g_strv_builder_addv(contacts, (const char **)j->discovery_relays);
          g_strv_builder_add(contacts, urls[i]); g_strfreev(j->discovery_relays); j->discovery_relays = g_strv_builder_end(contacts);
        }
        if (j->bootstrap_round && (!j->bootstrap_relays || !g_strv_contains((const char *const *)j->bootstrap_relays, urls[i]))) {
          g_autoptr(GStrvBuilder) boot = g_strv_builder_new();
          if (j->bootstrap_relays) g_strv_builder_addv(boot, (const char **)j->bootstrap_relays);
          g_strv_builder_add(boot, urls[i]); g_strfreev(j->bootstrap_relays); j->bootstrap_relays = g_strv_builder_end(boot);
        }
        attempted++;
      }
    }
    g_auto(GStrv) unique = g_strv_builder_end(batch);
    completed += fetch_relays(j, signer, unique, events);
    j->bootstrap_round = FALSE;
    g_strfreev(urls); urls = events[1] ? nostr_publish_nip65_relays(events[1], NOSTR_PUBLISH_NIP65_WRITE, NULL) : NULL;
  }
  j->no_relays = attempted == 0;
  if (g_cancellable_set_error_if_cancelled(j->cancel, error)) return FALSE;
  if (!attempted) { j->bootstrap_used = FALSE; return TRUE; } /* Editable, not an offline claim. */
  if (!completed || (!events[0] && completed != attempted)) {
    g_autofree char *skipped = j->auth_required_relays ? g_strjoinv("\n", j->auth_required_relays) : NULL;
    if (skipped) g_set_error(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, _("Profile unavailable — relay requires authentication. Bootstrap discovery never signs or sends AUTH.\n%s"), skipped);
    else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, _("Profile unavailable. Check your relays and try again"));
    return FALSE;
  }
  return TRUE;
}
static GrottoProfileInfo *profile_parse(Job *j, const char *event, GError **error) {
  GrottoProfileInfo *p = grotto_profile_info_copy(NULL);
  if (!event) return p;
  NostrEvent *e = nostr_event_new();
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!verified(event, kinds[0], j->hex, &p->created_at) || nostr_event_deserialize(e, event)) goto invalid;
  if (!json_parser_load_from_data(parser, nostr_event_get_content(e), -1, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) goto invalid;
  JsonObject *content = json_node_get_object(json_parser_get_root(parser));
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) { g_free(p->fields[i]); p->fields[i] = g_strdup(string(content, field_names[i])); }
  JsonNode *bot = json_object_get_member(content, "bot");
  p->bot = bot && JSON_NODE_HOLDS_VALUE(bot) && json_node_get_value_type(bot) == G_TYPE_BOOLEAN && json_node_get_boolean(bot);
  if (!grotto_profile_info_validate(p, error)) { nostr_event_free(e); grotto_profile_info_free(p); return NULL; }
  p->published = TRUE; nostr_event_free(e); return p;
invalid:
  nostr_event_free(e); grotto_profile_info_free(p); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The published profile is not valid metadata")); return NULL;
}
static GBytes *read_stream(GInputStream *stream, gsize limit, GCancellable *cancel, GError **error) {
  g_autoptr(GByteArray) bytes = g_byte_array_new(); guint8 buffer[16384]; gssize n;
  while ((n = g_input_stream_read(stream, buffer, MIN(sizeof buffer, limit + 1 - bytes->len), cancel, error)) > 0) {
    g_byte_array_append(bytes, buffer, n);
    if (bytes->len > limit) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, _("Choose an image smaller than 5 MB")); return NULL; }
  }
  return n < 0 ? NULL : g_byte_array_free_to_bytes(g_steal_pointer(&bytes));
}
static void image_size(GdkPixbufLoader *loader, int width, int height, gpointer data) {
  gboolean *ok = data; *ok = width > 0 && height > 0 && width <= 8192 && height <= 8192 && (guint64)width * height <= IMAGE_PIXELS;
  if (!*ok) gdk_pixbuf_loader_set_size(loader, 1, 1);
}
GdkTexture *grotto_profile_decode_image(GBytes *bytes, char **mime, GError **error) {
  gsize len; const guint8 *data = g_bytes_get_data(bytes, &len);
  /* Select a raster decoder before parsing; a renamed SVG must never reach
   * the generic loader (which can resolve external resources). */
  const char *type = len >= 8 && !memcmp(data, "\211PNG\r\n\032\n", 8) ? "png" :
    len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff ? "jpeg" :
    len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WEBP", 4) ? "webp" : NULL;
  if (!type || len > IMAGE_LIMIT) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("Choose a PNG, JPEG or WebP image smaller than 5 MB")); return NULL;
  }
  g_autoptr(GdkPixbufLoader) loader = gdk_pixbuf_loader_new_with_type(type, error);
  if (!loader) return NULL;
  gboolean size_ok = FALSE;
  g_signal_connect(loader, "size-prepared", G_CALLBACK(image_size), &size_ok);
  gboolean ok = gdk_pixbuf_loader_write(loader, data, len, error);
  if (!gdk_pixbuf_loader_close(loader, ok ? error : NULL)) ok = FALSE;
  if (!ok || !size_ok) {
    if (error && !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("Choose an image up to 8192 pixels per side and 24 megapixels")); return NULL;
  }
  if (mime) *mime = g_strdup(!strcmp(type, "png") ? "image/png" : !strcmp(type, "jpeg") ? "image/jpeg" : "image/webp");
  return gdk_texture_new_for_pixbuf(gdk_pixbuf_loader_get_pixbuf(loader));
}
static GdkTexture *download_image(Job *j, const char *url, gboolean online) {
  if (!endpoint(url, FALSE)) return NULL;
  g_autofree char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, url, -1);
  g_autofree char *name = g_strdup_printf("%s-image-%s", j->hex, hash);
  g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "grotto", "profiles", NULL);
  g_autofree char *path = g_build_filename(dir, name, NULL);
  g_autoptr(GBytes) bytes = NULL;
  if (online && !g_cancellable_is_cancelled(j->cancel)) {
    g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 15, NULL);
    g_autoptr(SoupMessage) message = soup_message_new("GET", url); soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
    g_autoptr(GInputStream) stream = soup_session_send(session, message, j->cancel, NULL);
    if (stream && soup_message_get_status(message) == SOUP_STATUS_OK) bytes = read_stream(stream, IMAGE_LIMIT, j->cancel, NULL);
    GdkTexture *texture = bytes ? grotto_profile_decode_image(bytes, NULL, NULL) : NULL;
    if (texture) {
      if (!g_cancellable_is_cancelled(j->cancel) && !g_mkdir_with_parents(dir, 0700)) {
        gsize size; const char *data = g_bytes_get_data(bytes, &size);
        g_file_set_contents_full(path, data, size, G_FILE_SET_CONTENTS_CONSISTENT, 0600, NULL);
      }
      return texture;
    }
  }
  GStatBuf st; g_autofree char *data = NULL; gsize size;
  if (g_stat(path, &st) || st.st_size > IMAGE_LIMIT || !g_file_get_contents(path, &data, &size, NULL)) return NULL;
  g_clear_pointer(&bytes, g_bytes_unref); bytes = g_bytes_new_take(g_steal_pointer(&data), size);
  return grotto_profile_decode_image(bytes, NULL, NULL);
}
typedef struct { gboolean done, ok; guint accepted; } Publication;
static void published(NostrPublisher *publisher, const NostrPublishResult *result, gpointer data) {
  (void)publisher; Publication *p = data; p->done = TRUE; p->ok = nostr_publish_result_get_verdict(result) == NOSTR_PUBLISH_VERDICT_PUBLISHED;
  p->accepted = nostr_publish_result_get_n_accepted(result);
}
static gboolean publish_deadline(gpointer data) { nostr_publisher_tick(data, g_get_real_time() / G_USEC_PER_SEC); return G_SOURCE_CONTINUE; }
static char *save_profile(Job *j, NostrPublishSigner *signer, char **events, GError **error) {
  g_auto(GStrv) urls = events[1] ? nostr_publish_nip65_relays(events[1], NOSTR_PUBLISH_NIP65_WRITE, NULL) : configured(j, TRUE);
  if (!urls || !urls[0]) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No write relays are configured for this identity")); return NULL; }
  /* Keep fields from other clients (including extension fields) when editing. */
  g_autoptr(JsonParser) parser = json_parser_new();
  JsonObject *content = NULL; guint64 timestamp = 0;
  if (events[0]) {
    NostrEvent *e = nostr_event_new(); nostr_event_deserialize(e, events[0]);
    if (json_parser_load_from_data(parser, nostr_event_get_content(e), -1, NULL) && JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) content = json_node_get_object(json_parser_get_root(parser));
    timestamp = nostr_event_get_created_at(e); nostr_event_free(e);
  }
  if (timestamp > j->info->created_at) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, _("This profile changed elsewhere. Refresh it before saving")); return NULL; }
  g_autoptr(JsonObject) fresh = content ? json_object_ref(content) : json_object_new();
  for (guint i = 0; i < GROTTO_PROFILE_N_FIELDS; i++) json_object_set_string_member(fresh, field_names[i], j->info->fields[i]);
  json_object_set_boolean_member(fresh, "bot", j->info->bot);
  g_autoptr(JsonNode) node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, fresh);
  g_autofree char *text = json_to_string(node, FALSE);
  NostrEvent *event = nostr_event_new(); nostr_event_set_kind(event, NOSTR_KIND_PROFILE_METADATA); nostr_event_set_pubkey(event, j->hex);
  nostr_event_set_created_at(event, MAX((guint64)(g_get_real_time() / G_USEC_PER_SEC), timestamp + 1)); nostr_event_set_content(event, text);
  g_autofree char *raw = nostr_event_serialize_compact(event); nostr_event_free(event);
  g_strfreev(j->destinations); j->destinations = g_strdupv(urls);
  g_autofree char *signed_json = nostr_publish_signer_sign_event_json(signer, raw, j->cancel, error);
  if (!signed_json || g_cancellable_set_error_if_cancelled(j->cancel, error)) return NULL;
  NostrPublisher *publisher = nostr_publisher_new(signer);
  g_autoptr(GPtrArray) connections = open_publish(j, signer, publisher, signed_json, urls);
  if (g_cancellable_set_error_if_cancelled(j->cancel, error)) {
    g_clear_pointer(&connections, g_ptr_array_unref); nostr_publisher_free(publisher); return NULL;
  }
  for (guint i = 0; i < connections->len; i++) ((Fetch *)g_ptr_array_index(connections, i))->publishing = TRUE;
  NostrPublishPolicy policy; nostr_publish_policy_init(&policy); policy.ok_wait_sec = 240;
  Publication p = {0};
  gboolean started = nostr_publisher_publish_signed(publisher, signed_json, (const char *const *)urls, &policy, 0, NULL, published, &p, NULL, error);
  g_autoptr(GSource) timer = g_timeout_source_new_seconds(1); g_source_set_callback(timer, publish_deadline, publisher, NULL); g_source_attach(timer, g_main_context_get_thread_default());
  while (started && !p.done && !g_cancellable_is_cancelled(j->cancel)) g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
  g_source_destroy(timer); g_clear_pointer(&connections, g_ptr_array_unref); nostr_publisher_free(publisher);
  if (!started) return NULL;
  if (g_cancellable_set_error_if_cancelled(j->cancel, error)) return NULL;
  if (!p.ok) {
    if (p.accepted) cache_write(j, kinds[0], signed_json);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, p.accepted
      ? _("Your profile reached some relays, but not all. Close the editor and refresh before trying again")
      : _("No relay confirmed publication. Your edits are still here")); return NULL;
  }
  cache_write(j, kinds[0], signed_json); return g_steal_pointer(&signed_json);
}
static void upload_done(GObject *source, GAsyncResult *result, gpointer data) { (void)source; *(GAsyncResult **)data = g_object_ref(result); }
static gboolean upload(Job *j, NostrPublishSigner *signer, GError **error) {
  char **servers = j->destinations;
  if (!servers || !servers[0]) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No Blossom servers found. Set up Media Servers in Nostr Settings first")); return FALSE; }
  g_autofree char *hash = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, j->bytes);
  NostrEvent *event = nostr_bud02_create_auth_event(NOSTR_BUD02_ACTION_UPLOAD, hash, 0, NULL); nostr_event_set_pubkey(event, j->hex);
  g_autofree char *raw = nostr_event_serialize_compact(event); nostr_event_free(event);
  g_autofree char *auth = nostr_publish_signer_sign_event_json(signer, raw, j->cancel, error); if (!auth) return FALSE;
  for (guint i = 0; servers[i] && !g_cancellable_is_cancelled(j->cancel); i++) {
    g_autoptr(GnBlossomClient) client = gn_blossom_client_new(servers[i]); gn_blossom_client_set_limits(client, IMAGE_LIMIT, 60, "Grotto/" GROTTO_VERSION);
    GAsyncResult *result = NULL; gn_blossom_client_upload_async(client, j->bytes, hash, j->mime, auth, j->cancel, upload_done, &result);
    while (!result) g_main_context_iteration(g_main_context_get_thread_default(), TRUE);
    g_autofree char *url = gn_blossom_client_upload_finish(client, result, NULL); g_object_unref(result);
    if (url && !*url) { g_free(url); url = g_strdup_printf("%s%s%s", servers[i], g_str_has_suffix(servers[i], "/") ? "" : "/", hash); }
    if (!url || !endpoint(url, FALSE)) continue;
    guint index = j->action == GROTTO_PROFILE_CHOOSE_AVATAR ? 0 : 1, field = index ? GROTTO_PROFILE_BANNER : GROTTO_PROFILE_PICTURE;
    g_free(j->info->fields[field]); j->info->fields[field] = g_strdup(url); g_set_object(&j->info->images[index], j->preview); return TRUE;
  }
  if (!g_cancellable_set_error_if_cancelled(j->cancel, error)) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Image upload failed. Check your media servers and try again"));
  return FALSE;
}
static GrottoNip05Status verify_nip05(Job *j, const char *identifier) {
  if (!identifier || !*identifier) return GROTTO_NIP05_UNCHECKED;
  g_auto(GStrv) parts = g_strsplit(identifier, "@", -1);
  if (g_strv_length(parts) != 2 || !*parts[0] || !*parts[1]) return GROTTO_NIP05_INVALID;
  for (const char *p = parts[0]; *p; p++)
    if (!g_ascii_islower(*p) && !g_ascii_isdigit(*p) && !strchr("-_.", *p)) return GROTTO_NIP05_INVALID;
  for (const char *p = parts[1]; *p; p++)
    if (!g_ascii_isalnum(*p) && *p != '-' && *p != '.') return GROTTO_NIP05_INVALID;
  g_autofree char *url = g_strdup_printf("https://%s/.well-known/nostr.json?name=%s", parts[1], parts[0]);
  g_autoptr(SoupSession) session = soup_session_new_with_options("timeout", 10, NULL);
  g_autoptr(SoupMessage) message = soup_message_new("GET", url);
  if (!message) return GROTTO_NIP05_INVALID;
  soup_message_set_flags(message, SOUP_MESSAGE_NO_REDIRECT);
  g_autoptr(GInputStream) stream = soup_session_send(session, message, j->cancel, NULL);
  g_autoptr(GBytes) bytes = stream && soup_message_get_status(message) == SOUP_STATUS_OK ? read_stream(stream, 65536, j->cancel, NULL) : NULL;
  if (!bytes) return GROTTO_NIP05_UNAVAILABLE;
  gsize length; const char *data = g_bytes_get_data(bytes, &length);
  return grotto_profile_nip05_response(data, length, parts[0], j->hex);
}
GrottoNip05Status grotto_profile_nip05_response(const char *data, gsize length, const char *name, const char *hex) {
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, data, length, NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))) return GROTTO_NIP05_UNAVAILABLE;
  JsonNode *names = json_object_get_member(json_node_get_object(json_parser_get_root(parser)), "names");
  if (!names || !JSON_NODE_HOLDS_OBJECT(names)) return GROTTO_NIP05_UNAVAILABLE;
  const char *key = string(json_node_get_object(names), name);
  return strlen(key) == 64 && !strcmp(key, hex) ? GROTTO_NIP05_VERIFIED : GROTTO_NIP05_MISMATCH;
}
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; Job *j = data; j->cancel = cancel;
  g_autoptr(GError) error = NULL;
  g_autoptr(GMainContext) context = g_main_context_new(); g_main_context_push_thread_default(context);
  /* Each operation owns a connection. Closing it on this worker cancels
   * the daemon continuation too; a local GCancellable alone cannot cancel
   * an already-delivered D-Bus signing request on the UI's shared bus. */
  g_autofree char *address = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, cancel, &error);
  j->bus = address ? g_dbus_connection_new_for_address_sync(address,
    G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
    NULL, cancel, &error) : NULL;
  if (j->bus) g_dbus_connection_set_exit_on_close(j->bus, FALSE);
  else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
    g_clear_error(&error); g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE,
      _("Signer unavailable. Open Grotto and try again"));
  }
  const NostrPublishSignerVTable table = {.sign_event_json = sign_event};
  g_autoptr(NostrPublishSigner) signer = nostr_publish_signer_new_from_vtable(&table, j);
  char *events[G_N_ELEMENTS(kinds)] = {0};
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) events[i] = cache_read(j, kinds[i]);
  gboolean network = TRUE;
  g_autoptr(GSettingsSchema) schema = g_settings_schema_source_lookup(g_settings_schema_source_get_default(), "org.nostr.Grotto", TRUE);
  if (schema) {
    g_autoptr(GSettings) settings = g_settings_new_full(schema, NULL, NULL);
    network = !g_settings_get_boolean(settings, "use-tor-for-relays");
  }
  if (!network && !error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
    _("Profile network access is unavailable in Tor mode. The saved profile is still available"));
  gboolean metadata = j->action == GROTTO_PROFILE_FETCH || j->action == GROTTO_PROFILE_SAVE;
  gboolean online = network && j->bus && (!metadata || discover(j, signer, events, &error));
  GrottoProfileInfo *result = NULL;
  if (j->action == GROTTO_PROFILE_FETCH) {
    if (online || events[0]) { g_clear_error(&error); result = profile_parse(j, events[0], &error); if (result) { result->cached = !online; result->tor_blocked = !network; } }
    if (result) {
      if (online) for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) cache_write(j, kinds[i], events[i]);
      for (guint i = 0; i < 2; i++) result->images[i] = download_image(j, result->fields[i ? GROTTO_PROFILE_BANNER : GROTTO_PROFILE_PICTURE], FALSE);
    }
  } else if (j->action == GROTTO_PROFILE_VERIFY_NIP05) {
    result = grotto_profile_info_copy(j->info);
    result->nip05_status = network ? verify_nip05(j, result->fields[GROTTO_PROFILE_NIP05]) : GROTTO_NIP05_UNAVAILABLE;
  } else if (online && j->action == GROTTO_PROFILE_LOAD_IMAGES) {
    result = grotto_profile_info_copy(j->info);
    for (guint i = 0; i < 2; i++) {
      g_clear_object(&result->images[i]);
      result->images[i] = download_image(j, result->fields[i ? GROTTO_PROFILE_BANNER : GROTTO_PROFILE_PICTURE], TRUE);
    }
    if ((*result->fields[GROTTO_PROFILE_PICTURE] && !result->images[0]) || (*result->fields[GROTTO_PROFILE_BANNER] && !result->images[1])) {
      g_clear_pointer(&result, grotto_profile_info_free);
      g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED, _("Some profile images could not be loaded. Check the URLs or try again"));
    }
  } else if (online && j->action == GROTTO_PROFILE_SAVE) {
    g_autofree char *saved = save_profile(j, signer, events, &error);
    if (saved) {
      g_free(events[0]); events[0] = g_steal_pointer(&saved);
      /* Re-fetch through the same EOSE-aware path, retaining the acknowledged
       * event if a read replica has not caught up yet. Never sign twice. */
      g_autoptr(GError) refresh_error = NULL; gboolean refreshed = discover(j, signer, events, &refresh_error);
      result = profile_parse(j, events[0], &error); if (result) { result->cached = !refreshed;
        for (guint i = 0; i < 2; i++) {
          guint field = i ? GROTTO_PROFILE_BANNER : GROTTO_PROFILE_PICTURE;
          result->images[i] = j->info->images[i] && !g_strcmp0(result->fields[field], j->info->fields[field]) ? g_object_ref(j->info->images[i]) : download_image(j, result->fields[field], FALSE);
        }
      }
    }
  } else if (online && upload(j, signer, &error)) result = grotto_profile_info_copy(j->info);
  if (result && metadata) {
    result->no_relays = j->no_relays; result->bootstrap_used = j->bootstrap_used;
    result->discovery_relays = g_strdupv(j->discovery_relays);
    result->auth_required_relays = g_strdupv(j->auth_required_relays);
    result->bootstrap_relays = g_strdupv(j->bootstrap_relays);
    result->publish_relays = events[1] ? nostr_publish_nip65_relays(events[1], NOSTR_PUBLISH_NIP65_WRITE, NULL) : configured(j, TRUE);
    if (online) for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) cache_write(j, kinds[i], events[i]);
    if (online && !j->no_relays) result->nip05_status = verify_nip05(j, result->fields[GROTTO_PROFILE_NIP05]);
  }
  if (g_cancellable_is_cancelled(cancel)) { g_clear_pointer(&result, grotto_profile_info_free); g_clear_error(&error); g_cancellable_set_error_if_cancelled(cancel, &error); }
  for (guint i = 0; i < G_N_ELEMENTS(kinds); i++) g_free(events[i]);
  if (j->bus) g_dbus_connection_close_sync(j->bus, NULL, NULL);
  g_main_context_pop_thread_default(context);
  if (result) g_task_return_pointer(task, result, (GDestroyNotify)grotto_profile_info_free);
  else if (error) g_task_return_error(task, g_steal_pointer(&error));
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Profile operation failed");
}
AdwAlertDialog *grotto_profile_upload_confirmation(GdkTexture *preview, gboolean banner, const char *const *servers) {
  g_autofree char *destinations = g_strjoinv("\n", (char **)servers);
  g_autofree char *body = g_strdup_printf(_("Uploading makes this image public immediately, before Save. Cancelling the profile edit will not delete it or recall copies.\n\nThe selected crop is re-encoded as PNG without the original EXIF, location or device metadata. Only the previewed pixels are uploaded.\n\nPNG, JPEG or WebP only; up to 5 MiB, 8192 pixels per side and 24 megapixels.\n\nVerified kind-10063 servers, tried in this order:\n%s"), destinations);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Crop and Upload Profile Image?"), body));
  GtkWidget *picture = gtk_picture_new_for_paintable(GDK_PAINTABLE(preview)); gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE); gtk_widget_set_size_request(picture, -1, 180);
  gtk_picture_set_alternative_text(GTK_PICTURE(picture), banner ? _("Selected Banner") : _("Selected Avatar"));
  adw_alert_dialog_set_extra_child(dialog, picture);
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "upload", _("Upload Cropped Image"), NULL);
  adw_alert_dialog_set_default_response(dialog, "cancel"); adw_alert_dialog_set_close_response(dialog, "cancel");
  adw_alert_dialog_set_response_appearance(dialog, "upload", ADW_RESPONSE_SUGGESTED); return dialog;
}
AdwAlertDialog *grotto_profile_images_confirmation(const GrottoProfileInfo *info) {
  g_autofree char *body = g_strdup_printf(_("Loading this identity's avatar and banner contacts the HTTPS origins below and reveals your network address to them. This applies to your own and watch-only identities. A signed profile authenticates the URLs, not the image bytes or the server. Image origins are never contacted automatically or by signing reviews.\n\nAvatar: %s\nBanner: %s"), info->fields[GROTTO_PROFILE_PICTURE], info->fields[GROTTO_PROFILE_BANNER]);
  AdwAlertDialog *dialog = ADW_ALERT_DIALOG(adw_alert_dialog_new(_("Load Profile Images?"), body));
  adw_alert_dialog_add_responses(dialog, "cancel", _("Cancel"), "load", _("Load Images"), NULL);
  adw_alert_dialog_set_default_response(dialog, "cancel"); adw_alert_dialog_set_close_response(dialog, "cancel"); return dialog;
}
static void preview_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel) {
  (void)source; Job *j = data; g_autoptr(GError) error = NULL;
  g_autoptr(GFileInputStream) stream = g_file_read(j->file, cancel, &error);
  j->bytes = stream ? read_stream(G_INPUT_STREAM(stream), IMAGE_LIMIT, cancel, &error) : NULL;
  j->preview = j->bytes ? grotto_profile_decode_image(j->bytes, &j->mime, &error) : NULL;
  if (j->preview) {
    g_autofree char *server_event = cache_read(j, NOSTR_KIND_BLOSSOM_SERVER_LIST);
    j->destinations = tag_urls(server_event, "server", FALSE);
    if (!j->destinations[0]) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, _("No verified Blossom server list found. Refresh your profile after setting up Media Servers in Nostr Settings"));
  }
  if (!error) g_task_return_boolean(task, TRUE); else g_task_return_error(task, g_steal_pointer(&error));
}
static void preview_answer(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data;
  const char *answer = adw_alert_dialog_choose_finish(ADW_ALERT_DIALOG(source), result);
  Job *j = g_task_get_task_data(task); j->crop_frozen = TRUE;
  GtkWidget *controls = adw_alert_dialog_get_extra_child(ADW_ALERT_DIALOG(source));
  if (controls) gtk_widget_set_sensitive(controls, FALSE);
  if (!strcmp(answer, "upload") || !strcmp(answer, "load")) {
    g_task_run_in_thread(task, worker);
  }
  else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Image selection cancelled");
  g_object_unref(task);
}
GBytes *grotto_profile_crop_image(GdkTexture *source, gboolean banner, double zoom, double x, double y) {
  g_autoptr(GdkPixbuf) original = gdk_pixbuf_get_from_texture(source);
  int width = gdk_pixbuf_get_width(original), height = gdk_pixbuf_get_height(original);
  double ratio = banner ? 3.0 : 1.0;
  int cw = MAX(1, (int)(MIN(width, height * ratio) / CLAMP(zoom, 1.0, 4.0)));
  int ch = MAX(1, MIN(height, (int)(cw / ratio)));
  int left = (int)((width - cw) * CLAMP(x, 0.0, 1.0)), top = (int)((height - ch) * CLAMP(y, 0.0, 1.0));
  g_autoptr(GdkPixbuf) region = gdk_pixbuf_new_subpixbuf(original, left, top, cw, ch);
  int outw = MIN(cw, banner ? 1500 : 512), outh = MAX(1, (int)(outw / ratio));
  g_autoptr(GdkPixbuf) scaled = gdk_pixbuf_scale_simple(region, outw, outh, GDK_INTERP_BILINEAR);
  char *bytes = NULL; gsize length = 0;
  if (!gdk_pixbuf_save_to_buffer(scaled, &bytes, &length, "png", NULL, NULL)) return NULL;
  return g_bytes_new_take(bytes, length);
}
typedef struct { Job *job; GTask *task; GdkTexture *original; GtkPicture *picture; GtkRange *zoom, *x, *y; } Crop;
static void crop_free(gpointer data) { Crop *c = data; g_object_unref(c->original); g_object_unref(c->task); g_free(c); }
static void crop_changed(GtkRange *range, gpointer data) {
  (void)range; Crop *c = data; if (c->job->crop_frozen) return;
  g_autoptr(GBytes) bytes = grotto_profile_crop_image(c->original, c->job->action == GROTTO_PROFILE_CHOOSE_BANNER,
    gtk_range_get_value(c->zoom), gtk_range_get_value(c->x), gtk_range_get_value(c->y));
  if (!bytes) return;
  g_clear_pointer(&c->job->bytes, g_bytes_unref); c->job->bytes = g_bytes_ref(bytes);
  g_clear_object(&c->job->preview); c->job->preview = gdk_texture_new_from_bytes(bytes, NULL);
  g_free(c->job->mime); c->job->mime = g_strdup("image/png");
  gtk_picture_set_paintable(c->picture, GDK_PAINTABLE(c->job->preview));
}
static GtkRange *crop_range(GtkBox *box, const char *label, double min, double max, double value) {
  GtkWidget *scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, .01);
  gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE); gtk_range_set_value(GTK_RANGE(scale), value);
  gtk_accessible_update_property(GTK_ACCESSIBLE(scale), GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);
  GtkWidget *text = gtk_label_new(label); gtk_label_set_xalign(GTK_LABEL(text), 0);
  gtk_box_append(box, text); gtk_box_append(box, scale); return GTK_RANGE(scale);
}
static void preview_ready(GObject *source, GAsyncResult *result, gpointer data) {
  (void)source; GTask *task = data; Job *j = g_task_get_task_data(task); g_autoptr(GError) error = NULL;
  if (!g_task_propagate_boolean(G_TASK(result), &error)) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) g_task_return_error(task, g_steal_pointer(&error));
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, _("The image could not be opened. Choose a PNG, JPEG or WebP image smaller than 5 MB, up to 8192 pixels per side and 24 megapixels"));
    g_object_unref(task); return;
  }
  AdwAlertDialog *dialog = grotto_profile_upload_confirmation(j->preview, j->action == GROTTO_PROFILE_CHOOSE_BANNER, (const char *const *)j->destinations);
  Crop *crop = g_new0(Crop, 1); crop->job = j; crop->task = g_object_ref(task); crop->original = g_object_ref(j->preview);
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
  crop->picture = GTK_PICTURE(gtk_picture_new()); gtk_picture_set_can_shrink(crop->picture, TRUE);
  gtk_picture_set_alternative_text(crop->picture, _("Cropped Image Preview"));
  gtk_widget_set_size_request(GTK_WIDGET(crop->picture), -1, 180); gtk_box_append(GTK_BOX(box), GTK_WIDGET(crop->picture));
  GtkWidget *guidance = gtk_label_new(j->action == GROTTO_PROFILE_CHOOSE_BANNER ? _("Banner: 3∶1, up to 1500 × 500 pixels") : _("Avatar: square, up to 512 × 512 pixels"));
  gtk_label_set_wrap(GTK_LABEL(guidance), TRUE); gtk_box_append(GTK_BOX(box), guidance);
  crop->zoom = crop_range(GTK_BOX(box), _("Zoom"), 1, 4, 1);
  crop->x = crop_range(GTK_BOX(box), _("Horizontal Position"), 0, 1, .5);
  crop->y = crop_range(GTK_BOX(box), _("Vertical Position"), 0, 1, .5);
  g_signal_connect(crop->zoom, "value-changed", G_CALLBACK(crop_changed), crop);
  g_signal_connect(crop->x, "value-changed", G_CALLBACK(crop_changed), crop);
  g_signal_connect(crop->y, "value-changed", G_CALLBACK(crop_changed), crop);
  adw_alert_dialog_set_extra_child(dialog, box); g_object_set_data_full(G_OBJECT(dialog), "crop", crop, crop_free); crop_changed(NULL, crop);
  adw_alert_dialog_choose(ADW_ALERT_DIALOG(dialog), g_task_get_source_object(task), g_task_get_cancellable(task), preview_answer, task);
}
static void chosen(GObject *source, GAsyncResult *result, gpointer data) {
  GTask *task = data; Job *j = g_task_get_task_data(task); g_autoptr(GError) error = NULL;
  j->file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), result, &error);
  if (!j->file || !g_file_is_native(j->file)) {
    if (j->file) g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, _("Choose a local image file"));
    else if (g_error_matches(error, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED) || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
      g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Image selection cancelled");
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, _("Image selection is unavailable. Try again"));
    g_object_unref(task); return;
  }
  GTask *preview = g_task_new(NULL, g_task_get_cancellable(task), preview_ready, task);
  g_task_set_task_data(preview, j, NULL); g_task_run_in_thread(preview, preview_worker); g_object_unref(preview);
}
void grotto_profile_service_async(GtkWindow *parent, GrottoApprovalController *controller, GrottoProfileAction action, const char *npub,
  const GrottoProfileInfo *info, GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
  g_autoptr(GCancellable) owned_cancel = cancel ? g_object_ref(cancel) : g_cancellable_new();
  GTask *task = g_task_new(parent, owned_cancel, callback, data); guint8 pk[32];
  if (action < GROTTO_PROFILE_FETCH || action > GROTTO_PROFILE_VERIFY_NIP05 || (action != GROTTO_PROFILE_FETCH && !info) || !npub || nostr_nip19_decode_npub(npub, pk) || !grotto_profile_info_validate(info, NULL)) {
    g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid identity or profile"); g_object_unref(task); return;
  }
  Job *j = g_new0(Job, 1); j->controller = controller ? g_object_ref(controller) : NULL;
  j->ui_context = g_main_context_ref_thread_default(); j->capabilities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); j->action = action;
  j->cache_scope = parent ? g_object_ref(G_OBJECT(parent)) : g_object_new(G_TYPE_OBJECT, NULL);
  j->cache_lock = g_object_get_data(j->cache_scope, "grotto-profile-cache-lock");
  if (!j->cache_lock) {
    j->cache_lock = g_new0(GMutex, 1); g_mutex_init(j->cache_lock);
    g_object_set_data_full(j->cache_scope, "grotto-profile-cache-lock", j->cache_lock, cache_lock_free);
  } j->npub = g_strdup(npub); j->info = grotto_profile_info_copy(info);
  j->bootstrap_used = action != GROTTO_PROFILE_FETCH && info && info->bootstrap_used;
  if (j->bootstrap_used) { j->discovery_relays = g_strdupv(info->discovery_relays); j->bootstrap_relays = g_strdupv(info->bootstrap_relays); }
  for (guint i = 0; i < 32; i++) g_snprintf(j->hex + 2 * i, 3, "%02x", pk[i]);
  g_task_set_task_data(task, j, job_free);
  if (action == GROTTO_PROFILE_LOAD_IMAGES) {
    AdwAlertDialog *dialog = grotto_profile_images_confirmation(info);
    adw_alert_dialog_choose(dialog, GTK_WIDGET(parent), owned_cancel, preview_answer, task);
  } else if (action == GROTTO_PROFILE_CHOOSE_AVATAR || action == GROTTO_PROFILE_CHOOSE_BANNER) {
    g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new(); gtk_file_dialog_set_title(dialog, action == GROTTO_PROFILE_CHOOSE_AVATAR ? _("Choose Avatar") : _("Choose Banner"));
    g_autoptr(GListStore) filters = g_list_store_new(GTK_TYPE_FILE_FILTER); GtkFileFilter *filter = gtk_file_filter_new(); gtk_file_filter_set_name(filter, _("Profile Images"));
    gtk_file_filter_add_mime_type(filter, "image/png"); gtk_file_filter_add_mime_type(filter, "image/jpeg"); gtk_file_filter_add_mime_type(filter, "image/webp"); g_list_store_append(filters, filter); g_object_unref(filter);
    gtk_file_dialog_set_filters(dialog, G_LIST_MODEL(filters)); gtk_file_dialog_open(dialog, parent, cancel, chosen, task);
  } else { g_task_run_in_thread(task, worker); g_object_unref(task); }
}
