#include "secret-clipboard.h"
#include "secure-mem.h"
#include <string.h>
typedef struct { gchar *text; gsize length; } SecretBytes;
struct _GrottoSecretClipboardLease {
  GdkClipboard *clipboard;
  GdkContentProvider *provider;
  GSource *timeout;
  gulong changed;
};
static void secret_bytes_free(gpointer data) {
  SecretBytes *secret = data;
  gnostr_secure_clear(secret->text, secret->length); g_free(secret->text); g_free(secret);
}
static gboolean clear_owned(gpointer data) {
  GrottoSecretClipboardLease *lease = data;
  if (lease->provider && gdk_clipboard_get_content(lease->clipboard) == lease->provider)
    gdk_clipboard_set_content(lease->clipboard, NULL);
  g_clear_object(&lease->provider);
  return G_SOURCE_REMOVE;
}
static void clipboard_changed(GdkClipboard *clipboard, gpointer data) {
  GrottoSecretClipboardLease *lease = data;
  if (lease->provider && gdk_clipboard_get_content(clipboard) != lease->provider) {
    /* No need to retain our secret bytes until timeout after another copy. */
    g_source_destroy(lease->timeout);
    g_clear_object(&lease->provider);
  }
}
void grotto_secret_clipboard_lease_free(GrottoSecretClipboardLease *lease) {
  if (!lease) return;
  g_signal_handler_disconnect(lease->clipboard, lease->changed);
  g_source_destroy(lease->timeout); g_source_unref(lease->timeout);
  clear_owned(lease);
  g_object_unref(lease->clipboard); g_free(lease);
}
GrottoSecretClipboardLease *grotto_secret_clipboard_copy(GdkClipboard *clipboard, const gchar *text) {
  g_return_val_if_fail(GDK_IS_CLIPBOARD(clipboard) && text, NULL);
  SecretBytes *secret = g_new0(SecretBytes, 1);
  secret->text = g_strdup(text); secret->length = strlen(text);
  GBytes *bytes = g_bytes_new_with_free_func(secret->text, secret->length, secret_bytes_free, secret);
  GrottoSecretClipboardLease *lease = g_new0(GrottoSecretClipboardLease, 1);
  lease->clipboard = g_object_ref(clipboard);
  lease->provider = gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", bytes);
  g_bytes_unref(bytes);
  lease->timeout = g_timeout_source_new_seconds(60);
  g_source_set_callback(lease->timeout, clear_owned, lease, NULL);
  g_source_attach(lease->timeout, NULL);
  gdk_clipboard_set_content(clipboard, lease->provider);
  lease->changed = g_signal_connect(clipboard, "changed", G_CALLBACK(clipboard_changed), lease);
  return lease;
}
