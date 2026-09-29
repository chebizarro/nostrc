#ifndef GH_STATUS_H
#define GH_STATUS_H

#include <glib-object.h>

G_BEGIN_DECLS

/* Nostr Signer reachability as the banner needs it. An activatable signer is
 * AVAILABLE: every signer call may auto-start it over D-Bus, so it is not a
 * problem state and there is nothing for a "Start Signer" button to do. */
typedef enum {
  GH_STATUS_SIGNER_UNKNOWN,     /* not checked yet: no banner */
  GH_STATUS_SIGNER_AVAILABLE,   /* running, or D-Bus activatable */
  GH_STATUS_SIGNER_UNAVAILABLE, /* neither running nor activatable */
  GH_STATUS_SIGNER_NO_BUS       /* no session bus to reach it on */
} GhStatusSigner;

/* The active account's private-message receive path, from its relay-list
 * lookup to its inbox subscription (see gh-inbox-status.h for the mapping
 * from GhAccountRelays and GhDmInbox). */
typedef enum {
  GH_STATUS_INBOX_INACTIVE,      /* no receive path (no account, or a build without one) */
  GH_STATUS_INBOX_NO_SOURCES,    /* no relay configured to find the account's lists */
  GH_STATUS_INBOX_LOOKING,       /* looking up the account's relay lists */
  GH_STATUS_INBOX_LOOKUP_FAILED, /* the lookup relays failed; no inbox list known */
  GH_STATUS_INBOX_MISSING,       /* the account published no usable inbox (10050) list */
  GH_STATUS_INBOX_CONNECTING,
  GH_STATUS_INBOX_BACKFILLING,
  GH_STATUS_INBOX_LIVE,
  GH_STATUS_INBOX_UNREACHABLE,   /* every inbox relay failed */
  GH_STATUS_INBOX_ERROR          /* a local failure; see gh_status_get_inbox_error() */
} GhStatusInbox;

/* The active account's encrypted message store (gh-account-store.h; charter
 * §3.4, §7.15 #14-#16). Never a plaintext fallback: every state but OPEN and
 * EPHEMERAL means nothing is received. */
typedef enum {
  GH_STATUS_STORE_NONE,        /* no account, or a build without the store */
  GH_STATUS_STORE_OPENING,     /* key lookup and open in progress */
  GH_STATUS_STORE_OPEN,
  GH_STATUS_STORE_EPHEMERAL,   /* the user chose "Continue Without Saving Messages" */
  GH_STATUS_STORE_LOCKED,      /* keyring locked; unlocking needs the user (#14) */
  GH_STATUS_STORE_UNAVAILABLE, /* no Secret Service to hold the key (#15) */
  GH_STATUS_STORE_KEY_MISSING, /* the store exists, its key item is gone */
  GH_STATUS_STORE_CORRUPT,     /* damaged; shown read-only (#16) */
  GH_STATUS_STORE_ERROR        /* any other failure; see gh_status_get_store_error() */
} GhStatusStore;

/* The one sidebar banner (charter §7.1 "one banner at a time", §7.15). Listed
 * from the highest priority down; NONE when nothing needs saying. */
typedef enum {
  GH_STATUS_BANNER_NONE,
  GH_STATUS_BANNER_STORE_LOCKED,       /* §7.15 #14 [Unlock] */
  GH_STATUS_BANNER_STORE_UNAVAILABLE,  /* §7.15 #15 [Continue Without Saving Messages] */
  GH_STATUS_BANNER_STORE_KEY_MISSING,  /* [Start Fresh on This Device…] */
  GH_STATUS_BANNER_STORE_CORRUPT,      /* §7.15 #16 [Reset Storage…] */
  GH_STATUS_BANNER_STORE_ERROR,        /* [Try Again] */
  GH_STATUS_BANNER_OFFLINE,            /* §7.15 #5 */
  GH_STATUS_BANNER_TOR_UNREACHABLE,    /* §7.15 #6 */
  GH_STATUS_BANNER_SIGNER_UNAVAILABLE, /* §7.15 #3 */
  GH_STATUS_BANNER_SIGNER_NO_BUS,      /* §7.15 #3, without a session bus */
  GH_STATUS_BANNER_INBOX_ERROR,
  GH_STATUS_BANNER_NO_RELAYS,
  GH_STATUS_BANNER_LOOKUP_FAILED,
  GH_STATUS_BANNER_INBOX_MISSING,      /* §7.15 #7 */
  GH_STATUS_BANNER_INBOX_UNREACHABLE,  /* §7.15 #8 */
  GH_STATUS_BANNER_STORE_EPHEMERAL,    /* the explicit in-memory choice, kept visible */
  GH_STATUS_BANNER_STORE_OPENING,
  GH_STATUS_BANNER_LOOKING,
  GH_STATUS_BANNER_CONNECTING,
  GH_STATUS_BANNER_BACKFILLING
} GhStatusBanner;
#define GH_STATUS_BANNER_LAST GH_STATUS_BANNER_BACKFILLING

