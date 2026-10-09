#ifndef GH_PUBLIC_NOTE_UI_H
#define GH_PUBLIC_NOTE_UI_H

#include "gh-window.h"
#include "gh-account-controller.h"
#include "gh-account-store.h"
#include "gh-account-relays.h"

G_BEGIN_DECLS

typedef struct {
  GhAccountController *accounts;
  GhAccountStore *store;
  GhAccountRelays *relays;
  GSettings *settings;
} GhPublicNoteUiConfig;

/* Installs offline reference resolution and explicit Find/Repost/Quote actions
 * on this window's conversation view. The view owns the attachment. */
void gh_public_note_ui_attach(GhWindow *window, const GhPublicNoteUiConfig *config);

G_END_DECLS
#endif
