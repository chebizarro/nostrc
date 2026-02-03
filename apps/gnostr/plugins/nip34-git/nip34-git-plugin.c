/* SPDX-License-Identifier: GPL-3.0-or-later
 * nip34-git-plugin.c - NIP-34 Git Repository Plugin
 *
 * Implements NIP-34 (Git Stuff) for Nostr-based git repository integration.
 * Provides repository browser and graphical git client functionality.
 *
 * Event kinds handled:
 * - 30617: Repository announcement (addressable)
 * - 1617: Patches
 * - 1621: Issues
 * - 1622: Issue replies / Status changes
 *
 * Copyright (C) 2026 Gnostr Contributors
 */

#include "nip34-git-plugin.h"
#include <gnostr-plugin-api.h>
#include <libpeas.h>
#include <json-glib/json-glib.h>

#ifdef HAVE_LIBGIT2
#include "gnostr-git-client.h"
#endif

/* NIP-34 Event Kinds */
#define NIP34_KIND_REPOSITORY     30617  /* Addressable: repository announcement */
#define NIP34_KIND_PATCH          1617   /* Patch proposal */
#define NIP34_KIND_ISSUE          1621   /* Issue */
#define NIP34_KIND_ISSUE_REPLY    1622   /* Issue reply / status change */

struct _Nip34GitPlugin
{
  GObject parent_instance;

  GnostrPluginContext *context;
  gboolean active;

  /* Repository cache */
  GHashTable *repositories;     /* d-tag -> RepoInfo */

  /* Subscriptions */
  guint64 repo_subscription;    /* Subscription for repository events */
  guint64 patch_subscription;   /* Subscription for patch events */

  /* UI state */
  gboolean browser_visible;
  gboolean client_visible;

#ifdef HAVE_LIBGIT2
  /* Git client window (lazily created) */
  GtkWindow *git_client_window;
#endif
};

/* Repository metadata */
typedef struct {
  gchar *id;                    /* Event ID */
  gchar *d_tag;                 /* Unique identifier (d tag) */
  gchar *name;                  /* Repository name */
  gchar *description;           /* Repository description */
  gchar *clone_url;             /* Git clone URL */
  gchar **maintainers;          /* Array of maintainer pubkeys */
  gchar **relays;               /* Preferred relays for this repo */
  gchar *web_url;               /* Optional web interface URL */
  gint64 created_at;            /* Creation timestamp */
  gint64 updated_at;            /* Last update timestamp */
} RepoInfo;

/* Implement GnostrPlugin interface */
static void gnostr_plugin_iface_init(GnostrPluginInterface *iface);

/* Implement GnostrEventHandler interface */
static void gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface);

/* Implement GnostrUIExtension interface */
static void gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface);

G_DEFINE_TYPE_WITH_CODE(Nip34GitPlugin, nip34_git_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_EVENT_HANDLER, gnostr_event_handler_iface_init)
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_UI_EXTENSION, gnostr_ui_extension_iface_init))

static void
repo_info_free(gpointer data)
{
  RepoInfo *info = (RepoInfo *)data;
  if (!info) return;

  g_free(info->id);
  g_free(info->d_tag);
  g_free(info->name);
  g_free(info->description);
  g_free(info->clone_url);
  g_free(info->web_url);
  g_strfreev(info->maintainers);
  g_strfreev(info->relays);
  g_free(info);
}

static void
nip34_git_plugin_dispose(GObject *object)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(object);

  g_clear_pointer(&self->repositories, g_hash_table_unref);

  G_OBJECT_CLASS(nip34_git_plugin_parent_class)->dispose(object);
}

static void
nip34_git_plugin_class_init(Nip34GitPluginClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS(klass);
  object_class->dispose = nip34_git_plugin_dispose;
}

static void
nip34_git_plugin_init(Nip34GitPlugin *self)
{
  self->repositories = g_hash_table_new_full(g_str_hash, g_str_equal,
                                              g_free, repo_info_free);
  self->browser_visible = FALSE;
  self->client_visible = FALSE;
}

/* ============================================================================
 * Repository parsing
 * ============================================================================ */

