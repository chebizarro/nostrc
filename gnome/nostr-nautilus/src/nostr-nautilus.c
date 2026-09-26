/* nostr-nautilus.c - GNOME Files context-menu items for Nostr (nostrc-xlf3)
 *
 * SPDX-License-Identifier: MIT
 *
 * A NautilusMenuProvider that offers
 *
 *   Share to Nostr…              → nostr-share FILE…
 *   Upload to Blossom…           → nostr-share --kind 1063 FILE…
 *   Encrypt for Nostr Contact…   → nostr-nautilus-seal FILE… (npub entry)
 *                                   → nostr-seal encrypt --to … FILE
 *   Decrypt…                     → nostr-seal-gtk FILE…  (else nostr-seal decrypt FILE)
 *   Share This Repository…       → nostr-share DIR  (folder background, git repos)
 *
 * The provider only looks at what Nautilus already knows (MIME type, file
 * type, location) plus a stat() or two for .git and PATH lookups: no
 * network, no signer, no file reads. The tools do all UI and signing.
 *
 * TODO(nostrc-tepd): once nostr-share records published files in the xattr
 * user.nostr.event, add a NautilusInfoProvider that maps it to a
 * "nostr-published" emblem. nostr-share writes no xattr today.
 */
#define G_LOG_DOMAIN "nostr-nautilus"

#include <nautilus-extension.h>
#include <gio/gdesktopappinfo.h>
#include <string.h>

#include "nn-core.h"
#include "nn-launch.h"

#ifndef NN_SEAL_HELPER_PATH
#define NN_SEAL_HELPER_PATH "/usr/libexec/nostr-nautilus-seal"
#endif
#define NN_SHARE_DESKTOP_ID "org.nostr.Share.desktop"

/* --- Tool discovery (per menu request: PATH lookups + GLib's cached
 *     desktop-file index, so installing a tool needs no Files restart) --- */

typedef struct {
  NnTools          tools;
  GDesktopAppInfo *share_app;
  gchar           *share_exe;
  gchar           *seal_exe;
  gchar           *seal_gtk_exe;
  gchar           *seal_helper;
} NnToolbox;

static void
nn_toolbox_init(NnToolbox *tb)
{
  memset(tb, 0, sizeof *tb);

  tb->share_app = g_desktop_app_info_new(NN_SHARE_DESKTOP_ID);
  if (tb->share_app != NULL) {
    const gchar *exe = g_app_info_get_executable(G_APP_INFO(tb->share_app));
    tb->share_exe = exe ? g_find_program_in_path(exe) : NULL;
    tb->tools.share_mimes = g_app_info_get_supported_types(G_APP_INFO(tb->share_app));
  }
  tb->seal_exe = g_find_program_in_path("nostr-seal");
  tb->seal_gtk_exe = g_find_program_in_path("nostr-seal-gtk");

  const gchar *helper = g_getenv("NOSTR_NAUTILUS_SEAL_HELPER");
  if (helper == NULL || *helper == '\0')
    helper = NN_SEAL_HELPER_PATH;
  if (g_file_test(helper, G_FILE_TEST_IS_EXECUTABLE))
    tb->seal_helper = g_strdup(helper);

  tb->tools.share_exe = tb->share_exe;
  tb->tools.seal_exe = tb->seal_exe;
  tb->tools.seal_gtk_exe = tb->seal_gtk_exe;
  tb->tools.seal_helper = tb->seal_helper;
}

static void
nn_toolbox_clear(NnToolbox *tb)
{
  g_clear_object(&tb->share_app);
  g_free(tb->share_exe);
  g_free(tb->seal_exe);
  g_free(tb->seal_gtk_exe);
  g_free(tb->seal_helper);
}

/* --- NautilusFileInfo → NnFile ------------------------------------------ */

typedef struct {
  NnFile *files;
  guint   n;
} NnSelection;

