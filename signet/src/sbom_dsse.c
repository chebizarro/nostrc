/* SPDX-License-Identifier: MIT
 * Bahia SBOM DSSE signing, deliberately not a general sign-hash API.
 */
#include "signet/key_store.h"

#include <glib.h>
#include <json-glib/json-glib.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <secp256k1.h>
#include <secp256k1_schnorrsig.h>
#include <string.h>

#define SBOM_PAYLOAD_TYPE "application/vnd.in-toto+json"
#define SBOM_STATEMENT_TYPE "https://in-toto.io/Statement/v1"
#define SBOM_MAX_PAYLOAD (64u * 1024u)

static const char *string_member(JsonObject *obj, const char *name) {
  JsonNode *node = json_object_get_member(obj, name);
  if (!node || !JSON_NODE_HOLDS_VALUE(node) ||
      json_node_get_value_type(node) != G_TYPE_STRING) return NULL;
  const char *value = json_node_get_string(node);
  return value && *value ? value : NULL;
}

static bool has_string_member(JsonObject *obj, const char *name) {
  JsonNode *node = json_object_get_member(obj, name);
  return node && JSON_NODE_HOLDS_VALUE(node) &&
      json_node_get_value_type(node) == G_TYPE_STRING;
}

static bool has_boolean_member(JsonObject *obj, const char *name) {
  JsonNode *node = json_object_get_member(obj, name);
  return node && JSON_NODE_HOLDS_VALUE(node) &&
      json_node_get_value_type(node) == G_TYPE_BOOLEAN;
}

static JsonObject *object_member(JsonObject *obj, const char *name) {
  JsonNode *node = json_object_get_member(obj, name);
  return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}

static bool hex_digest(const char *text, size_t length) {
  if (!text || strlen(text) != length) return false;
  for (const char *p = text; *p; ++p)
    if (!g_ascii_isxdigit(*p)) return false;
  return true;
}

static bool only_members(JsonObject *object, const char *const *allowed) {
  GList *members = json_object_get_members(object);
  bool valid = true;
  for (GList *item = members; item; item = item->next) {
    bool found = false;
    for (size_t i = 0; allowed[i]; ++i)
      if (strcmp((const char *)item->data, allowed[i]) == 0) { found = true; break; }
    if (!found) { valid = false; break; }
  }
  g_list_free(members);
  return valid;
}

typedef struct {
  GHashTable *names_by_object;
  bool duplicate;
} UniqueMembers;

static void check_unique_member(JsonParser *parser, JsonObject *object,
                                const gchar *name, gpointer user_data) {
  (void)parser;
  UniqueMembers *state = user_data;
  GHashTable *names = g_hash_table_lookup(state->names_by_object, object);
  if (!names) {
    names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_insert(state->names_by_object, object, names);
  }
  if (g_hash_table_contains(names, name)) state->duplicate = true;
  else g_hash_table_add(names, g_strdup(name));
}

