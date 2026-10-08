#include "gh-inbox-setup.h"
#include "gh-relay-list-setup.h"
#include "gh-auth-policy.h"
#include "gh-identity.h"

#include <glib/gi18n.h>
#include <json-glib/json-glib.h>
#include <nostr-event.h>
#include <nostr-filter.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define KIND_GIFT_WRAP 1059 /* NIP-59 */
#define MAX_TARGETS 16      /* GhRelayPublish's per-publish limit */

/* ---- relay addresses ------------------------------------------------------- */

static gboolean
invalid(GError **error, const gchar *message)
{
  g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
  return FALSE;
}

static gboolean
is_loopback(const gchar *host)
{
  return g_str_equal(host, "localhost") || g_str_equal(host, "::1") ||
         g_str_has_prefix(host, "127.");
}

/* ASCII host names (GUri has already turned an IDN into its xn-- form) or
 * an IP literal. */
static gboolean
host_is_valid(const gchar *host)
{
  if (strchr(host, ':'))
    return g_hostname_is_ip_address(host);
  if (*host == '.' || *host == '-' || g_str_has_suffix(host, "."))
    return FALSE;
  for (const gchar *c = host; *c; c++)
    if (!g_ascii_isalnum(*c) && *c != '.' && *c != '-')
      return FALSE;
  return TRUE;
}

gchar *
gh_inbox_setup_normalize_url(const gchar *input, GError **error)
{
  g_autofree gchar *text = g_strstrip(g_strdup(input ? input : ""));
  if (!*text) {
    invalid(error, _("Enter a relay address."));
    return NULL;
  }
  for (const gchar *c = text; *c; c++)
    if (g_ascii_isspace(*c)) {
      invalid(error, _("A relay address can't contain spaces."));
      return NULL;
    }
  /* A bare host name means a secure relay. */
  g_autofree gchar *candidate = strstr(text, "://") ? g_strdup(text)
                                                    : g_strconcat("wss://", text, NULL);
  g_autoptr(GUri) uri = g_uri_parse(candidate, G_URI_FLAGS_NONE, NULL);
  if (!uri) {
    invalid(error, _("That doesn't look like a relay address."));
    return NULL;
  }
  g_autofree gchar *scheme = g_ascii_strdown(g_uri_get_scheme(uri), -1);
  const gchar *raw_host = g_uri_get_host(uri);
  g_autofree gchar *host = raw_host ? g_ascii_strdown(raw_host, -1) : NULL;
  if (!g_str_equal(scheme, "wss") && !g_str_equal(scheme, "ws")) {
    invalid(error, _("Relay addresses start with wss:// and this one doesn't."));
    return NULL;
  }
  if (g_uri_get_userinfo(uri)) {
    invalid(error, _("A relay address can't include a user name or password."));
    return NULL;
  }
  if (g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
    invalid(error, _("A relay address can't include “?” or “#” parts."));
    return NULL;
  }
  if (!host || !*host || !host_is_valid(host)) {
    invalid(error, _("That relay address has no valid server name."));
    return NULL;
  }
  /* PD-5: an unencrypted connection would show sign-ins and requests to
   * anyone on the network; only this device's own relays may use one. */
  if (g_str_equal(scheme, "ws") && !is_loopback(host)) {
    invalid(error, _("This relay doesn't use a secure connection. Use an address that starts "
                     "with wss:// instead."));
    return NULL;
  }
  g_autofree gchar *path = g_strdup(g_uri_get_path(uri));
  while (g_str_has_suffix(path, "/"))
    path[strlen(path) - 1] = '\0';
  gint port = g_uri_get_port(uri);
  g_autofree gchar *port_text = port > 0 ? g_strdup_printf(":%d", port) : g_strdup("");
  gboolean bracket = strchr(host, ':') != NULL;
  gchar *url = g_strdup_printf("%s://%s%s%s%s%s", scheme, bracket ? "[" : "", host,
                               bracket ? "]" : "", port_text, path);
  if (!gh_relay_url_validate(url, NULL)) {
    g_free(url);
    invalid(error, _("That doesn't look like a relay address."));
    return NULL;
  }
  return url;
}

/* ---- suggestions -------------------------------------------------------- */

void
gh_inbox_suggestion_free(GhInboxSuggestion *suggestion)
{
  if (!suggestion)
    return;
  g_free(suggestion->url);
  g_free(suggestion->name);
  g_free(suggestion->description);
  g_free(suggestion);
}

static gboolean
bad_data(GError **error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);

static gboolean
bad_data(GError **error, const gchar *format, ...)
{
  va_list args;
  va_start(args, format);
  g_autofree gchar *message = g_strdup_vprintf(format, args);
  va_end(args);
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "relay suggestions: %s", message);
  return FALSE;
}

static const gchar *
string_member(JsonObject *object, const gchar *name)
{
  JsonNode *node = json_object_get_member(object, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)
    return NULL;
  const gchar *value = json_node_get_string(node);
  return value && *value ? value : NULL;
}

