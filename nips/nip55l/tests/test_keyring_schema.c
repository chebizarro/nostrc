/* test_keyring_schema — unit test for the unified libsecret key schema
 * (nostrc-bml6) owned by gnome/seahorse (gnostr-secret) and adopted by the
 * nip55l signer daemon.
 *
 * Pure functions only — no Secret Service needed:
 *   - the schema the daemon stores under is org.gnostr.Signer/identity and
 *     declares every attribute the daemon and the former Seahorse helper
 *     wrote (a consumer/schema drift tripwire),
 *   - identity → attribute table mapping (defaults, hardware flag),
 *   - legacy org.gnostr.Signer/key and org.gnostr.Key → identity mapping,
 *     including key_type → origin and the hardware-reference refusal,
 *   - the Seahorse label format (nostrc-djvs).
 * The end-to-end migration against a real gnome-keyring is exercised by
 * test_signer_dbus_contract (phase 3) when gnome-keyring-daemon exists.
 */
#include "seahorse/secret_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) do { if (!(cond)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); exit(1); \
  } } while (0)

static const char *NPUB = "npub1sg6plzptd64u62a878hep2kev88swjh3tw00gjsfl8f237lmu63q0uf63m";

static gboolean schema_has(const SecretSchema *schema, const char *attr) {
  for (int i = 0; i < 32 && schema->attributes[i].name; i++)
    if (strcmp(schema->attributes[i].name, attr) == 0)
      return schema->attributes[i].type == SECRET_SCHEMA_ATTRIBUTE_STRING;
  return FALSE;
}

static void test_schema_shape(void) {
  CHECK(strcmp(gnostr_secret_schema.name, "org.gnostr.Signer/identity") == 0);
  CHECK(strcmp(GNOSTR_SECRET_SCHEMA_NAME, gnostr_secret_schema.name) == 0);
  /* Daemon identity attributes (pre-bml6 items must stay valid). */
  const char *daemon_attrs[] = { "key_id", "npub", "label", "hardware",
                                 "owner_uid", "owner_username", NULL };
  /* Former Seahorse helper attributes. */
  const char *helper_attrs[] = { "curve", "origin", "hardware_slot", NULL };
  /* Written by apps/gnostr-signer/src/secret_store.c under the same name. */
  const char *app_attrs[] = { "fingerprint", "created_at", NULL };
  for (int i = 0; daemon_attrs[i]; i++) CHECK(schema_has(&gnostr_secret_schema, daemon_attrs[i]));
  for (int i = 0; helper_attrs[i]; i++) CHECK(schema_has(&gnostr_secret_schema, helper_attrs[i]));
  for (int i = 0; app_attrs[i]; i++) CHECK(schema_has(&gnostr_secret_schema, app_attrs[i]));

  CHECK(strcmp(gnostr_secret_legacy_signer_key_schema.name, "org.gnostr.Signer/key") == 0);
  CHECK(schema_has(&gnostr_secret_legacy_signer_key_schema, "key_type"));
  CHECK(strcmp(gnostr_secret_legacy_helper_schema.name, "org.gnostr.Key") == 0);
  CHECK(schema_has(&gnostr_secret_legacy_helper_schema, "uid"));
  CHECK(strcmp(gnostr_secret_migration_schema.name, "org.gnostr.Signer/migration") == 0);
}

static void test_identity_to_attributes(void) {
  CHECK(gnostr_secret_identity_to_attributes(NULL) == NULL);
  GnostrSecretIdentity none = { .label = "x" };
  CHECK(gnostr_secret_identity_to_attributes(&none) == NULL);

  /* Minimal: defaults for key_id / origin / hardware / curve. */
  GnostrSecretIdentity min = { .npub = NPUB };
  GHashTable *a = gnostr_secret_identity_to_attributes(&min);
  CHECK(a != NULL);
  CHECK(g_hash_table_size(a) == 5);
  CHECK(strcmp(g_hash_table_lookup(a, "key_id"), NPUB) == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "npub"), NPUB) == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "curve"), "secp256k1") == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "origin"), "software") == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "hardware"), "false") == 0);
  CHECK(g_hash_table_lookup(a, "label") == NULL);
  g_hash_table_unref(a);

  /* Full, with empty strings treated as unset. */
  GnostrSecretIdentity full = {
    .key_id = "work", .npub = NPUB, .label = "Alice", .owner_uid = "1000",
    .owner_username = "", .origin = "hardware", .hardware_slot = "slot-2",
    .created_at = "2026-01-01T00:00:00Z",
  };
  a = gnostr_secret_identity_to_attributes(&full);
  CHECK(strcmp(g_hash_table_lookup(a, "key_id"), "work") == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "label"), "Alice") == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "owner_uid"), "1000") == 0);
  CHECK(g_hash_table_lookup(a, "owner_username") == NULL);
  CHECK(strcmp(g_hash_table_lookup(a, "hardware"), "true") == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "hardware_slot"), "slot-2") == 0);
  CHECK(strcmp(g_hash_table_lookup(a, "created_at"), "2026-01-01T00:00:00Z") == 0);
  /* Every emitted attribute is declared by the schema (libsecret rejects
   * stores with undeclared attributes). */
  GHashTableIter it; gpointer k;
  g_hash_table_iter_init(&it, a);
  while (g_hash_table_iter_next(&it, &k, NULL)) CHECK(schema_has(&gnostr_secret_schema, k));
  g_hash_table_unref(a);
}