/* Application actions the store banners activate. The app's service container
 * (gh-app-services.h) installs them on the GApplication. */
#define GH_STATUS_ACTION_STORE_UNLOCK    "app.store-unlock"
#define GH_STATUS_ACTION_STORE_RETRY     "app.store-retry"
#define GH_STATUS_ACTION_STORE_EPHEMERAL "app.store-continue-without-saving"
/* KEY_MISSING and CORRUPT: a confirmation dialog, then crypto-shred and a new
 * store (gh_store_status_confirm_start_fresh()). */
#define GH_STATUS_ACTION_STORE_START_FRESH "app.store-start-fresh"
/* [Set Up] on the "no inbox relays" banners (§7.15 #7, and no relay at all):
 * the onboarding inbox step (gh-onboarding-view.h installs it on the
 * window). */
#define GH_STATUS_ACTION_SETUP_INBOX "win.setup-inbox"

GType gh_status_signer_get_type(void);
GType gh_status_inbox_get_type(void);
GType gh_status_banner_get_type(void);
GType gh_status_store_get_type(void);
#define GH_TYPE_STATUS_SIGNER (gh_status_signer_get_type())
#define GH_TYPE_STATUS_INBOX (gh_status_inbox_get_type())
#define GH_TYPE_STATUS_BANNER (gh_status_banner_get_type())
#define GH_TYPE_STATUS_STORE (gh_status_store_get_type())

#define GH_TYPE_STATUS (gh_status_get_type())
G_DECLARE_FINAL_TYPE(GhStatus, gh_status, GH, STATUS, GObject)

/* The window's status inputs and the single banner derived from them. It
 * holds no service: account, network and signer inputs are set by
 * gh-account-ui.c, the inbox input by gh-inbox-status.c, the store input by
 * gh-store-status.c, and tor-unreachable
 * is reserved for the Tor transport (charter G09), which nothing sets yet.
 * Every input is a writable property; "banner" notifies when it changes.
 * No banner is shown without an active account: the sidebar's account pages
 * explain those states. */
GhStatus *gh_status_new(void);

void gh_status_set_account_active(GhStatus *self, gboolean active);
void gh_status_set_network_available(GhStatus *self, gboolean available);
void gh_status_set_tor_unreachable(GhStatus *self, gboolean unreachable);
void gh_status_set_signer(GhStatus *self, GhStatusSigner signer);
/* error is the inbox's own explanation for GH_STATUS_INBOX_ERROR and
 * GH_STATUS_INBOX_UNREACHABLE; ignored otherwise. */
void gh_status_set_inbox(GhStatus *self, GhStatusInbox inbox, const gchar *error);
/* error explains GH_STATUS_STORE_ERROR, KEY_MISSING and CORRUPT; ignored
 * otherwise. */
void gh_status_set_store(GhStatus *self, GhStatusStore store, const gchar *error);

GhStatusInbox gh_status_get_inbox(GhStatus *self);
/* NULL unless the inbox input is ERROR or UNREACHABLE with an explanation. */
const gchar *gh_status_get_inbox_error(GhStatus *self);
GhStatusStore gh_status_get_store(GhStatus *self);
/* NULL unless the store input carries an explanation. */
const gchar *gh_status_get_store_error(GhStatus *self);
GhStatusBanner gh_status_get_banner(GhStatus *self);

/* Translated banner copy; "" for NONE. */
const gchar *gh_status_banner_get_title(GhStatusBanner banner);
/* The banner's button and the detailed action it activates, or NULL when the
 * state has none yet. [Set Up] (§7.15 #7, and "no relay is set up yet")
 * opens the onboarding inbox step (GH_STATUS_ACTION_SETUP_INBOX). Charter
 * §7.15 also lists [Network Settings] (#6) and [Details] (#8, preferences
 * G17); those surfaces do not exist, so no button pretends to open them.
 * The store banners' actions (GH_STATUS_ACTION_STORE_*) are application
 * actions. */
const gchar *gh_status_banner_get_button_label(GhStatusBanner banner);
const gchar *gh_status_banner_get_action(GhStatusBanner banner);
/* TRUE for states that need the user's attention (announced to assistive
 * technology); FALSE for NONE and progress states. */
gboolean gh_status_banner_is_problem(GhStatusBanner banner);

G_END_DECLS
#endif
