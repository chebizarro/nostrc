#include "nostr.h"
#include <json-glib/json-glib.h>

static void glib_json_init(void) {
    // Initialize GLib and JSON-GLib
    g_type_init();
}

static void glib_json_cleanup(void) {
    // No cleanup required for JSON-GLib
}

static char* glib_json_serialize(const NostrEvent *event) {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "id");
    json_builder_add_string_value(builder, event->id);
    json_builder_set_member_name(builder, "pubkey");
    json_builder_add_string_value(builder, event->pubkey);
    json_builder_set_member_name(builder, "kind");
    json_builder_add_int_value(builder, event->kind);
    json_builder_set_member_name(builder, "content");
    json_builder_add_string_value(builder, event->content);
    json_builder_set_member_name(builder, "sig");
    json_builder_add_string_value(builder, event->sig);

    json_builder_set_member_name(builder, "tags");
    json_builder_begin_array(builder);
    for (char **tag = event->tags; *tag != NULL; ++tag) {
        json_builder_add_string_value(builder, *tag);
    }
    json_builder_end_array(builder);

    json_builder_end_object(builder);

    JsonNode *root = json_builder_get_root(builder);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_root(gen, root);
    gchar *json_str = json_generator_to_data(gen, NULL);

    g_object_unref(gen);
    json_node_free(root);
    g_object_unref(builder);

    return json_str;
}

static NostrEvent* glib_json_deserialize(const char *json_str) {
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json_str, -1, NULL)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root(parser);
    JsonObject *object = json_node_get_object(root);

    NostrEvent *event = nostr_event_new();
    event->id = g_strdup(json_object_get_string_member(object, "id"));
    event->pubkey = g_strdup(json_object_get_string_member(object, "pubkey"));
    event->kind = json_object_get_int_member(object, "kind");
    event->content = g_strdup(json_object_get_string_member(object, "content"));
    event->sig = g_strdup(json_object_get_string_member(object, "sig"));

    JsonArray *tags = json_object_get_array_member(object, "tags");
    size_t tag_count = json_array_get_length(tags);
    event->tags = (char **)g_malloc((tag_count + 1) * sizeof(char *));
    for (size_t i = 0; i < tag_count; ++i) {
        event->tags[i] = g_strdup(json_array_get_string_element(tags, i));
    }
    event->tags[tag_count] = NULL;

    g_object_unref(parser);
    return event;
}

static const NostrJsonInterface glib_json_interface = {
    .init = glib_json_init,
    .cleanup = glib_json_cleanup,
    .serialize = glib_json_serialize,
    .deserialize = glib_json_deserialize
};
