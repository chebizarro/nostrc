/* ns-private.c - Private shares to one npub: NIP-17 over NIP-59 (nostrc-k95e)
 *
 * SPDX-License-Identifier: MIT
 */
#include "ns-private.h"
#include "ns-kind.h"

#include "nostr-event.h"
#include <nostr/nip59/nip59.h>

#include <gio/gio.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <string.h>

/* NIP-59: seal and wrap timestamps up to two days in the past. */
#define NS_PRIVATE_TIME_WINDOW (2 * 24 * 3600)

#define NS_GCM_TAG_LEN 16

void
ns_file_key_clear(NsFileKey *key)
{
  if (key != NULL)
    OPENSSL_cleanse(key, sizeof(*key));
}

static GBytes *
aes_gcm(gboolean encrypt, const guint8 *in, gsize in_len, const NsFileKey *key,
        GError **error)
{
  if (!encrypt && in_len < NS_GCM_TAG_LEN) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT, "ciphertext too short");
    return NULL;
  }
  if (in_len > G_MAXINT - NS_GCM_TAG_LEN) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_TOO_LARGE, "file too large to encrypt");
    return NULL;
  }
  gsize body = encrypt ? in_len : in_len - NS_GCM_TAG_LEN;
  guint8 *out = g_malloc(body + NS_GCM_TAG_LEN);
  EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
  int len = 0, fin = 0;
  gboolean ok =
    ctx != NULL &&
    (encrypt ? EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL)
             : EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL)) == 1 &&
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(key->nonce), NULL) == 1 &&
    (encrypt ? EVP_EncryptInit_ex(ctx, NULL, NULL, key->key, key->nonce)
             : EVP_DecryptInit_ex(ctx, NULL, NULL, key->key, key->nonce)) == 1;
  if (ok && encrypt) {
    ok = EVP_EncryptUpdate(ctx, out, &len, in, (int)body) == 1 &&
         EVP_EncryptFinal_ex(ctx, out + len, &fin) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, NS_GCM_TAG_LEN, out + body) == 1;
  } else if (ok) {
    ok = EVP_DecryptUpdate(ctx, out, &len, in, (int)body) == 1 &&
         EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, NS_GCM_TAG_LEN,
                             (void *)(in + body)) == 1 &&
         EVP_DecryptFinal_ex(ctx, out + len, &fin) == 1;
  }
  EVP_CIPHER_CTX_free(ctx);
  if (!ok) {
    OPENSSL_cleanse(out, body + NS_GCM_TAG_LEN);
    g_free(out);
    g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT,
                        encrypt ? "AES-256-GCM encryption failed"
                                : "AES-256-GCM authentication failed");
    return NULL;
  }
  return g_bytes_new_take(out, encrypt ? body + NS_GCM_TAG_LEN : body);
}

GBytes *
ns_private_encrypt_file(GBytes *plain, NsFileKey *out_key, GError **error)
{
  if (RAND_bytes(out_key->key, sizeof(out_key->key)) != 1 ||
      RAND_bytes(out_key->nonce, sizeof(out_key->nonce)) != 1) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_BAD_INPUT, "no randomness for a file key");
    return NULL;
  }
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(plain, &n);
  return aes_gcm(TRUE, d, n, out_key, error);
}

GBytes *
ns_private_decrypt_file(GBytes *sealed, const NsFileKey *key, GError **error)
{
  gsize n = 0;
  const guint8 *d = g_bytes_get_data(sealed, &n);
  return aes_gcm(FALSE, d, n, key, error);
}

static gchar *
hex(const guint8 *b, gsize n)
{
  GString *s = g_string_sized_new(n * 2);
  for (gsize i = 0; i < n; i++)
    g_string_append_printf(s, "%02x", b[i]);
  return g_string_free(s, FALSE);
}

void
ns_tags_add_private_file(JsonArray *tags, const NsBlobMeta *sealed,
                         const gchar *plain_mime, const gchar *plain_sha256,
                         guint width, guint height, const NsFileKey *key)
{
  g_autofree gchar *k = hex(key->key, sizeof(key->key));
  g_autofree gchar *n = hex(key->nonce, sizeof(key->nonce));
  g_autofree gchar *size = g_strdup_printf("%" G_GUINT64_FORMAT, sealed->size);
  ns_tags_add(tags, "file-type", plain_mime, NULL);
  ns_tags_add(tags, "encryption-algorithm", "aes-gcm", NULL);
  ns_tags_add(tags, "decryption-key", k, NULL);
  ns_tags_add(tags, "decryption-nonce", n, NULL);
  ns_tags_add(tags, "x", sealed->sha256, NULL);
  ns_tags_add(tags, "ox", plain_sha256, NULL);
  ns_tags_add(tags, "size", size, NULL);
  if (width > 0 && height > 0) {
    g_autofree gchar *dim = g_strdup_printf("%ux%u", width, height);
    ns_tags_add(tags, "dim", dim, NULL);
  }
  OPENSSL_cleanse(k, strlen(k));
}

