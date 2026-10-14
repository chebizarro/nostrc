#pragma once
#include <adwaita.h>
/* GUI owner-context entry point for G08 URI activation and G10 Connect page.
 * NULL URI presents Paste / Camera / Create. An incoming URI is only parsed
 * here; the daemon contacts its relays only after the connect review is allowed. */
void grotto_pairing_present(GtkWidget *parent, const gchar *uri);