static void
nn_selection_clear(NnSelection *s)
{
  for (guint i = 0; i < s->n; i++) {
    g_free((gchar *)s->files[i].name);
    g_free((gchar *)s->files[i].mime);
    g_free((gchar *)s->files[i].path);
    g_free((gchar *)s->files[i].uri);
  }
  g_free(s->files);
  s->files = NULL;
  s->n = 0;
}

static void
nn_file_from_info(NnFile *f, NautilusFileInfo *info)
{
  g_autoptr(GFile) location = nautilus_file_info_get_location(info);
  f->name = nautilus_file_info_get_name(info);
  f->mime = nautilus_file_info_get_mime_type(info);
  f->type = nautilus_file_info_get_file_type(info);
  f->uri = nautilus_file_info_get_uri(info);
  f->path = location ? g_file_get_path(location) : NULL;
  f->is_git_repo = f->type == G_FILE_TYPE_DIRECTORY && nn_dir_is_git_repo(f->path);
}

/* FALSE when no action can apply (e.g. a plain directory is selected), so
 * a large selection stops after the first such file. */
static gboolean
nn_selection_from_list(NnSelection *s, GList *infos)
{
  s->n = 0;
  s->files = g_new0(NnFile, g_list_length(infos));
  for (GList *l = infos; l != NULL; l = l->next) {
    NnFile *f = &s->files[s->n++];
    nn_file_from_info(f, NAUTILUS_FILE_INFO(l->data));
    if (f->type == G_FILE_TYPE_DIRECTORY && !f->is_git_repo)
      return FALSE;
    if (f->type != G_FILE_TYPE_DIRECTORY && f->type != G_FILE_TYPE_REGULAR)
      return FALSE;
  }
  return s->n > 0;
}

/* --- Menu items ----------------------------------------------------------- */

typedef struct {
  NnAction     action;
  const gchar *name;
  const gchar *label;
  const gchar *tip;
} NnItemSpec;

static const NnItemSpec nn_items[] = {
  { NN_ACTION_SHARE,   "NostrNautilus::share",   "Share to Nostr…",
    "Publish to Nostr with nostr-share (preview before signing)" },
  { NN_ACTION_UPLOAD,  "NostrNautilus::upload",  "Upload to Blossom…",
    "Upload to your Blossom servers and publish a NIP-94 file event" },
  { NN_ACTION_ENCRYPT, "NostrNautilus::encrypt", "Encrypt for Nostr Contact…",
    "Seal for one or more npubs with nostr-seal" },
  { NN_ACTION_DECRYPT, "NostrNautilus::decrypt", "Decrypt…",
    "Open with your Nostr signer (nostr-seal)" },
};

static void
on_item_activate(NautilusMenuItem *item, gpointer user_data)
{
  (void)item;
  GPtrArray *argvs = user_data;
  for (guint i = 0; i < argvs->len; i++) {
    const gchar *const *argv = g_ptr_array_index(argvs, i);
    g_autoptr(GError) error = NULL;
    if (!nn_launch_argv(argv, &error))
      g_warning("cannot launch %s: %s", argv[0], error ? error->message : "unknown error");
  }
}

static NautilusMenuItem *
nn_menu_item_new(const gchar *name, const gchar *label, const gchar *tip, GPtrArray *argvs)
{
  NautilusMenuItem *item = nautilus_menu_item_new(name, label, tip, NULL);
  g_signal_connect_data(item, "activate", G_CALLBACK(on_item_activate),
                        argvs, (GClosureNotify)(void (*)(void))g_ptr_array_unref, 0);
  return item;
}

/* --- Provider ------------------------------------------------------------- */

#define NN_TYPE_MENU_PROVIDER (nn_menu_provider_get_type())
G_DECLARE_FINAL_TYPE(NnMenuProvider, nn_menu_provider, NN, MENU_PROVIDER, GObject)

struct _NnMenuProvider {
  GObject parent_instance;
};

