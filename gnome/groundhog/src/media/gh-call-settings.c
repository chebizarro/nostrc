#include "gh-call-settings.h"
#include <libsecret/secret.h>

static const SecretSchema turn_schema = {
  .name = "org.nostr.Groundhog.CallTurn",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "url", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "username", SECRET_SCHEMA_ATTRIBUTE_STRING }, { NULL, 0 }
  }
};

gboolean
gh_call_ice_url_validate(const gchar *url, gboolean turn, GError **error)
{
  g_autoptr(GUri) uri = url ? g_uri_parse(url, G_URI_FLAGS_NONE, NULL) : NULL;
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  if (!scheme || (turn ? (g_strcmp0(scheme, "turn") && g_strcmp0(scheme, "turns"))
                       : g_strcmp0(scheme, "stun")) ||
      !g_uri_get_host(uri) || !*g_uri_get_host(uri) ||
      g_uri_get_userinfo(uri) || g_uri_get_fragment(uri) ||
      (g_uri_get_path(uri) && *g_uri_get_path(uri)) ||
      (g_uri_get_query(uri) && g_strcmp0(g_uri_get_query(uri), "transport=udp") &&
       g_strcmp0(g_uri_get_query(uri), "transport=tcp"))) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "Invalid ICE server URL (use stun://, turn:// or turns:// without credentials)");
    return FALSE;
  }
  return TRUE;
}

void
gh_call_turn_password_store_async(const gchar *url, const gchar *username,
                                  const gchar *password, GCancellable *cancel,
                                  GAsyncReadyCallback callback, gpointer data)
{
  secret_password_store(&turn_schema, SECRET_COLLECTION_DEFAULT,
      "Groundhog call TURN credential", password, cancel, callback, data,
      "url", url, "username", username, NULL);
}

gboolean
gh_call_turn_password_store_finish(GAsyncResult *result, GError **error)
{
  return secret_password_store_finish(result, error);
}

void
gh_call_turn_password_lookup_async(const gchar *url, const gchar *username,
                                   GCancellable *cancel,
                                   GAsyncReadyCallback callback, gpointer data)
{
  secret_password_lookup(&turn_schema, cancel, callback, data,
                        "url", url, "username", username, NULL);
}

gchar *
gh_call_turn_password_lookup_finish(GAsyncResult *result, GError **error)
{
  gchar *password = secret_password_lookup_finish(result, error);
  if (!password && (!error || !*error))
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Store a TURN credential in the platform Secret Service before calling");
  return password;
}

void gh_call_turn_password_free(gchar *password) { secret_password_free(password); }
