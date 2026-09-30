#ifndef GH_FEATURES_H
#define GH_FEATURES_H

#include "gh-preferences-dialog.h"

G_BEGIN_DECLS

/*
 * The one list of what this build performs among the features preferences
 * control (privacy charter §7.1, §7.11: no fake support; W13b review B2),
 * plus the few other UI surfaces that must not offer what is missing
 * (GH_FEATURE_ENCRYPTED_GROUPS).
 * Preferences shows a row whose feature is 0 here unbound, insensitive and
 * "Not available in this version yet" (gh-preferences-dialog.h says which
 * row needs which feature).
 *
 * Flipping a gate is one line: the change that lands a feature's consumer
 * sets its GH_FEATURE_* below to 1 (or to the GROUNDHOG_HAVE_* its code is
 * built under). tests/check_privacy.py (preference-consumers) reads this
 * file: every key the dialog binds whose features are not all a literal 0
 * here must be read somewhere in src/ outside the dialog. tests/ui/
 * test_preferences.c asserts each row of this build from the same values.
 *
 * Features that exist only with optional libraries follow the executable's
 * GROUNDHOG_HAVE_* definitions; CMake gives the preferences test the same
 * ones.
 */

#ifndef GROUNDHOG_HAVE_EXPIRY
#define GROUNDHOG_HAVE_EXPIRY 0
#endif
#ifndef GROUNDHOG_HAVE_NOTIFIER
#define GROUNDHOG_HAVE_NOTIFIER 0
#endif
#ifndef GROUNDHOG_HAVE_OUTBOX
#define GROUNDHOG_HAVE_OUTBOX 0
#endif
#ifndef GROUNDHOG_HAVE_TOR
#define GROUNDHOG_HAVE_TOR 0
#endif
#ifndef GROUNDHOG_HAVE_ATTACHMENTS
#define GROUNDHOG_HAVE_ATTACHMENTS 0
#endif

/* G09: network modes and the Tor transport (src/net/gh-net-session.c and
 * gh-relay-net.c, installed by gh-app-services.c before any connection). */
#define GH_FEATURE_TOR              GROUNDHOG_HAVE_TOR
/* G16: src/app/gh-notifier.c, run by gh-app-services.c with the store. */
#define GH_FEATURE_NOTIFICATIONS    GROUNDHOG_HAVE_NOTIFIER
/* G13: the composer sends through the durable outbox (gh-send-ui.c); a build
 * without it disables the composer, so Enter has nothing to send. */
#define GH_FEATURE_COMPOSER         GROUNDHOG_HAVE_OUTBOX
#define GH_FEATURE_REMOTE_IMAGES    0 /* no remote image loader */
#define GH_FEATURE_PROFILE_PICTURES 0 /* no profile picture fetcher */
#define GH_FEATURE_LINK_PREVIEWS    0 /* no link preview fetcher */
#define GH_FEATURE_REQUEST_FILTER   0 /* message requests are always kept apart */
/* G21/G22: encrypted Blossom attachments (src/media/gh-attachments.c,
 * src/app/gh-attachment-ui.c, run by gh-app-services.c with the store); a
 * build without OpenSSL or libsoup has none. */
#define GH_FEATURE_ATTACHMENTS      GROUNDHOG_HAVE_ATTACHMENTS
#define GH_FEATURE_EXPIRY           GROUNDHOG_HAVE_EXPIRY /* G07: gh-expiry.c, gh-app-services.c */
/* Not a preference: encrypted groups (Marmot MLS, nostrc-qp24.13). When 1,
 * gh-app-outbox.c runs GhMlsService beside each outbox, gh-app-services.c
 * attaches their UI (gh-mls-ui.c, nostrc-9xf5: New Group's encrypted page,
 * invitations, Group Info, the composer, the header count, "Some messages
 * in this group can't be read yet") and New Message's 10-recipient limit
 * points to encrypted groups (GhNewMessageConfig.encrypted_groups; charter
 * §7.9).
 *
 * The UI has landed (nostrc-9xf5) and is tested with the flag at 0: its
 * tests attach it directly (tests/ui/test_mls_ui.c). What flips it to 1, in
 * one line here (nostrc-9xf5 depends on each bead):
 *  - a complete, bounded group backfill: nostrc-dha5 (the 200-event queue),
 *    nostrc-5rfp (the lossless subscription's hard cap), nostrc-cpwf
 *    (until-based paging), nostrc-kzun (the >200-event backlog test) and
 *    nostrc-iihf (a stalled relay's live traffic doesn't hold a busy group);
 *  - honest group state: nostrc-xrya (a removed member sees the group end)
 *    and nostrc-oya4 (no "N messages" count of held events);
 *  - nostrc-8kb2 (these conditions; the real multi-engine composer routing
 *    tested: groundhog-composer /delegates);
 *  - and, per the privacy charter §7.9 ("compiled and shown only when
 *    qp24.13 passes acceptance"), nostrc-qp24.13's acceptance: two-device
 *    and third-party adopted-spec interop (nostrc-77pa, nostrc-7gx7).
 *    libmarmot 0.10.0 refuses MDK 0.8 (White Noise) members by default and
 *    has no adopted-profile group engine yet (nostrc-qp24.5.1), so "groups
 *    with people who use apps that support Marmot" is not yet true.
 * To try the UI before then, configure with
 * -DGROUNDHOG_ENCRYPTED_GROUPS_PREVIEW=ON: CMake defines it to 1 for the
 * executable only, with a warning, and refuses it for a release build type,
 * CPack, a distribution prefix or a packaging environment, and refuses to
 * install such a build (cmake/GroundhogPreviewGuard.cmake). */
#ifndef GH_FEATURE_ENCRYPTED_GROUPS
#define GH_FEATURE_ENCRYPTED_GROUPS 0
#endif

static inline GhPreferencesFeatures
gh_features_for_preferences(void)
{
  return (GH_FEATURE_TOR ? GH_PREFERENCES_FEATURE_TOR : 0) |
         (GH_FEATURE_NOTIFICATIONS ? GH_PREFERENCES_FEATURE_NOTIFICATIONS : 0) |
         (GH_FEATURE_COMPOSER ? GH_PREFERENCES_FEATURE_COMPOSER : 0) |
         (GH_FEATURE_REMOTE_IMAGES ? GH_PREFERENCES_FEATURE_REMOTE_IMAGES : 0) |
         (GH_FEATURE_PROFILE_PICTURES ? GH_PREFERENCES_FEATURE_PROFILE_PICTURES : 0) |
         (GH_FEATURE_LINK_PREVIEWS ? GH_PREFERENCES_FEATURE_LINK_PREVIEWS : 0) |
         (GH_FEATURE_REQUEST_FILTER ? GH_PREFERENCES_FEATURE_REQUEST_FILTER : 0) |
         (GH_FEATURE_ATTACHMENTS ? GH_PREFERENCES_FEATURE_ATTACHMENTS : 0) |
         (GH_FEATURE_EXPIRY ? GH_PREFERENCES_FEATURE_EXPIRY : 0);
}

G_END_DECLS
#endif
