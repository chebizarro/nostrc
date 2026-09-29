#include "gh-nip05.h"

#include "gh-recipient.h"

#include <json-glib/json-glib.h>
#include <string.h>

G_DEFINE_QUARK(gh-nip05-error-quark, gh_nip05_error)

struct _GhNip05 {
  GObject parent_instance;
  GSettings *settings;
  GhHttpTransport transport;
  gpointer transport_data;
  GhNetHttp *http; /* the default transport's data, when no transport was given */
};

G_DEFINE_FINAL_TYPE(GhNip05, gh_nip05, G_TYPE_OBJECT)

static gboolean
parse_address(const gchar *address, gchar **out_local, gchar **out_domain, GError **error)
{
  if (gh_recipient_parse_nip05(address, out_local, out_domain))
    return TRUE;
  g_set_error_literal(error, GH_NIP05_ERROR, GH_NIP05_ERROR_ADDRESS,
                      "Not an address of the form name@example.com");
  return FALSE;
}

gchar *
gh_nip05_dup_url(const gchar *address, GError **error)
{
  g_autofree gchar *local = NULL, *domain = NULL;
  if (!parse_address(address, &local, &domain, error))
    return NULL;
  /* The local part is a-z0-9-_. only, so it needs no escaping. */
  g_autofree gchar *query = g_strconcat("name=", local, NULL);
  g_autoptr(GUri) uri = g_uri_build(G_URI_FLAGS_NONE, "https", NULL, domain, -1,
                                    "/.well-known/nostr.json", query, NULL);
  return g_uri_to_string(uri);
}

gchar *
gh_nip05_parse_document(GBytes *document, const gchar *local, GError **error)
{
  g_return_val_if_fail(document != NULL && local != NULL, NULL);
  gsize size = 0;
  const gchar *data = g_bytes_get_data(document, &size);
  g_autoptr(JsonParser) parser = json_parser_new();
  if (size > GH_NIP05_MAX_DOCUMENT || !data || !g_utf8_validate(data, size, NULL) ||
      !json_parser_load_from_data(parser, data, size, NULL)) {
    g_set_error_literal(error, GH_NIP05_ERROR, GH_NIP05_ERROR_RESPONSE,
                        "The server's answer is not a NIP-05 document");
    return NULL;
  }
  JsonNode *root = json_parser_get_root(parser);
  JsonObject *object = root && JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
  JsonNode *names_node = object ? json_object_get_member(object, "names") : NULL;
  if (!names_node || !JSON_NODE_HOLDS_OBJECT(names_node)) {
    g_set_error_literal(error, GH_NIP05_ERROR, GH_NIP05_ERROR_RESPONSE,
                        "The server's answer is not a NIP-05 document");
    return NULL;
  }
  JsonObject *names = json_node_get_object(names_node);
  /* Names are matched case-insensitively (NIP-05); the first match wins. */
  const gchar *pubkey = NULL;
  g_autoptr(GList) members = json_object_get_members(names);
  for (GList *l = members; l && !pubkey; l = l->next) {
    if (g_ascii_strcasecmp(l->data, local) != 0)
      continue;
    JsonNode *value = json_object_get_member(names, l->data);
    if (value && JSON_NODE_HOLDS_VALUE(value) && json_node_get_value_type(value) == G_TYPE_STRING)
      pubkey = json_node_get_string(value);
    else
      break;
  }
  if (!pubkey) {
    g_set_error_literal(error, GH_NIP05_ERROR, GH_NIP05_ERROR_NOT_FOUND,
                        "The server does not list this name");
    return NULL;
  }
  if (!gh_recipient_is_pubkey(pubkey)) {
    g_set_error_literal(error, GH_NIP05_ERROR, GH_NIP05_ERROR_RESPONSE,
                        "The server listed the name with an invalid public key");
    return NULL;
  }
  return g_ascii_strdown(pubkey, -1);
}

