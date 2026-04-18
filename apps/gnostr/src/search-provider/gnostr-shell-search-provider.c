/**
 * gnostr-shell-search-provider.c - GNOME Shell SearchProvider2 for Nostr
 *
 * Surfaces people (kind 0 profiles) and notes (kind 1) from the local
 * NDB cache when the user types in GNOME Shell's search overlay.
 *
 * Result ID encoding:
 *   profile:<64-char-hex-pubkey>
 *   note:<64-char-hex-event-id>
 *
 * nostrc-sl86
 */

#define G_LOG_DOMAIN "gnostr-search-provider"

#include "gnostr-shell-search-provider.h"
#include <nostr-gobject-1.0/storage_ndb.h>
#include <nostr-gobject-1.0/nostr_profile_provider.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <string.h>

/* ---- constants --------------------------------------------------------- */

#define OBJECT_PATH    "/org/gnostr/SearchProvider"
#define MAX_RESULTS    20
#define MAX_PROFILES   10
#define MAX_NOTES      10

/* ---- SearchProvider2 introspection XML --------------------------------- */

static const gchar introspection_xml[] =
  "<node>"
  "  <interface name='org.gnome.Shell.SearchProvider2'>"
  "    <method name='GetInitialResultSet'>"
  "      <arg type='as' name='terms'    direction='in'/>"
  "      <arg type='as' name='results'  direction='out'/>"
  "    </method>"
  "    <method name='GetSubsearchResultSet'>"
  "      <arg type='as' name='previous_results' direction='in'/>"
  "      <arg type='as' name='terms'            direction='in'/>"
  "      <arg type='as' name='results'          direction='out'/>"
  "    </method>"
  "    <method name='GetResultMetas'>"
  "      <arg type='as'    name='identifiers' direction='in'/>"
  "      <arg type='aa{sv}' name='metas'      direction='out'/>"
  "    </method>"
  "    <method name='ActivateResult'>"
  "      <arg type='s'  name='identifier' direction='in'/>"
  "      <arg type='as' name='terms'      direction='in'/>"
  "      <arg type='u'  name='timestamp'  direction='in'/>"
  "    </method>"
  "    <method name='LaunchSearch'>"
  "      <arg type='as' name='terms'     direction='in'/>"
  "      <arg type='u'  name='timestamp' direction='in'/>"
  "    </method>"
  "  </interface>"
  "</node>";

static GDBusNodeInfo *s_introspection = NULL;

/* ---- helpers ----------------------------------------------------------- */

/**
 * Join search terms with spaces to form a single query string.
 */
static gchar *
terms_to_query(GVariant *terms_variant)
{
  GVariantIter iter;
  const gchar *term;
  GString *buf = g_string_new(NULL);

  g_variant_iter_init(&iter, terms_variant);
  while (g_variant_iter_next(&iter, "&s", &term)) {
    if (buf->len > 0)
      g_string_append_c(buf, ' ');
    g_string_append(buf, term);
  }

  return g_string_free(buf, FALSE);
}

/**
 * Perform a combined profile + note search against NDB and return an
 * array of result ID strings (profile:<hex> or note:<hex>).
 */
