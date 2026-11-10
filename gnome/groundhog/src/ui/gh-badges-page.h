#ifndef GH_BADGES_PAGE_H
#define GH_BADGES_PAGE_H
#include <adwaita.h>
#include "gh-badge-types.h"
G_BEGIN_DECLS
/* GhBadgesPage: Preferences › Account › Profile Badges (plan §3.12,
 * nostrc-9lwl7.13): the verified awards this account received, the ordered
 * selection shown on its profile (add, remove, move), and the explicit
 * Publish Profile Badges… confirmation. Every network action (Look Up
 * Awards…, publishing) asks first; artwork loads only under Load Remote
 * Images and a badge reads fine without it. Backed by a copied
 * GhBadgesBackend whose owner is referenced; account switches (a changed
 * generation) dismiss the page. */
#define GH_TYPE_BADGES_PAGE (gh_badges_page_get_type())
G_DECLARE_FINAL_TYPE(GhBadgesPage, gh_badges_page, GH, BADGES_PAGE, AdwNavigationPage)
GhBadgesPage *gh_badges_page_new(const GhBadgesBackend *backend);
/* "Awarded by npub1abc…wxyz" for a badge (transfer full). */
gchar *gh_badge_describe_issuer(const GhBadge *badge);
/* The name a badge is shown by: its name, else its identifier (borrowed). */
const gchar *gh_badge_display_name(const GhBadge *badge);
/* app.look-up-badges (account, pubkey): confirms the discovery relays, runs
 * the lookup, and offers any relay the list hinted at as a separate,
 * explicitly approved second lookup. Conversation Info offers it for an
 * accepted conversation's people, never for a Message Request. */
void gh_badges_ui_install(GtkApplication *app, const GhBadgesBackend *backend);
void gh_badges_ui_uninstall(GtkApplication *app);
G_END_DECLS
#endif