GPtrArray *
gh_inbox_setup_parse_suggestions(GBytes *json, GError **error)
{
  g_return_val_if_fail(json != NULL, NULL);
  gsize size = 0;
  const gchar *data = g_bytes_get_data(json, &size);
  g_autoptr(JsonParser) parser = json_parser_new();
  g_autoptr(GError) local = NULL;
  if (!json_parser_load_from_data(parser, data ? data : "", (gssize)size, &local)) {
    bad_data(error, "%s", local->message);
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *top = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  JsonNode *relays = top ? json_object_get_member(top, "relays") : NULL;
  if (!relays || !JSON_NODE_HOLDS_ARRAY(relays)) {
    bad_data(error, "no \"relays\" array");
    return NULL;
  }
  JsonArray *array = json_node_get_array(relays);
  guint n = json_array_get_length(array);
  if (n == 0 || n > GH_INBOX_SETUP_MAX_SUGGESTIONS) {
    bad_data(error, "%u relays (1..%d expected)", n, GH_INBOX_SETUP_MAX_SUGGESTIONS);
    return NULL;
  }
  g_autoptr(GPtrArray) out =
    g_ptr_array_new_with_free_func((GDestroyNotify)gh_inbox_suggestion_free);
  for (guint i = 0; i < n; i++) {
    JsonNode *node = json_array_get_element(array, i);
    JsonObject *entry = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    const gchar *url = entry ? string_member(entry, "url") : NULL;
    const gchar *name = entry ? string_member(entry, "name") : NULL;
    const gchar *description = entry ? string_member(entry, "description") : NULL;
    const gchar *reads = entry ? string_member(entry, "private_reads") : NULL;
    if (!url || !name || !description || !reads) {
      bad_data(error, "entry %u needs url, name, description and private_reads", i);
      return NULL;
    }
    g_autofree gchar *normalized = gh_inbox_setup_normalize_url(url, NULL);
    if (!normalized || !g_str_equal(normalized, url) || !g_str_has_prefix(url, "wss://")) {
      bad_data(error, "entry %u: \"%s\" is not a normalized secure relay URL", i, url);
      return NULL;
    }
    for (guint j = 0; j < out->len; j++)
      if (g_str_equal(((GhInboxSuggestion *)g_ptr_array_index(out, j))->url, url)) {
        bad_data(error, "entry %u repeats \"%s\"", i, url);
        return NULL;
      }
    GhInboxPrivateReads private_reads;
    if (g_str_equal(reads, "yes"))
      private_reads = GH_INBOX_PRIVATE_READS_YES;
    else if (g_str_equal(reads, "no"))
      private_reads = GH_INBOX_PRIVATE_READS_NO;
    else if (g_str_equal(reads, "unknown"))
      private_reads = GH_INBOX_PRIVATE_READS_UNKNOWN;
    else {
      bad_data(error, "entry %u: private_reads must be yes, no or unknown", i);
      return NULL;
    }
    GhInboxSuggestion *suggestion = g_new0(GhInboxSuggestion, 1);
    suggestion->url = g_steal_pointer(&normalized);
    suggestion->name = g_strdup(name);
    suggestion->description = g_strdup(description);
    suggestion->private_reads = private_reads;
    g_ptr_array_add(out, suggestion);
  }
  return g_steal_pointer(&out);
}

GPtrArray *
gh_inbox_setup_load_suggestions(GError **error)
{
  g_autoptr(GBytes) bytes = g_resources_lookup_data(GH_INBOX_SETUP_SUGGESTIONS_RESOURCE,
                                                    G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
#ifdef GROUNDHOG_RELAY_SUGGESTIONS
  /* Fallback for test binaries that don't have the GResource compiled in:
   * read directly from the source-tree file. In the installed application
   * the GResource lookup above always succeeds, so this path is dead. */
  if (!bytes) {
    g_autofree gchar *text = NULL;
    gsize len = 0;
    if (!g_file_get_contents(GROUNDHOG_RELAY_SUGGESTIONS, &text, &len, error))
      return NULL;
    bytes = g_bytes_new_take(g_steal_pointer(&text), len);
  }
#endif
  if (!bytes) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "relay suggestions not found");
    return NULL;
  }
  return gh_inbox_setup_parse_suggestions(bytes, error);
}

/* ---- the kind-10050 list ---------------------------------------------------- */

static gchar *
build_list(const gchar *pubkey_hex, const gchar *const *relays, gint64 created_at, gint kind)
{
  g_return_val_if_fail(pubkey_hex != NULL && relays != NULL, NULL);
  NostrEvent *event = nostr_event_new();
  NostrTags *tags = nostr_tags_new(0);
  if (!event || !tags) {
    if (tags)
      nostr_tags_free(tags);
    if (event)
      nostr_event_free(event);
    return NULL;
  }
  for (guint i = 0; relays[i]; i++)
    nostr_tags_append(tags, kind == GH_INBOX_SETUP_KIND
                              ? nostr_tag_new("relay", relays[i], NULL)
                              : nostr_tag_new("r", relays[i], "write", NULL));
  nostr_event_set_kind(event, kind);
  nostr_event_set_created_at(event, created_at);
  nostr_event_set_content(event, "");
  nostr_event_set_pubkey(event, pubkey_hex);
  nostr_event_set_tags(event, tags);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  gchar *json = raw ? g_strdup(raw) : NULL;
  free(raw);
  return json;
}

gchar *
gh_inbox_setup_build_unsigned(const gchar *pubkey_hex, const gchar *const *relays,
                              gint64 created_at)
{
  return build_list(pubkey_hex, relays, created_at, GH_INBOX_SETUP_KIND);
}

gchar *
gh_inbox_setup_build_relay_list_unsigned(const gchar *pubkey_hex, const gchar *const *relays,
                                         gint64 created_at)
{
  return build_list(pubkey_hex, relays, created_at, GH_INBOX_SETUP_RELAY_LIST_KIND);
}

