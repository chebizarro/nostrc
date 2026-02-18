/*
 * nip_c0_snippets.c - NIP-C0 (0xC0/192) Code Snippets Implementation
 *
 * Implements code snippet parsing, building, and language normalization for:
 *   - Kind 192 (0xC0): Code Snippet events
 */

#define G_LOG_DOMAIN "nip-c0-snippets"

#include "nip_c0_snippets.h"
#include <json-glib/json-glib.h>
#include <string.h>
#include <time.h>

/* Language normalization mapping */
typedef struct {
  const char *alias;
  const char *canonical;
  const char *display_name;
} LanguageMapping;

static const LanguageMapping language_mappings[] = {
  /* JavaScript variants */
  { "js", "javascript", "JavaScript" },
  { "javascript", "javascript", "JavaScript" },
  { "node", "javascript", "JavaScript" },
  { "nodejs", "javascript", "JavaScript" },
  { "ecmascript", "javascript", "JavaScript" },
  { "es6", "javascript", "JavaScript" },

  /* TypeScript */
  { "ts", "typescript", "TypeScript" },
  { "typescript", "typescript", "TypeScript" },

  /* Python variants */
  { "py", "python", "Python" },
  { "python", "python", "Python" },
  { "python3", "python", "Python" },
  { "py3", "python", "Python" },

  /* Rust */
  { "rs", "rust", "Rust" },
  { "rust", "rust", "Rust" },

  /* Go */
  { "go", "go", "Go" },
  { "golang", "go", "Go" },

  /* C variants */
  { "c", "c", "C" },
  { "h", "c", "C" },

  /* C++ variants */
  { "cpp", "cpp", "C++" },
  { "c++", "cpp", "C++" },
  { "cxx", "cpp", "C++" },
  { "hpp", "cpp", "C++" },
  { "cc", "cpp", "C++" },

  /* C# */
  { "cs", "csharp", "C#" },
  { "csharp", "csharp", "C#" },
  { "c#", "csharp", "C#" },

  /* Java */
  { "java", "java", "Java" },

  /* Kotlin */
  { "kt", "kotlin", "Kotlin" },
  { "kotlin", "kotlin", "Kotlin" },

  /* Swift */
  { "swift", "swift", "Swift" },

  /* Ruby */
  { "rb", "ruby", "Ruby" },
  { "ruby", "ruby", "Ruby" },

  /* PHP */
  { "php", "php", "PHP" },

  /* Perl */
  { "pl", "perl", "Perl" },
  { "perl", "perl", "Perl" },

  /* Shell/Bash */
  { "sh", "shell", "Shell" },
  { "bash", "shell", "Shell" },
  { "shell", "shell", "Shell" },
  { "zsh", "shell", "Shell" },
  { "fish", "shell", "Shell" },

  /* SQL */
  { "sql", "sql", "SQL" },
  { "mysql", "sql", "SQL" },
  { "postgresql", "sql", "SQL" },
  { "sqlite", "sql", "SQL" },

  /* HTML/CSS */
  { "html", "html", "HTML" },
  { "htm", "html", "HTML" },
  { "css", "css", "CSS" },
  { "scss", "css", "CSS" },
  { "sass", "css", "CSS" },
  { "less", "css", "CSS" },

  /* Markup/Config */
  { "json", "json", "JSON" },
  { "yaml", "yaml", "YAML" },
  { "yml", "yaml", "YAML" },
  { "toml", "toml", "TOML" },
  { "xml", "xml", "XML" },
  { "md", "markdown", "Markdown" },
  { "markdown", "markdown", "Markdown" },

  /* Lua */
  { "lua", "lua", "Lua" },

  /* Elixir */
  { "ex", "elixir", "Elixir" },
  { "exs", "elixir", "Elixir" },
  { "elixir", "elixir", "Elixir" },

  /* Haskell */
  { "hs", "haskell", "Haskell" },
  { "haskell", "haskell", "Haskell" },

  /* Scala */
  { "scala", "scala", "Scala" },

  /* Clojure */
  { "clj", "clojure", "Clojure" },
  { "clojure", "clojure", "Clojure" },

  /* Zig */
  { "zig", "zig", "Zig" },

  /* Nim */
  { "nim", "nim", "Nim" },

  /* Dart */
  { "dart", "dart", "Dart" },

  /* R */
  { "r", "r", "R" },

  /* Julia */
  { "jl", "julia", "Julia" },
  { "julia", "julia", "Julia" },

  /* OCaml */
  { "ml", "ocaml", "OCaml" },
  { "ocaml", "ocaml", "OCaml" },

  /* F# */
  { "fs", "fsharp", "F#" },
  { "fsharp", "fsharp", "F#" },
  { "f#", "fsharp", "F#" },

  /* Erlang */
  { "erl", "erlang", "Erlang" },
  { "erlang", "erlang", "Erlang" },

  /* Solidity */
  { "sol", "solidity", "Solidity" },
  { "solidity", "solidity", "Solidity" },

  /* Move (Sui/Aptos) */
  { "move", "move", "Move" },

  /* WASM */
  { "wasm", "wasm", "WebAssembly" },
  { "wat", "wasm", "WebAssembly" },

  /* Assembly */
  { "asm", "asm", "Assembly" },
  { "s", "asm", "Assembly" },

  /* Dockerfile */
  { "dockerfile", "dockerfile", "Dockerfile" },
  { "docker", "dockerfile", "Dockerfile" },

  /* Makefile */
  { "makefile", "makefile", "Makefile" },
  { "make", "makefile", "Makefile" },

  /* Nix */
  { "nix", "nix", "Nix" },

  /* Sentinel */
  { NULL, NULL, NULL }
};

