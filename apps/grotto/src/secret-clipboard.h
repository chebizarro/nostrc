#pragma once
#include <gtk/gtk.h>
/* Clipboard managers may retain secrets. Clears only this exact provider. */
/* Returns the auto-clear source id (useful for lifecycle tests). */
guint grotto_secret_clipboard_copy(GdkClipboard *clipboard, const gchar *text);
