#pragma once
#include <gtk/gtk.h>
/* An owner-scoped, expiring copy. Clipboard managers may already have retained
 * it. Freeing the lease revokes only this exact provider, never a later copy. */
typedef struct _GrottoSecretClipboardLease GrottoSecretClipboardLease;
GrottoSecretClipboardLease *grotto_secret_clipboard_copy(GdkClipboard *clipboard, const gchar *text);
void grotto_secret_clipboard_lease_free(GrottoSecretClipboardLease *lease);
G_DEFINE_AUTOPTR_CLEANUP_FUNC(GrottoSecretClipboardLease, grotto_secret_clipboard_lease_free)
