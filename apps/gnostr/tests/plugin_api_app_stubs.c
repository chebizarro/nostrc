/* App-shell symbols that src/gnostr-plugin-api.c references and the plugin
 * API tests (test_plugin_raw_relay_api.c, test_plugin_publish_ack.c) never
 * reach: they create no main window and have no signer. These tests used to
 * link with the symbols left undefined (-undefined dynamic_lookup,
 * --unresolved-symbols=ignore-in-object-files), which the tree-wide -z now
 * turns into a load failure on Linux (nostrc-taue, nostrc-ig0z). Defining
 * them here links the tests completely.
 *
 * The window and repo browser are real, empty types, so the plugin API's
 * GNOSTR_IS_MAIN_WINDOW() / GNOSTR_IS_REPO_BROWSER() checks are well defined
 * (and false). The signer service reports that this process has none, as a
 * headless process would. Anything else aborts: a test that starts reaching
 * it needs a deliberate fake, not a silent no-op. */

#include "ipc/gnostr-signer-service.h"
#include "ui/gnostr-main-window.h"
#include "ui/gnostr-main-window-private.h"
#include "ui/gnostr-repo-browser.h"
#include "util/utils.h"

#define UNREACHABLE() g_error("%s: not available in the plugin API tests", G_STRFUNC)

/* struct _GnostrMainWindow comes from gnostr-main-window-private.h. */
G_DEFINE_FINAL_TYPE(GnostrMainWindow, gnostr_main_window, ADW_TYPE_APPLICATION_WINDOW)
static void gnostr_main_window_class_init(GnostrMainWindowClass *klass) { (void)klass; }
static void gnostr_main_window_init(GnostrMainWindow *self) { (void)self; }

struct _GnostrRepoBrowser { GtkWidget parent_instance; };
G_DEFINE_FINAL_TYPE(GnostrRepoBrowser, gnostr_repo_browser, GTK_TYPE_WIDGET)
static void gnostr_repo_browser_class_init(GnostrRepoBrowserClass *klass) { (void)klass; }
static void gnostr_repo_browser_init(GnostrRepoBrowser *self) { (void)self; }

GtkWidget *
gnostr_main_window_get_repo_browser(GnostrMainWindow *self)
{
  (void)self;
  UNREACHABLE();
  return NULL;
}

void
gnostr_main_window_open_profile(GtkWidget *window, const char *pubkey_hex)
{
  (void)window; (void)pubkey_hex;
  UNREACHABLE();
}

void
gnostr_main_window_view_thread(GtkWidget *window, const char *root_event_id)
{
  (void)window; (void)root_event_id;
  UNREACHABLE();
}

void
gnostr_main_window_navigate_to_dm_conversation_internal(GnostrMainWindow *self,
                                                        const char *peer_pubkey)
{
  (void)self; (void)peer_pubkey;
  UNREACHABLE();
}

void
gnostr_repo_browser_add_repository(GnostrRepoBrowser *self, const char *id,
                                   const char *name, const char *description,
                                   const char *clone_url, const char *web_url,
                                   const char *maintainer_pubkey, gint64 updated_at)
{
  (void)self; (void)id; (void)name; (void)description; (void)clone_url;
  (void)web_url; (void)maintainer_pubkey; (void)updated_at;
  UNREACHABLE();
}

void
gnostr_repo_browser_clear(GnostrRepoBrowser *self)
{
  (void)self;
  UNREACHABLE();
}

void
gnostr_repo_browser_add_patch(GnostrRepoBrowser *self, const char *id,
                              const char *pubkey, const char *repo_ref,
                              const char *subject, const char *content,
                              gboolean is_root, gint64 created_at)
{
  (void)self; (void)id; (void)pubkey; (void)repo_ref; (void)subject;
  (void)content; (void)is_root; (void)created_at;
  UNREACHABLE();
}

void
gnostr_repo_browser_add_issue(GnostrRepoBrowser *self, const char *id,
                              const char *pubkey, const char *repo_ref,
                              const char *subject, const char *content,
                              const char *status, gint64 created_at)
{
  (void)self; (void)id; (void)pubkey; (void)repo_ref; (void)subject;
  (void)content; (void)status; (void)created_at;
  UNREACHABLE();
}

GNostrPool *
gnostr_get_shared_query_pool(void)
{
  UNREACHABLE();
  return NULL;
}

GnostrSignerService *
gnostr_signer_service_get_default(void)
{
  return NULL;
}

gboolean
gnostr_signer_service_is_available(GnostrSignerService *self)
{
  (void)self;
  return FALSE;
}

const char *
gnostr_signer_service_get_pubkey(GnostrSignerService *self)
{
  (void)self;
  return NULL;
}

void
gnostr_signer_service_sign_event_async(GnostrSignerService *self, const char *event_json,
                                       GCancellable *cancellable,
                                       GnostrSignerCallback callback, gpointer user_data)
{
  (void)self; (void)event_json; (void)cancellable; (void)callback; (void)user_data;
  UNREACHABLE();
}

void
gnostr_signer_service_nip44_encrypt_async(GnostrSignerService *self, const char *peer_pubkey,
                                          const char *plaintext, GCancellable *cancellable,
                                          GnostrNip44Callback callback, gpointer user_data)
{
  (void)self; (void)peer_pubkey; (void)plaintext; (void)cancellable;
  (void)callback; (void)user_data;
  UNREACHABLE();
}

void
gnostr_signer_service_nip44_decrypt_async(GnostrSignerService *self, const char *peer_pubkey,
                                          const char *ciphertext, GCancellable *cancellable,
                                          GnostrNip44Callback callback, gpointer user_data)
{
  (void)self; (void)peer_pubkey; (void)ciphertext; (void)cancellable;
  (void)callback; (void)user_data;
  UNREACHABLE();
}

void
gnostr_signer_service_nip44_encrypt_bytes_async(GnostrSignerService *self,
                                                const char *peer_pubkey, GBytes *plaintext,
                                                GCancellable *cancellable,
                                                GnostrNip44Callback callback,
                                                gpointer user_data)
{
  (void)self; (void)peer_pubkey; (void)plaintext; (void)cancellable;
  (void)callback; (void)user_data;
  UNREACHABLE();
}

void
gnostr_signer_service_nip44_decrypt_bytes_async(GnostrSignerService *self,
                                                const char *peer_pubkey,
                                                const char *ciphertext,
                                                GCancellable *cancellable,
                                                GnostrNip44BytesCallback callback,
                                                gpointer user_data)
{
  (void)self; (void)peer_pubkey; (void)ciphertext; (void)cancellable;
  (void)callback; (void)user_data;
  UNREACHABLE();
}
