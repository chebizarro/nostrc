/* SPDX-License-Identifier: MIT */
#include "secret_json.h"
#include <secure_buf.h>
#include <string.h>

#ifdef SIGNET_ENABLE_TEST_HOOKS
static void (*wipe_hook)(const void *, size_t);
void signet_secret_json_set_wipe_hook(void (*hook)(const void *, size_t)) {
  wipe_hook = hook;
}
#endif

void signet_secret_bytes_free(void *data, size_t size) {
  if (!data) return;
  secure_wipe(data, size);
#ifdef SIGNET_ENABLE_TEST_HOOKS
  if (wipe_hook) wipe_hook(data, size);
#endif
  g_free(data);
}

void signet_secret_gfree(char *text) {
  if (text) signet_secret_bytes_free(text, strlen(text));
}

void signet_secret_json_wipe(JsonNode *node) {
  if (!node) return;
  if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING) {
    const char *text = json_node_get_string(node);
    if (text) {
      size_t len = strlen(text);
      secure_wipe((void *)text, len);
#ifdef SIGNET_ENABLE_TEST_HOOKS
      if (wipe_hook) wipe_hook(text, len);
#endif
    }
  } else if (JSON_NODE_HOLDS_OBJECT(node)) {
    JsonObject *object = json_node_get_object(node);
    GList *members = json_object_get_members(object);
    for (GList *it = members; it; it = it->next)
      signet_secret_json_wipe(json_object_get_member(object, it->data));
    g_list_free(members);
  } else if (JSON_NODE_HOLDS_ARRAY(node)) {
    JsonArray *array = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(array); i++)
      signet_secret_json_wipe(json_array_get_element(array, i));
  }
}

/* json_node_copy shares JsonValue/objects/arrays. Strings and containers must
 * instead have independent storage when they outlive a secret parser. */
JsonNode *signet_secret_json_copy(JsonNode *node) {
  if (!node) return NULL;
  JsonNode *copy = json_node_new(json_node_get_node_type(node));
  if (JSON_NODE_HOLDS_OBJECT(node)) {
    JsonObject *object = json_object_new();
    JsonObject *source = json_node_get_object(node);
    GList *members = json_object_get_members(source);
    for (GList *it = members; it; it = it->next)
      json_object_set_member(object, it->data,
          signet_secret_json_copy(json_object_get_member(source, it->data)));
    g_list_free(members);
    json_node_take_object(copy, object);
  } else if (JSON_NODE_HOLDS_ARRAY(node)) {
    JsonArray *array = json_array_new(), *source = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(source); i++)
      json_array_add_element(array, signet_secret_json_copy(json_array_get_element(source, i)));
    json_node_take_array(copy, array);
  } else if (JSON_NODE_HOLDS_VALUE(node)) {
    switch (json_node_get_value_type(node)) {
      case G_TYPE_STRING: json_node_set_string(copy, json_node_get_string(node)); break;
      case G_TYPE_BOOLEAN: json_node_set_boolean(copy, json_node_get_boolean(node)); break;
      case G_TYPE_INT64: json_node_set_int(copy, json_node_get_int(node)); break;
      case G_TYPE_DOUBLE: json_node_set_double(copy, json_node_get_double(node)); break;
      default: g_assert_not_reached();
    }
  }
  return copy;
}

void signet_secret_json_node_unref(JsonNode *node) {
  if (!node) return;
  signet_secret_json_wipe(node);
  json_node_unref(node);
}
void signet_secret_json_parser_unref(JsonParser *parser) {
  if (!parser) return;
  signet_secret_json_wipe(json_parser_get_root(parser));
  g_object_unref(parser);
}
void signet_secret_json_builder_unref(JsonBuilder *builder) {
  if (!builder) return;
  JsonNode *root = json_builder_get_root(builder);
  signet_secret_json_node_unref(root);
  g_object_unref(builder);
}
void signet_secret_json_generator_unref(JsonGenerator *generator) {
  if (!generator) return;
  signet_secret_json_wipe(json_generator_get_root(generator));
  g_object_unref(generator);
}