static RepoInfo *
parse_repository_event(const gchar *event_json)
{
  if (!event_json || *event_json == '\0')
    return NULL;

  JsonParser *parser = json_parser_new();
  GError *error = NULL;

  if (!json_parser_load_from_data(parser, event_json, -1, &error))
    {
      g_warning("[NIP-34] Failed to parse event JSON: %s", error->message);
      g_error_free(error);
      g_object_unref(parser);
      return NULL;
    }

  JsonNode *root = json_parser_get_root(parser);
  if (!JSON_NODE_HOLDS_OBJECT(root))
    {
      g_object_unref(parser);
      return NULL;
    }

  JsonObject *event = json_node_get_object(root);
  RepoInfo *info = g_new0(RepoInfo, 1);

  /* Get event ID */
  if (json_object_has_member(event, "id"))
    info->id = g_strdup(json_object_get_string_member(event, "id"));

  /* Get created_at timestamp */
  if (json_object_has_member(event, "created_at"))
    info->created_at = json_object_get_int_member(event, "created_at");

  /* Parse tags array */
  if (!json_object_has_member(event, "tags"))
    {
      g_object_unref(parser);
      repo_info_free(info);
      return NULL;
    }

  JsonArray *tags = json_object_get_array_member(event, "tags");
  guint n_tags = json_array_get_length(tags);

  GPtrArray *maintainers_list = g_ptr_array_new();
  GPtrArray *relays_list = g_ptr_array_new();

  for (guint i = 0; i < n_tags; i++)
    {
      JsonArray *tag = json_array_get_array_element(tags, i);
      if (!tag || json_array_get_length(tag) < 2)
        continue;

      const gchar *tag_name = json_array_get_string_element(tag, 0);
      const gchar *tag_value = json_array_get_string_element(tag, 1);

      if (!tag_name || !tag_value)
        continue;

      if (g_strcmp0(tag_name, "d") == 0)
        {
          g_free(info->d_tag);
          info->d_tag = g_strdup(tag_value);
        }
      else if (g_strcmp0(tag_name, "name") == 0)
        {
          g_free(info->name);
          info->name = g_strdup(tag_value);
        }
      else if (g_strcmp0(tag_name, "description") == 0)
        {
          g_free(info->description);
          info->description = g_strdup(tag_value);
        }
      else if (g_strcmp0(tag_name, "clone") == 0)
        {
          g_free(info->clone_url);
          info->clone_url = g_strdup(tag_value);
        }
      else if (g_strcmp0(tag_name, "web") == 0)
        {
          g_free(info->web_url);
          info->web_url = g_strdup(tag_value);
        }
      else if (g_strcmp0(tag_name, "maintainers") == 0 ||
               g_strcmp0(tag_name, "p") == 0)
        {
          /* Collect all maintainer pubkeys from this tag */
          for (guint j = 1; j < json_array_get_length(tag); j++)
            {
              const gchar *pubkey = json_array_get_string_element(tag, j);
              if (pubkey && *pubkey)
                g_ptr_array_add(maintainers_list, g_strdup(pubkey));
            }
        }
      else if (g_strcmp0(tag_name, "relays") == 0 ||
               g_strcmp0(tag_name, "relay") == 0)
        {
          for (guint j = 1; j < json_array_get_length(tag); j++)
            {
              const gchar *relay = json_array_get_string_element(tag, j);
              if (relay && *relay)
                g_ptr_array_add(relays_list, g_strdup(relay));
            }
        }
    }

  /* Convert arrays to NULL-terminated string arrays */
  if (maintainers_list->len > 0)
    {
      g_ptr_array_add(maintainers_list, NULL);
      info->maintainers = (gchar **)g_ptr_array_free(maintainers_list, FALSE);
    }
  else
    {
      g_ptr_array_free(maintainers_list, TRUE);
    }

  if (relays_list->len > 0)
    {
      g_ptr_array_add(relays_list, NULL);
      info->relays = (gchar **)g_ptr_array_free(relays_list, FALSE);
    }
  else
    {
      g_ptr_array_free(relays_list, TRUE);
    }

  info->updated_at = info->created_at;

  g_object_unref(parser);

  /* Must have at least a d-tag to be valid */
  if (!info->d_tag)
    {
      repo_info_free(info);
      return NULL;
    }

  return info;
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

/* Push a repository to the main browser UI */
static void
push_repo_to_browser(Nip34GitPlugin *self, RepoInfo *info)
{
  if (!self->context || !info)
    return;

  /* Get first maintainer pubkey if available */
  const char *maintainer = (info->maintainers && info->maintainers[0])
                             ? info->maintainers[0] : NULL;

  gnostr_plugin_context_add_repository(self->context,
                                       info->d_tag,
                                       info->name,
                                       info->description,
                                       info->clone_url,
                                       info->web_url,
                                       maintainer,
                                       info->updated_at);
}

/* Callback for repository subscription events */
static void
on_repository_event(const char *event_json, gpointer user_data)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(user_data);

  g_debug("[NIP-34] Received repository event from subscription");

  if (!self->active || !event_json)
    {
      g_debug("[NIP-34] Ignoring event (active=%d, json=%s)",
              self->active, event_json ? "yes" : "NULL");
      return;
    }

  RepoInfo *info = parse_repository_event(event_json);
  if (info && info->d_tag)
    {
      g_hash_table_replace(self->repositories,
                           g_strdup(info->d_tag), info);
      g_debug("[NIP-34] Subscription: cached repository %s (%s)",
              info->name ? info->name : "(unnamed)", info->d_tag);

      /* Push to browser UI */
      push_repo_to_browser(self, info);
    }
  else
    {
      g_debug("[NIP-34] Failed to parse repository event");
      repo_info_free(info);
    }
}