static bool valid_sbom_statement(const uint8_t *payload, size_t length) {
  if (!payload || length == 0 || length > SBOM_MAX_PAYLOAD ||
      memchr(payload, 0, length) || !g_utf8_validate((const char *)payload, length, NULL))
    return false;
  /* JSON-GLib exposes decoded strings as C strings, so reject escaped NUL
   * before any strcmp/strlen can mistake a truncated value for a valid one. */
  for (size_t i = 0; i + 5 < length; ++i)
    if (payload[i] == '\\' && payload[i + 1] == 'u' &&
        memcmp(payload + i + 2, "0000", 4) == 0) return false;

  JsonParser *parser = json_parser_new();
  if (!parser) return false;
  UniqueMembers unique = {
      .names_by_object = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                               NULL, (GDestroyNotify)g_hash_table_unref)
  };
  g_signal_connect(parser, "object-member", G_CALLBACK(check_unique_member), &unique);
  json_parser_set_strict(parser, TRUE);
  GError *error = NULL;
  bool valid = false;
  if (!json_parser_load_from_data(parser, (const char *)payload, length, &error))
    goto done;
  if (unique.duplicate) goto done;
  JsonNode *root = json_parser_get_root(parser);
  if (!root || !JSON_NODE_HOLDS_OBJECT(root)) goto done;
  JsonObject *statement = json_node_get_object(root);
  static const char *const statement_fields[] =
      {"_type", "subject", "predicateType", "predicate", NULL};
  if (json_object_get_size(statement) != 4 ||
      !only_members(statement, statement_fields) ||
      g_strcmp0(string_member(statement, "_type"), SBOM_STATEMENT_TYPE) != 0)
    goto done;
  const char *predicate_type = string_member(statement, "predicateType");
  const char *format = NULL;
  if (!predicate_type ||
      (strcmp(predicate_type, "https://spdx.dev/Document") != 0 &&
       strcmp(predicate_type, "https://cyclonedx.org/bom") != 0)) goto done;

  JsonNode *subjects_node = json_object_get_member(statement, "subject");
  if (!subjects_node || !JSON_NODE_HOLDS_ARRAY(subjects_node)) goto done;
  JsonArray *subjects = json_node_get_array(subjects_node);
  /* Bahia's SBOM attestation builder emits exactly one artifact subject. */
  if (json_array_get_length(subjects) != 1) goto done;
  JsonNode *subject_node = json_array_get_element(subjects, 0);
  if (!subject_node || !JSON_NODE_HOLDS_OBJECT(subject_node)) goto done;
  JsonObject *subject = json_node_get_object(subject_node);
  static const char *const subject_fields[] = {"name", "digest", NULL};
  if (json_object_get_size(subject) != 2 ||
      !only_members(subject, subject_fields) || !has_string_member(subject, "name")) goto done;
  JsonObject *subject_digest = object_member(subject, "digest");
  if (!subject_digest || json_object_get_size(subject_digest) != 1) goto done;
  GList *digest_names = json_object_get_members(subject_digest);
  const char *digest_name = digest_names ? digest_names->data : NULL;
  const char *digest_value = digest_name ? string_member(subject_digest, digest_name) : NULL;
  bool digest_ok = digest_name && *digest_name && digest_value &&
      (strcmp(digest_name, "sha256") != 0 || hex_digest(digest_value, 64)) &&
      (strcmp(digest_name, "git") != 0 ||
       hex_digest(digest_value, 40) || hex_digest(digest_value, 64));
  g_list_free(digest_names);
  if (!digest_ok) goto done;

  JsonObject *predicate = object_member(statement, "predicate");
  if (!predicate) goto done;
  static const char *const predicate_fields[] =
      {"format", "location", "digest", "generator", "timestamp", "ntia", NULL};
  if (!only_members(predicate, predicate_fields)) goto done;
  format = string_member(predicate, "format");
  if (!format ||
      (strcmp(predicate_type, "https://spdx.dev/Document") == 0 && strcmp(format, "spdx") != 0) ||
      (strcmp(predicate_type, "https://cyclonedx.org/bom") == 0 && strcmp(format, "cyclonedx") != 0))
    goto done;
  JsonObject *sbom_digest = object_member(predicate, "digest");
  if (!sbom_digest || json_object_get_size(sbom_digest) != 1 ||
      !hex_digest(string_member(sbom_digest, "sha256"), 64)) goto done;
  JsonObject *location = object_member(predicate, "location");
  if (!location || !string_member(location, "uri")) goto done;
  static const char *const location_fields[] = {"type", "uri", "mediaType", NULL};
  if (!only_members(location, location_fields)) goto done;
  if (json_object_has_member(location, "mediaType") &&
      !has_string_member(location, "mediaType")) goto done;
  const char *location_type = string_member(location, "type");
  if (!location_type ||
      (strcmp(location_type, "blossom") != 0 &&
       strcmp(location_type, "oci-referrer") != 0 &&
       strcmp(location_type, "package-backend") != 0)) goto done;
  if (json_object_has_member(predicate, "generator")) {
    JsonObject *generator = object_member(predicate, "generator");
    static const char *const generator_fields[] = {"id", "version", "pubkey", NULL};
    if (!generator || !only_members(generator, generator_fields) ||
        !has_string_member(generator, "id") ||
        (json_object_has_member(generator, "version") &&
         !has_string_member(generator, "version")) ||
        (json_object_has_member(generator, "pubkey") &&
         !has_string_member(generator, "pubkey"))) goto done;
  }
  if (json_object_has_member(predicate, "timestamp") &&
      !has_string_member(predicate, "timestamp")) goto done;
  if (json_object_has_member(predicate, "ntia")) {
    JsonObject *ntia = object_member(predicate, "ntia");
    static const char *const ntia_fields[] = {
        "hasSupplierName", "hasComponentName", "hasComponentVersion",
        "hasUniqueID", "hasRelationship", "hasAuthor", "hasTimestamp",
        "isCompliant", NULL};
    if (!ntia || json_object_get_size(ntia) != 8 ||
        !only_members(ntia, ntia_fields)) goto done;
    for (size_t i = 0; ntia_fields[i]; ++i)
      if (!has_boolean_member(ntia, ntia_fields[i])) goto done;
  }
  valid = true;