static GVariant *
do_search(const gchar *query)
{
  GVariantBuilder builder;
  g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

  if (!query || !*query) {
    return g_variant_builder_end(&builder);
  }

  void *txn = NULL;
  if (storage_ndb_begin_query(&txn, NULL) != 0 || !txn) {
    g_debug("search-provider: NDB query transaction unavailable");
    return g_variant_builder_end(&builder);
  }

  int total = 0;

  /* --- Profile search (kind 0) --- */
  {
    char **results = NULL;
    int    count   = 0;

    if (storage_ndb_search_profile(txn, query, MAX_PROFILES,
                                   &results, &count, NULL) == 0 && count > 0) {
      for (int i = 0; i < count && total < MAX_RESULTS; i++) {
        if (!results[i])
          continue;

        /* Extract pubkey from event JSON */
        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, results[i], -1, NULL)) {
          JsonNode *root = json_parser_get_root(p);
          if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "pubkey")) {
              const char *pk = json_object_get_string_member(obj, "pubkey");
              if (pk && strlen(pk) == 64) {
                g_autofree gchar *id = g_strdup_printf("profile:%s", pk);
                g_variant_builder_add(&builder, "s", id);
                total++;
              }
            }
          }
        }
        g_object_unref(p);
      }
      storage_ndb_free_results(results, count);
    }
  }

  /* --- Note search (kind 1) --- */
  {
    char config_json[128];
    snprintf(config_json, sizeof config_json,
             "{\"limit\":%d,\"kinds\":[1]}", MAX_NOTES);

    char **results = NULL;
    int    count   = 0;

    if (storage_ndb_text_search(txn, query, config_json,
                                &results, &count, NULL) == 0 && count > 0) {
      for (int i = 0; i < count && total < MAX_RESULTS; i++) {
        if (!results[i])
          continue;

        JsonParser *p = json_parser_new();
        if (json_parser_load_from_data(p, results[i], -1, NULL)) {
          JsonNode *root = json_parser_get_root(p);
          if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *obj = json_node_get_object(root);
            if (json_object_has_member(obj, "id")) {
              const char *eid = json_object_get_string_member(obj, "id");
              if (eid && strlen(eid) == 64) {
                g_autofree gchar *id = g_strdup_printf("note:%s", eid);
                g_variant_builder_add(&builder, "s", id);
                total++;
              }
            }
          }
        }
        g_object_unref(p);
      }
      storage_ndb_free_results(results, count);
    }
  }

  storage_ndb_end_query(txn);

  g_debug("search-provider: query '%s' → %d results", query, total);
  return g_variant_builder_end(&builder);
}

/**
 * Build result metadata for a profile:<hex> identifier.
 */
static void
build_profile_meta(GVariantBuilder *dict, const gchar *pubkey_hex)
{
  g_autofree gchar *id_str = g_strdup_printf("profile:%s", pubkey_hex);
  g_variant_builder_add(dict, "{sv}", "id",
                        g_variant_new_string(id_str));

  GnostrProfileMeta *meta = gnostr_profile_provider_get(pubkey_hex);
  if (meta) {
    const char *name = (meta->display_name && *meta->display_name)
                       ? meta->display_name
                       : (meta->name && *meta->name ? meta->name : NULL);
    g_variant_builder_add(dict, "{sv}", "name",
                          g_variant_new_string(name ? name : pubkey_hex));

    if (meta->about && *meta->about) {
      /* Truncate bio for the Shell overlay */
      g_autofree gchar *desc = g_strndup(meta->about, 120);
      g_variant_builder_add(dict, "{sv}", "description",
                            g_variant_new_string(desc));
    } else if (meta->nip05 && *meta->nip05) {
      g_variant_builder_add(dict, "{sv}", "description",
                            g_variant_new_string(meta->nip05));
    }

    if (meta->picture && *meta->picture) {
      /* Use GThemedIcon-compatible serialization for remote URLs */
      g_autoptr(GIcon) icon = g_icon_new_for_string(meta->picture, NULL);
      if (icon) {
        g_autofree gchar *icon_str = g_icon_to_string(icon);
        if (icon_str) {
          g_variant_builder_add(dict, "{sv}", "gicon",
                                g_variant_new_string(icon_str));
        }
      }
    }

    gnostr_profile_meta_free(meta);
  } else {
    /* No cached profile — show truncated pubkey */
    g_autofree gchar *short_pk = g_strndup(pubkey_hex, 12);
    g_autofree gchar *display  = g_strdup_printf("npub…%s", short_pk);
    g_variant_builder_add(dict, "{sv}", "name",
                          g_variant_new_string(display));
  }
}

/**
 * Build result metadata for a note:<hex> identifier.
 */