/* Load cached repositories from plugin storage */
static void
load_cached_repositories(Nip34GitPlugin *self, GnostrPluginContext *context)
{
  GError *error = NULL;
  GBytes *data = gnostr_plugin_context_load_data(context, "repositories", &error);

  if (error)
    {
      g_debug("[NIP-34] No cached repositories: %s", error->message);
      g_error_free(error);
      return;
    }

  if (!data)
    return;

  gsize size;
  const char *json = g_bytes_get_data(data, &size);
  if (!json || size == 0)
    {
      g_bytes_unref(data);
      return;
    }

  /* Parse JSON array of repository events */
  JsonParser *parser = json_parser_new();
  if (json_parser_load_from_data(parser, json, size, NULL))
    {
      JsonNode *root = json_parser_get_root(parser);
      if (JSON_NODE_HOLDS_ARRAY(root))
        {
          JsonArray *arr = json_node_get_array(root);
          guint len = json_array_get_length(arr);
          for (guint i = 0; i < len; i++)
            {
              JsonNode *node = json_array_get_element(arr, i);
              if (JSON_NODE_HOLDS_OBJECT(node))
                {
                  JsonGenerator *gen = json_generator_new();
                  json_generator_set_root(gen, node);
                  char *event_json = json_generator_to_data(gen, NULL);
                  g_object_unref(gen);

                  RepoInfo *info = parse_repository_event(event_json);
                  if (info && info->d_tag)
                    {
                      g_hash_table_replace(self->repositories,
                                           g_strdup(info->d_tag), info);
                      /* Push to browser UI */
                      push_repo_to_browser(self, info);
                    }
                  else
                    {
                      repo_info_free(info);
                    }
                  g_free(event_json);
                }
            }
          g_debug("[NIP-34] Loaded %u cached repositories",
                  g_hash_table_size(self->repositories));
        }
    }
  g_object_unref(parser);
  g_bytes_unref(data);
}

/* Callback for relay events request completion */
static void
on_refresh_relay_events_done(GObject      *source,
                             GAsyncResult *res,
                             gpointer      user_data)
{
  (void)source;
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(user_data);
  GError *error = NULL;

  gboolean ok = gnostr_plugin_context_request_relay_events_finish(
      self->context, res, &error);

  if (!ok) {
    g_debug("[NIP-34] Relay refresh failed: %s",
            error ? error->message : "unknown");
    g_clear_error(&error);
  } else {
    g_debug("[NIP-34] Relay refresh completed - querying nostrdb for results");

    /* Query nostrdb for all repository events and push to browser.
     * This is more reliable than waiting for subscription callbacks. */
    const char *query_filter = "{\"kinds\":[30617],\"limit\":500}";
    GPtrArray *events = gnostr_plugin_context_query_events(self->context, query_filter, &error);
    if (error) {
      g_debug("[NIP-34] Post-refresh query failed: %s", error->message);
      g_error_free(error);
    } else if (events && events->len > 0) {
      g_debug("[NIP-34] Found %u repository events after refresh", events->len);

      /* Clear existing repos and repopulate */
      gnostr_plugin_context_clear_repositories(self->context);
      g_hash_table_remove_all(self->repositories);

      for (guint i = 0; i < events->len; i++) {
        const char *event_json = g_ptr_array_index(events, i);
        if (event_json) {
          RepoInfo *info = parse_repository_event(event_json);
          if (info && info->d_tag) {
            g_hash_table_replace(self->repositories, g_strdup(info->d_tag), info);
            push_repo_to_browser(self, info);
          } else {
            repo_info_free(info);
          }
        }
      }
      g_ptr_array_unref(events);
    } else {
      g_debug("[NIP-34] No repository events found after refresh");
    }
  }

  g_object_unref(self);
}

