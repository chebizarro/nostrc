#include "gh-nip17-file.h"

#include <nostr-event.h>
#include <nostr-tag.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIZE_TAG G_GUINT64_CONSTANT(9007199254740991) /* 2^53 - 1 */
#define MAX_DIMENSION 65535

/* Explicit wipe the compiler may not drop. */
static void
wipe(gpointer data, gsize size)
{
  volatile guint8 *p = data;
  while (size--)
    *p++ = 0;
}

GhNip17File *
gh_nip17_file_copy(const GhNip17File *file)
{
  g_return_val_if_fail(file != NULL, NULL);
  GhNip17File *copy = g_new0(GhNip17File, 1);
  *copy = *file;
  copy->url = g_strdup(file->url);
  copy->file_type = g_strdup(file->file_type);
  return copy;
}

void
gh_nip17_file_free(GhNip17File *file)
{
  if (!file)
    return;
  g_free(file->url);
  g_free(file->file_type);
  wipe(file, sizeof *file);
  g_free(file);
}

static gboolean
invalid(GError **error, const gchar *why)
{
  g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Not a valid NIP-17 file message: %s",
              why);
  return FALSE;
}

static gboolean
hex_decode(const gchar *hex, guint8 *out, gsize size)
{
  if (!hex || strlen(hex) != size * 2)
    return FALSE;
  for (gsize i = 0; i < size; i++) {
    gint hi = g_ascii_xdigit_value(hex[2 * i]);
    gint lo = g_ascii_xdigit_value(hex[2 * i + 1]);
    if (hi < 0 || lo < 0)
      return FALSE;
    out[i] = (guint8)((hi << 4) | lo);
  }
  return TRUE;
}

static void
hex_encode(const guint8 *data, gsize size, gchar *out)
{
  static const gchar digits[] = "0123456789abcdef";
  for (gsize i = 0; i < size; i++) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0xf];
  }
  out[2 * size] = '\0';
}

/* 64 hex digits (either case) into out as lowercase. */
static gboolean
sha256_hex(const gchar *value, gchar out[65])
{
  guint8 digest[32];
  if (!hex_decode(value, digest, sizeof digest))
    return FALSE;
  hex_encode(digest, sizeof digest, out);
  return TRUE;
}

static gboolean
lower_hex64(const gchar *value)
{
  if (!value || strlen(value) != 64)
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isdigit(*p) && (*p < 'a' || *p > 'f'))
      return FALSE;
  return TRUE;
}

/* An absolute http(s) URL with a host, no user info, no whitespace or
 * control characters. Where it may be fetched from (https, or http to .onion
 * in Tor mode) is GhNetHttp's decision at download time. */
static gboolean
url_valid(const gchar *url)
{
  if (!url || !*url || strlen(url) > GH_NIP17_FILE_MAX_URL)
    return FALSE;
  for (const guchar *p = (const guchar *)url; *p; p++)
    if (*p <= 0x20 || *p == 0x7f)
      return FALSE;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
  const gchar *scheme = uri ? g_uri_get_scheme(uri) : NULL;
  const gchar *host = uri ? g_uri_get_host(uri) : NULL;
  return scheme && host && *host && !g_uri_get_userinfo(uri) &&
         (g_ascii_strcasecmp(scheme, "https") == 0 || g_ascii_strcasecmp(scheme, "http") == 0);
}

static gboolean
mime_token(const gchar *start, const gchar *end)
{
  if (start == end)
    return FALSE;
  for (const gchar *p = start; p < end; p++)
    if (!g_ascii_isalnum(*p) && !strchr("!#$&-^_.+", *p))
      return FALSE;
  return TRUE;
}

gchar *
gh_nip17_file_normalize_type(const gchar *value)
{
  if (!value)
    return NULL;
  const gchar *semicolon = strchr(value, ';');
  g_autofree gchar *bare = g_strstrip(g_strndup(value, semicolon ? (gsize)(semicolon - value)
                                                                 : strlen(value)));
  const gchar *slash = strchr(bare, '/');
  if (!slash || strlen(bare) > GH_NIP17_FILE_MAX_TYPE || !mime_token(bare, slash) ||
      !mime_token(slash + 1, bare + strlen(bare)))
    return NULL;
  return g_ascii_strdown(bare, -1);
}

static gboolean
parse_size(const gchar *value, guint64 *out)
{
  if (!value || !*value || strlen(value) > 16 || (value[0] == '0' && value[1]))
    return FALSE;
  for (const gchar *p = value; *p; p++)
    if (!g_ascii_isdigit(*p))
      return FALSE;
  return g_ascii_string_to_unsigned(value, 10, 0, MAX_SIZE_TAG, out, NULL);
}

static gboolean
parse_dimension(const gchar *start, gsize length, guint *out)
{
  if (length == 0 || length > 5 || start[0] == '0')
    return FALSE;
  guint value = 0;
  for (gsize i = 0; i < length; i++) {
    if (!g_ascii_isdigit(start[i]))
      return FALSE;
    value = value * 10 + (guint)(start[i] - '0');
  }
  if (value > MAX_DIMENSION)
    return FALSE;
  *out = value;
  return TRUE;
}

