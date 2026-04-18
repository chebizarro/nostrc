/* nd-token-store.c - Bearer token management via libsecret
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-token-store.h"

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#include <gio/gio.h>
#include <string.h>

#define ND_TOKEN_STORE_ERROR (nd_token_store_error_quark())

G_DEFINE_QUARK(nd-token-store-error-quark, nd_token_store_error)

struct _NdTokenStore {
  gint ref_count;
#ifdef HAVE_LIBSECRET
  SecretSchema schema;
#endif
};

#ifdef HAVE_LIBSECRET
static const SecretSchema nd_token_schema = {
  "org.nostr.Dav.Token", SECRET_SCHEMA_NONE,
  {
    { "account_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};
#endif

/**
 * Generate a 32-byte random token, base64url-encoded (no padding).
 */
static gchar *
generate_random_token(void)
{
  guint8 buf[32];

  /* GLib's g_random_int_range uses a CSPRNG on modern systems.
   * For extra safety we read from /dev/urandom if available. */
  GFile *urandom = g_file_new_for_path("/dev/urandom");
  GError *err = NULL;
  GFileInputStream *stream = g_file_read(urandom, NULL, &err);
  g_object_unref(urandom);

  if (stream != NULL) {
    gsize bytes_read = 0;
    g_input_stream_read_all(G_INPUT_STREAM(stream), buf, sizeof(buf),
                            &bytes_read, NULL, NULL);
    g_object_unref(stream);
    if (bytes_read == sizeof(buf))
      goto encode;
  }
  g_clear_error(&err);

  /* Fallback: GLib random */
  for (gsize i = 0; i < sizeof(buf); i++)
    buf[i] = (guint8)g_random_int_range(0, 256);

encode:;
  gchar *b64 = g_base64_encode(buf, sizeof(buf));

  /* Convert to base64url: + → -, / → _, strip = padding */
  for (gchar *p = b64; *p; p++) {
    if (*p == '+') *p = '-';
    else if (*p == '/') *p = '_';
  }
  /* Strip trailing '=' */
  gchar *eq = strchr(b64, '=');
  if (eq) *eq = '\0';

  /* Clear raw bytes */
  memset(buf, 0, sizeof(buf));

  return b64;
}

NdTokenStore *
nd_token_store_new(void)
{
  NdTokenStore *store = g_new0(NdTokenStore, 1);
  store->ref_count = 1;
#ifdef HAVE_LIBSECRET
  store->schema = nd_token_schema;
#endif
  return store;
}

void
nd_token_store_free(NdTokenStore *store)
{
  if (store == NULL) return;
  if (--store->ref_count > 0) return;
  g_free(store);
}

gchar *
nd_token_store_ensure_token(NdTokenStore *store,
                            const gchar  *account_id,
                            GError      **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(account_id != NULL, NULL);

#ifdef HAVE_LIBSECRET
  /* Try to retrieve existing token */
  GError *local_err = NULL;
  gchar *existing = secret_password_lookup_sync(
    &store->schema, NULL, &local_err,
    "account_id", account_id,
    "application", "nostr-dav",
    NULL);

  if (existing != NULL)
    return existing;

  g_clear_error(&local_err);

  /* Generate and store a new token */
  gchar *token = generate_random_token();

  g_autofree gchar *label = g_strdup_printf("nostr-dav token for %s", account_id);

  gboolean stored = secret_password_store_sync(
    &store->schema,
    SECRET_COLLECTION_DEFAULT,
    label,
    token,
    NULL, &local_err,
    "account_id", account_id,
    "application", "nostr-dav",
    NULL);

  if (!stored) {
    g_propagate_error(error, local_err);
    g_free(token);
    return NULL;
  }

  return token;
#else
  /* No libsecret: generate an ephemeral in-memory token.
   * This is acceptable for development but NOT for production. */
  (void)error;
  g_warning("nostr-dav: libsecret not available; using ephemeral token (insecure)");
  return generate_random_token();
#endif
}

gboolean
nd_token_store_validate(NdTokenStore *store,
                        const gchar  *account_id,
                        const gchar  *token)
{
  g_return_val_if_fail(store != NULL, FALSE);
  g_return_val_if_fail(account_id != NULL, FALSE);
  g_return_val_if_fail(token != NULL, FALSE);

#ifdef HAVE_LIBSECRET
  GError *err = NULL;
  gchar *stored = secret_password_lookup_sync(
    &store->schema, NULL, &err,
    "account_id", account_id,
    "application", "nostr-dav",
    NULL);

  if (stored == NULL) {
    g_clear_error(&err);
    return FALSE;
  }

  /* Constant-time comparison to prevent timing attacks */
  gsize slen = strlen(stored);
  gsize tlen = strlen(token);
  gsize len = (slen > tlen) ? slen : tlen;
  guint diff = (guint)(slen ^ tlen);
  for (gsize i = 0; i < len; i++) {
    guchar a = (i < slen) ? (guchar)stored[i] : 0;
    guchar b = (i < tlen) ? (guchar)token[i] : 0;
    diff |= (guint)(a ^ b);
  }

  secret_password_free(stored);
  return diff == 0;
#else
  /* Without libsecret we can't validate — accept everything in dev mode */
  (void)store; (void)account_id; (void)token;
  g_warning("nostr-dav: no libsecret — skipping auth validation");
  return TRUE;
#endif
}

gchar *
nd_token_store_get_token(NdTokenStore *store,
                         const gchar  *account_id,
                         GError      **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(account_id != NULL, NULL);

#ifdef HAVE_LIBSECRET
  return secret_password_lookup_sync(
    &store->schema, NULL, error,
    "account_id", account_id,
    "application", "nostr-dav",
    NULL);
#else
  (void)error;
  return NULL;
#endif
}