/* Action handler for "refresh" action - fetches NIP-34 events from relays */
static void
on_refresh_action(GnostrPluginContext *context,
                  const char          *action_name,
                  GVariant            *parameter,
                  gpointer             user_data)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(user_data);
  (void)action_name;
  (void)parameter;

  g_debug("[NIP-34] Refresh action triggered - fetching from relays");

  /* NIP-34 event kinds to fetch */
  int kinds[] = {
    NIP34_KIND_REPOSITORY,   /* 30617 */
    NIP34_KIND_PATCH,        /* 1617 */
    NIP34_KIND_ISSUE,        /* 1621 */
    NIP34_KIND_ISSUE_REPLY   /* 1622 */
  };

  /* Request events from relays - they will be ingested to nostrdb,
   * which will trigger our subscription callbacks */
  gnostr_plugin_context_request_relay_events_async(
      context,
      kinds,
      G_N_ELEMENTS(kinds),
      100,  /* limit */
      NULL, /* cancellable */
      on_refresh_relay_events_done,
      g_object_ref(self));
}

#ifdef HAVE_LIBGIT2
/* Action handler for "open-git-client" action */
static void
on_open_git_client_action(GnostrPluginContext *context,
                           const char          *action_name,
                           GVariant            *parameter,
                           gpointer             user_data)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(user_data);
  (void)action_name;

  /* Get clone URL from parameter if provided */
  const char *clone_url = NULL;
  if (parameter && g_variant_is_of_type(parameter, G_VARIANT_TYPE_STRING)) {
    clone_url = g_variant_get_string(parameter, NULL);
  }

  /* Create window if needed */
  if (!self->git_client_window) {
    GtkWindow *parent = gnostr_plugin_context_get_main_window(context);

    self->git_client_window = GTK_WINDOW(gtk_window_new());
    gtk_window_set_title(self->git_client_window, "Git Client");
    gtk_window_set_default_size(self->git_client_window, 800, 600);
    if (parent) {
      gtk_window_set_transient_for(self->git_client_window, parent);
    }

    /* Create git client widget */
    GnostrGitClient *client = gnostr_git_client_new();
    gtk_window_set_child(self->git_client_window, GTK_WIDGET(client));

    /* Store client reference for later access */
    g_object_set_data(G_OBJECT(self->git_client_window), "git-client", client);
  }

  /* If clone URL provided, initiate clone */
  if (clone_url && *clone_url) {
    GnostrGitClient *client = g_object_get_data(
        G_OBJECT(self->git_client_window), "git-client");
    if (client) {
      /* For now, show the window and let user choose where to clone.
       * A proper implementation would show a file chooser dialog. */
      g_debug("[NIP-34] Clone requested for: %s", clone_url);

      /* Show a message about the clone URL */
      GtkWindow *parent = gnostr_plugin_context_get_main_window(context);
      if (parent) {
        /* Copy to clipboard as a temporary solution */
        GdkClipboard *clipboard = gdk_display_get_clipboard(
            gtk_widget_get_display(GTK_WIDGET(parent)));
        gdk_clipboard_set_text(clipboard, clone_url);
      }
    }
  }

  gtk_window_present(self->git_client_window);
  g_debug("[NIP-34] Git client window presented");
}
#endif