gchar *
gh_inbox_setup_build_relay_list_edit_unsigned(const gchar *base_json,
                                                     const gchar *pubkey_hex,
                                                     const gchar *const *write_relays,
                                                     gint64 created_at)
{
  g_return_val_if_fail(pubkey_hex != NULL && write_relays != NULL, NULL);
  if (!base_json)
    return gh_inbox_setup_build_relay_list_unsigned(pubkey_hex, write_relays, created_at);
  NostrEvent *base = nostr_event_new();
  if (!base || nostr_event_deserialize_compact(base, base_json, NULL) != 1) {
    if (base) nostr_event_free(base);
    return NULL;
  }
  NostrEvent *event = nostr_event_new();
  NostrTags *tags = nostr_tags_new(0);
  gsize n_write = 0;
  while (write_relays[n_write]) n_write++;
  g_autofree gboolean *covered = g_new0(gboolean, n_write + 1);
  NostrTags *old_tags = nostr_event_get_tags(base);
  for (size_t i = 0; old_tags && i < nostr_tags_size(old_tags); i++) {
    NostrTag *tag = nostr_tags_get(old_tags, i);
    if (!tag || nostr_tag_size(tag) == 0) continue;
    const gchar *key = nostr_tag_get(tag, 0);
    const gchar *url = g_strcmp0(key, "r") == 0 && nostr_tag_size(tag) >= 2
      ? nostr_tag_get(tag, 1) : NULL;
    const gchar *marker = url && nostr_tag_size(tag) >= 3 ? nostr_tag_get(tag, 2) : NULL;
    gint match = -1;
    g_autofree gchar *normal = url ? gh_inbox_setup_normalize_url(url, NULL) : NULL;
    for (gsize w = 0; normal && w < n_write && match < 0; w++)
      if (g_str_equal(normal, write_relays[w])) match = (gint)w;
    if (url && (!marker || g_str_equal(marker, "read") || g_str_equal(marker, "write"))) {
      if (match >= 0) covered[match] = TRUE;
      if (marker && g_str_equal(marker, "write") && match < 0)
        continue;
      if (marker && g_str_equal(marker, "read") && match >= 0)
        nostr_tags_append(tags, nostr_tag_new("r", url, NULL));
      else if (!marker && match < 0)
        nostr_tags_append(tags, nostr_tag_new("r", url, "read", NULL));
      else {
        NostrTag *copy = nostr_tag_new(key, NULL);
        for (size_t j = 1; j < nostr_tag_size(tag); j++)
          nostr_tag_append(copy, nostr_tag_get(tag, j));
        nostr_tags_append(tags, copy);
      }
      continue;
    }
    NostrTag *copy = nostr_tag_new(key, NULL);
    for (size_t j = 1; j < nostr_tag_size(tag); j++)
      nostr_tag_append(copy, nostr_tag_get(tag, j));
    nostr_tags_append(tags, copy);
  }
  for (gsize w = 0; w < n_write; w++)
    if (!covered[w])
      nostr_tags_append(tags, nostr_tag_new("r", write_relays[w], "write", NULL));
  nostr_event_set_kind(event, GH_INBOX_SETUP_RELAY_LIST_KIND);
  nostr_event_set_created_at(event, created_at);
  const gchar *content = nostr_event_get_content(base);
  nostr_event_set_content(event, content ? content : "");
  nostr_event_set_pubkey(event, pubkey_hex);
  nostr_event_set_tags(event, tags);
  char *raw = nostr_event_serialize_compact(event);
  nostr_event_free(event);
  nostr_event_free(base);
  gchar *json = raw ? g_strdup(raw) : NULL;
  free(raw);
  return json;
}

/* ---- the private-reads check ---------------------------------------------- */

GhInboxProbeResult
gh_inbox_probe_classify_closed(const gchar *reason)
{
  const gchar *text = reason ? reason : "";
  while (g_ascii_isspace(*text))
    text++;
  /* Some relays wrap the machine-readable prefix: "ERROR: auth-required: ...". */
  if (g_ascii_strncasecmp(text, "error:", 6) == 0) {
    const gchar *inner = text + 6;
    while (g_ascii_isspace(*inner))
      inner++;
    if (g_str_has_prefix(inner, "auth-required:") || g_str_has_prefix(inner, "restricted:"))
      text = inner;
  }
  if (g_str_has_prefix(text, "auth-required:") || g_str_has_prefix(text, "restricted:"))
    return GH_INBOX_PROBE_PRIVATE;
  return GH_INBOX_PROBE_REFUSED;
}

struct _GhInboxProbe {
  gint refs;
  guint64 generation;
  GhRelayTransport transport;
  GhRelayAuthTransport auth;
  gboolean custom_transport;
  gboolean has_auth;
  gpointer transport_data;
  GhInboxProbeFunc callback;
  gpointer user_data;
  GPtrArray *urls;       /* in the order added */
  GHashTable *results;   /* url -> GhInboxProbeResult */
  GhRelayScope *scope;
  guint deadline_seconds;
  guint deadline_source;
  guint close_source;
  gboolean started;
  gboolean cancelled;    /* no callback runs any more */
};

GhInboxProbe *
gh_inbox_probe_new(guint64 account_generation, const GhRelayTransport *transport,
                   const GhRelayAuthTransport *auth_transport, gpointer transport_data,
                   GhInboxProbeFunc callback, gpointer user_data)
{
  g_return_val_if_fail(!transport || (transport->open && transport->close), NULL);
  g_return_val_if_fail(!auth_transport || transport, NULL);
  GhInboxProbe *probe = g_new0(GhInboxProbe, 1);
  probe->refs = 1;
  probe->generation = account_generation;
  if (transport) {
    probe->transport = *transport;
    probe->custom_transport = TRUE;
  }
  if (auth_transport) {
    probe->auth = *auth_transport;
    probe->has_auth = TRUE;
  }
  probe->transport_data = transport_data;
  probe->callback = callback;
  probe->user_data = user_data;
  probe->urls = g_ptr_array_new_with_free_func(g_free);
  probe->results = g_hash_table_new(g_str_hash, g_str_equal);
  probe->deadline_seconds = 15;
  return probe;
}

GhInboxProbe *
gh_inbox_probe_ref(GhInboxProbe *probe)
{
  g_return_val_if_fail(probe != NULL, NULL);
  g_atomic_int_inc(&probe->refs);
  return probe;
}

/* Revokes the scope's generation and closes its connections. */
static void
close_scope(GhInboxProbe *probe)
{
  g_clear_handle_id(&probe->deadline_source, g_source_remove);
  g_clear_handle_id(&probe->close_source, g_source_remove);
  if (probe->scope) {
    gh_relay_scope_cancel(probe->scope);
    g_clear_pointer(&probe->scope, gh_relay_scope_unref);
  }
}

void
gh_inbox_probe_unref(GhInboxProbe *probe)
{
  if (!probe || !g_atomic_int_dec_and_test(&probe->refs))
    return;
  probe->cancelled = TRUE;
  close_scope(probe);
  g_hash_table_unref(probe->results);
  g_ptr_array_unref(probe->urls);
  g_free(probe);
}

gboolean
gh_inbox_probe_add_url(GhInboxProbe *probe, const gchar *url, GError **error)
{
  g_return_val_if_fail(probe != NULL, FALSE);
  g_return_val_if_fail(!probe->started, FALSE);
  if (!gh_relay_url_validate(url, error))
    return FALSE;
  if (g_hash_table_contains(probe->results, url))
    return TRUE;
  if (probe->urls->len >= MAX_TARGETS) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "at most 16 relays per check");
    return FALSE;
  }
  gchar *copy = g_strdup(url);
  g_ptr_array_add(probe->urls, copy);
  g_hash_table_insert(probe->results, copy, GUINT_TO_POINTER(GH_INBOX_PROBE_PENDING));
  return TRUE;
}