GnostrCodeSnippet *
gnostr_code_snippet_new(void)
{
  GnostrCodeSnippet *snippet = g_new0(GnostrCodeSnippet, 1);
  return snippet;
}

void
gnostr_code_snippet_free(GnostrCodeSnippet *snippet)
{
  if (!snippet) return;

  g_free(snippet->event_id);
  g_free(snippet->pubkey);
  g_free(snippet->code);
  g_free(snippet->title);
  g_free(snippet->language);
  g_free(snippet->description);
  g_strfreev(snippet->tags);
  g_free(snippet->runtime);
  g_free(snippet->license);
  g_free(snippet);
}

GnostrCodeSnippet *
gnostr_code_snippet_dup(const GnostrCodeSnippet *snippet)
{
  if (!snippet) return NULL;

  GnostrCodeSnippet *dup = gnostr_code_snippet_new();

  dup->event_id = g_strdup(snippet->event_id);
  dup->pubkey = g_strdup(snippet->pubkey);
  dup->created_at = snippet->created_at;
  dup->code = g_strdup(snippet->code);
  dup->title = g_strdup(snippet->title);
  dup->language = g_strdup(snippet->language);
  dup->description = g_strdup(snippet->description);

  if (snippet->tags && snippet->tag_count > 0) {
    dup->tags = g_new0(gchar *, snippet->tag_count + 1);
    for (gsize i = 0; i < snippet->tag_count; i++) {
      dup->tags[i] = g_strdup(snippet->tags[i]);
    }
    dup->tag_count = snippet->tag_count;
  }

  dup->runtime = g_strdup(snippet->runtime);
  dup->license = g_strdup(snippet->license);

  return dup;
}