static void
nip34_git_plugin_activate(GnostrPlugin        *plugin,
                          GnostrPluginContext *context)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(plugin);

  g_debug("[NIP-34] Activating Git Repository plugin");

  self->context = context;
  self->active = TRUE;

  /* Subscribe to repository announcement events */
  const char *repo_filter = "{\"kinds\":[30617]}";
  self->repo_subscription = gnostr_plugin_context_subscribe_events(
      context, repo_filter, G_CALLBACK(on_repository_event), self, NULL);

  if (self->repo_subscription > 0)
    g_debug("[NIP-34] Subscribed to repository events (id=%lu)",
            (unsigned long)self->repo_subscription);

  /* Load cached repository list from plugin storage */
  load_cached_repositories(self, context);

  /* Query existing repositories in the database */
  GError *error = NULL;
  const char *query_filter = "{\"kinds\":[30617],\"limit\":100}";
  GPtrArray *events = gnostr_plugin_context_query_events(context, query_filter, &error);
  if (error) {
    g_debug("[NIP-34] Initial query failed: %s", error->message);
    g_error_free(error);
  } else if (events && events->len > 0) {
    g_debug("[NIP-34] Found %u existing repository events", events->len);
    for (guint i = 0; i < events->len; i++) {
      const char *event_json = g_ptr_array_index(events, i);
      if (event_json) {
        RepoInfo *info = parse_repository_event(event_json);
        if (info && info->d_tag) {
          g_hash_table_replace(self->repositories, g_strdup(info->d_tag), info);
          push_repo_to_browser(self, info);
        } else {
          repo_info_free(info);
        }
      }
    }
    g_ptr_array_unref(events);
  } else {
    g_debug("[NIP-34] No existing repositories in database");
  }

  /* Register refresh action for on-demand relay fetching */
  gnostr_plugin_context_register_action(context, "nip34-refresh",
                                         on_refresh_action, self);
  g_debug("[NIP-34] Registered 'nip34-refresh' action");

  /* Auto-fetch from relays on startup if no local repos found */
  if (g_hash_table_size(self->repositories) == 0) {
    g_debug("[NIP-34] No local repos - auto-fetching from relays");
    on_refresh_action(context, "nip34-refresh", NULL, self);
  }

#ifdef HAVE_LIBGIT2
  /* Register action handler for git client */
  gnostr_plugin_context_register_action(context, "open-git-client",
                                         on_open_git_client_action, self);
  g_debug("[NIP-34] Registered 'open-git-client' action");
#endif
}

/* Save repositories to plugin storage for persistence */
static void
save_cached_repositories(Nip34GitPlugin *self, GnostrPluginContext *context)
{
  if (g_hash_table_size(self->repositories) == 0)
    return;

  /* Build JSON array of repository d-tags for cache */
  JsonBuilder *builder = json_builder_new();
  json_builder_begin_array(builder);

  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, self->repositories);
  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      RepoInfo *info = value;
      if (!info)
        continue;

      json_builder_begin_object(builder);
      if (info->id)
        {
          json_builder_set_member_name(builder, "id");
          json_builder_add_string_value(builder, info->id);
        }
      if (info->d_tag)
        {
          json_builder_set_member_name(builder, "d_tag");
          json_builder_add_string_value(builder, info->d_tag);
        }
      if (info->name)
        {
          json_builder_set_member_name(builder, "name");
          json_builder_add_string_value(builder, info->name);
        }
      if (info->clone_url)
        {
          json_builder_set_member_name(builder, "clone_url");
          json_builder_add_string_value(builder, info->clone_url);
        }
      if (info->description)
        {
          json_builder_set_member_name(builder, "description");
          json_builder_add_string_value(builder, info->description);
        }
      json_builder_end_object(builder);
    }

  json_builder_end_array(builder);

  JsonGenerator *gen = json_generator_new();
  JsonNode *root = json_builder_get_root(builder);
  json_generator_set_root(gen, root);
  gsize len;
  char *json = json_generator_to_data(gen, &len);

  GBytes *data = g_bytes_new_take(json, len);
  GError *error = NULL;
  if (!gnostr_plugin_context_store_data(context, "repositories", data, &error))
    {
      g_warning("[NIP-34] Failed to save repository cache: %s",
                error ? error->message : "unknown error");
      g_clear_error(&error);
    }
  else
    {
      g_debug("[NIP-34] Saved %u repositories to cache",
              g_hash_table_size(self->repositories));
    }

  g_bytes_unref(data);
  json_node_unref(root);
  g_object_unref(gen);
  g_object_unref(builder);
}