void
gh_inbox_probe_set_deadline(GhInboxProbe *probe, guint seconds)
{
  g_return_if_fail(probe != NULL);
  probe->deadline_seconds = CLAMP(seconds, 1, 120);
}

GhInboxProbeResult
gh_inbox_probe_get_result(GhInboxProbe *probe, const gchar *url)
{
  g_return_val_if_fail(probe != NULL, GH_INBOX_PROBE_PENDING);
  return GPOINTER_TO_UINT(g_hash_table_lookup(probe->results, url));
}

gboolean
gh_inbox_probe_is_complete(GhInboxProbe *probe)
{
  g_return_val_if_fail(probe != NULL, FALSE);
  if (!probe->started)
    return FALSE;
  for (guint i = 0; i < probe->urls->len; i++)
    if (gh_inbox_probe_get_result(probe, g_ptr_array_index(probe->urls, i)) ==
        GH_INBOX_PROBE_PENDING)
      return FALSE;
  return TRUE;
}

static gboolean
close_idle(gpointer data)
{
  GhInboxProbe *probe = data;
  probe->close_source = 0;
  close_scope(probe);
  return G_SOURCE_REMOVE;
}

static void
settle(GhInboxProbe *probe, const gchar *url, GhInboxProbeResult result, const gchar *detail)
{
  gpointer key = NULL, value = NULL;
  if (probe->cancelled || !g_hash_table_lookup_extended(probe->results, url, &key, &value) ||
      GPOINTER_TO_UINT(value) != GH_INBOX_PROBE_PENDING)
    return;
  g_hash_table_insert(probe->results, key, GUINT_TO_POINTER(result));
  gboolean complete = gh_inbox_probe_is_complete(probe);
  /* The scope may be inside a transport callback: close from an idle. */
  if (complete && probe->scope && !probe->close_source)
    probe->close_source = g_idle_add_full(G_PRIORITY_DEFAULT, close_idle,
                                          gh_inbox_probe_ref(probe),
                                          (GDestroyNotify)gh_inbox_probe_unref);
  if (complete)
    g_clear_handle_id(&probe->deadline_source, g_source_remove);
  gh_inbox_probe_ref(probe);
  if (probe->callback)
    probe->callback(probe, key, result, detail, probe->user_data);
  gh_inbox_probe_unref(probe);
}

static void
on_scope_update(GhRelayScope *scope, const GhRelayUpdate *update, gpointer data)
{
  GhInboxProbe *probe = data;
  if (probe->cancelled || scope != probe->scope)
    return;
  switch (update->notice) {
  case GH_RELAY_NOTICE_EOSE:
    settle(probe, update->url, GH_INBOX_PROBE_OPEN, NULL);
    break;
  case GH_RELAY_NOTICE_CLOSED:
    settle(probe, update->url, gh_inbox_probe_classify_closed(update->detail), update->detail);
    break;
  case GH_RELAY_NOTICE_ERROR:
  case GH_RELAY_NOTICE_DISCONNECTED:
    settle(probe, update->url, GH_INBOX_PROBE_UNREACHABLE,
           update->detail ? update->detail : _("The connection was lost"));
    break;
  case GH_RELAY_NOTICE_EVENT: /* nothing is addressed to a random key */
  case GH_RELAY_NOTICE_AUTH:  /* recorded by the scope; never answered here */
  case GH_RELAY_NOTICE_OK:
  default:
    break;
  }
}

static gboolean
on_deadline(gpointer data)
{
  GhInboxProbe *probe = data;
  probe->deadline_source = 0;
  gh_inbox_probe_ref(probe);
  for (guint i = 0; i < probe->urls->len && !probe->cancelled; i++)
    settle(probe, g_ptr_array_index(probe->urls, i), GH_INBOX_PROBE_UNREACHABLE,
           _("The relay didn't answer in time"));
  gh_inbox_probe_unref(probe);
  return G_SOURCE_REMOVE;
}

/* A random 32-byte key: the relay learns nothing about who is asking. */
static gchar *
random_pubkey(void)
{
  GString *hex = g_string_sized_new(64);
  for (guint i = 0; i < 8; i++)
    g_string_append_printf(hex, "%08x", g_random_int());
  return g_string_free(hex, FALSE);
}

static NostrFilters *
probe_filters(void)
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
  const int kinds[] = { KIND_GIFT_WRAP };
  g_autofree gchar *nobody = random_pubkey();
  nostr_filter_set_kinds(filter, kinds, G_N_ELEMENTS(kinds));
  nostr_filter_tags_append(filter, "p", nobody, NULL);
  nostr_filter_set_limit(filter, 1);
  gboolean added = nostr_filters_add(filters, filter);
  nostr_filter_free(filter); /* contents moved into the vector */
  if (!added) {
    nostr_filters_free(filters);
    return NULL;
  }
  return filters;
}

gboolean
gh_inbox_probe_start(GhInboxProbe *probe, GError **error)
{
  g_return_val_if_fail(probe != NULL, FALSE);
  g_return_val_if_fail(!probe->started, FALSE);
  if (probe->urls->len == 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "no relay to check");
    return FALSE;
  }
  NostrFilters *filters = probe_filters();
  if (!filters) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "could not build the check");
    return FALSE;
  }
  probe->scope = probe->custom_transport
    ? gh_relay_scope_new_with_transport(probe->generation, filters, &probe->transport,
                                        probe->transport_data, on_scope_update, probe)
    : gh_relay_scope_new(probe->generation, filters, on_scope_update, probe);
  if (probe->has_auth)
    gh_relay_scope_set_auth_transport(probe->scope, &probe->auth);
  /* Every URL keeps the default AUTH identity, NONE: nothing is signed. */
  for (guint i = 0; i < probe->urls->len; i++)
    if (!gh_relay_scope_add_url(probe->scope, g_ptr_array_index(probe->urls, i), error)) {
      close_scope(probe);
      return FALSE;
    }
  probe->started = TRUE;
  probe->deadline_source = g_timeout_add_seconds(probe->deadline_seconds, on_deadline, probe);
  gh_inbox_probe_ref(probe);
  gh_relay_scope_start(probe->scope);
  gh_inbox_probe_unref(probe);
  return TRUE;
}