GnostrCodeSnippet *
gnostr_code_snippet_parse(const char *event_json)
{
  if (!event_json || !*event_json) return NULL;

  g_autoptr(JsonParser) parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error)) {
    g_warning("NIP-C0: Failed to parse event JSON: %s", error->message);
    g_error_free(error);
    return NULL;
  }

  JsonNode *root_node = json_parser_get_root(parser);
  if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) {
    g_debug("NIP-C0: Invalid JSON structure");
    return NULL;
  }

  JsonObject *root = json_node_get_object(root_node);

  /* Verify kind */
  if (!json_object_has_member(root, "kind")) {
    g_debug("NIP-C0: Missing kind field");
    return NULL;
  }

  gint64 kind = json_object_get_int_member(root, "kind");
  if (kind != NIPC0_KIND_SNIPPET) {
    g_debug("NIP-C0: Not a code snippet event (kind=%lld)", (long long)kind);
    return NULL;
  }

  GnostrCodeSnippet *snippet = gnostr_code_snippet_new();

  /* Extract event metadata */
  if (json_object_has_member(root, "id")) {
    snippet->event_id = g_strdup(json_object_get_string_member(root, "id"));
  }

  if (json_object_has_member(root, "pubkey")) {
    snippet->pubkey = g_strdup(json_object_get_string_member(root, "pubkey"));
  }

  if (json_object_has_member(root, "created_at")) {
    snippet->created_at = json_object_get_int_member(root, "created_at");
  }

  /* Extract content (the actual code) */
  if (json_object_has_member(root, "content")) {
    snippet->code = g_strdup(json_object_get_string_member(root, "content"));
  }

  /* Parse tags */
  if (json_object_has_member(root, "tags")) {
    JsonArray *tags = json_object_get_array_member(root, "tags");
    guint tags_len = json_array_get_length(tags);

    /* First pass: count "t" tags */
    gsize t_tag_count = 0;
    for (guint i = 0; i < tags_len; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2) continue;

      const char *tag_name = json_array_get_string_element(tag, 0);
      if (g_strcmp0(tag_name, "t") == 0) {
        t_tag_count++;
      }
    }

    /* Allocate tags array if needed */
    if (t_tag_count > 0) {
      snippet->tags = g_new0(gchar *, t_tag_count + 1);
    }

    /* Second pass: extract all tags */
    gsize t_idx = 0;
    for (guint i = 0; i < tags_len; i++) {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2) continue;

      const char *tag_name = json_array_get_string_element(tag, 0);
      const char *tag_value = json_array_get_string_element(tag, 1);
      if (!tag_name || !tag_value) continue;

      if (g_strcmp0(tag_name, "title") == 0) {
        g_free(snippet->title);
        snippet->title = g_strdup(tag_value);
      }
      else if (g_strcmp0(tag_name, "lang") == 0) {
        g_free(snippet->language);
        snippet->language = gnostr_code_snippet_normalize_language(tag_value);
      }
      else if (g_strcmp0(tag_name, "description") == 0) {
        g_free(snippet->description);
        snippet->description = g_strdup(tag_value);
      }
      else if (g_strcmp0(tag_name, "t") == 0) {
        if (snippet->tags && t_idx < t_tag_count) {
          snippet->tags[t_idx++] = g_strdup(tag_value);
        }
      }
      else if (g_strcmp0(tag_name, "runtime") == 0) {
        g_free(snippet->runtime);
        snippet->runtime = g_strdup(tag_value);
      }
      else if (g_strcmp0(tag_name, "license") == 0) {
        g_free(snippet->license);
        snippet->license = g_strdup(tag_value);
      }
    }

    snippet->tag_count = t_idx;
  }


  g_debug("NIP-C0: Parsed snippet '%s' (lang=%s, %zu tags)",
          snippet->title ? snippet->title : "(untitled)",
          snippet->language ? snippet->language : "(unknown)",
          snippet->tag_count);

  return snippet;
}

gchar *
gnostr_code_snippet_build_tags(const GnostrCodeSnippet *snippet)
{
  if (!snippet) return NULL;

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_array(builder);

  /* Add title tag if present */
  if (snippet->title && *snippet->title) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "title");
    json_builder_add_string_value(builder, snippet->title);
    json_builder_end_array(builder);
  }

  /* Add lang tag if present */
  if (snippet->language && *snippet->language) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "lang");
    json_builder_add_string_value(builder, snippet->language);
    json_builder_end_array(builder);
  }

  /* Add description tag if present */
  if (snippet->description && *snippet->description) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "description");
    json_builder_add_string_value(builder, snippet->description);
    json_builder_end_array(builder);
  }

  /* Add t tags for categories */
  if (snippet->tags) {
    for (gsize i = 0; i < snippet->tag_count && snippet->tags[i]; i++) {
      json_builder_begin_array(builder);
      json_builder_add_string_value(builder, "t");
      json_builder_add_string_value(builder, snippet->tags[i]);
      json_builder_end_array(builder);
    }
  }

  /* Add runtime tag if present */
  if (snippet->runtime && *snippet->runtime) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "runtime");
    json_builder_add_string_value(builder, snippet->runtime);
    json_builder_end_array(builder);
  }

  /* Add license tag if present */
  if (snippet->license && *snippet->license) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "license");
    json_builder_add_string_value(builder, snippet->license);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);

  gchar *result = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  return result;
}

