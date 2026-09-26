/* nd-token-store.c - Bearer token management (file-backed, fail-closed)
 *
 * SPDX-License-Identifier: MIT
 */

#include "nd-token-store.h"

#ifdef HAVE_LIBSECRET
#include <libsecret/secret.h>
#endif

#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/random.h>
#include <unistd.h>

#define ND_TOKEN_BYTES      32   /* 256-bit bearer token */
#define ND_TOKEN_MIN_CHARS  43   /* base64url length of 32 bytes, unpadded */
#define ND_TOKEN_MAX_FILE   256

G_DEFINE_QUARK(nd-token-store-error-quark, nd_token_store_error)

struct _NdTokenStore {
  gchar   *dir;
  gchar   *path;
  gboolean mirror_to_keyring;
  gchar   *account_id;   /* account the loaded token is bound to */
  gchar   *token;        /* loaded token, NULL until ensured */
};

#ifdef HAVE_LIBSECRET
static const SecretSchema nd_token_schema = {
  .name = "org.nostr.Dav.Token",
  .flags = SECRET_SCHEMA_NONE,
  .attributes = {
    { "account_id", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { "application", SECRET_SCHEMA_ATTRIBUTE_STRING },
    { NULL, 0 }
  }
};
#endif

/* ---- Token material ---- */

static gchar *
generate_token(GError **error)
{
  guint8 buf[ND_TOKEN_BYTES];

  /* No non-CSPRNG fallback: without kernel entropy we refuse to mint. */
  if (getentropy(buf, sizeof(buf)) != 0) {
    int saved = errno;
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot obtain random bytes for token: %s", g_strerror(saved));
    return NULL;
  }

  gchar *b64 = g_base64_encode(buf, sizeof(buf));
  memset(buf, 0, sizeof(buf));

  for (gchar *p = b64; *p; p++) {
    if (*p == '+') *p = '-';
    else if (*p == '/') *p = '_';
  }
  gchar *eq = strchr(b64, '=');
  if (eq) *eq = '\0';

  return b64;
}

static gboolean
token_is_well_formed(const gchar *token)
{
  gsize len = strlen(token);
  if (len < ND_TOKEN_MIN_CHARS)
    return FALSE;
  for (gsize i = 0; i < len; i++) {
    gchar c = token[i];
    if (!g_ascii_isalnum(c) && c != '-' && c != '_')
      return FALSE;
  }
  return TRUE;
}

/* ---- Filesystem checks ---- */

static gboolean
check_config_dir(const gchar *dir, GError **error)
{
  if (g_mkdir_with_parents(dir, 0700) != 0) {
    int saved = errno;
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot create %s: %s", dir, g_strerror(saved));
    return FALSE;
  }

  struct stat st;
  if (lstat(dir, &st) != 0) {
    int saved = errno;
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot stat %s: %s", dir, g_strerror(saved));
    return FALSE;
  }

  if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_PERMISSIONS,
                "Refusing to use %s: it must be a directory owned by the "
                "current user and not group/world-writable "
                "(mode %04o, uid %u); fix with: chmod 700 %s",
                dir, (guint)(st.st_mode & 07777), (guint)st.st_uid, dir);
    return FALSE;
  }

  return TRUE;
}

/* Reads and validates an existing token file. Returns TRUE with
 * *out_token == NULL if the file does not exist. */