void
gh_inbox_probe_cancel(GhInboxProbe *probe)
{
  g_return_if_fail(probe != NULL);
  probe->cancelled = TRUE;
  close_scope(probe);
}

/* ---- publishing the list ---------------------------------------------------- */

typedef struct {
  GhInboxSetupRelay pub; /* first: handed out as GhInboxSetupRelay */
  gchar *url;
  gchar *message;
  gchar *probe_detail;
} Target;

static void
target_free(gpointer data)
{
  Target *target = data;
  g_free(target->url);
  g_free(target->message);
  g_free(target->probe_detail);
  g_free(target);
}

static Target *
target_find(GPtrArray *targets, const gchar *url)
{
  for (guint i = 0; targets && i < targets->len; i++) {
    Target *target = g_ptr_array_index(targets, i);
    if (g_str_equal(target->url, url))
      return target;
  }
  return NULL;
}

static void
target_add(GPtrArray *targets, const gchar *url, GhInboxSetupRole role)
{
  Target *target = target_find(targets, url);
  if (!target) {
    target = g_new0(Target, 1);
    target->url = g_strdup(url);
    target->pub.url = target->url;
    target->pub.outcome = GH_RELAY_PUBLISH_PENDING;
    target->pub.probe = GH_INBOX_PROBE_PENDING;
    g_ptr_array_add(targets, target);
  }
  target->pub.roles |= role;
}

struct _GhInboxSetup {
  GObject parent_instance;
  GhInboxSetupConfig config; /* accounts, account_relays, settings are owned refs */
  GhInboxSetupState state;
  GError *error;
  guint64 generation;
  GCancellable *cancellable;
  GPtrArray *targets;        /* Target */
  gboolean adopt_discovery;
  gboolean adopted;
  gchar *event_id;
  GhRelayPublish *publish;
  gboolean publish_done;
  GhInboxProbe *probe;
  /* The optional kind-10002 relay list (nostrc-0bdg). */
  gboolean relay_list_requested;
  GhRelayListSetup *relay_list;
};

enum { SIGNAL_CHANGED, N_SIGNALS };
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE(GhInboxSetup, gh_inbox_setup, G_TYPE_OBJECT)

static void
emit_changed(GhInboxSetup *self)
{
  g_signal_emit(self, signals[SIGNAL_CHANGED], 0);
}

static gboolean
discovery_is_empty(GhInboxSetup *self)
{
  if (!self->config.settings)
    return FALSE;
  g_auto(GStrv) urls = g_settings_get_strv(self->config.settings, "discovery-relays");
  return urls[0] == NULL;
}

/* Normalized, de-duplicated URLs of a list read from a setting or a signed
 * relay list; entries Groundhog would refuse to type in are skipped. */
static void
add_listed(GPtrArray *targets, const gchar *const *urls, GhInboxSetupRole role)
{
  for (guint i = 0; urls && urls[i]; i++) {
    g_autofree gchar *url = gh_inbox_setup_normalize_url(urls[i], NULL);
    if (url)
      target_add(targets, url, role);
  }
}

static GPtrArray *
compute_targets(GhInboxSetup *self, const gchar *const *inbox_relays, gboolean adopt,
                GError **error)
{
  GhAccountController *accounts = self->config.accounts;
  if (gh_account_controller_get_state(accounts) != GH_ACCOUNT_STATE_ACTIVE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED,
                        _("Choose an account first."));
    return NULL;
  }
  g_autoptr(GPtrArray) targets = g_ptr_array_new_with_free_func(target_free);
  GhInboxSetupRole inbox_role = GH_INBOX_SETUP_ROLE_INBOX;
  if (adopt && discovery_is_empty(self))
    inbox_role |= GH_INBOX_SETUP_ROLE_DISCOVERY;
  for (guint i = 0; inbox_relays && inbox_relays[i]; i++) {
    g_autofree gchar *url = gh_inbox_setup_normalize_url(inbox_relays[i], error);
    if (!url)
      return NULL;
    target_add(targets, url, inbox_role);
  }
  if (targets->len == 0) {
    invalid(error, _("Choose at least one relay."));
    return NULL;
  }
  if (targets->len > GH_INBOX_SETUP_MAX_INBOX_RELAYS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                _("Choose at most %d relays."), GH_INBOX_SETUP_MAX_INBOX_RELAYS);
    return NULL;
  }
  /* Charter §4.3: the list also goes where the account publishes (its own
   * 10002 write relays) and where it is looked up (discovery-relays). The
   * lists must be the active account's own, never an older generation's. */
  GhAccountRelays *relays = self->config.account_relays;
  if (relays && gh_account_relays_get_generation(relays) ==
                  gh_account_controller_get_generation(accounts))
    add_listed(targets, gh_account_relays_get_write_relays(relays), GH_INBOX_SETUP_ROLE_WRITE);
  if (self->config.settings) {
    g_auto(GStrv) discovery = g_settings_get_strv(self->config.settings, "discovery-relays");
    add_listed(targets, (const gchar *const *)discovery, GH_INBOX_SETUP_ROLE_DISCOVERY);
  }
  /* nostrc-mi1z: when adopting discovery with no configured relays yet, also
   * publish to the relay suggestions so other apps can find the lists on
   * well-known relays, not only on the chosen message relays. */
  if (adopt && discovery_is_empty(self)) {
    g_autoptr(GPtrArray) suggestions = gh_inbox_setup_load_suggestions(NULL);
    for (guint i = 0; suggestions && i < suggestions->len; i++) {
      GhInboxSuggestion *s = g_ptr_array_index(suggestions, i);
      target_add(targets, s->url, GH_INBOX_SETUP_ROLE_DISCOVERY);
    }
  }
  if (targets->len > MAX_TARGETS) {
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                _("That's too many relays to publish to at once (%u; the limit is %d)."),
                targets->len, MAX_TARGETS);
    return NULL;
  }
  return g_steal_pointer(&targets);
}

