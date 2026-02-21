/*
 * bc-upstream-client.c - BcUpstreamClient: fetches blobs from remote Blossom servers
 *
 * SPDX-License-Identifier: MIT
 */

#include "bc-upstream-client.h"

#include <libsoup/soup.h>
#include <string.h>

/* ---- BcFetchResult ---- */

BcFetchResult *
bc_fetch_result_copy(const BcFetchResult *result)
{
  if (result == NULL)
    return NULL;

  BcFetchResult *copy = g_new0(BcFetchResult, 1);
  copy->data       = result->data ? g_bytes_ref(result->data) : NULL;
  copy->mime_type  = g_strdup(result->mime_type);
  copy->server_url = g_strdup(result->server_url);
  return copy;
}

void
bc_fetch_result_free(BcFetchResult *result)
{
  if (result == NULL)
    return;
  g_clear_pointer(&result->data, g_bytes_unref);
  g_free(result->mime_type);
  g_free(result->server_url);
  g_free(result);
}

/* ---- Error quark ---- */

G_DEFINE_QUARK(bc-upstream-client-error-quark, bc_upstream_client_error)

/* ---- Private structure ---- */

struct _BcUpstreamClient {
  GObject       parent_instance;

  gchar       **server_urls;   /* NULL-terminated, owned */
  SoupSession  *session;       /* Shared HTTP session */
};

G_DEFINE_TYPE(BcUpstreamClient, bc_upstream_client, G_TYPE_OBJECT)

/* ---- GObject lifecycle ---- */

static void
bc_upstream_client_finalize(GObject *obj)
{
  BcUpstreamClient *self = BC_UPSTREAM_CLIENT(obj);

  g_strfreev(self->server_urls);
  g_clear_object(&self->session);

  G_OBJECT_CLASS(bc_upstream_client_parent_class)->finalize(obj);
}

static void
bc_upstream_client_class_init(BcUpstreamClientClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->finalize = bc_upstream_client_finalize;
}

static void
bc_upstream_client_init(BcUpstreamClient *self)
{
  self->session = soup_session_new();
  soup_session_set_timeout(self->session, 30);
  soup_session_set_user_agent(self->session, "blossom-cache/1.0");
}

/* ---- Public API ---- */

BcUpstreamClient *
bc_upstream_client_new(const gchar * const *server_urls)
{
  BcUpstreamClient *self = g_object_new(BC_TYPE_UPSTREAM_CLIENT, NULL);

  if (server_urls != NULL) {
    self->server_urls = g_strdupv((gchar **)server_urls);
  } else {
    self->server_urls = g_new0(gchar *, 1);
  }

  return self;
}

void
bc_upstream_client_set_servers(BcUpstreamClient    *self,
                               const gchar * const *server_urls)
{
  g_return_if_fail(BC_IS_UPSTREAM_CLIENT(self));

  g_strfreev(self->server_urls);
  if (server_urls != NULL) {
    self->server_urls = g_strdupv((gchar **)server_urls);
  } else {
    self->server_urls = g_new0(gchar *, 1);
  }
}