static gboolean
read_token_file(const gchar *path, gchar **out_token, GError **error)
{
  *out_token = NULL;

  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    int saved = errno;
    if (saved == ENOENT)
      return TRUE;
    g_set_error(error, ND_TOKEN_STORE_ERROR,
                saved == ELOOP ? ND_TOKEN_STORE_ERROR_PERMISSIONS
                               : ND_TOKEN_STORE_ERROR_IO,
                "Cannot open token file %s: %s", path, g_strerror(saved));
    return FALSE;
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int saved = errno;
    close(fd);
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot stat token file %s: %s", path, g_strerror(saved));
    return FALSE;
  }

  if (!S_ISREG(st.st_mode) || st.st_uid != getuid() ||
      (st.st_mode & 077) != 0) {
    close(fd);
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_PERMISSIONS,
                "Refusing to use token file %s: it must be a regular file "
                "owned by the current user with mode 0600 "
                "(found mode %04o, uid %u); fix with: chmod 600 %s",
                path, (guint)(st.st_mode & 07777), (guint)st.st_uid, path);
    return FALSE;
  }

  gchar buf[ND_TOKEN_MAX_FILE + 1];
  gsize total = 0;
  while (total < ND_TOKEN_MAX_FILE) {
    ssize_t n = read(fd, buf + total, ND_TOKEN_MAX_FILE - total);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0) {
      int saved = errno;
      close(fd);
      g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                  "Cannot read token file %s: %s", path, g_strerror(saved));
      return FALSE;
    }
    if (n == 0)
      break;
    total += (gsize)n;
  }
  close(fd);
  buf[total] = '\0';

  g_strchomp(buf);
  if (!token_is_well_formed(buf)) {
    memset(buf, 0, sizeof(buf));
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_INVALID,
                "Token file %s is malformed (expected at least %d base64url "
                "characters); delete it to mint a new token",
                path, ND_TOKEN_MIN_CHARS);
    return FALSE;
  }

  *out_token = g_strdup(buf);
  memset(buf, 0, sizeof(buf));
  return TRUE;
}

static gboolean
write_all(int fd, const gchar *data, gsize len)
{
  while (len > 0) {
    ssize_t n = write(fd, data, len);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return FALSE;
    data += n;
    len -= (gsize)n;
  }
  return TRUE;
}

/* Publishes a new token file atomically: write a 0600 temp file, fsync,
 * then link(2) it into place. link() fails with EEXIST instead of
 * clobbering, giving O_EXCL semantics without ever exposing a partially
 * written token file. Returns TRUE with *out_lost_race if another
 * process published first. */
static gboolean
publish_token_file(NdTokenStore *store,
                   const gchar  *token,
                   gboolean     *out_lost_race,
                   GError      **error)
{
  *out_lost_race = FALSE;

  g_autofree gchar *tmp = g_build_filename(store->dir, ".token-XXXXXX", NULL);
  int fd = g_mkstemp_full(tmp, O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) {
    int saved = errno;
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot create temporary token file in %s: %s",
                store->dir, g_strerror(saved));
    return FALSE;
  }

  g_autofree gchar *line = g_strconcat(token, "\n", NULL);
  gboolean ok = write_all(fd, line, strlen(line)) && fsync(fd) == 0;
  int saved = errno;
  memset(line, 0, strlen(line));
  close(fd);

  if (!ok) {
    g_unlink(tmp);
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot write token file %s: %s", tmp, g_strerror(saved));
    return FALSE;
  }

  if (link(tmp, store->path) != 0) {
    saved = errno;
    g_unlink(tmp);
    if (saved == EEXIST) {
      *out_lost_race = TRUE;
      return TRUE;
    }
    g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                "Cannot create token file %s: %s", store->path,
                g_strerror(saved));
    return FALSE;
  }
  g_unlink(tmp);

  int dfd = open(store->dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    (void)fsync(dfd);
    close(dfd);
  }
  return TRUE;
}

static void
mirror_to_keyring(NdTokenStore *store, const gchar *token)
{
#ifdef HAVE_LIBSECRET
  if (!store->mirror_to_keyring)
    return;

  GError *err = NULL;
  g_autofree gchar *label =
    g_strdup_printf("nostr-dav token for %s", store->account_id);
  if (!secret_password_store_sync(&nd_token_schema, SECRET_COLLECTION_DEFAULT,
                                  label, token, NULL, &err,
                                  "account_id", store->account_id,
                                  "application", "nostr-dav",
                                  NULL)) {
    g_message("nostr-dav: token not mirrored to keyring (%s); "
              "the token file remains authoritative",
              err ? err->message : "unknown error");
    g_clear_error(&err);
  }
#else
  (void)store;
  (void)token;
#endif
}