GPtrArray *
gh_inbox_setup_plan(GhInboxSetup *self, const gchar *const *inbox_relays, gboolean adopt_discovery,
                    GError **error)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), NULL);
  return compute_targets(self, inbox_relays, adopt_discovery, error);
}

static void
stop_network(GhInboxSetup *self)
{
  if (self->cancellable)
    g_cancellable_cancel(self->cancellable);
  if (self->publish)
    gh_relay_publish_cancel(self->publish);
  if (self->relay_list)
    gh_relay_list_setup_cancel(self->relay_list);
  if (self->probe)
    gh_inbox_probe_cancel(self->probe);
}

static void
fail(GhInboxSetup *self, GError *error)
{
  if (self->state != GH_INBOX_SETUP_SIGNING && self->state != GH_INBOX_SETUP_PUBLISHING) {
    g_error_free(error);
    return;
  }
  stop_network(self);
  g_clear_error(&self->error);
  self->error = error;
  self->state = GH_INBOX_SETUP_FAILED;
  emit_changed(self);
}

/* The message relays that kept the list become the discovery relays, so
 * Groundhog finds it again (only when none were set and the user agreed). */
static void
adopt_discovery(GhInboxSetup *self)
{
  if (!self->adopt_discovery || !discovery_is_empty(self))
    return;
  g_autoptr(GPtrArray) urls = g_ptr_array_new();
  for (guint i = 0; i < self->targets->len; i++) {
    Target *target = g_ptr_array_index(self->targets, i);
    if ((target->pub.roles & GH_INBOX_SETUP_ROLE_INBOX) &&
        target->pub.outcome == GH_RELAY_PUBLISH_ACCEPTED)
      g_ptr_array_add(urls, target->url);
  }
  if (urls->len == 0)
    return;
  g_ptr_array_add(urls, NULL);
  self->adopted = g_settings_set_strv(self->config.settings, "discovery-relays",
                                      (const gchar *const *)urls->pdata);
}

static void
maybe_finish(GhInboxSetup *self)
{
  if (self->state != GH_INBOX_SETUP_PUBLISHING || !self->publish_done ||
      !gh_inbox_probe_is_complete(self->probe))
    return;
  switch (gh_inbox_setup_get_relay_list_state(self)) {
  case GH_INBOX_SETUP_RELAY_LIST_WAITING:
  case GH_INBOX_SETUP_RELAY_LIST_CHECKING:
  case GH_INBOX_SETUP_RELAY_LIST_SIGNING:
  case GH_INBOX_SETUP_RELAY_LIST_PUBLISHING:
    return;
  default:
    break;
  }
  if (gh_inbox_setup_get_n_accepted(self) == 0) {
    fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED,
                                   _("No relay accepted your list.")));
    return;
  }
  self->state = GH_INBOX_SETUP_DONE;
  adopt_discovery(self);
  emit_changed(self);
}

static void
on_publish_update(GhRelayPublish *publish, const GhRelayPublishResult *result, gpointer data)
{
  GhInboxSetup *self = data;
  Target *target = target_find(self->targets, result->url);
  if (publish != self->publish || !target)
    return;
  target->pub.outcome = result->outcome;
  target->pub.prefix = result->prefix;
  g_free(target->message);
  target->message = g_strdup(result->message);
  target->pub.message = target->message;
  emit_changed(self);
}

static void
on_publish_done(GhRelayPublish *publish, const GhRelayPublishSummary *summary, gpointer data)
{
  GhInboxSetup *self = data;
  (void)summary;
  if (publish != self->publish)
    return;
  self->publish_done = TRUE;
  maybe_finish(self);
}

static void
on_probe_result(GhInboxProbe *probe, const gchar *url, GhInboxProbeResult result,
                const gchar *detail, gpointer data)
{
  GhInboxSetup *self = data;
  Target *target = target_find(self->targets, url);
  if (probe != self->probe || !target)
    return;
  target->pub.probe = result;
  g_free(target->probe_detail);
  target->probe_detail = g_strdup(detail);
  target->pub.probe_detail = target->probe_detail;
  emit_changed(self);
  maybe_finish(self);
}

static gboolean
start_publish(GhInboxSetup *self, const gchar *signed_json, GError **error)
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
  self->event_id = g_strdup(gh_relay_publish_get_event_id(self->publish));
  if (config->publish_transport && config->publish_auth_transport)
    gh_relay_publish_set_auth_transport(self->publish, config->publish_auth_transport);
  /* Own list publish (charter Â§4.3): the one purpose here that may sign in
   * as the account, and only on these publication connections. GhAuthPolicy
   * signs through its one GhAccountAuth of this generation, so the inbox,
   * the self-copy and this setup ask the signer at most once per relay at a
   * time, and a relay the user declined is not asked again (§4.4 R6). */
  GhAuthPolicy *policy = gh_auth_policy_get_for_accounts(config->accounts);
  for (guint i = 0; i < self->targets->len; i++) {
    Target *target = g_ptr_array_index(self->targets, i);
    if (!gh_relay_publish_add_url(self->publish, target->url, error))
      return FALSE;
    g_autoptr(GError) auth_error = NULL;
    if (!gh_auth_policy_apply_publish(policy, self->publish, GH_AUTH_PURPOSE_OWN_LIST_PUBLISH,
                                      target->url, &auth_error)) {
      /* Only a stale generation is refused (the signed list is current). */
      g_debug("Groundhog will not sign in to publish its message relays: %s",
              auth_error->message);
      g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, _("The account changed."));
      return FALSE;
    }
  }
  /* The message relays are checked on separate, never-authenticated
   * connections (§4.3 isolation). */
  self->probe = gh_inbox_probe_new(self->generation, config->probe_transport,
                                   config->probe_auth_transport, config->probe_transport_data,
                                   on_probe_result, self);
  for (guint i = 0; i < self->targets->len; i++) {
    Target *target = g_ptr_array_index(self->targets, i);
    if (!(target->pub.roles & GH_INBOX_SETUP_ROLE_INBOX))
      continue;
    target->pub.probed = TRUE;
    if (!gh_inbox_probe_add_url(self->probe, target->url, error))
      return FALSE;
  }
  self->state = GH_INBOX_SETUP_PUBLISHING;
  emit_changed(self);
  /* Either may report (and even finish) before returning. */
  if (!gh_inbox_probe_start(self->probe, error))
    return FALSE;
  if (self->state == GH_INBOX_SETUP_PUBLISHING && !gh_relay_publish_start(self->publish, error))
    return FALSE;
  return TRUE;
}