BcFetchResult *
bc_upstream_client_fetch(BcUpstreamClient *self,
                         const gchar      *sha256,
                         GCancellable     *cancellable,
                         GError          **error)
{
  g_return_val_if_fail(BC_IS_UPSTREAM_CLIENT(self), NULL);
  g_return_val_if_fail(sha256 != NULL, NULL);

  if (self->server_urls == NULL || self->server_urls[0] == NULL) {
    g_set_error_literal(error,
                        BC_UPSTREAM_CLIENT_ERROR,
                        BC_UPSTREAM_CLIENT_ERROR_ALL_FAILED,
                        "No upstream servers configured");
    return NULL;
  }

  g_autoptr(GString) errors_combined = g_string_new(NULL);
  gboolean any_404 = TRUE;

  for (gsize i = 0; self->server_urls[i] != NULL; i++) {
    const gchar *server = self->server_urls[i];
    g_autofree gchar *url = g_strdup_printf("%s/%s", server, sha256);

    g_debug("upstream: trying %s", url);

    SoupMessage *msg = soup_message_new(SOUP_METHOD_GET, url);
    if (msg == NULL) {
      g_string_append_printf(errors_combined, "  %s: invalid URL\n", server);
      any_404 = FALSE;
      continue;
    }

    GError *local_err = NULL;
    GBytes *body = soup_session_send_and_read(self->session, msg,
                                              cancellable, &local_err);

    if (local_err != NULL) {
      g_string_append_printf(errors_combined, "  %s: %s\n",
                             server, local_err->message);
      g_clear_error(&local_err);
      g_object_unref(msg);
      any_404 = FALSE;
      continue;
    }

    guint status = soup_message_get_status(msg);

    if (status == 404) {
      g_string_append_printf(errors_combined, "  %s: 404 Not Found\n", server);
      g_bytes_unref(body);
      g_object_unref(msg);
      continue;
    }

    if (status < 200 || status >= 300) {
      g_string_append_printf(errors_combined, "  %s: HTTP %u\n", server, status);
      g_bytes_unref(body);
      g_object_unref(msg);
      any_404 = FALSE;
      continue;
    }

    /* Success! Extract Content-Type */
    const gchar *content_type = NULL;
    SoupMessageHeaders *resp_headers = soup_message_get_response_headers(msg);
    if (resp_headers != NULL) {
      content_type = soup_message_headers_get_content_type(resp_headers, NULL);
    }

    BcFetchResult *result = g_new0(BcFetchResult, 1);
    result->data       = body;  /* Transfer ownership */
    result->mime_type  = g_strdup(content_type);
    result->server_url = g_strdup(server);

    g_object_unref(msg);

    g_debug("upstream: fetched %s from %s (%" G_GSIZE_FORMAT " bytes)",
            sha256, server, g_bytes_get_size(body));

    return result;
  }

  /* All servers failed */
  if (any_404) {
    g_set_error(error,
                BC_UPSTREAM_CLIENT_ERROR,
                BC_UPSTREAM_CLIENT_ERROR_NOT_FOUND,
                "Blob %s not found on any upstream server:\n%s",
                sha256, errors_combined->str);
  } else {
    g_set_error(error,
                BC_UPSTREAM_CLIENT_ERROR,
                BC_UPSTREAM_CLIENT_ERROR_ALL_FAILED,
                "All upstream servers failed for %s:\n%s",
                sha256, errors_combined->str);
  }

  return NULL;
}

/* ---- Fetch with BUD-10 proxy hints ---- */

static gchar *
normalize_server_url(const gchar *server)
{
  if (server == NULL || server[0] == '\0')
    return NULL;

  /* If it already has a scheme, use as-is */
  if (g_str_has_prefix(server, "http://") || g_str_has_prefix(server, "https://"))
    return g_strdup(server);

  /* BUD-10: "When no scheme is specified, clients SHOULD try https:// first" */
  return g_strdup_printf("https://%s", server);
}

BcFetchResult *
bc_upstream_client_fetch_with_hints(BcUpstreamClient    *self,
                                    const gchar         *sha256,
                                    const gchar * const *server_hints,
                                    GCancellable        *cancellable,
                                    GError             **error)
{
  g_return_val_if_fail(BC_IS_UPSTREAM_CLIENT(self), NULL);
  g_return_val_if_fail(sha256 != NULL, NULL);

  /* Build a combined server list: hints first, then configured servers */
  g_autoptr(GPtrArray) all_servers = g_ptr_array_new_with_free_func(g_free);

  /* Add hint servers (with URL normalization) */
  if (server_hints != NULL) {
    for (gsize i = 0; server_hints[i] != NULL; i++) {
      gchar *normalized = normalize_server_url(server_hints[i]);
      if (normalized != NULL)
        g_ptr_array_add(all_servers, normalized);
    }
  }

  /* Add configured upstream servers (avoiding duplicates) */
  if (self->server_urls != NULL) {
    for (gsize i = 0; self->server_urls[i] != NULL; i++) {
      gboolean duplicate = FALSE;
      for (guint j = 0; j < all_servers->len; j++) {
        if (g_str_equal(self->server_urls[i], g_ptr_array_index(all_servers, j))) {
          duplicate = TRUE;
          break;
        }
      }
      if (!duplicate)
        g_ptr_array_add(all_servers, g_strdup(self->server_urls[i]));
    }
  }

  if (all_servers->len == 0) {
    g_set_error_literal(error,
                        BC_UPSTREAM_CLIENT_ERROR,
                        BC_UPSTREAM_CLIENT_ERROR_ALL_FAILED,
                        "No servers available (no hints and no configured upstream)");
    return NULL;
  }

  /* Temporarily replace server list and fetch */
  g_auto(GStrv) original = self->server_urls;
  g_ptr_array_add(all_servers, NULL); /* NULL-terminate */
  self->server_urls = g_strdupv((gchar **)all_servers->pdata);

  BcFetchResult *result = bc_upstream_client_fetch(self, sha256, cancellable, error);

  /* Restore original servers */
  g_strfreev(self->server_urls);
  self->server_urls = g_steal_pointer(&original);

  return result;
}