static void test_origin_from_key_type(void) {
  CHECK(strcmp(gnostr_secret_origin_from_key_type(NULL), "software") == 0);
  CHECK(strcmp(gnostr_secret_origin_from_key_type(""), "software") == 0);
  CHECK(strcmp(gnostr_secret_origin_from_key_type("nostr"), "software") == 0);
  CHECK(strcmp(gnostr_secret_origin_from_key_type("something-new"), "software") == 0);
  CHECK(strcmp(gnostr_secret_origin_from_key_type("hardware"), "hardware") == 0);
  CHECK(strcmp(gnostr_secret_origin_from_key_type("HSM"), "hardware") == 0);
  CHECK(strcmp(gnostr_secret_origin_from_key_type("tpm"), "hardware") == 0);
}

static GHashTable *attrs_of(const char *const *kv) {
  GHashTable *t = g_hash_table_new(g_str_hash, g_str_equal);
  for (int i = 0; kv[i]; i += 2) g_hash_table_insert(t, (gpointer)kv[i], (gpointer)kv[i + 1]);
  return t;
}

static void test_legacy_signer_key_mapping(void) {
  const char *kv[] = { "application", "gnostr-signer", "label", "Main",
                       "npub", NPUB, "key_type", "nostr",
                       "created_at", "2025-05-05T05:05:05Z", NULL };
  GHashTable *legacy = attrs_of(kv);
  GnostrSecretIdentity id; const gchar *why = "unset";
  CHECK(gnostr_secret_legacy_to_identity(GNOSTR_SECRET_LEGACY_SIGNER_KEY, legacy, &id, &why));
  CHECK(why == NULL);
  CHECK(strcmp(id.npub, NPUB) == 0 && strcmp(id.key_id, NPUB) == 0);
  CHECK(strcmp(id.label, "Main") == 0);
  CHECK(strcmp(id.origin, "software") == 0);
  CHECK(strcmp(id.created_at, "2025-05-05T05:05:05Z") == 0);
  CHECK(id.hardware_slot == NULL && id.owner_uid == NULL);
  /* "application" has no place in the unified schema and must not leak. */
  GHashTable *a = gnostr_secret_identity_to_attributes(&id);
  CHECK(g_hash_table_lookup(a, "application") == NULL);
  CHECK(strcmp(g_hash_table_lookup(a, "label"), "Main") == 0);
  g_hash_table_unref(a);
  g_hash_table_unref(legacy);

  /* A hardware key_type carries no private key: refuse. */
  const char *hw[] = { "npub", NPUB, "key_type", "hsm", NULL };
  legacy = attrs_of(hw);
  CHECK(!gnostr_secret_legacy_to_identity(GNOSTR_SECRET_LEGACY_SIGNER_KEY, legacy, &id, &why));
  CHECK(why != NULL && strstr(why, "hardware") != NULL);
  g_hash_table_unref(legacy);

  /* Missing npub is not fatal: the migration derives it from the secret. */
  const char *nonpub[] = { "label", "Orphan", NULL };
  legacy = attrs_of(nonpub);
  CHECK(gnostr_secret_legacy_to_identity(GNOSTR_SECRET_LEGACY_SIGNER_KEY, legacy, &id, &why));
  CHECK(id.npub == NULL && strcmp(id.label, "Orphan") == 0);
  g_hash_table_unref(legacy);
}

static void test_legacy_helper_mapping(void) {
  const char *kv[] = { "type", "nostr-key", "npub", NPUB, "uid", "bob",
                       "curve", "secp256k1", "origin", "software", NULL };
  GHashTable *legacy = attrs_of(kv);
  GnostrSecretIdentity id; const gchar *why = NULL;
  CHECK(gnostr_secret_legacy_to_identity(GNOSTR_SECRET_LEGACY_HELPER_KEY, legacy, &id, &why));
  CHECK(strcmp(id.label, "bob") == 0);          /* uid → label */
  CHECK(strcmp(id.origin, "software") == 0);
  GHashTable *a = gnostr_secret_identity_to_attributes(&id);
  CHECK(g_hash_table_lookup(a, "type") == NULL); /* discriminator dropped */
  CHECK(g_hash_table_lookup(a, "uid") == NULL);
  g_hash_table_unref(a);
  g_hash_table_unref(legacy);

  const char *hw[] = { "type", "nostr-key", "npub", NPUB, "origin", "hardware",
                       "hardware_slot", "yubikey:9c", NULL };
  legacy = attrs_of(hw);
  CHECK(!gnostr_secret_legacy_to_identity(GNOSTR_SECRET_LEGACY_HELPER_KEY, legacy, &id, &why));
  CHECK(why != NULL);
  g_hash_table_unref(legacy);

  CHECK(!gnostr_secret_legacy_to_identity(GNOSTR_SECRET_LEGACY_HELPER_KEY, NULL, &id, &why));
}

static void test_label(void) {
  gchar *l = gnostr_secret_store_build_label("alice", NPUB);
  CHECK(strcmp(l, "Nostr key: alice (npub1sg6plzpt…)") == 0);
  g_free(l);
  l = gnostr_secret_store_build_label("", NPUB);
  CHECK(strcmp(l, "Nostr key: npub1sg6plzpt…") == 0);
  g_free(l);
  l = gnostr_secret_store_build_label(NULL, "npub1short");
  CHECK(strcmp(l, "Nostr key: npub1short") == 0);
  g_free(l);
  CHECK(gnostr_secret_store_build_label("alice", NULL) == NULL);
}

int main(void) {
  test_schema_shape();
  test_identity_to_attributes();
  test_origin_from_key_type();
  test_legacy_signer_key_mapping();
  test_legacy_helper_mapping();
  test_label();
  printf("test_keyring_schema: PASS\n");
  return 0;
}