static gboolean
parse_dim(const gchar *value, guint *width, guint *height)
{
  const gchar *x = value ? strchr(value, 'x') : NULL;
  return x && parse_dimension(value, (gsize)(x - value), width) &&
         parse_dimension(x + 1, strlen(x + 1), height);
}

typedef enum {
  TAG_TYPE, TAG_ALGORITHM, TAG_KEY, TAG_NONCE, TAG_X, TAG_OX, TAG_SIZE, TAG_DIM, N_TAGS
} FileTag;

static const gchar *const tag_names[N_TAGS] = {
  "file-type", "encryption-algorithm", "decryption-key", "decryption-nonce", "x", "ox", "size",
  "dim"
};

static GhNip17File *
file_from_event(const NostrEvent *rumor, GError **error)
{
  if (nostr_event_get_kind(rumor) != GH_NIP17_FILE_KIND) {
    invalid(error, "not kind 15");
    return NULL;
  }
  const gchar *values[N_TAGS] = { NULL };
  NostrTags *tags = nostr_event_get_tags(rumor);
  for (size_t i = 0; tags && i < nostr_tags_size(tags); i++) {
    NostrTag *tag = nostr_tags_get(tags, i);
    const gchar *name = tag && nostr_tag_size(tag) >= 1 ? nostr_tag_get(tag, 0) : NULL;
    for (guint t = 0; name && t < N_TAGS; t++) {
      if (!g_str_equal(name, tag_names[t]))
        continue;
      if (values[t] || nostr_tag_size(tag) < 2) {
        invalid(error, "a file tag is repeated or empty");
        return NULL;
      }
      values[t] = nostr_tag_get(tag, 1);
    }
  }
  g_autoptr(GhNip17File) file = g_new0(GhNip17File, 1);
  const gchar *url = nostr_event_get_content(rumor);
  if (!url_valid(url)) {
    invalid(error, "the content is not an http(s) address");
    return NULL;
  }
  file->url = g_strdup(url);
  file->file_type = gh_nip17_file_normalize_type(values[TAG_TYPE]);
  if (!file->file_type) {
    invalid(error, "file-type is missing or not a MIME type");
    return NULL;
  }
  if (!values[TAG_ALGORITHM] || g_ascii_strcasecmp(values[TAG_ALGORITHM], "aes-gcm") != 0) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                        "The file is encrypted in a way Groundhog cannot open");
    return NULL;
  }
  if (!hex_decode(values[TAG_KEY], file->key, sizeof file->key)) {
    invalid(error, "decryption-key is not 32 bytes of hex");
    return NULL;
  }
  if (hex_decode(values[TAG_NONCE], file->nonce, GH_NIP17_FILE_NONCE_SIZE))
    file->nonce_size = GH_NIP17_FILE_NONCE_SIZE;
  else if (hex_decode(values[TAG_NONCE], file->nonce, GH_NIP17_FILE_NONCE_MAX))
    file->nonce_size = GH_NIP17_FILE_NONCE_MAX;
  else {
    invalid(error, "decryption-nonce is not 12 or 16 bytes of hex");
    return NULL;
  }
  if (!sha256_hex(values[TAG_X], file->x)) {
    invalid(error, "x is not a SHA-256");
    return NULL;
  }
  if (values[TAG_OX] && !sha256_hex(values[TAG_OX], file->ox)) {
    invalid(error, "ox is not a SHA-256");
    return NULL;
  }
  if (values[TAG_SIZE] && !parse_size(values[TAG_SIZE], &file->size)) {
    invalid(error, "size is not a byte count");
    return NULL;
  }
  if (values[TAG_DIM] && !parse_dim(values[TAG_DIM], &file->width, &file->height)) {
    invalid(error, "dim is not <width>x<height>");
    return NULL;
  }
  return g_steal_pointer(&file);
}

GhNip17File *
gh_nip17_file_from_rumor(const gchar *rumor_json, GError **error)
{
  if (!rumor_json || !g_utf8_validate(rumor_json, -1, NULL)) {
    invalid(error, "malformed");
    return NULL;
  }
  NostrEvent *rumor = nostr_event_new();
  if (!rumor || nostr_event_deserialize_unsigned(rumor, rumor_json, NULL) !=
                  NOSTR_EVENT_VALIDATION_OK) {
    if (rumor)
      nostr_event_free(rumor);
    invalid(error, "malformed");
    return NULL;
  }
  GhNip17File *file = file_from_event(rumor, error);
  nostr_event_free(rumor);
  return file;
}

/* A tag's (key, value) pair; the value may be the key or nonce: wiped. */
static void
pair_free(gpointer data)
{
  gchar **pair = data;
  if (pair && pair[1])
    wipe(pair[1], strlen(pair[1]));
  g_strfreev(pair);
}