done:
  if (error) g_error_free(error);
  g_object_unref(parser);
  g_hash_table_unref(unique.names_by_object);
  return valid;
}

typedef struct {
  uint8_t digest[32];
  uint8_t signature[64];
} SBOMSignWork;

static int sign_digest_in_custody(const uint8_t secret_key[32], void *user_data) {
  SBOMSignWork *work = user_data;
  secp256k1_context *ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
  if (!ctx) return -1;
  secp256k1_keypair keypair;
  uint8_t aux[32];
  int rc = -1;
  if (secp256k1_keypair_create(ctx, &keypair, secret_key) == 1 &&
      RAND_bytes(aux, sizeof(aux)) == 1 &&
      secp256k1_schnorrsig_sign32(ctx, work->signature, work->digest,
                                  &keypair, aux) == 1) rc = 0;
  OPENSSL_cleanse(aux, sizeof(aux));
  OPENSSL_cleanse(&keypair, sizeof(keypair));
  secp256k1_context_destroy(ctx);
  return rc;
}

int signet_key_store_sign_bahia_sbom_dsse(SignetKeyStore *ks,
                                          const char *agent_id,
                                          const char *owner, int64_t epoch,
                                          const uint8_t *payload,
                                          size_t payload_len,
                                          char **out_signature_b64) {
  if (out_signature_b64) *out_signature_b64 = NULL;
  if (!out_signature_b64 || !ks || !agent_id || !owner || epoch < 1 ||
      !valid_sbom_statement(payload, payload_len)) return -1;

  /* Hash exact caller bytes; never reserialize JSON before DSSE PAE. */
  char prefix[128];
  int prefix_len = g_snprintf(prefix, sizeof(prefix), "DSSEv1 %zu %s %zu ",
                              strlen(SBOM_PAYLOAD_TYPE), SBOM_PAYLOAD_TYPE,
                              payload_len);
  if (prefix_len <= 0 || (size_t)prefix_len >= sizeof(prefix)) return -1;
  SBOMSignWork work = {0};
  GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256);
  if (!hash) return -1;
  g_checksum_update(hash, (const guchar *)prefix, (gssize)prefix_len);
  g_checksum_update(hash, payload, payload_len);
  gsize digest_len = sizeof(work.digest);
  g_checksum_get_digest(hash, work.digest, &digest_len);
  g_checksum_free(hash);
  if (digest_len != sizeof(work.digest)) return -1;
  int rc = signet_key_store_with_signing_key(ks, agent_id, owner, epoch,
                                              sign_digest_in_custody, &work);
  if (rc == 0) {
    *out_signature_b64 = g_base64_encode(work.signature, sizeof(work.signature));
    if (!*out_signature_b64) rc = -1;
  }
  OPENSSL_cleanse(&work, sizeof(work));
  return rc == 0 ? 0 : -1;
}