gchar *
ns_private_rumor_json(gint kind, gint64 created_at, const gchar *sender_hex,
                      JsonArray *tags, const gchar *content, gboolean pretty)
{
  g_autofree gchar *unsigned_json =
    ns_event_unsigned_json(kind, created_at, sender_hex, tags, content, FALSE);
  gchar id[65] = "";
  if (sender_hex != NULL && *sender_hex != '\0') {
    NostrEvent *ev = nostr_event_new();
    if (nostr_event_deserialize_unsigned(ev, unsigned_json, NULL) != NOSTR_EVENT_VALIDATION_OK ||
        nostr_event_compute_id(ev, id) != NOSTR_EVENT_VALIDATION_OK)
      id[0] = '\0';
    nostr_event_free(ev);
  }
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, unsigned_json, -1, NULL))
    return g_steal_pointer(&unsigned_json);
  JsonObject *o = json_node_get_object(json_parser_get_root(p));
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  if (id[0] != '\0') {
    json_builder_set_member_name(b, "id");
    json_builder_add_string_value(b, id);
  }
  static const gchar *const order[] = { "pubkey", "created_at", "kind", "tags", "content" };
  for (gsize i = 0; i < G_N_ELEMENTS(order); i++)
    if (json_object_has_member(o, order[i])) {
      json_builder_set_member_name(b, order[i]);
      json_builder_add_value(b, json_node_copy(json_object_get_member(o, order[i])));
    }
  json_builder_end_object(b);
  g_autoptr(JsonNode) root = json_builder_get_root(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  json_generator_set_pretty(gen, pretty);
  json_generator_set_root(gen, root);
  return json_generator_to_data(gen, NULL);
}

/* A NIP-44 v2 payload: canonical base64, 132 .. 87472 characters. */
static gboolean
looks_nip44(const gchar *s)
{
  gsize n = strlen(s);
  if (n < 132 || n > 87472)
    return FALSE;
  for (gsize i = 0; i < n; i++)
    if (!g_ascii_isalnum(s[i]) && s[i] != '+' && s[i] != '/' && s[i] != '=')
      return FALSE;
  return TRUE;
}

/* NIP-59 gift wrap of @seal for @receiver_hex. nips/nip59 makes a key for
 * this wrap alone and wipes it, and dates the wrap from the CSPRNG; with no
 * randomness it wraps nothing rather than stamp the send time. */
static gchar *
wrap_seal(NostrEvent *seal, const gchar *receiver_hex, GError **error)
{
  gchar *out = NULL;
  NostrEvent *wrap = nostr_nip59_wrap(seal, receiver_hex, NULL);
  if (wrap != NULL) {
    char *s = nostr_event_serialize_compact(wrap);
    out = g_strdup(s);
    free(s);
    nostr_event_free(wrap);
  }
  if (out == NULL)
    g_set_error_literal(error, NS_ERROR, NS_ERROR_PUBLISH, "gift-wrapping failed");
  return out;
}

/* Defence in depth on what we are about to publish: a kind 1059 not by
 * the sender, naming exactly its receiver, dated strictly in the past
 * within the NIP-59 window. */
static gboolean
wrap_shape_ok(NostrEvent *wrap, const gchar *wrap_json, const gchar *sender_hex,
              const gchar *receiver_hex, gint64 asked_at)
{
  gint64 t = nostr_event_get_created_at(wrap);
  if (nostr_event_get_kind(wrap) != NS_KIND_GIFT_WRAP ||
      g_ascii_strcasecmp(nostr_event_get_pubkey(wrap), sender_hex) == 0 ||
      t >= asked_at || t < asked_at - NS_PRIVATE_TIME_WINDOW - 1)
    return FALSE;
  g_autoptr(JsonParser) p = json_parser_new();
  if (!json_parser_load_from_data(p, wrap_json, -1, NULL))
    return FALSE;
  JsonArray *tags = json_object_get_array_member(json_node_get_object(json_parser_get_root(p)),
                                                 "tags");
  if (tags == NULL || json_array_get_length(tags) != 1)
    return FALSE;
  JsonArray *pt = json_array_get_array_element(tags, 0);
  return pt != NULL && json_array_get_length(pt) >= 2 &&
         g_strcmp0(json_array_get_string_element(pt, 0), "p") == 0 &&
         g_ascii_strcasecmp(json_array_get_string_element(pt, 1), receiver_hex) == 0;
}