/* ---- the relay list (nostrc-0bdg; gh-relay-list-setup.h) -------------------- */

static void
on_relay_list_changed(GhInboxSetup *self)
{
  emit_changed(self);
  maybe_finish(self);
}

/* After the message list is out: the pre-publish check of every target,
 * then one more signer request (gh-relay-list-setup.h). */
static void
start_relay_list(GhInboxSetup *self)
{
  if (!self->relay_list_requested)
    return;
  g_autoptr(GPtrArray) write = g_ptr_array_new();
  g_autoptr(GPtrArray) all = g_ptr_array_new();
  for (guint i = 0; i < self->targets->len; i++) {
    Target *target = g_ptr_array_index(self->targets, i);
    if (target->pub.roles & GH_INBOX_SETUP_ROLE_INBOX)
      g_ptr_array_add(write, target->url);
    g_ptr_array_add(all, target->url);
  }
  g_ptr_array_add(write, NULL);
  g_ptr_array_add(all, NULL);
  self->relay_list = gh_relay_list_setup_new(&self->config);
  g_signal_connect_object(self->relay_list, "changed", G_CALLBACK(on_relay_list_changed), self,
                          G_CONNECT_SWAPPED);
  g_autoptr(GError) error = NULL;
  if (!gh_relay_list_setup_start(self->relay_list, (const gchar *const *)write->pdata,
                                 (const gchar *const *)all->pdata, &error))
    g_debug("Groundhog's relay list was not set up: %s", error->message);
  emit_changed(self);
}

static void
on_signed(GObject *source, GAsyncResult *result, gpointer data)
{
  GhInboxSetup *self = data; /* a reference held for this call */
  (void)source;
  g_autoptr(GError) error = NULL;
  g_autofree gchar *signed_json = gh_account_controller_sign_finish(result, &error);
  if (self->state != GH_INBOX_SETUP_SIGNING) {
    g_object_unref(self);
    return;
  }
  if (!signed_json)
    fail(self, g_steal_pointer(&error));
  else if (!gh_account_controller_is_current(self->config.accounts, self->generation))
    fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, _("The account changed.")));
  else if (!start_publish(self, signed_json, &error))
    fail(self, g_steal_pointer(&error));
  else
    start_relay_list(self);
  g_object_unref(self);
}

static void
on_accounts_changed(GhInboxSetup *self)
{
  if ((self->state == GH_INBOX_SETUP_SIGNING || self->state == GH_INBOX_SETUP_PUBLISHING) &&
      !gh_account_controller_is_current(self->config.accounts, self->generation))
    fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                   _("The account changed before your list was published.")));
}

gboolean
gh_inbox_setup_relay_list_needed(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), FALSE);
  return gh_relay_list_offer(&self->config) != GH_RELAY_LIST_OFFER_NONE;
}

GhRelayListOffer
gh_inbox_setup_get_relay_list_offer(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), GH_RELAY_LIST_OFFER_NONE);
  return gh_relay_list_offer(&self->config);
}

gboolean
gh_inbox_setup_start(GhInboxSetup *self, const gchar *const *inbox_relays, gboolean adopt,
                     GError **error)
{
  return gh_inbox_setup_start_full(self, inbox_relays, adopt, FALSE, error);
}

gboolean
gh_inbox_setup_start_full(GhInboxSetup *self, const gchar *const *inbox_relays, gboolean adopt,
                          gboolean relay_list, GError **error)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), FALSE);
  if (self->state != GH_INBOX_SETUP_IDLE) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "this setup already ran");
    return FALSE;
  }
  g_autoptr(GPtrArray) targets = compute_targets(self, inbox_relays, adopt, error);
  if (!targets)
    return FALSE;
  GhAccountController *accounts = self->config.accounts;
  g_autofree gchar *pubkey = gh_identity_pubkey_hex(gh_account_controller_get_active_npub(accounts));
  g_autoptr(GPtrArray) inbox = g_ptr_array_new();
  for (guint i = 0; i < targets->len; i++) {
    Target *target = g_ptr_array_index(targets, i);
    if (target->pub.roles & GH_INBOX_SETUP_ROLE_INBOX)
      g_ptr_array_add(inbox, target->url);
  }
  g_ptr_array_add(inbox, NULL);
  g_autofree gchar *unsigned_json = pubkey ? gh_inbox_setup_build_unsigned(pubkey,
    (const gchar *const *)inbox->pdata, g_get_real_time() / G_USEC_PER_SEC) : NULL;
  if (!unsigned_json) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "could not build the relay list");
    return FALSE;
  }
  self->relay_list_requested = relay_list && gh_inbox_setup_relay_list_needed(self);
  self->targets = g_steal_pointer(&targets);
  self->adopt_discovery = adopt;
  self->generation = gh_account_controller_get_generation(accounts);
  self->cancellable = g_cancellable_new();
  self->state = GH_INBOX_SETUP_SIGNING;
  emit_changed(self);
  /* Nothing is contacted until the signer answers (PT-9, signer denial). */
  gh_account_controller_sign_with_cancellable_async(accounts, unsigned_json, self->cancellable,
                                                    on_signed, g_object_ref(self));
  return TRUE;
}

void
gh_inbox_setup_cancel(GhInboxSetup *self)
{
  g_return_if_fail(GH_IS_INBOX_SETUP(self));
  fail(self, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, _("Publishing was stopped.")));
}

GhInboxSetupState
gh_inbox_setup_get_state(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), GH_INBOX_SETUP_IDLE);
  return self->state;
}

const GError *
gh_inbox_setup_get_error(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), NULL);
  return self->error;
}

