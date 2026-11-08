/* SPDX-License-Identifier: MIT
 * Crypto-only peer for the actual-daemon wire regression. No admission store,
 * decision or method implementation lives in this executable.
 */
#include "test_check.h"
#include "signet/store.h"
#include <nostr-event.h>
#include <nostr-keys.h>
#include <nostr-kinds.h>
#include <nostr-tag.h>
#include <nostr/nip17/nip17.h>
#include <glib.h>
#include <json-glib/json-glib.h>
#include <sodium.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

static void protected_file(const char *directory, const char *name, const char *value) {
  char *path = g_build_filename(directory, name, NULL);
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  CHECK(fd >= 0);
  size_t len = strlen(value), offset = 0;
  while (offset < len) {
    ssize_t n = write(fd, value + offset, len - offset);
    CHECK(n > 0);
    offset += (size_t)n;
  }
  CHECK(close(fd) == 0);
  g_free(path);
}
static JsonObject *parse_object(const char *text) {
  JsonParser *parser = json_parser_new();
  CHECK(json_parser_load_from_data(parser, text, -1, NULL));
  JsonNode *root = json_parser_get_root(parser);
  CHECK(root && JSON_NODE_HOLDS_OBJECT(root));
  JsonObject *object = json_object_ref(json_node_get_object(root));
  g_object_unref(parser);
  return object;
}
static char *encode(JsonObject *object) {
  JsonNode *node = json_node_new(JSON_NODE_OBJECT);
  json_node_set_object(node, object);
  char *text = json_to_string(node, FALSE);
  json_node_free(node);
  return text;
}
static void emit(JsonObject *object) {
  char *text = encode(object);
  CHECK(puts(text) >= 0);
  CHECK(fflush(stdout) == 0);
  g_free(text);
  json_object_unref(object);
}
static void keypair(char **secret, char **pubkey) {
  *secret = nostr_key_generate_private();
  CHECK(*secret);
  *pubkey = nostr_key_get_public(*secret);
  CHECK(*pubkey);
}
static void wipe_free(char *secret) {
  if (!secret) return;
  sodium_memzero(secret, strlen(secret));
  free(secret);
}
int main(int argc, char **argv) {
  CHECK(argc == 2 && sodium_init() >= 0);
  struct rlimit limit = {0, 0};
  CHECK(setrlimit(RLIMIT_CORE, &limit) == 0);
  char *operator_secret = NULL, *operator_pubkey = NULL;
  char *boot_secret = NULL, *boot_pubkey = NULL;
  char *bunker_secret = NULL, *bunker_pubkey = NULL;
  keypair(&operator_secret, &operator_pubkey);
  keypair(&boot_secret, &boot_pubkey);
  keypair(&bunker_secret, &bunker_pubkey);
  protected_file(argv[1], "bunker.hex", bunker_secret);
  wipe_free(bunker_secret);
  uint8_t master[32];
  char master_hex[65];
  randombytes_buf(master, sizeof master);
  sodium_bin2hex(master_hex, sizeof master_hex, master, sizeof master);
  protected_file(argv[1], "master.hex", master_hex);
  sodium_memzero(master, sizeof master);
  sodium_memzero(master_hex, sizeof master_hex);
  JsonObject *ready = json_object_new();
  json_object_set_string_member(ready, "type", "ready");
  json_object_set_string_member(ready, "bunker_pubkey", bunker_pubkey);
  json_object_set_string_member(ready, "operator_pubkey", operator_pubkey);
  json_object_set_string_member(ready, "boot_pubkey", boot_pubkey);
  json_object_set_boolean_member(ready, "sqlcipher_available", signet_store_sqlcipher_available());
  emit(ready);
  char *line = NULL;
  size_t capacity = 0;
  ssize_t length;
  while ((length = getline(&line, &capacity, stdin)) > 0) {
    CHECK(length <= 65536);
    JsonObject *command = parse_object(line);
    const char *operation = json_object_get_string_member(command, "op");
    const char *actor = json_object_get_string_member(command, "actor");
    CHECK(!strcmp(actor, "operator") || !strcmp(actor, "boot"));
    const char *secret = !strcmp(actor, "operator") ? operator_secret : boot_secret;
    const char *pubkey = !strcmp(actor, "operator") ? operator_pubkey : boot_pubkey;
    JsonObject *output = json_object_new();
    if (!strcmp(operation, "wrap")) {
      JsonObject *request = json_object_get_object_member(command, "request");
      CHECK(request);
      char *content = encode(request);
      int64_t at = json_object_has_member(command, "rumor_at")
          ? json_object_get_int_member(command, "rumor_at") : (int64_t)time(NULL);
      /* A valid peer request is constructed explicitly, not by the daemon's
       * convenience rumor builder whose output this regression is checking. */
      NostrEvent *rumor = nostr_event_new();
      CHECK(rumor);
      nostr_event_set_pubkey(rumor, pubkey);
      nostr_event_set_kind(rumor, NOSTR_KIND_DIRECT_MESSAGE);
      nostr_event_set_created_at(rumor, at);
      nostr_event_set_content(rumor, content);
      NostrTag *recipient = nostr_tag_new("p", bunker_pubkey, NULL);
      CHECK(recipient);
      NostrTags *tags = nostr_tags_new(1, recipient);
      CHECK(tags);
      nostr_event_set_tags(rumor, tags);
      rumor->id = nostr_event_get_id(rumor);
      CHECK(rumor->id && nostr_event_validate_id(rumor, NULL) == NOSTR_EVENT_VALIDATION_OK);
      NostrEvent *seal = nostr_nip17_create_seal(rumor, secret, bunker_pubkey);
      CHECK(seal);
      NostrEvent *gift = nostr_nip17_create_gift_wrap(seal, bunker_pubkey);
      CHECK(gift);
      char *event_json = nostr_event_serialize_compact(gift);
      CHECK(event_json);
      JsonObject *event = parse_object(event_json);
      json_object_set_string_member(output, "type", "wrapped");
      json_object_set_object_member(output, "event", event);
      nostr_event_free(gift);
      nostr_event_free(seal);
      nostr_event_free(rumor);
      free(event_json);
      g_free(content);
    } else {
      CHECK(!strcmp(operation, "open"));
      JsonObject *event = json_object_get_object_member(command, "event");
      CHECK(event);
      char *event_json = encode(event);
      NostrEvent *gift = nostr_event_new();
      CHECK(gift && nostr_event_deserialize_compact(gift, event_json, NULL));
      NostrEvent *seal = nostr_nip17_unwrap_gift_wrap(gift, secret);
      CHECK(seal && nostr_event_validate(seal, NULL) == NOSTR_EVENT_VALIDATION_OK);
      CHECK(!strcmp(nostr_event_get_pubkey(seal), bunker_pubkey));
      NostrEvent *rumor = nostr_nip17_unwrap_seal(seal, secret);
      json_object_set_string_member(output, "type", "opened");
      bool valid = rumor && rumor->id && !rumor->sig &&
          nostr_event_validate_id(rumor, NULL) == NOSTR_EVENT_VALIDATION_OK &&
          nostr_event_get_kind(rumor) == NOSTR_KIND_DIRECT_MESSAGE &&
          !strcmp(nostr_event_get_pubkey(rumor), bunker_pubkey);
      json_object_set_boolean_member(output, "authority_authenticated", valid);
      if (valid) {
        char *rumor_json = nostr_event_serialize_compact(rumor);
        CHECK(rumor_json);
        json_object_set_object_member(output, "rumor", parse_object(rumor_json));
        json_object_set_object_member(output, "response", parse_object(rumor->content));
        sodium_memzero(rumor_json, strlen(rumor_json));
        free(rumor_json);
      } else {
        json_object_set_string_member(output, "error", "reply_rumor_invalid_id_or_unsigned_contract");
      }
      if (rumor && rumor->content) sodium_memzero(rumor->content, strlen(rumor->content));
      nostr_event_free(rumor);
      nostr_event_free(seal);
      g_free(event_json);
      nostr_event_free(gift);
    }
    emit(output);
    json_object_unref(command);
    sodium_memzero(line, (size_t)length);
  }
  free(line);
  wipe_free(operator_secret);
  wipe_free(boot_secret);
  free(operator_pubkey);
  free(boot_pubkey);
  free(bunker_pubkey);
  return 0;
}
