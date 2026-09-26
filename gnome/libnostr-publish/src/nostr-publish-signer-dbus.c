/* nostr-publish-signer-dbus.c - GDBusProxy signer implementation
 *
 * SPDX-License-Identifier: MIT
 *
 * Calls `org.nostr.Signer.SignEvent (in s eventJson, in s identity,
 * in s app_id, out s signed_event)` on the session bus — the wire
 * contract published in `nips/nip55l/dbus/org.nostr.Signer.xml`.
 * Wave 1 Milestone A D1.a kept the method name `SignEvent` but changed
 * its semantics: the daemon now returns the complete signed event JSON
 * (id + pubkey + sig included) rather than a bare 128-hex signature.
 * A pre-D1.a nostr-dav shipped with the placeholder name
 * `SignEventJson`; every publish attempt against a modern signer landed
 * as Error.UnknownMethod (nostrc-qqkw) until this file was corrected.
 * We pass an empty identity so the daemon uses the active account and
 * the caller-supplied `app_id` (nostr-dav passes "nostr-dav") so the
 * operator's approval prompts and ACL entries surface the caller by name.
 * Moved from gnome/nostr-dav (nd-signer-dbus.c).
 *
 * Result validated as minimally-well-formed JSON (contains `id`,
 * `pubkey`, `sig` string fields) so a malformed reply lands in
 * %NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED rather than propagating down the publisher
 * as an "unknown" transient. Full sig verification stays with
 * `nostr_event_validate` at the ingest side.
 */

#include "nostr-publish-signer.h"

#include <json-glib/json-glib.h>

#define NP_SIGNER_BUS_NAME       "org.nostr.Signer"
#define NP_SIGNER_OBJECT_PATH    "/org/nostr/signer"
#define NP_SIGNER_INTERFACE_NAME "org.nostr.Signer"
#define NP_SIGNER_METHOD_NAME    "SignEvent"

typedef struct {
  GDBusProxy *proxy;
  gchar      *app_id;
} NostrPublishSignerDbus;

static gboolean
looks_signed(const gchar *json_str, GError **error)
{
  g_autoptr(JsonParser) parser = json_parser_new();
  if (!json_parser_load_from_data(parser, json_str, -1, error)) {
    return FALSE;
  }

  JsonNode *root = json_parser_get_root(parser);
  if (root == NULL || JSON_NODE_TYPE(root) != JSON_NODE_OBJECT) {
    g_set_error_literal(error, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED,
                        "signer reply is not a JSON object");
    return FALSE;
  }
  JsonObject *obj = json_node_get_object(root);
  static const char *const fields[] = { "id", "pubkey", "sig" };
  for (gsize i = 0; i < G_N_ELEMENTS(fields); i++) {
    if (!json_object_has_member(obj, fields[i])) {
      g_set_error(error, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED,
                  "signer reply is missing '%s'", fields[i]);
      return FALSE;
    }
    JsonNode *n = json_object_get_member(obj, fields[i]);
    if (n == NULL || JSON_NODE_TYPE(n) != JSON_NODE_VALUE ||
        json_node_get_string(n) == NULL) {
      g_set_error(error, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED,
                  "signer reply '%s' is not a string", fields[i]);
      return FALSE;
    }
  }
  return TRUE;
}

/* Map a GDBusError code to the signer taxonomy. `Approval` / access-
 * denied replies come back with the standard access-denied code from the
 * daemon; treat everything else as transient. Nothing here is
 * exhaustive — the safe default is transient because the publisher can
 * always retry. */
static NostrPublishSignerError
map_dbus_error(GError *err)
{
  if (err == NULL)
    return NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT;
  if (g_dbus_error_is_remote_error(err)) {
    g_autofree gchar *name = g_dbus_error_get_remote_error(err);
    if (name != NULL &&
        (g_str_has_suffix(name, ".AccessDenied") ||
         g_str_has_suffix(name, ".Denied") ||
         g_str_has_suffix(name, ".ApprovalDenied")))
      return NOSTR_PUBLISH_SIGNER_ERROR_DENIED;
    if (name != NULL &&
        (g_str_has_suffix(name, ".InvalidArgs") ||
         g_str_has_suffix(name, ".InvalidEvent")))
      return NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED;
  }
  if (err->domain == G_IO_ERROR && err->code == G_IO_ERROR_CANCELLED)
    return NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT;
  return NOSTR_PUBLISH_SIGNER_ERROR_TRANSIENT;
}

static gchar *
dbus_sign_event_json(gpointer      user_data,
                     const gchar  *unsigned_json,
                     GCancellable *cancellable,
                     GError      **error)
{
  NostrPublishSignerDbus *dbus = user_data;

  GError *call_err = NULL;
  /* SignEvent takes three strings: the unsigned event JSON, the
   * identity selector (empty ⇒ active account), and an app_id used by
   * the daemon to identify the caller in approval prompts / ACLs. */
  g_autoptr(GVariant) reply =
    g_dbus_proxy_call_sync(dbus->proxy,
                           NP_SIGNER_METHOD_NAME,
                           g_variant_new("(sss)", unsigned_json, "",
                                         dbus->app_id),
                           G_DBUS_CALL_FLAGS_NONE,
                           30 * 1000, /* ms — matches signer approval UX */
                           cancellable, &call_err);
  if (reply == NULL) {
    NostrPublishSignerError code = map_dbus_error(call_err);
    g_set_error(error, NOSTR_PUBLISH_SIGNER_ERROR, code,
                "SignEvent: %s",
                call_err ? call_err->message : "(no reply)");
    g_clear_error(&call_err);
    return NULL;
  }

  const gchar *signed_json = NULL;
  g_variant_get(reply, "(&s)", &signed_json);
  if (signed_json == NULL || *signed_json == '\0') {
    g_set_error_literal(error, NOSTR_PUBLISH_SIGNER_ERROR, NOSTR_PUBLISH_SIGNER_ERROR_MALFORMED,
                        "SignEvent returned empty reply");
    return NULL;
  }

  GError *shape_err = NULL;
  if (!looks_signed(signed_json, &shape_err)) {
    g_propagate_error(error, shape_err);
    return NULL;
  }

  return g_strdup(signed_json);
}

static void
dbus_free(gpointer user_data)
{
  NostrPublishSignerDbus *dbus = user_data;
  g_clear_object(&dbus->proxy);
  g_free(dbus->app_id);
  g_free(dbus);
}

NostrPublishSigner *
nostr_publish_signer_new_dbus(GDBusConnection *connection,
                              const gchar     *app_id,
                              GError         **error)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), NULL);
  g_return_val_if_fail(app_id != NULL, NULL);

  GError *proxy_err = NULL;
  GDBusProxy *proxy =
    g_dbus_proxy_new_sync(connection,
                          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
                          G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS,
                          NULL,
                          NP_SIGNER_BUS_NAME,
                          NP_SIGNER_OBJECT_PATH,
                          NP_SIGNER_INTERFACE_NAME,
                          NULL,
                          &proxy_err);
  if (proxy == NULL) {
    g_propagate_prefixed_error(error, proxy_err,
                               "nostr-publish-signer: cannot build proxy: ");
    return NULL;
  }

  NostrPublishSignerDbus *dbus = g_new0(NostrPublishSignerDbus, 1);
  dbus->proxy = proxy;
  dbus->app_id = g_strdup(app_id);

  NostrPublishSignerVTable vt = {
    .sign_event_json    = dbus_sign_event_json,
    .user_data_destroy  = dbus_free,
  };
  return nostr_publish_signer_new_from_vtable(&vt, dbus);
}