static void
nip34_git_plugin_deactivate(GnostrPlugin        *plugin,
                            GnostrPluginContext *context)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(plugin);

  g_debug("[NIP-34] Deactivating Git Repository plugin");

  /* Save cached repositories before deactivation */
  if (context)
    save_cached_repositories(self, context);

  /* Cancel subscriptions */
  if (self->repo_subscription > 0 && context)
    {
      gnostr_plugin_context_unsubscribe_events(context, self->repo_subscription);
      self->repo_subscription = 0;
    }
  if (self->patch_subscription > 0 && context)
    {
      gnostr_plugin_context_unsubscribe_events(context, self->patch_subscription);
      self->patch_subscription = 0;
    }

  /* Clear cached data */
  g_hash_table_remove_all(self->repositories);

#ifdef HAVE_LIBGIT2
  /* Unregister action handler */
  if (context) {
    gnostr_plugin_context_unregister_action(context, "open-git-client");
  }

  /* Close git client window if open */
  if (self->git_client_window) {
    gtk_window_destroy(self->git_client_window);
    self->git_client_window = NULL;
  }
#endif

  self->active = FALSE;
  self->context = NULL;
}

static const char *
nip34_git_plugin_get_name(GnostrPlugin *plugin)
{
  (void)plugin;
  return "NIP-34 Git Repositories";
}

static const char *
nip34_git_plugin_get_description(GnostrPlugin *plugin)
{
  (void)plugin;
  return "Git repository browser and client for Nostr-based git collaboration";
}

static const char *const *
nip34_git_plugin_get_authors(GnostrPlugin *plugin)
{
  static const char *authors[] = { "Gnostr Contributors", NULL };
  (void)plugin;
  return authors;
}

static const char *
nip34_git_plugin_get_version(GnostrPlugin *plugin)
{
  (void)plugin;
  return "1.0";
}

static const int *
nip34_git_plugin_get_supported_kinds(GnostrPlugin *plugin, gsize *n_kinds)
{
  static const int kinds[] = {
    NIP34_KIND_REPOSITORY,
    NIP34_KIND_PATCH,
    NIP34_KIND_ISSUE,
    NIP34_KIND_ISSUE_REPLY
  };
  (void)plugin;
  if (n_kinds) *n_kinds = G_N_ELEMENTS(kinds);
  return kinds;
}

static void
gnostr_plugin_iface_init(GnostrPluginInterface *iface)
{
  iface->activate = nip34_git_plugin_activate;
  iface->deactivate = nip34_git_plugin_deactivate;
  iface->get_name = nip34_git_plugin_get_name;
  iface->get_description = nip34_git_plugin_get_description;
  iface->get_authors = nip34_git_plugin_get_authors;
  iface->get_version = nip34_git_plugin_get_version;
  iface->get_supported_kinds = nip34_git_plugin_get_supported_kinds;
}

/* ============================================================================
 * GnostrEventHandler interface implementation
 * ============================================================================ */

static gboolean
nip34_git_plugin_handle_event(GnostrEventHandler  *handler,
                              GnostrPluginContext *context,
                              GnostrPluginEvent   *event)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(handler);
  (void)context;

  if (!self->active)
    return FALSE;

  int kind = gnostr_plugin_event_get_kind(event);

  switch (kind)
    {
    case NIP34_KIND_REPOSITORY:
      {
        /* Parse and cache repository announcement */
        char *json = gnostr_plugin_event_to_json(event);
        RepoInfo *info = parse_repository_event(json);
        if (info && info->d_tag)
          {
            g_hash_table_replace(self->repositories,
                                 g_strdup(info->d_tag), info);
            g_debug("[NIP-34] Cached repository: %s", info->name ? info->name : info->d_tag);
            /* Push to browser UI */
            push_repo_to_browser(self, info);
          }
        else
          {
            repo_info_free(info);
          }
        g_free(json);
        return TRUE;
      }

    case NIP34_KIND_PATCH:
      g_debug("[NIP-34] Received patch event");
      return TRUE;

    case NIP34_KIND_ISSUE:
      g_debug("[NIP-34] Received issue event");
      return TRUE;

    case NIP34_KIND_ISSUE_REPLY:
      g_debug("[NIP-34] Received issue reply event");
      return TRUE;

    default:
      return FALSE;
    }
}

static gboolean
nip34_git_plugin_can_handle_kind(GnostrEventHandler *handler, int kind)
{
  (void)handler;
  return kind == NIP34_KIND_REPOSITORY ||
         kind == NIP34_KIND_PATCH ||
         kind == NIP34_KIND_ISSUE ||
         kind == NIP34_KIND_ISSUE_REPLY;
}