static void
build_note_meta(GVariantBuilder *dict, const gchar *event_id_hex)
{
  g_autofree gchar *id_str = g_strdup_printf("note:%s", event_id_hex);
  g_variant_builder_add(dict, "{sv}", "id",
                        g_variant_new_string(id_str));

  /* Look up the note in NDB to get content + author */
  void *txn = NULL;
  gboolean found = FALSE;

  if (storage_ndb_begin_query(&txn, NULL) == 0 && txn) {
    /* Convert hex event ID to binary for lookup */
    unsigned char id32[32];
    for (int i = 0; i < 32; i++) {
      unsigned int byte;
      if (sscanf(event_id_hex + i * 2, "%2x", &byte) != 1)
        break;
      id32[i] = (unsigned char)byte;
    }

    char *json = NULL;
    int   json_len = 0;
    if (storage_ndb_get_note_by_id(txn, id32, &json, &json_len, NULL) == 0 && json) {
      JsonParser *p = json_parser_new();
      if (json_parser_load_from_data(p, json, -1, NULL)) {
        JsonNode *root = json_parser_get_root(p);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
          JsonObject *obj = json_node_get_object(root);

          /* Content → description (truncated) */
          if (json_object_has_member(obj, "content")) {
            const char *content = json_object_get_string_member(obj, "content");
            if (content && *content) {
              g_autofree gchar *desc = g_strndup(content, 200);
              g_variant_builder_add(dict, "{sv}", "description",
                                    g_variant_new_string(desc));
            }
          }

          /* Author display name */
          if (json_object_has_member(obj, "pubkey")) {
            const char *pk = json_object_get_string_member(obj, "pubkey");
            if (pk && strlen(pk) == 64) {
              GnostrProfileMeta *meta = gnostr_profile_provider_get(pk);
              const char *author = NULL;
              if (meta) {
                author = (meta->display_name && *meta->display_name)
                         ? meta->display_name
                         : (meta->name && *meta->name ? meta->name : NULL);
              }
              g_autofree gchar *name_str = g_strdup_printf(
                  "Note by %s", author ? author : "unknown");
              g_variant_builder_add(dict, "{sv}", "name",
                                    g_variant_new_string(name_str));
              if (meta)
                gnostr_profile_meta_free(meta);
              found = TRUE;
            }
          }
        }
      }
      g_object_unref(p);
    }

    storage_ndb_end_query(txn);
  }

  if (!found) {
    g_autofree gchar *short_id = g_strndup(event_id_hex, 12);
    g_autofree gchar *display  = g_strdup_printf("Note %s…", short_id);
    g_variant_builder_add(dict, "{sv}", "name",
                          g_variant_new_string(display));
  }
}

/* ---- D-Bus method handler ---------------------------------------------- */