gchar *
gnostr_code_snippet_build_event_json(const char *code,
                                      const char *title,
                                      const char *language,
                                      const char *description,
                                      const char **tags,
                                      const char *runtime,
                                      const char *license)
{
  if (!code || !*code) {
    g_warning("NIP-C0: Cannot create snippet without code");
    return NULL;
  }

  g_autoptr(JsonBuilder) builder = json_builder_new();
  json_builder_begin_object(builder);

  /* Add kind */
  json_builder_set_member_name(builder, "kind");
  json_builder_add_int_value(builder, NIPC0_KIND_SNIPPET);

  /* Add created_at */
  json_builder_set_member_name(builder, "created_at");
  json_builder_add_int_value(builder, (gint64)time(NULL));

  /* Add content (the code) */
  json_builder_set_member_name(builder, "content");
  json_builder_add_string_value(builder, code);

  /* Build tags array */
  json_builder_set_member_name(builder, "tags");
  json_builder_begin_array(builder);

  /* Add title tag if provided */
  if (title && *title) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "title");
    json_builder_add_string_value(builder, title);
    json_builder_end_array(builder);
  }

  /* Add lang tag if provided */
  if (language && *language) {
    gchar *normalized = gnostr_code_snippet_normalize_language(language);
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "lang");
    json_builder_add_string_value(builder, normalized);
    json_builder_end_array(builder);
    g_free(normalized);
  }

  /* Add description tag if provided */
  if (description && *description) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "description");
    json_builder_add_string_value(builder, description);
    json_builder_end_array(builder);
  }

  /* Add t tags for categories */
  if (tags) {
    for (const char **t = tags; *t; t++) {
      if (*t && **t) {
        json_builder_begin_array(builder);
        json_builder_add_string_value(builder, "t");
        json_builder_add_string_value(builder, *t);
        json_builder_end_array(builder);
      }
    }
  }

  /* Add runtime tag if provided */
  if (runtime && *runtime) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "runtime");
    json_builder_add_string_value(builder, runtime);
    json_builder_end_array(builder);
  }

  /* Add license tag if provided */
  if (license && *license) {
    json_builder_begin_array(builder);
    json_builder_add_string_value(builder, "license");
    json_builder_add_string_value(builder, license);
    json_builder_end_array(builder);
  }

  json_builder_end_array(builder);  /* End tags array */
  json_builder_end_object(builder);

  g_autoptr(JsonGenerator) gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);

  gchar *result = json_generator_to_data(gen, NULL);

  json_node_unref(root);

  g_debug("NIP-C0: Built snippet event (title=%s, lang=%s)",
          title ? title : "(none)",
          language ? language : "(none)");

  return result;
}

gchar *
gnostr_code_snippet_normalize_language(const char *language)
{
  if (!language || !*language) {
    return g_strdup("text");
  }

  /* Convert to lowercase for comparison */
  gchar *lower = g_ascii_strdown(language, -1);

  /* Look up in mappings */
  for (const LanguageMapping *m = language_mappings; m->alias; m++) {
    if (g_strcmp0(lower, m->alias) == 0) {
      g_free(lower);
      return g_strdup(m->canonical);
    }
  }

  /* Not found in mappings, return as-is (already lowercase) */
  return lower;
}

gchar *
gnostr_code_snippet_get_language_display_name(const char *language)
{
  if (!language || !*language) {
    return g_strdup("Text");
  }

  /* Convert to lowercase for comparison */
  gchar *lower = g_ascii_strdown(language, -1);

  /* Look up in mappings by canonical name */
  for (const LanguageMapping *m = language_mappings; m->alias; m++) {
    if (g_strcmp0(lower, m->canonical) == 0) {
      g_free(lower);
      return g_strdup(m->display_name);
    }
  }

  /* Also check alias in case input isn't canonical */
  for (const LanguageMapping *m = language_mappings; m->alias; m++) {
    if (g_strcmp0(lower, m->alias) == 0) {
      g_free(lower);
      return g_strdup(m->display_name);
    }
  }

  g_free(lower);

  /* Not found - capitalize first letter */
  gchar *display = g_strdup(language);
  if (display && *display) {
    display[0] = g_ascii_toupper(display[0]);
  }
  return display;
}