static void
gnostr_event_handler_iface_init(GnostrEventHandlerInterface *iface)
{
  iface->handle_event = nip34_git_plugin_handle_event;
  iface->can_handle_kind = nip34_git_plugin_can_handle_kind;
}

/* ============================================================================
 * GnostrUIExtension interface implementation - Settings page
 * ============================================================================ */

typedef struct {
  Nip34GitPlugin *plugin;
  GtkListBox *repo_list;
  GtkLabel *status_label;
} SettingsPageData;

static void
settings_page_data_free(SettingsPageData *data)
{
  g_slice_free(SettingsPageData, data);
}

static void
update_repo_list(SettingsPageData *data)
{
  /* Clear existing items */
  GtkWidget *child;
  while ((child = gtk_widget_get_first_child(GTK_WIDGET(data->repo_list))) != NULL)
    gtk_list_box_remove(data->repo_list, child);

  guint n_repos = g_hash_table_size(data->plugin->repositories);

  if (n_repos == 0)
    {
      gtk_label_set_text(data->status_label, "No repositories found");
      return;
    }

  g_autofree char *status = g_strdup_printf("%u repositories", n_repos);
  gtk_label_set_text(data->status_label, status);

  /* Add repository rows */
  GHashTableIter iter;
  gpointer key, value;
  g_hash_table_iter_init(&iter, data->plugin->repositories);

  while (g_hash_table_iter_next(&iter, &key, &value))
    {
      RepoInfo *info = (RepoInfo *)value;

      GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_widget_set_margin_start(row, 12);
      gtk_widget_set_margin_end(row, 12);
      gtk_widget_set_margin_top(row, 8);
      gtk_widget_set_margin_bottom(row, 8);

      GtkWidget *name_label = gtk_label_new(info->name ? info->name : info->d_tag);
      gtk_widget_add_css_class(name_label, "heading");
      gtk_widget_set_halign(name_label, GTK_ALIGN_START);
      gtk_box_append(GTK_BOX(row), name_label);

      if (info->description)
        {
          GtkWidget *desc_label = gtk_label_new(info->description);
          gtk_widget_add_css_class(desc_label, "dim-label");
          gtk_label_set_ellipsize(GTK_LABEL(desc_label), PANGO_ELLIPSIZE_END);
          gtk_widget_set_halign(desc_label, GTK_ALIGN_START);
          gtk_box_append(GTK_BOX(row), desc_label);
        }

      if (info->clone_url)
        {
          GtkWidget *url_label = gtk_label_new(info->clone_url);
          gtk_widget_add_css_class(url_label, "monospace");
          gtk_widget_add_css_class(url_label, "dim-label");
          gtk_label_set_selectable(GTK_LABEL(url_label), TRUE);
          gtk_widget_set_halign(url_label, GTK_ALIGN_START);
          gtk_box_append(GTK_BOX(row), url_label);
        }

      gtk_list_box_append(data->repo_list, row);
    }
}

static void
on_refresh_button_clicked(GtkButton *button G_GNUC_UNUSED, gpointer user_data)
{
  SettingsPageData *data = user_data;

  gtk_label_set_text(data->status_label, "Refreshing...");
  g_debug("[NIP-34] Refresh button clicked");

  /* Query local storage for repository events */
  if (data->plugin && data->plugin->context)
    {
      const char *filter = "{\"kinds\":[30617],\"limit\":100}";
      g_debug("[NIP-34] Querying local storage with filter: %s", filter);

      GError *error = NULL;
      GPtrArray *events = gnostr_plugin_context_query_events(
          data->plugin->context, filter, &error);

      if (error)
        {
          g_warning("[NIP-34] Query failed: %s", error->message);
          gtk_label_set_text(data->status_label, "Query failed");
          g_error_free(error);
        }
      else if (events)
        {
          g_debug("[NIP-34] Query returned %u events", events->len);

          if (events->len == 0) {
            gtk_label_set_text(data->status_label,
                "No repository events in local storage");
          }

          for (guint i = 0; i < events->len; i++)
            {
              const char *event_json = g_ptr_array_index(events, i);
              RepoInfo *info = parse_repository_event(event_json);
              if (info && info->d_tag)
                {
                  g_debug("[NIP-34] Found repo: %s", info->name ? info->name : info->d_tag);
                  g_hash_table_replace(data->plugin->repositories,
                                       g_strdup(info->d_tag), info);
                  /* Push to browser UI */
                  push_repo_to_browser(data->plugin, info);
                }
              else
                {
                  repo_info_free(info);
                }
            }
          g_debug("[NIP-34] Refreshed %u repositories from storage",
                  g_hash_table_size(data->plugin->repositories));
          g_ptr_array_unref(events);
        }
      else
        {
          g_debug("[NIP-34] Query returned NULL (no events)");
          gtk_label_set_text(data->status_label, "No repositories found");
        }
    }
  else
    {
      g_warning("[NIP-34] Plugin or context is NULL");
      gtk_label_set_text(data->status_label, "Plugin not initialized");
    }

  update_repo_list(data);
}