static void nn_menu_provider_iface_init(NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED(NnMenuProvider, nn_menu_provider, G_TYPE_OBJECT, 0,
                               G_IMPLEMENT_INTERFACE_DYNAMIC(NAUTILUS_TYPE_MENU_PROVIDER,
                                                             nn_menu_provider_iface_init))

static GList *
nn_get_file_items(NautilusMenuProvider *provider, GList *files)
{
  (void)provider;
  if (files == NULL)
    return NULL;

  NnSelection sel = { 0 };
  if (!nn_selection_from_list(&sel, files)) {
    nn_selection_clear(&sel);
    return NULL;
  }

  NnToolbox tb;
  nn_toolbox_init(&tb);
  GList *items = NULL;
  for (guint i = 0; i < G_N_ELEMENTS(nn_items); i++) {
    GPtrArray *argvs = nn_action_argvs(nn_items[i].action, sel.files, sel.n, &tb.tools);
    if (argvs != NULL)
      items = g_list_append(items, nn_menu_item_new(nn_items[i].name, nn_items[i].label,
                                                    nn_items[i].tip, argvs));
  }
  g_debug("%u file(s): %u item(s)", sel.n, g_list_length(items));
  nn_toolbox_clear(&tb);
  nn_selection_clear(&sel);
  return items;
}

static GList *
nn_get_background_items(NautilusMenuProvider *provider, NautilusFileInfo *folder)
{
  (void)provider;
  if (folder == NULL)
    return NULL;

  NnSelection sel = { 0 };
  GList one = { .data = folder, .next = NULL, .prev = NULL };
  GList *items = NULL;
  if (nn_selection_from_list(&sel, &one) && sel.files[0].type == G_FILE_TYPE_DIRECTORY) {
    NnToolbox tb;
    nn_toolbox_init(&tb);
    GPtrArray *argvs = nn_action_argvs(NN_ACTION_SHARE, sel.files, 1, &tb.tools);
    if (argvs != NULL)
      items = g_list_append(items,
                            nn_menu_item_new("NostrNautilus::share-repo",
                                             "Share This Repository to Nostr…",
                                             "Announce this git repository (NIP-34) with nostr-share",
                                             argvs));
    nn_toolbox_clear(&tb);
  }
  nn_selection_clear(&sel);
  return items;
}

static void
nn_menu_provider_iface_init(NautilusMenuProviderInterface *iface)
{
  iface->get_file_items = nn_get_file_items;
  iface->get_background_items = nn_get_background_items;
}

static void nn_menu_provider_init(NnMenuProvider *self) { (void)self; }
static void nn_menu_provider_class_init(NnMenuProviderClass *klass) { (void)klass; }
static void nn_menu_provider_class_finalize(NnMenuProviderClass *klass) { (void)klass; }

/* --- Module entry points --------------------------------------------------- */

static GType nn_types[1];

G_MODULE_EXPORT void
nautilus_module_initialize(GTypeModule *module)
{
  nn_menu_provider_register_type(module);
  nn_types[0] = NN_TYPE_MENU_PROVIDER;
  g_debug("initialized (encrypt helper %s)", NN_SEAL_HELPER_PATH);
}

G_MODULE_EXPORT void
nautilus_module_shutdown(void)
{
}

G_MODULE_EXPORT void
nautilus_module_list_types(const GType **types, int *num_types)
{
  /* NOSTR_NAUTILUS_DISABLE=1 hides every item without uninstalling. */
  const gchar *off = g_getenv("NOSTR_NAUTILUS_DISABLE");
  if (off != NULL && *off != '\0' && !g_str_equal(off, "0")) {
    g_debug("disabled by NOSTR_NAUTILUS_DISABLE");
    *types = nn_types;
    *num_types = 0;
    return;
  }
  *types = nn_types;
  *num_types = G_N_ELEMENTS(nn_types);
}