static void
pair_add(GPtrArray *tags, const gchar *key, const gchar *value)
{
  gchar **pair = g_new0(gchar *, 3);
  pair[0] = g_strdup(key);
  pair[1] = g_strdup(value);
  g_ptr_array_add(tags, pair);
}

GPtrArray *
gh_nip17_file_dup_tags(const GhNip17File *file, GError **error)
{
  g_autofree gchar *file_type = file ? gh_nip17_file_normalize_type(file->file_type) : NULL;
  gchar x[65], ox[65] = "";
  if (!file || !url_valid(file->url) || !file_type ||
      (file->nonce_size != GH_NIP17_FILE_NONCE_SIZE &&
       file->nonce_size != GH_NIP17_FILE_NONCE_MAX) ||
      !sha256_hex(file->x, x) || (file->ox[0] && !sha256_hex(file->ox, ox)) ||
      file->size > MAX_SIZE_TAG || file->width > MAX_DIMENSION ||
      file->height > MAX_DIMENSION || (!file->width != !file->height)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A file message needs a complete encrypted file");
    return NULL;
  }
  gchar key[2 * GH_NIP17_FILE_KEY_SIZE + 1], nonce[2 * GH_NIP17_FILE_NONCE_MAX + 1];
  hex_encode(file->key, sizeof file->key, key);
  hex_encode(file->nonce, file->nonce_size, nonce);
  GPtrArray *tags = g_ptr_array_new_with_free_func(pair_free);
  pair_add(tags, "file-type", file_type);
  pair_add(tags, "encryption-algorithm", "aes-gcm");
  pair_add(tags, "decryption-key", key);
  pair_add(tags, "decryption-nonce", nonce);
  pair_add(tags, "x", x);
  if (ox[0])
    pair_add(tags, "ox", ox);
  if (file->size) {
    gchar size[24];
    g_snprintf(size, sizeof size, "%" G_GUINT64_FORMAT, file->size);
    pair_add(tags, "size", size);
  }
  if (file->width) {
    gchar dim[24];
    g_snprintf(dim, sizeof dim, "%ux%u", file->width, file->height);
    pair_add(tags, "dim", dim);
  }
  wipe(key, sizeof key);
  wipe(nonce, sizeof nonce);
  return tags;
}

gchar *
gh_nip17_file_rumor_new(const gchar *sender_pubkey_hex, const gchar *recipient_pubkey_hex,
                        const GhNip17File *file, gint64 created_at, gint64 expires_at,
                        gchar **out_rumor_id, GError **error)
{
  g_autofree gchar *sender = sender_pubkey_hex ? g_ascii_strdown(sender_pubkey_hex, -1) : NULL;
  g_autofree gchar *recipient = recipient_pubkey_hex ? g_ascii_strdown(recipient_pubkey_hex, -1)
                                                     : NULL;
  if (!lower_hex64(sender) || !lower_hex64(recipient) || created_at <= 0 ||
      (expires_at != 0 && expires_at <= created_at)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                        "A file message needs both keys, a time and a complete encrypted file");
    return NULL;
  }
  g_autoptr(GPtrArray) file_tags = gh_nip17_file_dup_tags(file, error);
  if (!file_tags)
    return NULL;
  NostrTags *tags = nostr_tags_new(0);
  nostr_tags_append(tags, nostr_tag_new("p", recipient, NULL));
  for (guint i = 0; i < file_tags->len; i++) {
    const gchar *const *pair = g_ptr_array_index(file_tags, i);
    nostr_tags_append(tags, nostr_tag_new(pair[0], pair[1], NULL));
  }
  if (expires_at) {
    gchar expiration[24];
    g_snprintf(expiration, sizeof expiration, "%" G_GINT64_FORMAT, expires_at);
    nostr_tags_append(tags, nostr_tag_new("expiration", expiration, NULL));
  }
  NostrEvent *rumor = nostr_event_new();
  nostr_event_set_kind(rumor, GH_NIP17_FILE_KIND);
  nostr_event_set_pubkey(rumor, sender);
  nostr_event_set_created_at(rumor, created_at);
  nostr_event_set_content(rumor, file->url);
  nostr_event_set_tags(rumor, tags);
  rumor->id = nostr_event_get_id(rumor);
  char *json = rumor->id && nostr_event_validate_id(rumor, NULL) == NOSTR_EVENT_VALIDATION_OK
                 ? nostr_event_serialize_compact(rumor) : NULL;
  if (json && out_rumor_id)
    *out_rumor_id = g_strdup(rumor->id);
  nostr_event_free(rumor);
  if (!json) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not build the file message");
    return NULL;
  }
  gchar *copy = g_strdup(json);
  wipe(json, strlen(json));
  free(json);
  return copy;
}

gboolean
gh_nip17_file_is_image(const GhNip17File *file)
{
  return file && file->file_type && g_str_has_prefix(file->file_type, "image/");
}
