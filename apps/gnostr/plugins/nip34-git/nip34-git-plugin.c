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

G_DEFINE_TYPE_WITH_CODE(Nip34GitPlugin, nip34_git_plugin, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(GNOSTR_TYPE_PLUGIN, gnostr_plugin_iface_init))

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
  /* TODO: Parse kind 30617 event to extract repository metadata
   *
   * Expected tags:
   * - ["d", "<unique-identifier>"]
   * - ["name", "<repo-name>"]
   * - ["description", "<description>"]
   * - ["clone", "<git-clone-url>"]
   * - ["web", "<web-url>"] (optional)
   * - ["maintainers", "<pubkey>", ...] (optional)
   * - ["relays", "<relay-url>", ...] (optional)
   * - ["r", "<reference>", ...] (optional, HEAD commit, etc.)
   */
  (void)event_json;
  return NULL;
}

/* ============================================================================
 * GnostrPlugin interface implementation
 * ============================================================================ */

static void
nip34_git_plugin_activate(GnostrPlugin        *plugin,
                          GnostrPluginContext *context)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(plugin);

  g_debug("[NIP-34] Activating Git Repository plugin");

  self->context = context;
  self->active = TRUE;

  /* TODO: Subscribe to repository announcement events
   * Filter: {"kinds": [30617], "authors": [followed_users]}
   */

  /* TODO: Register UI menu items
   * - "Repositories" in sidebar
   * - "Git Client" in tools menu
   */

  /* TODO: Load cached repository list from plugin storage */
}

static void
nip34_git_plugin_deactivate(GnostrPlugin        *plugin,
                            GnostrPluginContext *context)
{
  Nip34GitPlugin *self = NIP34_GIT_PLUGIN(plugin);
  (void)context;

  g_debug("[NIP-34] Deactivating Git Repository plugin");

  /* TODO: Cancel subscriptions when API is implemented */
  self->repo_subscription = 0;
  self->patch_subscription = 0;

  /* Clear cached data */
  g_hash_table_remove_all(self->repositories);

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
 * Plugin registration for libpeas
 * ============================================================================ */

G_MODULE_EXPORT void
peas_register_types(PeasObjectModule *module)
{
  peas_object_module_register_extension_type(module,
                                              GNOSTR_TYPE_PLUGIN,
                                              NIP34_TYPE_GIT_PLUGIN);
}