/* ---- Public API ---- */

gchar *
nd_token_store_default_dir(void)
{
  return g_build_filename(g_get_user_config_dir(), "nostr-dav", NULL);
}

NdTokenStore *
nd_token_store_new(const gchar *config_dir, gboolean mirror_to_keyring)
{
  g_return_val_if_fail(config_dir != NULL, NULL);

  NdTokenStore *store = g_new0(NdTokenStore, 1);
  store->dir = g_strdup(config_dir);
  store->path = g_build_filename(config_dir, "token", NULL);
  store->mirror_to_keyring = mirror_to_keyring;
  return store;
}

void
nd_token_store_free(NdTokenStore *store)
{
  if (store == NULL) return;
  if (store->token)
    memset(store->token, 0, strlen(store->token));
  g_free(store->token);
  g_free(store->account_id);
  g_free(store->path);
  g_free(store->dir);
  g_free(store);
}

const gchar *
nd_token_store_get_path(NdTokenStore *store)
{
  g_return_val_if_fail(store != NULL, NULL);
  return store->path;
}

gchar *
nd_token_store_ensure_token(NdTokenStore *store,
                            const gchar  *account_id,
                            GError      **error)
{
  g_return_val_if_fail(store != NULL, NULL);
  g_return_val_if_fail(account_id != NULL, NULL);

  if (store->token != NULL) {
    if (g_strcmp0(store->account_id, account_id) != 0) {
      g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_ACCOUNT,
                  "Token store is bound to account '%s'; nostr-dav v1 "
                  "supports a single account", store->account_id);
      return NULL;
    }
    return g_strdup(store->token);
  }

  if (!check_config_dir(store->dir, error))
    return NULL;

  gchar *token = NULL;
  if (!read_token_file(store->path, &token, error))
    return NULL;

  if (token == NULL) {
    token = generate_token(error);
    if (token == NULL)
      return NULL;

    gboolean lost_race = FALSE;
    if (!publish_token_file(store, token, &lost_race, error)) {
      g_free(token);
      return NULL;
    }

    if (lost_race) {
      /* Another instance minted first; adopt its token. */
      g_free(token);
      if (!read_token_file(store->path, &token, error))
        return NULL;
      if (token == NULL) {
        g_set_error(error, ND_TOKEN_STORE_ERROR, ND_TOKEN_STORE_ERROR_IO,
                    "Token file %s vanished while being created", store->path);
        return NULL;
      }
    } else {
      store->account_id = g_strdup(account_id);
      store->token = token;
      mirror_to_keyring(store, token);
      g_message("nostr-dav: minted new bearer token at %s", store->path);
      return g_strdup(token);
    }
  }

  store->account_id = g_strdup(account_id);
  store->token = token;
  return g_strdup(token);
}

gboolean
nd_token_store_has_token(NdTokenStore *store, const gchar *account_id)
{
  g_return_val_if_fail(store != NULL, FALSE);
  return store->token != NULL && account_id != NULL &&
         g_strcmp0(store->account_id, account_id) == 0;
}

gboolean
nd_token_store_validate(NdTokenStore *store,
                        const gchar  *account_id,
                        const gchar  *token)
{
  g_return_val_if_fail(store != NULL, FALSE);

  if (token == NULL || !nd_token_store_has_token(store, account_id))
    return FALSE;

  /* Constant-time comparison over the longer of the two lengths. */
  const gchar *stored = store->token;
  gsize slen = strlen(stored);
  gsize tlen = strlen(token);
  gsize len = MAX(slen, tlen);
  guint diff = (guint)(slen ^ tlen);
  for (gsize i = 0; i < len; i++) {
    guchar a = (i < slen) ? (guchar)stored[i] : 0;
    guchar b = (i < tlen) ? (guchar)token[i] : 0;
    diff |= (guint)(a ^ b);
  }
  return diff == 0;
}
