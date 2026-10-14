#include "secret-clipboard.h"
#include "secure-mem.h"
#include <string.h>
typedef struct { gchar *text; gsize length; } SecretBytes;
typedef struct { GdkClipboard *clipboard; GdkContentProvider *provider; } ClipboardLease;
static void secret_bytes_free(gpointer data) {
  SecretBytes *secret = data;
  gnostr_secure_clear(secret->text, secret->length); g_free(secret->text); g_free(secret);
}
static gboolean clear_owned(gpointer data) {
  ClipboardLease *lease = data;
  if (gdk_clipboard_get_content(lease->clipboard) == lease->provider)
    gdk_clipboard_set_content(lease->clipboard, NULL);
  return G_SOURCE_REMOVE;
}
static void lease_free(gpointer data) {
  ClipboardLease *lease = data;
  g_object_unref(lease->clipboard); g_object_unref(lease->provider); g_free(lease);
}
guint grotto_secret_clipboard_copy(GdkClipboard *clipboard, const gchar *text) {
  g_return_val_if_fail(GDK_IS_CLIPBOARD(clipboard) && text, 0);
  SecretBytes *secret = g_new0(SecretBytes, 1);
  secret->text = g_strdup(text); secret->length = strlen(text);
  GBytes *bytes = g_bytes_new_with_free_func(secret->text, secret->length, secret_bytes_free, secret);
  GdkContentProvider *provider = gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", bytes);
  g_bytes_unref(bytes);
  gdk_clipboard_set_content(clipboard, provider);
  ClipboardLease *lease = g_new0(ClipboardLease, 1);
  lease->clipboard = g_object_ref(clipboard); lease->provider = provider;
  return g_timeout_add_seconds_full(G_PRIORITY_DEFAULT, 60, clear_owned, lease, lease_free);
}
