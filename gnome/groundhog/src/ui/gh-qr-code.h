#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

/* A scannable QR texture with a four-module quiet zone. Transfer full. */
GdkTexture *gh_qr_code_texture_new(const gchar *text);

/* Presents the public npub as a NIP-21 QR code. */
void gh_npub_qr_dialog_present(GtkWidget *parent, const gchar *npub);

G_END_DECLS
