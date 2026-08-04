/* SPDX-License-Identifier: MIT
 *
 * git_credential_main.c - signet-git-credential: installable git credential
 * helper backed by signetd's authenticated D-Bus Credentials interface.
 *
 * Usage (configured by git, never invoked with secrets on argv):
 *   git config --global credential.helper '!signet-git-credential'
 *
 * Flow for `get`:
 *   1. git writes protocol/host attributes on stdin.
 *   2. The helper resolves the credential id:
 *        - $SIGNET_GIT_CREDENTIAL_ID when set, else
 *        - derived from $SIGNET_AGENT_ID + host ("git:<host>", api_token).
 *   3. GetToken is called on net.signet.Credentials over D-Bus. signetd
 *      enforces deny-list precedence, the credential.get_token capability,
 *      per-agent ownership BEFORE decryption, type deny rules, expiry and
 *      revocation, issues a tracking lease, and appends a hash-chained
 *      audit entry — the helper itself holds no key material and no policy.
 *   4. The token is written ONLY to the credential protocol stream.
 *
 * `store` and `erase` are accepted and ignored: PAT lifecycle (create,
 * rotate, revoke) happens exclusively over the encrypted ContextVM
 * management channel. Rotation is transparent here because the credential
 * id is stable and GetToken always returns the active version.
 */

#include "signet/git_credential.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>
#include <sodium.h>

/* Decode the hex payload returned by GetToken into a NUL-terminated secret
 * string. Returns wiped-on-free heap memory (g_free after sodium_memzero). */
static char *signet_git_decode_token_hex(const char *token_hex) {
  if (!token_hex || !token_hex[0]) return NULL;
  size_t hex_len = strlen(token_hex);
  if (hex_len % 2 != 0) return NULL;
  size_t bin_capacity = hex_len / 2;
  unsigned char *bin = g_malloc(bin_capacity + 1);
  size_t bin_len = 0;
  if (sodium_hex2bin(bin, bin_capacity, token_hex, hex_len,
                     NULL, &bin_len, NULL) != 0) {
    sodium_memzero(bin, bin_capacity);
    g_free(bin);
    return NULL;
  }
  bin[bin_len] = '\0';
  return (char *)bin;
}

static int signet_git_dbus_lookup(const SignetGitCredentialQuery *query,
                                  void *user_data,
                                  char **out_username,
                                  char **out_password) {
  (void)user_data;
  if (!query || !out_username || !out_password) return -1;
  *out_username = NULL;
  *out_password = NULL;

  /* Resolve the credential id without argv or materialized config. */
  const char *cred_env = g_getenv("SIGNET_GIT_CREDENTIAL_ID");
  char derived_id[70] = {0};
  const char *cred_id = NULL;
  if (cred_env && cred_env[0]) {
    cred_id = cred_env;
  } else {
    const char *agent_id = g_getenv("SIGNET_AGENT_ID");
    if (!agent_id || !agent_id[0] || !query->host || !query->host[0])
      return 1; /* not configured for this host: quiet miss */
    if (signet_git_credential_derive_id(agent_id, query->host,
                                        derived_id) != 0)
      return 1;
    cred_id = derived_id;
  }

  /* Refuse to send a PAT over a non-https remote. */
  if (query->protocol && query->protocol[0] &&
      strcmp(query->protocol, "https") != 0)
    return 1;

  const char *bus_env = g_getenv("SIGNET_GIT_HELPER_BUS");
  GBusType bus_type = (bus_env && strcmp(bus_env, "session") == 0)
                          ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;

  GError *err = NULL;
  GDBusConnection *conn = g_bus_get_sync(bus_type, NULL, &err);
  if (!conn) {
    if (err) g_error_free(err);
    return 1;
  }

  GVariant *reply = g_dbus_connection_call_sync(
      conn, "net.signet.Signer", "/net/signet/Signer",
      "net.signet.Credentials", "GetToken",
      g_variant_new("(s)", cred_id),
      G_VARIANT_TYPE("(sx)"),
      G_DBUS_CALL_FLAGS_NONE, 30000, NULL, &err);
  if (!reply) {
    /* Denials and misses are uniform NotFound from signetd; stay quiet so
     * nothing about credential existence leaks into git's output. */
    if (err) g_error_free(err);
    g_object_unref(conn);
    return 1;
  }

  const char *token_hex = NULL;
  gint64 expires_at = 0;
  g_variant_get(reply, "(&sx)", &token_hex, &expires_at);

  char *secret = signet_git_decode_token_hex(token_hex);
  g_variant_unref(reply);
  g_object_unref(conn);
  if (!secret || !secret[0]) {
    if (secret) g_free(secret);
    return 1;
  }

  const char *user_env = g_getenv("SIGNET_GIT_USERNAME");
  *out_username = g_strdup(
      user_env && user_env[0] ? user_env
      : (query->username && query->username[0] ? query->username
                                               : "x-access-token"));
  *out_password = secret;
  return 0;
}

int main(int argc, char **argv) {
  if (sodium_init() < 0) return 1;

  const char *action = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "get";

  /* Nothing but protocol output ever reaches stdout; diagnostics are
   * suppressed entirely rather than risk leaking near the secret. */
  int rc = signet_git_credential_serve(action, stdin, stdout,
                                       signet_git_dbus_lookup, NULL);
  return rc == 0 ? 0 : 1;
}
