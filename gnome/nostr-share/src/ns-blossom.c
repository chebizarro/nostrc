/* ns-blossom.c - Blossom (BUD-01/02) upload for nostr-share
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-blossom.h"
#include "ns-event.h"
#include "ns-kind.h"

#include <hanami/hanami-blossom-client.h>
#include <hanami/hanami-types.h>

#include <stdlib.h>
#include <string.h>

static hanami_error_t
sign_via_signer(const char *event_json, char **out_signed_json, void *user_data)
{
  NostrPublishSigner *signer = user_data;
  GError *err = NULL;
  gchar *signed_json = nostr_publish_signer_sign_event_json(signer, event_json,
                                                            NULL, &err);
  if (signed_json == NULL) {
    g_warning("nostr-share: Blossom auth signing failed: %s",
              err ? err->message : "unknown");
    g_clear_error(&err);
    return HANAMI_ERR_AUTH;
  }
  /* hanami frees with free(). */
  *out_signed_json = strdup(signed_json);
  g_free(signed_json);
  return *out_signed_json ? HANAMI_OK : HANAMI_ERR_NOMEM;
}

gboolean
ns_blossom_upload(const gchar *const *servers, NostrPublishSigner *signer,
                  const gchar *pubkey_hex, const gchar *mime, GBytes *data,
                  const gchar *sha256_hex, gchar **out_url, gchar **out_server,
                  GError **error)
{
  if (servers == NULL || servers[0] == NULL) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_NO_SERVERS,
                        "no Blossom servers configured");
    return FALSE;
  }

  hanami_signer_t hs = {
    .pubkey    = pubkey_hex,
    .sign      = sign_via_signer,
    .user_data = signer,
  };
  gsize len = 0;
  const guint8 *bytes = g_bytes_get_data(data, &len);
  GString *failures = g_string_new(NULL);

  for (guint i = 0; servers[i] != NULL; i++) {
    const gchar *server = servers[i];
    if (!g_str_has_prefix(server, "https://")) {
      g_string_append_printf(failures, "\n  %s: refused (https:// only)", server);
      continue;
    }
    g_autofree gchar *endpoint = g_strdup(server);
    gsize n = strlen(endpoint);
    while (n > 8 && endpoint[n - 1] == '/')
      endpoint[--n] = '\0';

    hanami_blossom_client_opts_t opts = {
      .endpoint        = endpoint,
      .timeout_seconds = 120,
      .user_agent      = "nostr-share/" NS_VERSION,
    };
    hanami_blossom_client_t *client = NULL;
    hanami_error_t rc = hanami_blossom_client_new(&opts, &hs, &client);
    if (rc != HANAMI_OK) {
      g_string_append_printf(failures, "\n  %s: %s", server, hanami_strerror(rc));
      continue;
    }
    hanami_blossom_client_set_upload_content_type(client, mime);

    hanami_blob_descriptor_t *desc = NULL;
    rc = hanami_blossom_upload(client, bytes, len, sha256_hex, &desc);
    hanami_blossom_client_free(client);
    if (rc != HANAMI_OK) {
      g_string_append_printf(failures, "\n  %s: %s", server, hanami_strerror(rc));
      hanami_blob_descriptor_free(desc);
      continue;
    }

    /* A server that stored different bytes than we sent is not one we
     * will link to. */
    if (desc != NULL && desc->sha256[0] != '\0' &&
        g_ascii_strcasecmp(desc->sha256, sha256_hex) != 0) {
      g_string_append_printf(failures, "\n  %s: descriptor hash mismatch", server);
      hanami_blob_descriptor_free(desc);
      continue;
    }

    gchar *url = NULL;
    if (desc != NULL && desc->url != NULL && g_str_has_prefix(desc->url, "https://"))
      url = g_strdup(desc->url);
    else
      url = ns_blossom_blob_url(endpoint, sha256_hex, mime);
    hanami_blob_descriptor_free(desc);

    *out_url = url;
    if (out_server) *out_server = g_strdup(endpoint);
    g_string_free(failures, TRUE);
    return TRUE;
  }

  g_set_error(error, NS_ERROR, NS_ERROR_UPLOAD, "Blossom upload failed:%s",
              failures->str);
  g_string_free(failures, TRUE);
  return FALSE;
}