gchar *
ns_private_gift_wrap(NostrPublishSigner *signer, const gchar *sender_hex,
                     const gchar *rumor_json, const gchar *receiver_hex,
                     GError **error)
{
  if (strlen(rumor_json) > NS_PRIVATE_MAX_RUMOR_BYTES) {
    g_set_error(error, NS_ERROR, NS_ERROR_TOO_LARGE,
                "the private message is %zu bytes; NIP-44 encryption caps it at "
                "about %u", strlen(rumor_json), NS_PRIVATE_MAX_RUMOR_BYTES);
    return NULL;
  }

  /* 1. Seal: NIP-44 to the receiver and the signature both happen in the
   *    signer daemon. No tags; the timestamp says nothing. */
  GError *local = NULL;
  g_autofree gchar *sealed_rumor =
    nostr_publish_signer_nip44_encrypt(signer, rumor_json, receiver_hex, NULL, &local);
  if (sealed_rumor == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER, "encrypting for the recipient failed: %s",
                local ? local->message : "unknown");
    g_clear_error(&local);
    return NULL;
  }
  if (!looks_nip44(sealed_rumor)) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_NO_SIGNER,
                        "the signer's NIP44Encrypt did not return a NIP-44 payload");
    return NULL;
  }
  int64_t seal_at = 0;
  if (nostr_nip59_randomize_timestamp(g_get_real_time() / G_USEC_PER_SEC,
                                      NS_PRIVATE_TIME_WINDOW, &seal_at) != NIP59_OK) {
    g_set_error_literal(error, NS_ERROR, NS_ERROR_PUBLISH,
                        "no randomness to hide the seal's time");
    return NULL;
  }
  g_autoptr(JsonArray) no_tags = json_array_new();
  g_autofree gchar *seal_unsigned =
    ns_event_unsigned_json(NS_KIND_SEAL, seal_at, NULL, no_tags, sealed_rumor, FALSE);
  g_autofree gchar *seal_json =
    nostr_publish_signer_sign_event_json(signer, seal_unsigned, NULL, &local);
  if (seal_json == NULL) {
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER, "signing the seal failed: %s",
                local ? local->message : "unknown");
    g_clear_error(&local);
    return NULL;
  }

  /* 2. The signer must have sealed as the identity the rumor names, with
   *    nothing added: otherwise the recipient's client rejects it (NIP-17
   *    seal/rumor pubkey check) — or worse, it names someone else. */
  NostrEvent *seal = nostr_event_new();
  gboolean seal_ok =
    nostr_event_deserialize_signed(seal, seal_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_validate(seal, NULL) == NOSTR_EVENT_VALIDATION_OK &&
    nostr_event_get_kind(seal) == NS_KIND_SEAL &&
    g_ascii_strcasecmp(nostr_event_get_pubkey(seal), sender_hex) == 0;
  if (seal_ok) {
    g_autoptr(JsonParser) p = json_parser_new();
    seal_ok = json_parser_load_from_data(p, seal_json, -1, NULL) &&
              json_array_get_length(json_object_get_array_member(
                json_node_get_object(json_parser_get_root(p)), "tags")) == 0;
  }
  if (!seal_ok) {
    nostr_event_free(seal);
    g_set_error(error, NS_ERROR, NS_ERROR_NO_SIGNER,
                "the signer returned a seal that is not an untagged kind 13 by %s "
                "(did the active identity change?)", sender_hex);
    return NULL;
  }

  /* 3. Gift wrap from a key made for this wrap alone. */
  gint64 asked_at = g_get_real_time() / G_USEC_PER_SEC + 1;
  gchar *wrap_json = wrap_seal(seal, receiver_hex, error);
  nostr_event_free(seal);
  if (wrap_json == NULL)
    return NULL;
  NostrEvent *wrap = nostr_event_new();
  gboolean ok = nostr_event_deserialize_signed(wrap, wrap_json, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                nostr_event_validate(wrap, NULL) == NOSTR_EVENT_VALIDATION_OK &&
                wrap_shape_ok(wrap, wrap_json, sender_hex, receiver_hex, asked_at);
  nostr_event_free(wrap);
  if (!ok) {
    g_free(wrap_json);
    g_set_error_literal(error, NS_ERROR, NS_ERROR_PUBLISH,
                        "gift-wrapping failed (the wrap would not have hidden its time "
                        "or recipient correctly)");
    return NULL;
  }
  return wrap_json;
}

static const gchar *
string_at(JsonArray *a, guint i)
{
  JsonNode *n = json_array_get_element(a, i);
  return n != NULL && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING
           ? json_node_get_string(n) : NULL;
}

static gboolean
host_is_local(const gchar *host)
{
  g_autofree gchar *h = g_ascii_strdown(host, -1);
  if (g_str_equal(h, "localhost") || g_str_has_suffix(h, ".localhost") ||
      g_str_has_suffix(h, ".local") || *h == '\0')
    return TRUE;
  gsize n = strlen(h);
  if (h[0] == '[' && n > 2 && h[n - 1] == ']') {   /* IPv6 literal */
    h[n - 1] = '\0';
    memmove(h, h + 1, n - 1);
  }
  g_autoptr(GInetAddress) a = g_inet_address_new_from_string(h);
  if (a == NULL)
    return FALSE;                                   /* a DNS name */
  if (g_inet_address_get_is_loopback(a) || g_inet_address_get_is_any(a) ||
      g_inet_address_get_is_link_local(a) || g_inet_address_get_is_site_local(a) ||
      g_inet_address_get_is_multicast(a))
    return TRUE;
  if (g_inet_address_get_family(a) == G_SOCKET_FAMILY_IPV6) {
    const guint8 *b = g_inet_address_to_bytes(a);
    if ((b[0] & 0xFE) == 0xFC)                      /* fc00::/7 unique local */
      return TRUE;
  }
  return FALSE;
}

gboolean
ns_private_remote_relay_ok(const gchar *url, gboolean allow_loopback)
{
  if (url == NULL)
    return FALSE;
  gboolean secure = g_str_has_prefix(url, "wss://");
  if (!secure && !g_str_has_prefix(url, "ws://"))
    return FALSE;
  g_autoptr(GUri) uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  if (uri == NULL || g_uri_get_host(uri) == NULL || g_uri_get_userinfo(uri) != NULL)
    return FALSE;
  const gchar *host = g_uri_get_host(uri);
  if (host_is_local(host)) {
    /* Tests only, and only loopback IP literals: never the `localhost`
     * name, which is how ws://localhost/ reaches relay.sock. */
    g_autoptr(GInetAddress) a = g_inet_address_new_from_string(host);
    return allow_loopback && a != NULL && g_inet_address_get_is_loopback(a);
  }
  return secure;
}

GStrv
ns_private_inbox_relays(const gchar *event_json, gboolean allow_loopback,
                        guint *out_dropped)
{
  guint dropped = 0, kept = 0;
  g_autoptr(GStrvBuilder) b = g_strv_builder_new();
  g_autoptr(GHashTable) seen = g_hash_table_new(g_str_hash, g_str_equal);
  g_autoptr(JsonParser) p = json_parser_new();
  if (event_json != NULL && json_parser_load_from_data(p, event_json, -1, NULL) &&
      JSON_NODE_HOLDS_OBJECT(json_parser_get_root(p))) {
    JsonObject *o = json_node_get_object(json_parser_get_root(p));
    JsonArray *tags = json_object_has_member(o, "tags")
                        ? json_object_get_array_member(o, "tags") : NULL;
    for (guint i = 0; tags && i < json_array_get_length(tags); i++) {
      JsonNode *tn = json_array_get_element(tags, i);
      if (!JSON_NODE_HOLDS_ARRAY(tn))
        continue;
      JsonArray *t = json_node_get_array(tn);
      if (json_array_get_length(t) < 2)
        continue;
      const gchar *name = string_at(t, 0);
      const gchar *url = string_at(t, 1);
      if (g_strcmp0(name, "relay") != 0 || url == NULL || g_hash_table_contains(seen, url))
        continue;
      g_hash_table_add(seen, (gpointer)url);
      if (!ns_private_remote_relay_ok(url, allow_loopback) ||
          kept >= NS_PRIVATE_MAX_INBOX_RELAYS) {
        dropped++;
        continue;
      }
      g_strv_builder_add(b, url);
      kept++;
    }
  }
  if (out_dropped)
    *out_dropped = dropped;
  return g_strv_builder_end(b);
}
