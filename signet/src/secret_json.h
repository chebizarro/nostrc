/* SPDX-License-Identifier: MIT */
#ifndef SIGNET_SECRET_JSON_H
#define SIGNET_SECRET_JSON_H
#include <json-glib/json-glib.h>

/* Only for private, mutable trees at end of use. Wiping affects every shallow
 * copy; deep-copy nodes that escape the owner's lifetime. The JSON library
 * retains ownership: wipe its heap string values, then use its own destructor
 * (never sodium_free or free on GLib allocations). Not an allocator hook: this
 * cannot erase JSON-GLib's already-freed scanner/error-path scratch storage. */
void signet_secret_json_wipe(JsonNode *node);
JsonNode *signet_secret_json_copy(JsonNode *node);
void signet_secret_json_node_unref(JsonNode *node);
void signet_secret_json_parser_unref(JsonParser *parser);
void signet_secret_json_builder_unref(JsonBuilder *builder);
void signet_secret_json_generator_unref(JsonGenerator *generator);
void signet_secret_gfree(char *text);

typedef JsonParser SignetSecretJsonParser;
typedef JsonBuilder SignetSecretJsonBuilder;
typedef JsonGenerator SignetSecretJsonGenerator;
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignetSecretJsonParser, signet_secret_json_parser_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignetSecretJsonBuilder, signet_secret_json_builder_unref)
G_DEFINE_AUTOPTR_CLEANUP_FUNC(SignetSecretJsonGenerator, signet_secret_json_generator_unref)
#ifdef SIGNET_ENABLE_TEST_HOOKS
/* Observes live storage after wiping and before its owning allocator frees it. */
void signet_secret_json_set_wipe_hook(void (*hook)(const void *, size_t));
#endif
#endif
