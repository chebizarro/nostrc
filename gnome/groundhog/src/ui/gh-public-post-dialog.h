#ifndef GH_PUBLIC_POST_DIALOG_H
#define GH_PUBLIC_POST_DIALOG_H

#include <adwaita.h>
#include "gh-account-controller.h"
#include "gh-account-relays.h"
#include "gh-store-public-notes.h"

G_BEGIN_DECLS

/* A public action only for a locally verified event. Each dialog freezes its
 * own account generation, signed original and reviewed unsigned event. */
AdwDialog *gh_public_post_dialog_new(GhAccountController *accounts,
    GhAccountRelays *relays, const GhPublicNote *original,
    const gchar *reference_uri, gboolean quote);

G_END_DECLS
#endif