static GtkWidget *
nip34_git_plugin_create_settings_page(GnostrUIExtension   *extension,
                                      GnostrPluginContext *context G_GNUC_UNUSED)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(extension);

  /* Create settings page container */
  GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  gtk_widget_set_margin_start(page, 18);
  gtk_widget_set_margin_end(page, 18);
  gtk_widget_set_margin_top(page, 18);
  gtk_widget_set_margin_bottom(page, 18);

  /* Title */
  GtkWidget *title = gtk_label_new("Git Repositories (NIP-34)");
  gtk_widget_add_css_class(title, "title-2");
  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(page), title);

  /* Description */
  GtkWidget *desc = gtk_label_new(
    "Browse and collaborate on git repositories published to Nostr relays. "
    "Repositories appear here when announced via kind 30617 events.");
  gtk_label_set_wrap(GTK_LABEL(desc), TRUE);
  gtk_label_set_xalign(GTK_LABEL(desc), 0);
  gtk_widget_add_css_class(desc, "dim-label");
  gtk_box_append(GTK_BOX(page), desc);

  /* Status and refresh */
  GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  gtk_widget_set_margin_top(header_box, 12);

  GtkWidget *status_label = gtk_label_new("Loading...");
  gtk_widget_set_hexpand(status_label, TRUE);
  gtk_widget_set_halign(status_label, GTK_ALIGN_START);
  gtk_box_append(GTK_BOX(header_box), status_label);

  GtkWidget *refresh_button = gtk_button_new_from_icon_name("view-refresh-symbolic");
  gtk_widget_set_tooltip_text(refresh_button, "Refresh repositories");
  gtk_box_append(GTK_BOX(header_box), refresh_button);

  gtk_box_append(GTK_BOX(page), header_box);

  /* Repository list */
  GtkWidget *scrolled = gtk_scrolled_window_new();
  gtk_scrolled_window_set_min_content_height(GTK_SCROLLED_WINDOW(scrolled), 300);
  gtk_widget_set_vexpand(scrolled, TRUE);

  GtkWidget *repo_list = gtk_list_box_new();
  gtk_list_box_set_selection_mode(GTK_LIST_BOX(repo_list), GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class(repo_list, "boxed-list");
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), repo_list);

  gtk_box_append(GTK_BOX(page), scrolled);

  /* Placeholder for empty state */
  GtkWidget *placeholder = gtk_label_new("No repositories found.\nClick refresh to search relays.");
  gtk_widget_add_css_class(placeholder, "dim-label");
  gtk_list_box_set_placeholder(GTK_LIST_BOX(repo_list), placeholder);

  /* Setup data and signals */
  SettingsPageData *data = g_slice_new0(SettingsPageData);
  data->plugin = self;
  data->repo_list = GTK_LIST_BOX(repo_list);
  data->status_label = GTK_LABEL(status_label);

  g_signal_connect_data(refresh_button, "clicked",
                        G_CALLBACK(on_refresh_button_clicked), data,
                        (GClosureNotify)settings_page_data_free, 0);

  /* Initial list update */
  update_repo_list(data);

  return page;
}

static void
gnostr_ui_extension_iface_init(GnostrUIExtensionInterface *iface)
{
  iface->create_settings_page = nip34_git_plugin_create_settings_page;
}

/* ============================================================================
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP34_TYPE_GIT_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_EVENT_HANDLER,
                                              NIP34_TYPE_GIT_PLUGIN);
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_UI_EXTENSION,
                                              NIP34_TYPE_GIT_PLUGIN);
}
