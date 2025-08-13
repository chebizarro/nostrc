#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define TYPE_SIGNER_WINDOW (signer_window_get_type())
G_DECLARE_FINAL_TYPE(SignerWindow, signer_window, SIGNER, WINDOW, AdwApplicationWindow)

SignerWindow *signer_window_new(AdwApplication *app);
void signer_window_show_page(SignerWindow *self, const char *name);

G_END_DECLS