guint
gh_inbox_setup_get_n_relays(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), 0);
  return self->targets ? self->targets->len : 0;
}

const GhInboxSetupRelay *
gh_inbox_setup_get_relay(GhInboxSetup *self, guint index)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), NULL);
  g_return_val_if_fail(index < gh_inbox_setup_get_n_relays(self), NULL);
  return &((Target *)g_ptr_array_index(self->targets, index))->pub;
}

const GhInboxSetupRelay *
gh_inbox_setup_lookup(GhInboxSetup *self, const gchar *url)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), NULL);
  Target *target = target_find(self->targets, url);
  return target ? &target->pub : NULL;
}

guint
gh_inbox_setup_get_n_accepted(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), 0);
  guint accepted = 0;
  for (guint i = 0; self->targets && i < self->targets->len; i++)
    accepted += ((Target *)g_ptr_array_index(self->targets, i))->pub.outcome ==
                GH_RELAY_PUBLISH_ACCEPTED;
  return accepted;
}

const gchar *
gh_inbox_setup_get_event_id(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), NULL);
  return self->event_id;
}

GhInboxSetupRelayList
gh_inbox_setup_get_relay_list_state(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), GH_INBOX_SETUP_RELAY_LIST_NONE);
  if (!self->relay_list_requested)
    return GH_INBOX_SETUP_RELAY_LIST_NONE;
  if (!self->relay_list)
    return self->state == GH_INBOX_SETUP_FAILED ? GH_INBOX_SETUP_RELAY_LIST_FAILED
                                                : GH_INBOX_SETUP_RELAY_LIST_WAITING;
  switch (gh_relay_list_setup_get_state(self->relay_list)) {
  case GH_RELAY_LIST_SETUP_CHECKING: return GH_INBOX_SETUP_RELAY_LIST_CHECKING;
  case GH_RELAY_LIST_SETUP_SIGNING: return GH_INBOX_SETUP_RELAY_LIST_SIGNING;
  case GH_RELAY_LIST_SETUP_PUBLISHING: return GH_INBOX_SETUP_RELAY_LIST_PUBLISHING;
  case GH_RELAY_LIST_SETUP_DONE: return GH_INBOX_SETUP_RELAY_LIST_DONE;
  case GH_RELAY_LIST_SETUP_SKIPPED: return GH_INBOX_SETUP_RELAY_LIST_SKIPPED;
  case GH_RELAY_LIST_SETUP_IDLE:   /* start refused it: nothing to offer any more */
    return GH_INBOX_SETUP_RELAY_LIST_SKIPPED;
  case GH_RELAY_LIST_SETUP_FAILED:
  default:
    return GH_INBOX_SETUP_RELAY_LIST_FAILED;
  }
}

guint
gh_inbox_setup_get_relay_list_n_accepted(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), 0);
  return self->relay_list ? gh_relay_list_setup_get_n_accepted(self->relay_list) : 0;
}

GhRelayListSetup *
gh_inbox_setup_get_relay_list(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), NULL);
  return self->relay_list;
}

gboolean
gh_inbox_setup_get_adopted_discovery(GhInboxSetup *self)
{
  g_return_val_if_fail(GH_IS_INBOX_SETUP(self), FALSE);
  return self->adopted;
}

GhInboxSetup *
gh_inbox_setup_new(const GhInboxSetupConfig *config)
{
  g_return_val_if_fail(config != NULL, NULL);
  g_return_val_if_fail(GH_IS_ACCOUNT_CONTROLLER(config->accounts), NULL);
  g_return_val_if_fail(!config->account_relays || GH_IS_ACCOUNT_RELAYS(config->account_relays),
                       NULL);
  g_return_val_if_fail(!config->settings || G_IS_SETTINGS(config->settings), NULL);
  GhInboxSetup *self = g_object_new(GH_TYPE_INBOX_SETUP, NULL);
  self->config = *config;
  g_object_ref(self->config.accounts);
  if (self->config.account_relays)
    g_object_ref(self->config.account_relays);
  if (self->config.settings)
    g_object_ref(self->config.settings);
  g_signal_connect_object(self->config.accounts, "changed", G_CALLBACK(on_accounts_changed), self,
                          G_CONNECT_SWAPPED);
  return self;
}

static void
gh_inbox_setup_dispose(GObject *object)
{
  GhInboxSetup *self = GH_INBOX_SETUP(object);
  if (self->config.accounts)
    g_signal_handlers_disconnect_by_data(self->config.accounts, self);
  /* Unfinished work ends quietly: a late signer answer finds it FAILED. */
  if (self->state == GH_INBOX_SETUP_SIGNING || self->state == GH_INBOX_SETUP_PUBLISHING)
    self->state = GH_INBOX_SETUP_FAILED;
  stop_network(self);
  g_clear_pointer(&self->publish, gh_relay_publish_unref);
  if (self->relay_list)
    g_signal_handlers_disconnect_by_data(self->relay_list, self);
  g_clear_object(&self->relay_list);
  g_clear_pointer(&self->probe, gh_inbox_probe_unref);
  g_clear_object(&self->config.settings);
  g_clear_object(&self->config.account_relays);
  g_clear_object(&self->config.accounts);
  g_clear_object(&self->cancellable);
  G_OBJECT_CLASS(gh_inbox_setup_parent_class)->dispose(object);
}

static void
gh_inbox_setup_finalize(GObject *object)
{
  GhInboxSetup *self = GH_INBOX_SETUP(object);
  g_clear_pointer(&self->targets, g_ptr_array_unref);
  g_clear_error(&self->error);
  g_free(self->event_id);
  G_OBJECT_CLASS(gh_inbox_setup_parent_class)->finalize(object);
}

static void
gh_inbox_setup_class_init(GhInboxSetupClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = gh_inbox_setup_dispose;
  object_class->finalize = gh_inbox_setup_finalize;
  signals[SIGNAL_CHANGED] = g_signal_new("changed", G_TYPE_FROM_CLASS(klass), G_SIGNAL_RUN_LAST,
                                         0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void
gh_inbox_setup_init(GhInboxSetup *self)
{
  self->state = GH_INBOX_SETUP_IDLE;
}
