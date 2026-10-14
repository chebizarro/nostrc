/* Shared, pure migration-event builder; custody and publication are callers' policy. */
#include "key_rotation.h"
#include <json-glib/json-glib.h>
#include <nostr-gobject-1.0/nostr_nip19.h>
#include <time.h>

gchar *key_rotation_build_migration_event(const gchar *old_pubkey_hex,
    const gchar *new_pubkey_hex, gint64 created_at, const gchar *content) {
  if (!old_pubkey_hex || !new_pubkey_hex) return NULL;
  if (!created_at) created_at = time(NULL);
  g_autoptr(GNostrNip19) npub = gnostr_nip19_encode_npub(new_pubkey_hex, NULL);
  g_autofree gchar *message = content && *content ? g_strdup(content) :
      g_strdup_printf("Migrating to new key: %s", npub ? gnostr_nip19_get_bech32(npub) : new_pubkey_hex);
  g_autoptr(JsonBuilder) b = json_builder_new();
  json_builder_begin_object(b);
  json_builder_set_member_name(b, "kind"); json_builder_add_int_value(b, KEY_MIGRATION_EVENT_KIND);
  json_builder_set_member_name(b, "pubkey"); json_builder_add_string_value(b, old_pubkey_hex);
  json_builder_set_member_name(b, "created_at"); json_builder_add_int_value(b, created_at);
  json_builder_set_member_name(b, "tags"); json_builder_begin_array(b);
  json_builder_begin_array(b);
  json_builder_add_string_value(b, "p"); json_builder_add_string_value(b, new_pubkey_hex);
  json_builder_add_string_value(b, ""); json_builder_add_string_value(b, "successor");
  json_builder_end_array(b);
  json_builder_begin_array(b); json_builder_add_string_value(b, "alt");
  json_builder_add_string_value(b, "Key migration announcement"); json_builder_end_array(b);
  json_builder_end_array(b);
  json_builder_set_member_name(b, "content"); json_builder_add_string_value(b, message);
  json_builder_end_object(b);
  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(b); json_generator_set_root(gen, root);
  gchar *result = json_generator_to_data(gen, NULL); json_node_unref(root); return result;
}
