#ifndef GH_FEATURES_H
#define GH_FEATURES_H

#include "gh-preferences-dialog.h"

G_BEGIN_DECLS

/*
 * The one list of what this build performs among the features preferences
 * control (privacy charter §7.1, §7.11: no fake support; W13b review B2).
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

#define GH_FEATURE_TOR              0 /* G09: network modes and the Tor transport */
/* G16: src/app/gh-notifier.c, run by gh-app-services.c with the store. */
#define GH_FEATURE_NOTIFICATIONS    GROUNDHOG_HAVE_NOTIFIER
/* G13: the composer sends through the durable outbox (gh-send-ui.c); a build
 * without it disables the composer, so Enter has nothing to send. */
#define GH_FEATURE_COMPOSER         GROUNDHOG_HAVE_OUTBOX
#define GH_FEATURE_REMOTE_IMAGES    0 /* no remote image loader */
#define GH_FEATURE_PROFILE_PICTURES 0 /* no profile picture fetcher */
#define GH_FEATURE_LINK_PREVIEWS    0 /* no link preview fetcher */
#define GH_FEATURE_REQUEST_FILTER   0 /* message requests are always kept apart */
#define GH_FEATURE_ATTACHMENTS      0 /* G21/G22: Blossom attachments */
#define GH_FEATURE_EXPIRY           GROUNDHOG_HAVE_EXPIRY /* G07: gh-expiry.c, gh-app-services.c */

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