static void
handle_method_call(GDBusConnection       *connection,
                   const gchar           *sender,
                   const gchar           *object_path,
                   const gchar           *interface_name,
                   const gchar           *method_name,
                   GVariant              *parameters,
                   GDBusMethodInvocation *invocation,
                   gpointer               user_data)
{
  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)user_data;

  if (g_strcmp0(method_name, "GetInitialResultSet") == 0) {
    GVariant *terms = NULL;
    g_variant_get(parameters, "(@as)", &terms);
    g_autofree gchar *query = terms_to_query(terms);
    g_variant_unref(terms);

    GVariant *results = do_search(query);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new_tuple(&results, 1));
    return;
  }

  if (g_strcmp0(method_name, "GetSubsearchResultSet") == 0) {
    GVariant *prev  = NULL;
    GVariant *terms = NULL;
    g_variant_get(parameters, "(@as@as)", &prev, &terms);
    g_autofree gchar *query = terms_to_query(terms);
    g_variant_unref(prev);
    g_variant_unref(terms);

    /* Re-search the full NDB instead of filtering previous_results;
     * NDB queries are fast (<5 ms) and this avoids stale result IDs. */
    GVariant *results = do_search(query);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new_tuple(&results, 1));
    return;
  }

  if (g_strcmp0(method_name, "GetResultMetas") == 0) {
    GVariant *identifiers = NULL;
    g_variant_get(parameters, "(@as)", &identifiers);

    GVariantBuilder array;
    g_variant_builder_init(&array, G_VARIANT_TYPE("aa{sv}"));

    GVariantIter iter;
    const gchar *id;
    g_variant_iter_init(&iter, identifiers);
    while (g_variant_iter_next(&iter, "&s", &id)) {
      GVariantBuilder dict;
      g_variant_builder_init(&dict, G_VARIANT_TYPE("a{sv}"));

      if (g_str_has_prefix(id, "profile:") && strlen(id) == 8 + 64) {
        build_profile_meta(&dict, id + 8);
      } else if (g_str_has_prefix(id, "note:") && strlen(id) == 5 + 64) {
        build_note_meta(&dict, id + 5);
      } else {
        /* Unknown ID format — return minimal meta so Shell doesn't break */
        g_variant_builder_add(&dict, "{sv}", "id",
                              g_variant_new_string(id));
        g_variant_builder_add(&dict, "{sv}", "name",
                              g_variant_new_string(id));
      }

      g_variant_builder_add_value(&array, g_variant_builder_end(&dict));
    }
    g_variant_unref(identifiers);

    GVariant *metas = g_variant_builder_end(&array);
    g_dbus_method_invocation_return_value(invocation,
                                          g_variant_new_tuple(&metas, 1));
    return;
  }

  if (g_strcmp0(method_name, "ActivateResult") == 0) {
    const gchar *identifier = NULL;
    GVariant *terms  = NULL;
    guint32   timestamp = 0;
    g_variant_get(parameters, "(&s@asu)", &identifier, &terms, &timestamp);
    g_variant_unref(terms);

    g_debug("search-provider: ActivateResult '%s'", identifier);

    /* Launch the app and let it handle the URI activation.
     * Build a nostr: URI for the identified entity. */
    GApplication *app = g_application_get_default();
    if (app) {
      g_autofree gchar *uri = NULL;

      if (g_str_has_prefix(identifier, "profile:") && strlen(identifier) == 8 + 64) {
        uri = g_strdup_printf("nostr:nprofile1%s", identifier + 8);
        /* NOTE: The nprofile bech32 requires proper encoding. For now we use
         * a simpler activation: just present the app with the pubkey hint. */
        g_free(uri);
        uri = g_strdup_printf("nostr:%s", identifier + 8);
      } else if (g_str_has_prefix(identifier, "note:") && strlen(identifier) == 5 + 64) {
        uri = g_strdup_printf("nostr:%s", identifier + 5);
      }

      /* Activate the application — the main window handles URI-based navigation */
      if (uri) {
        GFile *file = g_file_new_for_uri(uri);
        GList files = { .data = file, .next = NULL, .prev = NULL };
        g_application_open(app, (GFile **)&files.data, 1, "");
        g_object_unref(file);
      } else {
        g_application_activate(app);
      }
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

  if (g_strcmp0(method_name, "LaunchSearch") == 0) {
    GVariant *terms = NULL;
    guint32   timestamp = 0;
    g_variant_get(parameters, "(@asu)", &terms, &timestamp);
    g_autofree gchar *query = terms_to_query(terms);
    g_variant_unref(terms);

    g_debug("search-provider: LaunchSearch '%s'", query);

    /* Just activate the app — the user can search inside gnostr */
    GApplication *app = g_application_get_default();
    if (app)
      g_application_activate(app);

    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

  g_dbus_method_invocation_return_error(invocation,
                                        G_DBUS_ERROR,
                                        G_DBUS_ERROR_UNKNOWN_METHOD,
                                        "Unknown method: %s", method_name);
}

static const GDBusInterfaceVTable vtable = {
  .method_call  = handle_method_call,
  .get_property = NULL,
  .set_property = NULL,
};

/* ---- Public API -------------------------------------------------------- */

guint
gnostr_shell_search_provider_register(GDBusConnection *connection,
                                      GError         **error)
{
  g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection), 0);

  if (!s_introspection) {
    s_introspection = g_dbus_node_info_new_for_xml(introspection_xml, error);
    if (!s_introspection)
      return 0;
  }

  guint id = g_dbus_connection_register_object(connection,
                                                OBJECT_PATH,
                                                s_introspection->interfaces[0],
                                                &vtable,
                                                NULL,  /* user_data */
                                                NULL,  /* free func */
                                                error);
  if (id > 0) {
    g_message("search-provider: registered SearchProvider2 at %s", OBJECT_PATH);
  }

  return id;
}

void
gnostr_shell_search_provider_unregister(GDBusConnection *connection,
                                        guint            registration_id)
{
  if (registration_id == 0)
    return;

  g_return_if_fail(G_IS_DBUS_CONNECTION(connection));
  g_dbus_connection_unregister_object(connection, registration_id);

  g_debug("search-provider: unregistered SearchProvider2");
}
