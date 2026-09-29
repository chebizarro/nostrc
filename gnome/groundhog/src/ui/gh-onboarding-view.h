#ifndef GH_ONBOARDING_VIEW_H
#define GH_ONBOARDING_VIEW_H

#include <adwaita.h>

#include "gh-inbox-setup.h"
#include "gh-window.h"

G_BEGIN_DECLS

/* One row of the onboarding lists (identities, message-relay choices and
 * publish results), shown by data/ui/gh-onboarding-row.blp. key is the npub
 * or relay URL. */
#define GH_TYPE_ONBOARDING_ITEM (gh_onboarding_item_get_type())
G_DECLARE_FINAL_TYPE(GhOnboardingItem, gh_onboarding_item, GH, ONBOARDING_ITEM, GObject)

const gchar *gh_onboarding_item_get_key(GhOnboardingItem *self);
const gchar *gh_onboarding_item_get_title(GhOnboardingItem *self);
const gchar *gh_onboarding_item_get_subtitle(GhOnboardingItem *self);
gboolean gh_onboarding_item_get_checked(GhOnboardingItem *self);
const gchar *gh_onboarding_item_get_icon_name(GhOnboardingItem *self);

/*
 * GhOnboardingView (charter §7.8, §8.2 G14): the full-window first-run flow
 * of data/ui/gh-onboarding-view.blp.
 *
 *  welcome  the privacy promise and what Groundhog can't hide (§1.4)
 *  account  an identity from the signer-owned store (GhAccountController);
 *           selecting writes only org.nostr.Groundhog current-npub, never
 *           Gnostr's; "Use Without an Account" finishes read-only
 *  signer   Nostr Signer's availability and an optional test: one
 *           signature of a never-published ephemeral event and a NIP-44
 *           round trip to the account itself
 *  inbox    the kind-10050 message relays: the account's current list when
 *           discovery found one, the reviewed suggestions (unticked) and
 *           addresses the user types (checked with
 *           gh_inbox_setup_normalize_url); "Check Privacy" probes the ticked
 *           relays; "Set Up Later" finishes and leaves the banner
 *  confirm  exactly what publishing does and where (GhInboxSetup plan)
 *  publish  GhInboxSetup's progress and each relay's outcome
 *  done
 *
 * Nothing on the welcome, account, signer, inbox or confirm pages contacts
 * a relay (PD-13, PT-9): only Publish and Check Privacy do, on the relays
 * shown. "finished" is emitted when the flow ends by any path.
 * gh_onboarding_attach() puts it in the window.
 */
#define GH_TYPE_ONBOARDING_VIEW (gh_onboarding_view_get_type())
G_DECLARE_FINAL_TYPE(GhOnboardingView, gh_onboarding_view, GH, ONBOARDING_VIEW, AdwBin)

/* config->accounts and config->settings are required; the rest is passed to
 * each GhInboxSetup and GhInboxProbe (custom transports are for tests). */
GhOnboardingView *gh_onboarding_view_new(const GhInboxSetupConfig *config);
/* From the welcome page. */
void gh_onboarding_view_show_welcome(GhOnboardingView *self);
/* Only the inbox step (from the "no inbox relays" banner); the welcome page
 * when no account is active. */
void gh_onboarding_view_show_inbox(GhOnboardingView *self);
/* The visible page's tag: welcome, account, signer, inbox, confirm,
 * publish or done. */
const gchar *gh_onboarding_view_get_page(GhOnboardingView *self);
/* GhOnboardingItem models, for tests. */
GListModel *gh_onboarding_view_get_identities(GhOnboardingView *self);
GListModel *gh_onboarding_view_get_relays(GhOnboardingView *self);
GListModel *gh_onboarding_view_get_results(GhOnboardingView *self);
/* The current publication, or NULL before Publish. */
GhInboxSetup *gh_onboarding_view_get_setup(GhOnboardingView *self);

/* Adds a GhOnboardingView to window's root stack as "onboarding", installs
 * win.setup-inbox (GH_STATUS_ACTION_SETUP_INBOX, the banners' [Set Up]) and
 * shows the view on a first run, i.e. while no Groundhog account is chosen
 * (current-npub is empty). "finished" returns the window to "main".
 * Released with window. */
GhOnboardingView *gh_onboarding_attach(GhWindow *window, const GhInboxSetupConfig *config);

G_END_DECLS
#endif