static gchar *
network_mode(GhNip05 *self)
{
  if (!self->settings)
    return g_strdup("system");
  g_autoptr(GSettingsSchema) schema = NULL;
  g_object_get(self->settings, "settings-schema", &schema, NULL);
  if (!schema || !g_settings_schema_has_key(schema, "network-mode"))
    return g_strdup("system");
  return g_settings_get_string(self->settings, "network-mode");
}

static void
on_fetched(GObject *source, GAsyncResult *result, gpointer data)
{
  (void)source;
  g_autoptr(GTask) task = data;
  GhNip05 *self = g_task_get_source_object(task);
  GError *error = NULL;
  g_autoptr(GBytes) body = self->transport.get_finish(self->transport_data, result, &error);
  if (!body) {
    g_task_return_error(task, error);
    return;
  }
  gchar *pubkey = gh_nip05_parse_document(body, g_task_get_task_data(task), &error);
  if (pubkey)
    g_task_return_pointer(task, pubkey, g_free);
  else
    g_task_return_error(task, error);
}

void
gh_nip05_lookup_async(GhNip05 *self, const gchar *address, GCancellable *cancellable,
                      GAsyncReadyCallback callback, gpointer user_data)
{
  g_return_if_fail(GH_IS_NIP05(self));
  g_autoptr(GTask) task = g_task_new(self, cancellable, callback, user_data);
  g_task_set_source_tag(task, gh_nip05_lookup_async);
  GError *error = NULL;
  g_autofree gchar *local = NULL, *domain = NULL;
  if (!parse_address(address, &local, &domain, &error)) {
    g_task_return_error(task, error);
    return;
  }
  g_autofree gchar *mode = network_mode(self);
  if (g_str_equal(mode, "tor")) {
    /* Fail closed (charter P5): never a direct connection in Tor mode. */
    g_task_return_new_error(task, GH_NIP05_ERROR, GH_NIP05_ERROR_NETWORK_MODE,
                            "Addresses can't be looked up through Tor yet");
    return;
  }
  if (g_str_has_suffix(domain, ".onion")) {
    g_task_return_new_error(task, GH_NIP05_ERROR, GH_NIP05_ERROR_NETWORK_MODE,
                            ".onion addresses can only be reached through Tor");
    return;
  }
  g_autofree gchar *url = gh_nip05_dup_url(address, &error);
  if (!url) {
    g_task_return_error(task, error);
    return;
  }
  g_task_set_task_data(task, g_steal_pointer(&local), g_free);
  self->transport.get_async(self->transport_data, url, GH_NIP05_MAX_DOCUMENT, cancellable,
                            on_fetched, g_steal_pointer(&task));
}

gchar *
gh_nip05_lookup_finish(GhNip05 *self, GAsyncResult *result, GError **error)
{
  g_return_val_if_fail(g_task_is_valid(result, self), NULL);
  return g_task_propagate_pointer(G_TASK(result), error);
}

GhNip05 *
gh_nip05_new(GSettings *settings, const GhHttpTransport *transport, gpointer transport_data)
{
  g_return_val_if_fail(!settings || G_IS_SETTINGS(settings), NULL);
  g_return_val_if_fail(!transport || (transport->get_async && transport->get_finish), NULL);
  GhNip05 *self = g_object_new(GH_TYPE_NIP05, NULL);
  self->settings = settings ? g_object_ref(settings) : NULL;
  if (transport) {
    self->transport = *transport;
    self->transport_data = transport_data;
  } else {
    self->http = gh_net_http_new(settings);
    self->transport = *gh_net_http_transport();
    self->transport_data = self->http;
  }
  return self;
}

static void
gh_nip05_dispose(GObject *object)
{
  GhNip05 *self = GH_NIP05(object);
  g_clear_object(&self->http);
  g_clear_object(&self->settings);
  G_OBJECT_CLASS(gh_nip05_parent_class)->dispose(object);
}

static void
gh_nip05_class_init(GhNip05Class *klass)
{
  G_OBJECT_CLASS(klass)->dispose = gh_nip05_dispose;
}

static void
gh_nip05_init(GhNip05 *self)
{
  (void)self;
}
